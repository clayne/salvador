/*
 * shrink.c - compressor implementation
 *
 * Copyright (C) 2021 Emmanuel Marty
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

/*
 * Uses the libdivsufsort library Copyright (c) 2003-2008 Yuta Mori
 *
 * Implements the ZX0 encoding designed by Einar Saukas. https://github.com/einar-saukas/ZX0
 * Also inspired by Charles Bloom's compression blog. http://cbloomrants.blogspot.com/
 *
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "libsalvador.h"
#include "matchfinder.h"
#include "shrink.h"
#include "format.h"

#define MIN_ENCODED_MATCH_SIZE   2
#define TOKEN_SIZE               1
#define OFFSET_COST(__offset)    (((__offset) <= 128) ? 8 : (7 + salvador_get_elias_size((((__offset) - 1) >> 7) + 1)))

/**
 * Write bitpacked value to output (compressed) buffer
 *
 * @param pOutData pointer to output buffer
 * @param nOutOffset current write index into output buffer
 * @param nMaxOutDataSize maximum size of output buffer, in bytes
 * @param nValue value to write
 * @param nBits number of least significant bits to write in value
 * @param nCurBitsOffset write index into output buffer, of current byte being filled with bits
 * @param nCurBitShift bit shift count
 *
 * @return updated write index into output buffer, or -1 in case of an error
 */
static int salvador_write_bits(unsigned char *pOutData, int nOutOffset, const int nMaxOutDataSize, const int nValue, const int nBits, int *nCurBitsOffset, int *nCurBitShift) {
   int i;

   if (nOutOffset < 0) return -1;

   for (i = nBits - 1; i >= 0; i--) {
      if ((*nCurBitsOffset) == INT_MIN) {
         /* Allocate a new byte in the stream to pack bits in */
         if (nOutOffset >= nMaxOutDataSize) return -1;
         (*nCurBitsOffset) = nOutOffset;
         (*nCurBitShift) = 7;
         pOutData[nOutOffset++] = 0;
      }

      pOutData[(*nCurBitsOffset)] |= ((nValue >> i) & 1) << (*nCurBitShift);

      (*nCurBitShift) --;
      if ((*nCurBitShift) == -1) {
         /* Current byte is full */
         (*nCurBitsOffset) = INT_MIN;
      }
   }

   return nOutOffset;
}

/**
 * Get the number of bits required to encode a gamma value
 *
 * @param nValue value to encode as gamma
 *
 * @return number of bits required for encoding
 */
static int salvador_get_elias_size(const int nValue) {
   int i;
   int nBits = 0;

   for (i = 2; i <= nValue; i <<= 1)
      ;

   i >>= 1;
   while ((i >>= 1) > 0) {
      nBits++;
      nBits++;
   }

   nBits++;

   return nBits;
}

/**
 * Write elias gamma encoded value to output (compressed) buffer
 *
 * @param pOutData pointer to output buffer
 * @param nOutOffset current write index into output buffer
 * @param nMaxOutDataSize maximum size of output buffer, in bytes
 * @param nValue value to write with gamma encoding
 * @param nIsInverted 1 to write inverted match offset encoding (V2), 0 to write V1 encoding
 * @param nCurBitsOffset write index into output buffer, of current byte being filled with bits
 * @param nCurBitShift bit shift count
 * @param nFirstBit where to store first bit, NULL to write all bits out normally
 *
 * @return updated write index into output buffer, or -1 in case of an error
 */
static int salvador_write_elias_value(unsigned char* pOutData, int nOutOffset, const int nMaxOutDataSize, const int nValue, const int nIsInverted, int* nCurBitsOffset, int* nCurBitShift, unsigned char* nFirstBit) {
   int i;

   for (i = 2; i <= nValue; i <<= 1)
      ;

   i >>= 1;
   while ((i >>= 1) > 0) {
      if (nFirstBit) {
         (*nFirstBit) &= 0xfe;
         nFirstBit = NULL;
      }
      else {
         nOutOffset = salvador_write_bits(pOutData, nOutOffset, nMaxOutDataSize, 0, 1, nCurBitsOffset, nCurBitShift);
      }
      if (nIsInverted)
         nOutOffset = salvador_write_bits(pOutData, nOutOffset, nMaxOutDataSize, (nValue & i) ? 0 : 1, 1, nCurBitsOffset, nCurBitShift);
      else
         nOutOffset = salvador_write_bits(pOutData, nOutOffset, nMaxOutDataSize, (nValue & i) ? 1 : 0, 1, nCurBitsOffset, nCurBitShift);
   }

   if (nFirstBit) {
      (*nFirstBit) = ((*nFirstBit) & 0xfe) | 1;
      nFirstBit = NULL;
   }
   else {
      nOutOffset = salvador_write_bits(pOutData, nOutOffset, nMaxOutDataSize, 1, 1, nCurBitsOffset, nCurBitShift);
   }

   return nOutOffset;
}

/**
 * Get the number of extra bits required to represent a literals length
 *
 * @param nLength literals length
 *
 * @return number of extra bits required
 */
static inline int salvador_get_literals_varlen_size(const int nLength) {
   if (nLength > 0)
      return TOKEN_SIZE + salvador_get_elias_size(nLength);
   else
      return 0;
}

/**
 * Write extra literals length bytes to output (compressed) buffer. The caller must first check that there is enough
 * room to write the bytes.
 *
 * @param pOutData pointer to output buffer
 * @param nOutOffset current write index into output buffer
 * @param nMaxOutDataSize maximum size of output buffer, in bytes
 * @param nCurNibbleOffset write index into output buffer, of current byte being filled with nibbles
 * @param nLength literals length
 */
static inline int salvador_write_literals_varlen(unsigned char *pOutData, int nOutOffset, const int nMaxOutDataSize, int nLength, int *nCurBitsOffset, int *nCurBitShift) {
   return salvador_write_elias_value(pOutData, nOutOffset, nMaxOutDataSize, nLength, 0, nCurBitsOffset, nCurBitShift, NULL);
}

/**
 * Get the number of extra bits required to represent a non-rep match length
 *
 * @param nLength encoded match length (actual match length - MIN_ENCODED_MATCH_SIZE)
 * @param nIsRepMatch 1 if requesting bits required to represent a rep-match, 0 to represent the length of a match with an offset
 *
 * @return number of extra bits required
 */
#define salvador_get_match_varlen_size_norep(__nLength) salvador_get_elias_size((__nLength) + 1)

/**
 * Get the number of extra bits required to represent a repmatch length
 *
 * @param nLength encoded match length (actual match length - MIN_ENCODED_MATCH_SIZE)
 * @param nIsRepMatch 1 if requesting bits required to represent a rep-match, 0 to represent the length of a match with an offset
 *
 * @return number of extra bits required
 */
#define salvador_get_match_varlen_size_rep(__nLength) salvador_get_elias_size((__nLength) + 1 + 1)

/**
 * Write extra encoded match length bytes to output (compressed) buffer. The caller must first check that there is enough
 * room to write the bytes.
 *
 * @param pOutData pointer to output buffer
 * @param nOutOffset current write index into output buffer
 * @param nMaxOutDataSize maximum size of output buffer, in bytes
 * @param nCurNibbleOffset write index into output buffer, of current byte being filled with nibbles
 * @param nLength encoded match length (actual match length - MIN_ENCODED_MATCH_SIZE)
 * @param nIsRepMatch 1 if writing the match length for a rep-match, 0 if writing the length for a match with an offset
 * @param nFirstBit where to store first bit, NULL to write all bits out normally
 */
static inline int salvador_write_match_varlen(unsigned char *pOutData, int nOutOffset, const int nMaxOutDataSize, int nLength, int nIsRepMatch, int* nCurBitsOffset, int* nCurBitShift, unsigned char* nFirstBit) {
   return salvador_write_elias_value(pOutData, nOutOffset, nMaxOutDataSize, nLength + 1 + (nIsRepMatch ? 1 : 0), 0, nCurBitsOffset, nCurBitShift, nFirstBit);
}

/**
 * Insert forward rep candidate
 *
 * @param pCompressor compression context
 * @param pInWindow pointer to input data window (previously compressed bytes + bytes to compress)
 * @param i input data window position whose matches are being considered
 * @param nMatchOffset match offset to use as rep candidate
 * @param nStartOffset current offset in input window (typically the number of previously compressed bytes)
 * @param nEndOffset offset to end finding matches at (typically the size of the total input window in bytes
 * @param nDepth current insertion depth
 */
static void salvador_insert_forward_match(salvador_compressor *pCompressor, const unsigned char *pInWindow, const int i, const int nMatchOffset, const int nStartOffset, const int nEndOffset, int nDepth) {
   const salvador_arrival *arrival = pCompressor->arrival + ((i - nStartOffset) * NARRIVALS_PER_POSITION);
   const int *rle_len = (int*)pCompressor->intervals /* reuse */;
   salvador_visited* visited = ((salvador_visited*)pCompressor->pos_data) - nStartOffset /* reuse */;
   int j;

   for (j = 0; j < NARRIVALS_PER_POSITION && arrival[j].from_slot; j++) {
      if (arrival[j].num_literals) {
         int nRepOffset = arrival[j].rep_offset;

         if (nMatchOffset != nRepOffset && nRepOffset) {
            int nRepPos = arrival[j].rep_pos;

            if (nRepPos >= nStartOffset &&
               (nRepPos + 1) < nEndOffset &&
               visited[nRepPos].outer != nMatchOffset) {

               visited[nRepPos].outer = nMatchOffset;

               if (visited[nRepPos].inner != nMatchOffset && nRepPos >= nMatchOffset && pCompressor->match[((nRepPos - nStartOffset) << MATCHES_PER_INDEX_SHIFT) + NMATCHES_PER_INDEX - 1].length == 0) {
                  const unsigned char* pInWindowAtRepOffset = pInWindow + nRepPos;

                  if (pInWindowAtRepOffset[0] == pInWindowAtRepOffset[-nMatchOffset]) {
                     visited[nRepPos].inner = nMatchOffset;

                     const int nLen0 = rle_len[nRepPos - nMatchOffset];
                     const int nLen1 = rle_len[nRepPos];
                     int nMinLen = (nLen0 < nLen1) ? nLen0 : nLen1;

                     int nMaxRepLen = nEndOffset - nRepPos;
                     if (nMaxRepLen > LCP_MAX)
                        nMaxRepLen = LCP_MAX;

                     if (nMinLen > nMaxRepLen)
                        nMinLen = nMaxRepLen;

                     const unsigned char* pInWindowMax = pInWindowAtRepOffset + nMaxRepLen;
                     pInWindowAtRepOffset += nMinLen;

                     while ((pInWindowAtRepOffset + 8) < pInWindowMax && !memcmp(pInWindowAtRepOffset, pInWindowAtRepOffset - nMatchOffset, 8))
                        pInWindowAtRepOffset += 8;
                     while ((pInWindowAtRepOffset + 4) < pInWindowMax && !memcmp(pInWindowAtRepOffset, pInWindowAtRepOffset - nMatchOffset, 4))
                        pInWindowAtRepOffset += 4;
                     while (pInWindowAtRepOffset < pInWindowMax && pInWindowAtRepOffset[0] == pInWindowAtRepOffset[-nMatchOffset])
                        pInWindowAtRepOffset++;

                     const int nCurRepLen = (int)(pInWindowAtRepOffset - (pInWindow + nRepPos));

                     salvador_match* fwd_match = pCompressor->match + ((nRepPos - nStartOffset) << MATCHES_PER_INDEX_SHIFT);
                     unsigned short* fwd_depth = pCompressor->match_depth + ((nRepPos - nStartOffset) << MATCHES_PER_INDEX_SHIFT);
                     int r;

                     for (r = 0; fwd_match[r].length; r++) {
                        if (fwd_match[r].offset == nMatchOffset) {
                           if ((int)fwd_match[r].length < nCurRepLen && (fwd_depth[r] & 0x3fff) == 0) {
                              fwd_match[r].length = nCurRepLen;
                              fwd_depth[r] = 0;
                           }
                           r = NMATCHES_PER_INDEX;
                           break;
                        }
                     }

                     if (r < NMATCHES_PER_INDEX) {
                        fwd_match[r].offset = nMatchOffset;
                        fwd_match[r].length = nCurRepLen;
                        fwd_depth[r] = 0;

                        if (nDepth < 9)
                           salvador_insert_forward_match(pCompressor, pInWindow, nRepPos, nMatchOffset, nStartOffset, nEndOffset, nDepth + 1);
                     }
                  }
               }
            }
         }
      }
   }
}

/**
 * Attempt to pick optimal matches, so as to produce the smallest possible output that decompresses to the same input
 *
 * @param pCompressor compression context
 * @param pInWindow pointer to input data window (previously compressed bytes + bytes to compress)
 * @param nStartOffset current offset in input window (typically the number of previously compressed bytes)
 * @param nEndOffset offset to end finding matches at (typically the size of the total input window in bytes
 * @param nInsertForwardReps non-zero to insert forward repmatch candidates, zero to use the previously inserted candidates
 * @param nCurRepMatchOffset starting rep offset for this block
 * @param nArrivalsPerPosition number of arrivals to record per input buffer position
 * @param nBlockFlags bit 0: 1 for first block, 0 otherwise; bit 1: 1 for last block, 0 otherwise
 */
static void salvador_optimize_forward(salvador_compressor *pCompressor, const unsigned char *pInWindow, const int nStartOffset, const int nEndOffset, const int nInsertForwardReps, const int *nCurRepMatchOffset, const int nArrivalsPerPosition, const int nBlockFlags) {
   salvador_arrival *arrival = pCompressor->arrival - (nStartOffset * NARRIVALS_PER_POSITION);
   const int* rle_len = (int*)pCompressor->intervals /* reuse */;
   salvador_visited* visited = ((salvador_visited*)pCompressor->pos_data) - nStartOffset /* reuse */;
   const int nModeSwitchPenalty = 0;
   int i, j, n;

   if ((nEndOffset - nStartOffset) > pCompressor->block_size) return;

   memset(arrival + (nStartOffset * NARRIVALS_PER_POSITION), 0, sizeof(salvador_arrival) * ((nEndOffset - nStartOffset + 1) * NARRIVALS_PER_POSITION));

   arrival[nStartOffset * NARRIVALS_PER_POSITION].from_slot = -1;
   arrival[nStartOffset * NARRIVALS_PER_POSITION].rep_offset = *nCurRepMatchOffset;

   for (i = (nStartOffset * NARRIVALS_PER_POSITION); i != ((nEndOffset+1) * NARRIVALS_PER_POSITION); i++) {
      arrival[i].cost = 0x40000000;
   }

   if (nInsertForwardReps) {
      memset(visited + nStartOffset, 0, (nEndOffset - nStartOffset) * sizeof(salvador_visited));
   }

   for (i = nStartOffset; i != nEndOffset; i++) {
      salvador_arrival *cur_arrival = &arrival[i * NARRIVALS_PER_POSITION];
      int m;
      
      for (j = 0; j < nArrivalsPerPosition && cur_arrival[j].from_slot; j++) {
         const int nPrevCost = cur_arrival[j].cost & 0x3fffffff;
         int nCodingChoiceCost = nPrevCost + 8 /* literal */;
         int nScore = cur_arrival[j].score + 1;
         int nNumLiterals = cur_arrival[j].num_literals + 1;

         if (nNumLiterals > 1)
            nCodingChoiceCost -= salvador_get_literals_varlen_size(nNumLiterals - 1);
         nCodingChoiceCost += salvador_get_literals_varlen_size(nNumLiterals);

         if (nNumLiterals == 1)
            nCodingChoiceCost += nModeSwitchPenalty;

         salvador_arrival* pDestSlots = &cur_arrival[NARRIVALS_PER_POSITION];
         if (nCodingChoiceCost < pDestSlots[nArrivalsPerPosition - 1].cost ||
            (nCodingChoiceCost == pDestSlots[nArrivalsPerPosition - 1].cost && nScore < (pDestSlots[nArrivalsPerPosition - 1].score))) {
            int nRepOffset = cur_arrival[j].rep_offset;
            int exists = 0;

            for (n = 0;
               pDestSlots[n].cost < nCodingChoiceCost;
               n++) {
               if (pDestSlots[n].rep_offset == nRepOffset) {
                  exists = 1;
                  break;
               }
            }

            if (!exists) {
               for (;
                  pDestSlots[n].cost == nCodingChoiceCost && nScore >= (pDestSlots[n].score);
                  n++) {
                  if (pDestSlots[n].rep_offset == nRepOffset) {
                     exists = 1;
                     break;
                  }
               }

               if (!exists) {
                  if (n < nArrivalsPerPosition) {
                     int nn;

                     for (nn = n;
                        nn < nArrivalsPerPosition && pDestSlots[nn].cost == nCodingChoiceCost;
                        nn++) {
                        if (pDestSlots[nn].rep_offset == nRepOffset) {
                           exists = 1;
                           break;
                        }
                     }

                     if (!exists) {
                        int z;

                        for (z = n; z < nArrivalsPerPosition - 1 && pDestSlots[z].from_slot; z++) {
                           if (pDestSlots[z].rep_offset == nRepOffset)
                              break;
                        }

                        memmove(&pDestSlots[n + 1],
                           &pDestSlots[n],
                           sizeof(salvador_arrival) * (z - n));

                        salvador_arrival* pDestArrival = &pDestSlots[n];
                        pDestArrival->cost = nCodingChoiceCost;
                        pDestArrival->from_pos = i;
                        pDestArrival->from_slot = j + 1;
                        pDestArrival->rep_offset = nRepOffset;
                        pDestArrival->rep_pos = cur_arrival[j].rep_pos;
                        pDestArrival->match_len = 0;
                        pDestArrival->num_literals = nNumLiterals;
                        pDestArrival->score = nScore;
                     }
                  }
               }
            }
         }
      }

      if (i == nStartOffset && (nBlockFlags & 1)) continue;

      const salvador_match *match = pCompressor->match + ((i - nStartOffset) << MATCHES_PER_INDEX_SHIFT);
      const unsigned short *match_depth = pCompressor->match_depth + ((i - nStartOffset) << MATCHES_PER_INDEX_SHIFT);
      const int nNumArrivalsForThisPos = j;
      int nOverallMinRepLen = 0, nOverallMaxRepLen = 0;

      int nRepMatchArrivalIdx[(2 * NARRIVALS_PER_POSITION) + 1];
      int nNumRepMatchArrivals = 0;

      if (i < nEndOffset) {
         int nMaxRepLenForPos = nEndOffset - i;
         if (nMaxRepLenForPos > LCP_MAX)
            nMaxRepLenForPos = LCP_MAX;

         const unsigned char* pInWindowStart = pInWindow + i;
         const unsigned char* pInWindowMax = pInWindowStart + nMaxRepLenForPos;

         for (j = 0; j < nNumArrivalsForThisPos; j++) {
            if (cur_arrival[j].num_literals) {
               int nRepOffset = cur_arrival[j].rep_offset;

               if (nRepOffset) {
                  if (i >= nRepOffset) {
                     if (pInWindow[i] == pInWindow[i - nRepOffset]) {
                        const unsigned char* pInWindowAtPos;

                        int nLen0 = rle_len[i - nRepOffset];
                        int nLen1 = rle_len[i];
                        int nMinLen = (nLen0 < nLen1) ? nLen0 : nLen1;

                        if (nMinLen > nMaxRepLenForPos)
                           nMinLen = nMaxRepLenForPos;
                        pInWindowAtPos = pInWindowStart + nMinLen;

                        while ((pInWindowAtPos + 8) < pInWindowMax && !memcmp(pInWindowAtPos - nRepOffset, pInWindowAtPos, 8))
                           pInWindowAtPos += 8;
                        while ((pInWindowAtPos + 4) < pInWindowMax && !memcmp(pInWindowAtPos - nRepOffset, pInWindowAtPos, 4))
                           pInWindowAtPos += 4;
                        while (pInWindowAtPos < pInWindowMax && pInWindowAtPos[-nRepOffset] == pInWindowAtPos[0])
                           pInWindowAtPos++;
                        int nCurRepLen = (int)(pInWindowAtPos - pInWindowStart);

                        if (nOverallMaxRepLen < nCurRepLen)
                           nOverallMaxRepLen = nCurRepLen;
                        nRepMatchArrivalIdx[nNumRepMatchArrivals++] = j;
                        nRepMatchArrivalIdx[nNumRepMatchArrivals++] = nCurRepLen;
                     }
                  }
               }
            }
         }
      }
      nRepMatchArrivalIdx[nNumRepMatchArrivals] = -1;

      for (m = 0; m < NMATCHES_PER_INDEX && match[m].length; m++) {
         const int nOrigMatchLen = match[m].length;
         const int nOrigMatchOffset = match[m].offset;
         const unsigned int nOrigMatchDepth = match_depth[m] & 0x3fff;
         const int nScorePenalty = 3 + ((match[m].length & 0x8000) >> 15);
         unsigned int d;

         for (d = 0; d <= nOrigMatchDepth; d += (nOrigMatchDepth ? nOrigMatchDepth : 1)) {
            const int nMatchOffset = nOrigMatchOffset - d;
            int nMatchLen = nOrigMatchLen - d;

            if ((i + nMatchLen) > nEndOffset)
               nMatchLen = nEndOffset - i;

            if (nInsertForwardReps) {
               salvador_insert_forward_match(pCompressor, pInWindow, i, nMatchOffset, nStartOffset, nEndOffset, 0);
            }

            int nNoRepmatchOffsetCost = OFFSET_COST(nMatchOffset);
            int nNoRepmatchScore, nStartingMatchLen, k;

            int nNonRepMatchArrivalIdx = -1;
            for (j = 0; j < nNumArrivalsForThisPos; j++) {
               int nRepOffset = cur_arrival[j].rep_offset;

               if (nMatchOffset != nRepOffset || cur_arrival[j].num_literals == 0) {
                  const int nPrevCost = cur_arrival[j].cost & 0x3fffffff;

                  nNoRepmatchOffsetCost += nPrevCost /* the actual cost of the literals themselves accumulates up the chain */;
                  if (!cur_arrival[j].num_literals)
                     nNoRepmatchOffsetCost += nModeSwitchPenalty;

                  nNoRepmatchScore = cur_arrival[j].score + nScorePenalty;
                  nNonRepMatchArrivalIdx = j;
                  break;
               }
            }

            if (nMatchLen >= LEAVE_ALONE_MATCH_SIZE) {
               nStartingMatchLen = nMatchLen;
            }
            else {
               nStartingMatchLen = 1;
            }

            for (k = nStartingMatchLen; k <= nMatchLen; k++) {
               salvador_arrival* pDestSlots = &cur_arrival[k * NARRIVALS_PER_POSITION];

               /* Insert non-repmatch candidate */

               if (k >= 2 && nNonRepMatchArrivalIdx >= 0) {
                  int nMatchLenCost = salvador_get_match_varlen_size_norep(k - MIN_ENCODED_MATCH_SIZE) + TOKEN_SIZE /* token */;
                  int nCodingChoiceCost = nMatchLenCost + nNoRepmatchOffsetCost;

                  if (nCodingChoiceCost < pDestSlots[nArrivalsPerPosition - 2].cost ||
                     (nCodingChoiceCost == pDestSlots[nArrivalsPerPosition - 2].cost && nNoRepmatchScore < (pDestSlots[nArrivalsPerPosition - 2].score))) {
                     int exists = 0;

                     for (n = 0;
                        pDestSlots[n].cost < nCodingChoiceCost;
                        n++) {
                        if (pDestSlots[n].rep_offset == nMatchOffset) {
                           exists = 1;
                           break;
                        }
                     }

                     if (!exists) {
                        for (;
                           pDestSlots[n].cost == nCodingChoiceCost && nNoRepmatchScore >= (pDestSlots[n].score);
                           n++) {
                           if (pDestSlots[n].rep_offset == nMatchOffset) {
                              exists = 1;
                              break;
                           }
                        }

                        if (!exists) {
                           if (n < nArrivalsPerPosition - 1) {
                              int nn;

                              for (nn = n;
                                 nn < nArrivalsPerPosition && pDestSlots[nn].cost == nCodingChoiceCost;
                                 nn++) {
                                 if (pDestSlots[nn].rep_offset == nMatchOffset) {
                                    exists = 1;
                                    break;
                                 }
                              }

                              if (!exists) {
                                 int z;

                                 for (z = n; z < nArrivalsPerPosition - 1 && pDestSlots[z].from_slot; z++) {
                                    if (pDestSlots[z].rep_offset == nMatchOffset)
                                       break;
                                 }

                                 memmove(&pDestSlots[n + 1],
                                    &pDestSlots[n],
                                    sizeof(salvador_arrival) * (z - n));

                                 salvador_arrival* pDestArrival = &pDestSlots[n];
                                 pDestArrival->cost = nCodingChoiceCost;
                                 pDestArrival->from_pos = i;
                                 pDestArrival->from_slot = nNonRepMatchArrivalIdx + 1;
                                 pDestArrival->match_len = k;
                                 pDestArrival->num_literals = 0;
                                 pDestArrival->score = nNoRepmatchScore;
                                 pDestArrival->rep_offset = nMatchOffset;
                                 pDestArrival->rep_pos = i;
                              }
                           }
                        }
                     }
                  }
               }

               /* Insert repmatch candidates */

               if (k > nOverallMinRepLen  && k <= nOverallMaxRepLen) {
                  int nMatchLenCost = salvador_get_match_varlen_size_rep(k - MIN_ENCODED_MATCH_SIZE) + TOKEN_SIZE /* token */;
                  int nCurRepMatchArrival;

                  if (k <= LEAVE_ALONE_MATCH_SIZE)
                     nOverallMinRepLen = k;
                  else if (nOverallMaxRepLen == k)
                     nOverallMaxRepLen--;

                  for (nCurRepMatchArrival = 0; (j = nRepMatchArrivalIdx[nCurRepMatchArrival]) >= 0; nCurRepMatchArrival += 2) {
                     if (nRepMatchArrivalIdx[nCurRepMatchArrival + 1] >= k) {
                        const int nPrevCost = cur_arrival[j].cost & 0x3fffffff;
                        int nRepCodingChoiceCost = nPrevCost /* the actual cost of the literals themselves accumulates up the chain */ + nMatchLenCost;
                        int nScore = cur_arrival[j].score + 2;

                        if (nRepCodingChoiceCost < pDestSlots[nArrivalsPerPosition - 1].cost ||
                           (nRepCodingChoiceCost == pDestSlots[nArrivalsPerPosition - 1].cost && nScore < (pDestSlots[nArrivalsPerPosition - 1].score))) {
                           int nRepOffset = cur_arrival[j].rep_offset;
                           int exists = 0;

                           for (n = 0;
                              pDestSlots[n].cost < nRepCodingChoiceCost;
                              n++) {
                              if (pDestSlots[n].rep_offset == nRepOffset) {
                                 exists = 1;
                                 break;
                              }
                           }

                           if (!exists) {
                              for (;
                                 pDestSlots[n].cost == nRepCodingChoiceCost && nScore >= (pDestSlots[n].score);
                                 n++) {
                                 if (pDestSlots[n].rep_offset == nRepOffset) {
                                    exists = 1;
                                    break;
                                 }
                              }

                              if (!exists) {
                                 if (n < nArrivalsPerPosition) {
                                    int nn;

                                    for (nn = n;
                                       nn < nArrivalsPerPosition && pDestSlots[nn].cost == nRepCodingChoiceCost;
                                       nn++) {
                                       if (pDestSlots[nn].rep_offset == nRepOffset) {
                                          exists = 1;
                                          break;
                                       }
                                    }

                                    if (!exists) {
                                       int z;

                                       for (z = n; z < nArrivalsPerPosition - 1 && pDestSlots[z].from_slot; z++) {
                                          if (pDestSlots[z].rep_offset == nRepOffset)
                                             break;
                                       }

                                       memmove(&pDestSlots[n + 1],
                                          &pDestSlots[n],
                                          sizeof(salvador_arrival) * (z - n));

                                       salvador_arrival* pDestArrival = &pDestSlots[n];
                                       pDestArrival->cost = nRepCodingChoiceCost;
                                       pDestArrival->from_pos = i;
                                       pDestArrival->from_slot = j + 1;
                                       pDestArrival->match_len = k;
                                       pDestArrival->num_literals = 0;
                                       pDestArrival->score = nScore;
                                       pDestArrival->rep_offset = nRepOffset;
                                       pDestArrival->rep_pos = i;
                                    }
                                 }
                              }
                           }
                        }
                        else {
                           break;
                        }
                     }
                  }
               }
            }
         }

         if (nOrigMatchLen >= 512)
            break;
      }
   }
   
   if (!nInsertForwardReps) {
      const salvador_arrival* end_arrival = &arrival[(i * NARRIVALS_PER_POSITION) + 0];
      salvador_final_match* pBestMatch = pCompressor->best_match - nStartOffset;

      while (end_arrival->from_slot > 0 && end_arrival->from_pos >= 0 && (int)end_arrival->from_pos < nEndOffset) {
         pBestMatch[end_arrival->from_pos].length = end_arrival->match_len;
         if (end_arrival->match_len)
            pBestMatch[end_arrival->from_pos].offset = end_arrival->rep_offset;
         else
            pBestMatch[end_arrival->from_pos].offset = 0;

         end_arrival = &arrival[(end_arrival->from_pos * NARRIVALS_PER_POSITION) + (end_arrival->from_slot - 1)];
      }
   }
}

/**
 * Attempt to replace matches by literals when it makes the final bitstream smaller, and merge large matches
 *
 * @param pCompressor compression context
 * @param pInWindow pointer to input data window (previously compressed bytes + bytes to compress)
 * @param pBestMatch optimal matches to evaluate and update
 * @param nStartOffset current offset in input window (typically the number of previously compressed bytes)
 * @param nEndOffset offset to end finding matches at (typically the size of the total input window in bytes
 * @param nCurRepMatchOffset starting rep offset for this block
 * @param nBlockFlags bit 0: 1 for first block, 0 otherwise; bit 1: 1 for last block, 0 otherwise
 *
 * @return non-zero if the number of tokens was reduced, 0 if it wasn't
 */
static int salvador_reduce_commands(salvador_compressor *pCompressor, const unsigned char *pInWindow, salvador_final_match *pBestMatch, const int nStartOffset, const int nEndOffset, const int *nCurRepMatchOffset, const int nBlockFlags) {
   int i;
   int nNumLiterals = (nBlockFlags & 1) ? 1 : 0;
   int nRepMatchOffset = *nCurRepMatchOffset;
   int nFollowsLiteral = 0;
   int nDidReduce = 0;
   int nLastMatchLen = 0;

   for (i = nStartOffset + ((nBlockFlags & 1) ? 1 : 0); i < nEndOffset; ) {
      salvador_final_match *pMatch = pBestMatch + i;

      if (nFollowsLiteral &&
         pMatch->length == 0 &&
         (i + 1) < nEndOffset &&
         pBestMatch[i + 1].length >= MIN_ENCODED_MATCH_SIZE &&
         pBestMatch[i + 1].length < MAX_VARLEN &&
         pBestMatch[i + 1].offset &&
         i >= pBestMatch[i + 1].offset &&
         (i + pBestMatch[i + 1].length + 1) <= nEndOffset &&
         !memcmp(pInWindow + i - (pBestMatch[i + 1].offset), pInWindow + i, pBestMatch[i + 1].length + 1)) {
         int nCurLenSize, nReducedLenSize;

         if (nRepMatchOffset && pBestMatch[i + 1].offset == nRepMatchOffset) {
            nCurLenSize = salvador_get_match_varlen_size_rep(pBestMatch[i + 1].length - MIN_ENCODED_MATCH_SIZE);
            nReducedLenSize = salvador_get_match_varlen_size_rep(pBestMatch[i + 1].length + 1 - MIN_ENCODED_MATCH_SIZE);
         }
         else {
            nCurLenSize = salvador_get_match_varlen_size_norep(pBestMatch[i + 1].length - MIN_ENCODED_MATCH_SIZE);
            nReducedLenSize = salvador_get_match_varlen_size_norep(pBestMatch[i + 1].length + 1 - MIN_ENCODED_MATCH_SIZE);
         }

         if ((nReducedLenSize - nCurLenSize) <= 8) {
            /* Merge */
            pBestMatch[i].length = pBestMatch[i + 1].length + 1;
            pBestMatch[i].offset = pBestMatch[i + 1].offset;
            pBestMatch[i + 1].length = 0;
            pBestMatch[i + 1].offset = 0;
            nDidReduce = 1;
            nFollowsLiteral = 0;
            continue;
         }
      }

      if (pMatch->length >= MIN_ENCODED_MATCH_SIZE) {
         if (nFollowsLiteral && (i + pMatch->length) < nEndOffset /* Don't consider the last match in the block, we can only reduce a match inbetween other tokens */) {
            int nNextIndex = i + pMatch->length;
            int nNextLiterals = 0;

            while (nNextIndex < nEndOffset && pBestMatch[nNextIndex].length == 0) {
               nNextLiterals++;
               nNextIndex++;
            }

            if (nNextIndex < nEndOffset && pBestMatch[nNextIndex].length >= MIN_ENCODED_MATCH_SIZE) {
               /* This command is a match, is followed by 'nNextLiterals' literals and then by another match */

               if (nRepMatchOffset && pMatch->offset != nRepMatchOffset && (pBestMatch[nNextIndex].offset != pMatch->offset || pBestMatch[nNextIndex].offset == nRepMatchOffset ||
                  OFFSET_COST(pMatch->offset) > OFFSET_COST(pBestMatch[nNextIndex].offset))) {
                  /* Check if we can change the current match's offset to be the same as the previous match's offset, and get an extra repmatch. This will occur when
                   * matching large regions of identical bytes for instance, where there are too many offsets to be considered by the parser, and when not compressing to favor the
                   * ratio (the forward arrivals parser already has this covered). */
                  if (i >= nRepMatchOffset &&
                     (i - nRepMatchOffset + pMatch->length) <= nEndOffset &&
                     !memcmp(pInWindow + i - nRepMatchOffset, pInWindow + i - pMatch->offset, pMatch->length)) {
                     pMatch->offset = nRepMatchOffset;
                     nDidReduce = 1;
                  }
               }

               if (pBestMatch[nNextIndex].offset && pMatch->offset != pBestMatch[nNextIndex].offset && nRepMatchOffset != pBestMatch[nNextIndex].offset && nNextLiterals) {
                  /* Otherwise, try to gain a match forward as well */
                  if (i >= pBestMatch[nNextIndex].offset && (i - pBestMatch[nNextIndex].offset + pMatch->length) <= nEndOffset && pMatch->offset != nRepMatchOffset) {
                     int nMaxLen = 0;
                     while (nMaxLen < pMatch->length && pInWindow[i - pBestMatch[nNextIndex].offset + nMaxLen] == pInWindow[i - pMatch->offset + nMaxLen])
                        nMaxLen++;
                     if (nMaxLen >= pMatch->length) {
                        /* Replace */
                        pMatch->offset = pBestMatch[nNextIndex].offset;
                        nDidReduce = 1;
                     }
                     else if (nMaxLen >= 2) {
                        int nPartialSizeBefore, nPartialSizeAfter;

                        nPartialSizeBefore = salvador_get_match_varlen_size_norep(pMatch->length - MIN_ENCODED_MATCH_SIZE);
                        nPartialSizeBefore += OFFSET_COST(pMatch->offset);
                        nPartialSizeBefore += salvador_get_literals_varlen_size(nNextLiterals);

                        nPartialSizeAfter = salvador_get_match_varlen_size_rep(nMaxLen - MIN_ENCODED_MATCH_SIZE);
                        nPartialSizeAfter += salvador_get_literals_varlen_size(nNextLiterals + (pMatch->length - nMaxLen)) + ((pMatch->length - nMaxLen) << 3);

                        if (nPartialSizeAfter < nPartialSizeBefore) {
                           int j;

                           /* We gain a repmatch that is shorter than the original match as this is the best we can do, so it is followed by extra literals, but
                            * we have calculated that this is shorter */
                           pMatch->offset = pBestMatch[nNextIndex].offset;
                           for (j = nMaxLen; j < pMatch->length; j++) {
                              pBestMatch[i + j].length = 0;
                           }
                           pMatch->length = nMaxLen;
                           nDidReduce = 1;
                        }
                     }
                  }
               }

               if (pMatch->length < 9 /* Don't waste time considering large matches, they will always win over literals */) {
                  /* Calculate this command's current cost (excluding 'nNumLiterals' bytes) */

                  int nCurCommandSize = 0;
                  if (nNumLiterals != 0) {
                     nCurCommandSize += salvador_get_literals_varlen_size(nNumLiterals);
                     nCurCommandSize += (nNumLiterals << 3);
                  }
                  if (nRepMatchOffset && pMatch->offset == nRepMatchOffset && nNumLiterals != 0) {
                     /* Rep match */
                     nCurCommandSize += 1; /* rep-match follows */

                     /* Match length */
                     nCurCommandSize += salvador_get_match_varlen_size_rep(pMatch->length - MIN_ENCODED_MATCH_SIZE);
                  }
                  else {
                     /* Match with offset */
                     nCurCommandSize += 1; /* match with offset follows */

                     /* High bits of match offset */
                     nCurCommandSize += salvador_get_elias_size(((pMatch->offset - 1) >> 7) + 1);

                     /* Low byte of match offset */
                     nCurCommandSize += 7;

                     /* Match length */
                     nCurCommandSize += salvador_get_match_varlen_size_norep(pMatch->length - MIN_ENCODED_MATCH_SIZE);

                  }

                  /* Calculate the next command's current cost */
                  int nNextCommandSize = 0;
                  if (nNextLiterals != 0) {
                     nNextCommandSize += salvador_get_literals_varlen_size(nNextLiterals);
                     nNextCommandSize += (nNextLiterals << 3);
                  }
                  if (pMatch->offset && pBestMatch[nNextIndex].offset == pMatch->offset && nNextLiterals != 0) {
                     /* Rep match */
                     nNextCommandSize += 1; /* rep-match follows */

                     /* Match length */
                     nNextCommandSize += salvador_get_match_varlen_size_rep(pBestMatch[nNextIndex].length - MIN_ENCODED_MATCH_SIZE);
                  }
                  else {
                     /* Match with offset */
                     nNextCommandSize += 1; /* match with offset follows */

                     /* High bits of match offset */
                     nNextCommandSize += salvador_get_elias_size(((pBestMatch[nNextIndex].offset - 1) >> 7) + 1);

                     /* Low byte of match offset */
                     nNextCommandSize += 7;

                     /* Match length */
                     nNextCommandSize += salvador_get_match_varlen_size_norep(pBestMatch[nNextIndex].length - MIN_ENCODED_MATCH_SIZE);
                  }

                  int nOriginalCombinedCommandSize = nCurCommandSize + nNextCommandSize;

                  /* Calculate the cost of replacing this match command by literals + the next command with the cost of encoding these literals (excluding 'nNumLiterals' bytes) */
                  int nReducedCommandSize = (pMatch->length << 3);
                  nReducedCommandSize += salvador_get_literals_varlen_size(nNumLiterals + pMatch->length + nNextLiterals);
                  nReducedCommandSize += ((nNumLiterals + nNextLiterals) << 3);

                  if (nRepMatchOffset && pBestMatch[nNextIndex].offset == nRepMatchOffset && (nNumLiterals + pMatch->length + nNextLiterals) != 0) {
                     /* Rep match */
                     nReducedCommandSize += 1; /* rep-match follows */

                     /* Match length */
                     nReducedCommandSize += salvador_get_match_varlen_size_rep(pBestMatch[nNextIndex].length - MIN_ENCODED_MATCH_SIZE);
                  }
                  else {
                     /* Match with offset */
                     nReducedCommandSize += 1; /* match with offset follows */

                     /* High bits of match offset */
                     nReducedCommandSize += salvador_get_elias_size(((pBestMatch[nNextIndex].offset - 1) >> 7) + 1);

                     /* Low byte of match offset */
                     nReducedCommandSize += 7;

                     /* Match length */
                     nReducedCommandSize += salvador_get_match_varlen_size_norep(pBestMatch[nNextIndex].length - MIN_ENCODED_MATCH_SIZE);
                  }

                  if (nOriginalCombinedCommandSize >= nReducedCommandSize) {
                     /* Reduce */
                     int nMatchLen = pMatch->length;
                     int j;

                     for (j = 0; j < nMatchLen; j++) {
                        pBestMatch[i + j].length = 0;
                     }

                     nDidReduce = 1;
                     nFollowsLiteral = 0;
                     continue;
                  }
               }
            }
         }

         if ((i + pMatch->length) <= nEndOffset && pMatch->offset > 0 && pMatch->length >= MIN_ENCODED_MATCH_SIZE &&
            pBestMatch[i + pMatch->length].offset > 0 &&
            pBestMatch[i + pMatch->length].length >= MIN_ENCODED_MATCH_SIZE &&
            (pMatch->length + pBestMatch[i + pMatch->length].length) >= LEAVE_ALONE_MATCH_SIZE &&
            (pMatch->length + pBestMatch[i + pMatch->length].length) <= MAX_VARLEN &&
            (i + pMatch->length) > pMatch->offset &&
            (i + pMatch->length) > pBestMatch[i + pMatch->length].offset &&
            (i + pMatch->length + pBestMatch[i + pMatch->length].length) <= nEndOffset &&
            !memcmp(pInWindow + i - pMatch->offset + pMatch->length,
               pInWindow + i + pMatch->length - pBestMatch[i + pMatch->length].offset,
               pBestMatch[i + pMatch->length].length)) {

            int nNextIndex = i + pMatch->length + pBestMatch[i + pMatch->length].length;
            int nNextLiterals = 0;

            while (nNextIndex < nEndOffset && pBestMatch[nNextIndex].length == 0) {
               nNextIndex++;
               nNextLiterals++;
            }

            int nCurPartialSize = 0;
            if (nRepMatchOffset && pMatch->offset == nRepMatchOffset && nNumLiterals != 0) {
               /* Rep match */
               nCurPartialSize += 1; /* rep-match follows */

               /* Match length */
               nCurPartialSize += salvador_get_match_varlen_size_rep(pMatch->length - MIN_ENCODED_MATCH_SIZE);
            }
            else {
               /* Match with offset */
               nCurPartialSize += 1; /* match with offset follows */

               /* High bits of match offset */
               nCurPartialSize += salvador_get_elias_size(((pMatch->offset - 1) >> 7) + 1);

               /* Low byte of match offset */
               nCurPartialSize += 7;

               /* Match length */
               nCurPartialSize += salvador_get_match_varlen_size_norep(pMatch->length - MIN_ENCODED_MATCH_SIZE);
            }

            /* Match with offset */
            nCurPartialSize += 1; /* match with offset */

            /* High bits of match offset */
            nCurPartialSize += salvador_get_elias_size(((pBestMatch[i + pMatch->length].offset - 1) >> 7) + 1);

            /* Low byte of match offset */
            nCurPartialSize += 7;

            /* Match length */
            nCurPartialSize += salvador_get_match_varlen_size_norep(pBestMatch[i + pMatch->length].length - MIN_ENCODED_MATCH_SIZE);

            if (nNextIndex < nEndOffset) {
               if (pBestMatch[i + pMatch->length].offset && pBestMatch[nNextIndex].offset == pBestMatch[i + pMatch->length].offset && nNextLiterals != 0) {
                  /* Rep match */
                  nCurPartialSize += 1; /* rep-match follows */

                  /* Match length */
                  nCurPartialSize += salvador_get_match_varlen_size_rep(pBestMatch[nNextIndex].length - MIN_ENCODED_MATCH_SIZE);
               }
               else {
                  /* Match with offset */
                  nCurPartialSize += 1; /* match with offset follows */

                  /* High bits of match offset */
                  nCurPartialSize += salvador_get_elias_size(((pBestMatch[nNextIndex].offset - 1) >> 7) + 1);

                  /* Low byte of match offset */
                  nCurPartialSize += 7;

                  /* Match length */
                  nCurPartialSize += salvador_get_match_varlen_size_norep(pBestMatch[nNextIndex].length - MIN_ENCODED_MATCH_SIZE);
               }
            }

            int nReducedPartialSize = 0;
            if (nRepMatchOffset && pMatch->offset == nRepMatchOffset && nNumLiterals != 0) {
               /* Rep match */
               nReducedPartialSize += 1; /* rep-match follows */

               /* Match length */
               nReducedPartialSize += salvador_get_match_varlen_size_rep(pMatch->length + pBestMatch[i + pMatch->length].length - MIN_ENCODED_MATCH_SIZE);
            }
            else {
               /* Match with offset */
               nReducedPartialSize += 1; /* match with offset follows */

               /* High bits of match offset */
               nReducedPartialSize += salvador_get_elias_size(((pMatch->offset - 1) >> 7) + 1);

               /* Low byte of match offset */
               nReducedPartialSize += 7;

               /* Match length */
               nReducedPartialSize += salvador_get_match_varlen_size_norep(pMatch->length + pBestMatch[i + pMatch->length].length - MIN_ENCODED_MATCH_SIZE);
            }

            int nCannotReduce = 0;
            if (nNextIndex < nEndOffset) {
               if (pMatch->offset && pBestMatch[nNextIndex].offset == pMatch->offset && nNextLiterals != 0) {
                  /* Rep match */
                  nReducedPartialSize += 1; /* rep-match follows */

                  /* Match length */
                  nReducedPartialSize += salvador_get_match_varlen_size_rep(pBestMatch[nNextIndex].length - MIN_ENCODED_MATCH_SIZE);
               }
               else {
                  if (pBestMatch[nNextIndex].length >= MIN_ENCODED_MATCH_SIZE) {
                     /* Match with offset */
                     nReducedPartialSize += 1; /* match with offset follows */

                     /* High bits of match offset */
                     nReducedPartialSize += salvador_get_elias_size(((pBestMatch[nNextIndex].offset - 1) >> 7) + 1);

                     /* Low byte of match offset */
                     nReducedPartialSize += 7;

                     /* Match length */
                     nReducedPartialSize += salvador_get_match_varlen_size_norep(pBestMatch[nNextIndex].length - MIN_ENCODED_MATCH_SIZE);
                  }
                  else {
                     nCannotReduce = 1;
                  }
               }
            }

            if (nCurPartialSize >= nReducedPartialSize && !nCannotReduce) {
               int nMatchLen = pMatch->length;

               /* Join */

               pMatch->length += pBestMatch[i + nMatchLen].length;
               pBestMatch[i + nMatchLen].offset = 0;
               pBestMatch[i + nMatchLen].length = -1;
               nDidReduce = 1;
               nFollowsLiteral = 0;
               continue;
            }
         }

         nRepMatchOffset = pMatch->offset;

         i += pMatch->length;
         nNumLiterals = 0;
         nFollowsLiteral = 0;
      }
      else if (pMatch->length == 1) {
         if (nNumLiterals > 0) {
            int nNextIndex = i + pMatch->length;
            int nNextLiterals = 0;

            while (nNextIndex < nEndOffset && pBestMatch[nNextIndex].length == 0) {
               nNextLiterals++;
               nNextIndex++;
            }

            if (nNextLiterals > 0) {
               int nCurPartialSize = salvador_get_literals_varlen_size(nNumLiterals);
               nCurPartialSize += TOKEN_SIZE + salvador_get_match_varlen_size_rep(pMatch->length - MIN_ENCODED_MATCH_SIZE);
               nCurPartialSize += salvador_get_literals_varlen_size(nNextLiterals);

               int nReducedPartialSize = salvador_get_literals_varlen_size(nNumLiterals + 1 + nNextLiterals) + 8;

               if (nCurPartialSize >= nReducedPartialSize) {
                  pMatch->length = 0;
                  pMatch->offset = 0;
                  nDidReduce = 1;
                  continue;
               }
            }
         }

         nNumLiterals = 0;
         nFollowsLiteral = 0;
         i++;
      }
      else {
         nFollowsLiteral = 1;
         nNumLiterals++;
         i++;
      }
   }

   return nDidReduce;
}

/**
 * Emit a block of compressed data
 *
 * @param pCompressor compression context
 * @param pBestMatch optimal matches to emit
 * @param pInWindow pointer to input data window (previously compressed bytes + bytes to compress)
 * @param nStartOffset current offset in input window (typically the number of previously compressed bytes)
 * @param nEndOffset offset to end finding matches at (typically the size of the total input window in bytes
 * @param pOutData pointer to output buffer
 * @param nOutOffset starting offset into outpout buffer
 * @param nMaxOutDataSize maximum size of output buffer, in bytes
 * @param nCurBitsOffset write index into output buffer, of current byte being filled with bits
 * @param nCurBitShift bit shift count
 * @param nFinalLiterals output number of literals not written after writing this block, that need to be written in the next block
 * @param nCurRepMatchOffset starting rep offset for this block, updated after the block is compressed successfully
 * @param nBlockFlags bit 0: 1 for first block, 0 otherwise; bit 1: 1 for last block, 0 otherwise
 *
 * @return size of compressed data in output buffer, or -1 if the data is uncompressible
 */
static int salvador_write_block(salvador_compressor* pCompressor, salvador_final_match* pBestMatch, const unsigned char* pInWindow, const int nStartOffset, const int nEndOffset, unsigned char* pOutData, int nOutOffset, const int nMaxOutDataSize, int* nCurBitsOffset, int* nCurBitShift, int* nFinalLiterals, int* nCurRepMatchOffset, const int nBlockFlags) {
   int nRepMatchOffset = *nCurRepMatchOffset;
   const int nMaxOffset = pCompressor->max_offset;
   const int nIsInverted = (pCompressor->flags & FLG_IS_INVERTED) ? 1 : 0;
   int nNumLiterals = 0;
   int nInFirstLiteralOffset = 0;
   int nIsFirstCommand = (nBlockFlags & 1) ? 1 : 0;
   int i;

   for (i = nStartOffset; i < nEndOffset; ) {
      const salvador_final_match* pMatch = pBestMatch + i;

      if (pMatch->length >= 2 || (pMatch->length >= 1 && pMatch->offset == nRepMatchOffset && nNumLiterals != 0)) {
         int nMatchOffset = pMatch->offset;
         int nMatchLen = pMatch->length;
         int nEncodedMatchLen = nMatchLen - 2;

         if (nMatchOffset < MIN_OFFSET || nMatchOffset > nMaxOffset || nMatchOffset > MAX_OFFSET)
            return -1;

         if (nIsFirstCommand && nNumLiterals == 0) {
            /* The first block always starts with a literal */
            return -1;
         }

         if (nNumLiterals != 0) {
            /* Literals */

            if (nNumLiterals < pCompressor->stats.min_literals || pCompressor->stats.min_literals == -1)
               pCompressor->stats.min_literals = nNumLiterals;
            if (nNumLiterals > pCompressor->stats.max_literals)
               pCompressor->stats.max_literals = nNumLiterals;
            pCompressor->stats.total_literals += nNumLiterals;
            pCompressor->stats.literals_divisor++;

            if (!nIsFirstCommand) {
               nOutOffset = salvador_write_bits(pOutData, nOutOffset, nMaxOutDataSize, 0 /* literals follow */, 1, nCurBitsOffset, nCurBitShift);
               if (nOutOffset < 0) return -1;
            }
            else {
               /* The command code for the first literals is omitted */
               nIsFirstCommand = 0;
            }

            nOutOffset = salvador_write_literals_varlen(pOutData, nOutOffset, nMaxOutDataSize, nNumLiterals, nCurBitsOffset, nCurBitShift);
            if (nOutOffset < 0) return -1;

            if ((nOutOffset + nNumLiterals) > nMaxOutDataSize)
               return -1;
            memcpy(pOutData + nOutOffset, pInWindow + nInFirstLiteralOffset, nNumLiterals);
            nOutOffset += nNumLiterals;
         }

         if (nMatchOffset == nRepMatchOffset && nNumLiterals != 0) {
            /* Rep match */
            nOutOffset = salvador_write_bits(pOutData, nOutOffset, nMaxOutDataSize, 0 /* rep match */, 1, nCurBitsOffset, nCurBitShift);
            if (nOutOffset < 0) return -1;

            /* Write match length */
            nOutOffset = salvador_write_match_varlen(pOutData, nOutOffset, nMaxOutDataSize, nEncodedMatchLen, 1, nCurBitsOffset, nCurBitShift, NULL);
            if (nOutOffset < 0) return -1;
         }
         else {
            /* Match with offset */
            nOutOffset = salvador_write_bits(pOutData, nOutOffset, nMaxOutDataSize, 1 /* match with offset */, 1, nCurBitsOffset, nCurBitShift);
            if (nOutOffset < 0) return -1;

            /* Write high bits of match offset */
            nOutOffset = salvador_write_elias_value(pOutData, nOutOffset, nMaxOutDataSize, ((nMatchOffset - 1) >> 7) + 1, nIsInverted, nCurBitsOffset, nCurBitShift, NULL);
            if (nOutOffset < 0) return -1;

            /* Write low byte of match offset */
            if (nOutOffset >= nMaxOutDataSize)
               return -1;
            unsigned char* pFirstBit = &pOutData[nOutOffset];
            pOutData[nOutOffset++] = (255 - ((nMatchOffset - 1) & 0x7f)) << 1;

            /* Write match length */
            nOutOffset = salvador_write_match_varlen(pOutData, nOutOffset, nMaxOutDataSize, nEncodedMatchLen, 0, nCurBitsOffset, nCurBitShift, pFirstBit);
            if (nOutOffset < 0) return -1;
         }

         nNumLiterals = 0;

         if (nMatchOffset == nRepMatchOffset)
            pCompressor->stats.num_rep_matches++;

         nRepMatchOffset = nMatchOffset;

         if (nMatchOffset < pCompressor->stats.min_offset || pCompressor->stats.min_offset == -1)
            pCompressor->stats.min_offset = nMatchOffset;
         if (nMatchOffset > pCompressor->stats.max_offset)
            pCompressor->stats.max_offset = nMatchOffset;
         pCompressor->stats.total_offsets += (long long)nMatchOffset;

         if (nMatchLen < pCompressor->stats.min_match_len || pCompressor->stats.min_match_len == -1)
            pCompressor->stats.min_match_len = nMatchLen;
         if (nMatchLen > pCompressor->stats.max_match_len)
            pCompressor->stats.max_match_len = nMatchLen;
         pCompressor->stats.total_match_lens += nMatchLen;
         pCompressor->stats.match_divisor++;

         if (nMatchOffset == 1) {
            if (nMatchLen < pCompressor->stats.min_rle1_len || pCompressor->stats.min_rle1_len == -1)
               pCompressor->stats.min_rle1_len = nMatchLen;
            if (nMatchLen > pCompressor->stats.max_rle1_len)
               pCompressor->stats.max_rle1_len = nMatchLen;
            pCompressor->stats.total_rle1_lens += nMatchLen;
            pCompressor->stats.rle1_divisor++;
         }
         else if (nMatchOffset == 2) {
            if (nMatchLen < pCompressor->stats.min_rle2_len || pCompressor->stats.min_rle2_len == -1)
               pCompressor->stats.min_rle2_len = nMatchLen;
            if (nMatchLen > pCompressor->stats.max_rle2_len)
               pCompressor->stats.max_rle2_len = nMatchLen;
            pCompressor->stats.total_rle2_lens += nMatchLen;
            pCompressor->stats.rle2_divisor++;
         }

         i += nMatchLen;

         int nCurSafeDist = (i - nStartOffset) - nOutOffset;
         if (nCurSafeDist >= 0 && pCompressor->stats.safe_dist < nCurSafeDist)
            pCompressor->stats.safe_dist = nCurSafeDist;

         pCompressor->stats.commands_divisor++;
      }
      else {
         if (nNumLiterals == 0)
            nInFirstLiteralOffset = i;
         nNumLiterals++;
         i++;
      }
   }

   if (nBlockFlags & 2) {
      if (nNumLiterals < pCompressor->stats.min_literals || pCompressor->stats.min_literals == -1)
         pCompressor->stats.min_literals = nNumLiterals;
      if (nNumLiterals > pCompressor->stats.max_literals)
         pCompressor->stats.max_literals = nNumLiterals;
      pCompressor->stats.total_literals += nNumLiterals;
      pCompressor->stats.literals_divisor++;

      *nFinalLiterals = 0;

      if (nNumLiterals != 0) {
         /* Final Literals */

         if (!nIsFirstCommand) {
            nOutOffset = salvador_write_bits(pOutData, nOutOffset, nMaxOutDataSize, 0 /* literals follow */, 1, nCurBitsOffset, nCurBitShift);
            if (nOutOffset < 0) return -1;
         }
         else {
            /* The command code for the first literals is omitted. We are writing the final literals, so this must be a fully incompressible block */
            nIsFirstCommand = 0;
         }

         nOutOffset = salvador_write_literals_varlen(pOutData, nOutOffset, nMaxOutDataSize, nNumLiterals, nCurBitsOffset, nCurBitShift);
         if (nOutOffset < 0) return -1;

         if ((nOutOffset + nNumLiterals) > nMaxOutDataSize)
            return -1;
         memcpy(pOutData + nOutOffset, pInWindow + nInFirstLiteralOffset, nNumLiterals);
         nOutOffset += nNumLiterals;
         nNumLiterals = 0;
      }

      nOutOffset = salvador_write_bits(pOutData, nOutOffset, nMaxOutDataSize, 1 /* match with offset */, 1, nCurBitsOffset, nCurBitShift);
      if (nOutOffset < 0) return -1;

      nOutOffset = salvador_write_elias_value(pOutData, nOutOffset, nMaxOutDataSize, 256 /* EOD */, nIsInverted, nCurBitsOffset, nCurBitShift, NULL);
      if (nOutOffset < 0) return -1;
   }
   else {
      *nFinalLiterals = nNumLiterals;
   }

   *nCurRepMatchOffset = nRepMatchOffset;
   return nOutOffset;
}

/**
 * Select the most optimal matches, reduce the token count if possible, and then emit a block of compressed data
 *
 * @param pCompressor compression context
 * @param pInWindow pointer to input data window (previously compressed bytes + bytes to compress)
 * @param nPreviousBlockSize number of previously compressed bytes (or 0 for none)
 * @param nInDataSize number of input bytes to compress
 * @param pOutData pointer to output buffer
 * @param nMaxOutDataSize maximum size of output buffer, in bytes
 * @param nCurBitsOffset write index into output buffer, of current byte being filled with bits
 * @param nCurBitShift bit shift count
 * @param nFinalLiterals output number of literals not written after writing this block, that need to be written in the next block
 * @param nCurRepMatchOffset starting rep offset for this block, updated after the block is compressed successfully
 * @param nBlockFlags bit 0: 1 for first block, 0 otherwise; bit 1: 1 for last block, 0 otherwise
 *
 * @return size of compressed data in output buffer, or -1 if the data is uncompressible
 */
static int salvador_optimize_and_write_block(salvador_compressor *pCompressor, const unsigned char *pInWindow, const int nPreviousBlockSize, const int nInDataSize, unsigned char *pOutData, const int nMaxOutDataSize, int *nCurBitsOffset, int *nCurBitShift, int *nFinalLiterals, int *nCurRepMatchOffset, const int nBlockFlags) {
   int nOutOffset = 0;
   const int nEndOffset = nPreviousBlockSize + nInDataSize;
   int *rle_len = (int*)pCompressor->intervals /* reuse */;
   int *first_offset_for_byte = pCompressor->first_offset_for_byte;
   int *next_offset_for_pos = pCompressor->next_offset_for_pos;
   int *offset_cache = pCompressor->offset_cache;
   int i, nPosition;

   memset(pCompressor->best_match, 0, pCompressor->block_size * sizeof(salvador_final_match));

   /* Supplement small matches */

   memset(first_offset_for_byte, 0xff, sizeof(int) * 65536);
   memset(next_offset_for_pos, 0xff, sizeof(int) * nInDataSize);

   for (nPosition = nPreviousBlockSize; nPosition < (nEndOffset - 1); nPosition++) {
      next_offset_for_pos[nPosition - nPreviousBlockSize] = first_offset_for_byte[((unsigned int)pInWindow[nPosition]) | (((unsigned int)pInWindow[nPosition + 1]) << 8)];
      first_offset_for_byte[((unsigned int)pInWindow[nPosition]) | (((unsigned int)pInWindow[nPosition + 1]) << 8)] = nPosition;
   }

   for (nPosition = nPreviousBlockSize + 1; nPosition < (nEndOffset - 1); nPosition++) {
      salvador_match *match = pCompressor->match + ((nPosition - nPreviousBlockSize) << MATCHES_PER_INDEX_SHIFT);
      unsigned short *match_depth = pCompressor->match_depth + ((nPosition - nPreviousBlockSize) << MATCHES_PER_INDEX_SHIFT);
      int m = 0, nInserted = 0;
      int nMatchPos;

      while (m < 15 && match[m].length)
         m++;

      for (nMatchPos = next_offset_for_pos[nPosition - nPreviousBlockSize]; m < 15 && nMatchPos >= 0; nMatchPos = next_offset_for_pos[nMatchPos - nPreviousBlockSize]) {
         int nMatchOffset = nPosition - nMatchPos;

         if (nMatchOffset <= pCompressor->max_offset) {
            int nExistingMatchIdx;
            int nAlreadyExists = 0;

            for (nExistingMatchIdx = 0; nExistingMatchIdx < m; nExistingMatchIdx++) {
               if (match[nExistingMatchIdx].offset == nMatchOffset ||
                  (match[nExistingMatchIdx].offset - (match_depth[nExistingMatchIdx] & 0x3fff)) == nMatchOffset) {
                  nAlreadyExists = 1;
                  break;
               }
            }

            if (!nAlreadyExists) {
               int nMatchLen = 2;
               while (nMatchLen < 128 && (nPosition + nMatchLen + 4) < nEndOffset && !memcmp(pInWindow + nMatchPos + nMatchLen, pInWindow + nPosition + nMatchLen, 4))
                  nMatchLen += 4;
               while (nMatchLen < 128 && (nPosition + nMatchLen) < nEndOffset && pInWindow[nMatchPos + nMatchLen] == pInWindow[nPosition + nMatchLen])
                  nMatchLen++;
               match[m].length = nMatchLen;
               match[m].offset = nMatchOffset;
               match_depth[m] = 0x4000;
               m++;
               nInserted++;
               if (nInserted >= 15)
                  break;
            }
         }
         else {
            break;
         }
      }
   }

   i = 0;
   while (i < nEndOffset) {
      int nRangeStartIdx = i;
      unsigned char c = pInWindow[nRangeStartIdx];
      do {
         i++;
      }
      while (i < nEndOffset && pInWindow[i] == c);
      while (nRangeStartIdx < i) {
         rle_len[nRangeStartIdx] = i - nRangeStartIdx;
         nRangeStartIdx++;
      }
   }

   /* Compress and insert additional matches */
   salvador_optimize_forward(pCompressor, pInWindow, nPreviousBlockSize, nEndOffset, 1 /* nInsertForwardReps */, nCurRepMatchOffset, NARRIVALS_PER_POSITION / 2, nBlockFlags);

   /* Supplement matches further */

   memset(offset_cache, 0xff, sizeof(int) * 2048);

   for (nPosition = nPreviousBlockSize + 1; nPosition < (nEndOffset - 1); nPosition++) {
      salvador_match* match = pCompressor->match + ((nPosition - nPreviousBlockSize) << MATCHES_PER_INDEX_SHIFT);

      if (match[0].length < 8) {
         unsigned short* match_depth = pCompressor->match_depth + ((nPosition - nPreviousBlockSize) << MATCHES_PER_INDEX_SHIFT);
         int m = 0, nInserted = 0;
         int nMatchPos;
         int nMaxForwardPos = nPosition + 2 + 1 + 3;

         if (nMaxForwardPos > (nEndOffset - 2))
            nMaxForwardPos = nEndOffset - 2;

         while (m < NMATCHES_PER_INDEX && match[m].length) {
            offset_cache[match[m].offset & 2047] = nPosition;
            offset_cache[(match[m].offset - (match_depth[m] & 0x3fff)) & 2047] = nPosition;
            m++;
         }

         for (nMatchPos = next_offset_for_pos[nPosition - nPreviousBlockSize]; m < NMATCHES_PER_INDEX && nMatchPos >= 0; nMatchPos = next_offset_for_pos[nMatchPos - nPreviousBlockSize]) {
            const int nMatchOffset = nPosition - nMatchPos;

            if (nMatchOffset <= pCompressor->max_offset) {
               int nAlreadyExists = 0;

               if (offset_cache[nMatchOffset & 2047] == nPosition) {
                  int nExistingMatchIdx;

                  for (nExistingMatchIdx = 0; nExistingMatchIdx < m; nExistingMatchIdx++) {
                     if (match[nExistingMatchIdx].offset == nMatchOffset ||
                        (match[nExistingMatchIdx].offset - (match_depth[nExistingMatchIdx] & 0x3fff)) == nMatchOffset) {
                        nAlreadyExists = 1;

                        if (match_depth[nExistingMatchIdx] == 0x4000) {
                           int nMatchLen = 2;
                           while (nMatchLen < 128 && nPosition < (nEndOffset - nMatchLen) && pInWindow[nMatchPos + nMatchLen] == pInWindow[nPosition + nMatchLen])
                              nMatchLen++;
                           if (nMatchLen > (int)match[nExistingMatchIdx].length)
                              match[nExistingMatchIdx].length = nMatchLen;
                        }

                        break;
                     }
                  }
               }

               if (!nAlreadyExists) {
                  int nForwardPos = nPosition + 2 + 1;

                  if (nForwardPos >= nMatchOffset) {
                     int nGotMatch = 0;

                     while (nForwardPos < nMaxForwardPos) {
                        if (pInWindow[nForwardPos] == pInWindow[nForwardPos - nMatchOffset]) {
                           nGotMatch = 1;
                           break;
                        }
                        nForwardPos++;
                     }

                     if (nGotMatch) {
                        int nMatchLen = 2;
                        while (nMatchLen < 128 && (nPosition + nMatchLen + 4) < nEndOffset && !memcmp(pInWindow + nMatchPos + nMatchLen, pInWindow + nPosition + nMatchLen, 4))
                           nMatchLen += 4;
                        while (nMatchLen < 128 && (nPosition + nMatchLen ) < nEndOffset && pInWindow[nMatchPos + nMatchLen] == pInWindow[nPosition + nMatchLen])
                           nMatchLen++;
                        match[m].length = nMatchLen;
                        match[m].offset = nMatchOffset;
                        match_depth[m] = 0;
                        m++;

                        salvador_insert_forward_match(pCompressor, pInWindow, nPosition, nMatchOffset, nPreviousBlockSize, nEndOffset, 8);

                        nInserted++;
                        if (nInserted >= 9 || m >= NMATCHES_PER_INDEX)
                           break;
                     }
                  }
               }
            }
            else {
               break;
            }
         }
      }
   }

   /* Pick final matches */
   salvador_optimize_forward(pCompressor, pInWindow, nPreviousBlockSize, nEndOffset, 0 /* nInsertForwardReps */, nCurRepMatchOffset, NARRIVALS_PER_POSITION, nBlockFlags);

   /* Apply reduction and merge pass */
   int nDidReduce;
   int nPasses = 0;
   do {
      nDidReduce = salvador_reduce_commands(pCompressor, pInWindow, pCompressor->best_match - nPreviousBlockSize, nPreviousBlockSize, nEndOffset, nCurRepMatchOffset, nBlockFlags);
      nPasses++;
   } while (nDidReduce && nPasses < 20);

   /* Write compressed block */

   return salvador_write_block(pCompressor, pCompressor->best_match - nPreviousBlockSize, pInWindow, nPreviousBlockSize, nEndOffset, pOutData, nOutOffset, nMaxOutDataSize, nCurBitsOffset, nCurBitShift, nFinalLiterals, nCurRepMatchOffset, nBlockFlags);
}

/* Forward declaration */
static void salvador_compressor_destroy(salvador_compressor *pCompressor);

/**
 * Initialize compression context
 *
 * @param pCompressor compression context to initialize
 * @param nBlockSize maximum size of input data (bytes to compress only)
 * @param nMaxWindowSize maximum size of input data window (previously compressed bytes + bytes to compress)
 * @param nMaxArrivals maximum number of arrivals per position
 * @param nFlags compression flags
 *
 * @return 0 for success, non-zero for failure
 */
static int salvador_compressor_init(salvador_compressor *pCompressor, const int nBlockSize, const int nMaxWindowSize, const int nMaxArrivals, const int nFlags) {
   int nResult;

   nResult = divsufsort_init(&pCompressor->divsufsort_context);
   pCompressor->intervals = NULL;
   pCompressor->pos_data = NULL;
   pCompressor->open_intervals = NULL;
   pCompressor->match = NULL;
   pCompressor->match_depth = NULL;
   pCompressor->best_match = NULL;
   pCompressor->arrival = NULL;
   pCompressor->first_offset_for_byte = NULL;
   pCompressor->next_offset_for_pos = NULL;
   pCompressor->offset_cache = NULL;
   pCompressor->flags = nFlags;
   pCompressor->block_size = nBlockSize;

   memset(&pCompressor->stats, 0, sizeof(pCompressor->stats));
   pCompressor->stats.min_match_len = -1;
   pCompressor->stats.min_offset = -1;
   pCompressor->stats.min_rle1_len = -1;
   pCompressor->stats.min_rle2_len = -1;

   if (!nResult) {
      pCompressor->intervals = (unsigned long long *)malloc(nMaxWindowSize * sizeof(unsigned long long));

      if (pCompressor->intervals) {
         pCompressor->pos_data = (unsigned long long *)malloc(nMaxWindowSize * sizeof(unsigned long long));

         if (pCompressor->pos_data) {
            pCompressor->open_intervals = (unsigned long long *)malloc((LCP_AND_TAG_MAX + 1) * sizeof(unsigned long long));

            if (pCompressor->open_intervals) {
               pCompressor->arrival = (salvador_arrival *)malloc((nBlockSize + 1) * nMaxArrivals * sizeof(salvador_arrival));

               if (pCompressor->arrival) {
                  pCompressor->best_match = (salvador_final_match *)malloc(nBlockSize * sizeof(salvador_final_match));

                  if (pCompressor->best_match) {
                     pCompressor->match = (salvador_match *)malloc(nBlockSize * NMATCHES_PER_INDEX * sizeof(salvador_match));
                     if (pCompressor->match) {
                        pCompressor->match_depth = (unsigned short *)malloc(nBlockSize * NMATCHES_PER_INDEX * sizeof(unsigned short));
                        if (pCompressor->match_depth) {
                           pCompressor->first_offset_for_byte = (int*)malloc(65536 * sizeof(int));
                           if (pCompressor->first_offset_for_byte) {
                              pCompressor->next_offset_for_pos = (int*)malloc(nBlockSize * sizeof(int));
                              if (pCompressor->next_offset_for_pos) {
                                 if (nMaxArrivals == NARRIVALS_PER_POSITION) {
                                    pCompressor->offset_cache = (int*)malloc(2048 * sizeof(int));
                                    if (pCompressor->offset_cache) {
                                       return 0;
                                    }
                                 }
                                 else {
                                    return 0;
                                 }
                              }
                           }
                        }
                     }
                  }
               }
            }
         }
      }
   }

   salvador_compressor_destroy(pCompressor);
   return 100;
}

/**
 * Clean up compression context and free up any associated resources
 *
 * @param pCompressor compression context to clean up
 */
static void salvador_compressor_destroy(salvador_compressor *pCompressor) {
   divsufsort_destroy(&pCompressor->divsufsort_context);

   if (pCompressor->offset_cache) {
      free(pCompressor->offset_cache);
      pCompressor->offset_cache = NULL;
   }

   if (pCompressor->next_offset_for_pos) {
      free(pCompressor->next_offset_for_pos);
      pCompressor->next_offset_for_pos = NULL;
   }

   if (pCompressor->first_offset_for_byte) {
      free(pCompressor->first_offset_for_byte);
      pCompressor->first_offset_for_byte = NULL;
   }

   if (pCompressor->match_depth) {
      free(pCompressor->match_depth);
      pCompressor->match_depth = NULL;
   }

   if (pCompressor->match) {
      free(pCompressor->match);
      pCompressor->match = NULL;
   }

   if (pCompressor->arrival) {
      free(pCompressor->arrival);
      pCompressor->arrival = NULL;
   }

   if (pCompressor->best_match) {
      free(pCompressor->best_match);
      pCompressor->best_match = NULL;
   }

   if (pCompressor->open_intervals) {
      free(pCompressor->open_intervals);
      pCompressor->open_intervals = NULL;
   }

   if (pCompressor->pos_data) {
      free(pCompressor->pos_data);
      pCompressor->pos_data = NULL;
   }

   if (pCompressor->intervals) {
      free(pCompressor->intervals);
      pCompressor->intervals = NULL;
   }
}

/**
 * Compress one block of data
 *
 * @param pCompressor compression context
 * @param pInWindow pointer to input data window (previously compressed bytes + bytes to compress)
 * @param nPreviousBlockSize number of previously compressed bytes (or 0 for none)
 * @param nInDataSize number of input bytes to compress
 * @param pOutData pointer to output buffer
 * @param nMaxOutDataSize maximum size of output buffer, in bytes
 * @param nCurBitsOffset write index into output buffer, of current byte being filled with bits
 * @param nCurBitShift bit shift count
 * @param nFinalLiterals output number of literals not written after writing this block, that need to be written in the next block
 * @param nCurRepMatchOffset starting rep offset for this block, updated after the block is compressed successfully
 * @param nBlockFlags bit 0: 1 for first block, 0 otherwise; bit 1: 1 for last block, 0 otherwise
 *
 * @return size of compressed data in output buffer, or -1 if the data is uncompressible
 */
static int salvador_compressor_shrink_block(salvador_compressor *pCompressor, const unsigned char *pInWindow, const int nPreviousBlockSize, const int nInDataSize, unsigned char *pOutData, const int nMaxOutDataSize, int *nCurBitsOffset, int *nCurBitShift, int *nFinalLiterals, int *nCurRepMatchOffset, const int nBlockFlags) {
   int nCompressedSize;

   if (salvador_build_suffix_array(pCompressor, pInWindow, nPreviousBlockSize + nInDataSize))
      nCompressedSize = -1;
   else {
      if (nPreviousBlockSize) {
         salvador_skip_matches(pCompressor, 0, nPreviousBlockSize);
      }
      salvador_find_all_matches(pCompressor, NMATCHES_PER_INDEX, nPreviousBlockSize, nPreviousBlockSize + nInDataSize, nBlockFlags);

      nCompressedSize = salvador_optimize_and_write_block(pCompressor, pInWindow, nPreviousBlockSize, nInDataSize, pOutData, nMaxOutDataSize, nCurBitsOffset, nCurBitShift, nFinalLiterals, nCurRepMatchOffset, nBlockFlags);
   }

   return nCompressedSize;
}

/**
 * Get maximum compressed size of input(source) data
 *
 * @param nInputSize input(source) size in bytes
 *
 * @return maximum compressed size
 */
size_t salvador_get_max_compressed_size(size_t nInputSize) {
   return ((nInputSize + 65535) >> 16) * 128 + nInputSize;
}

/**
 * Compress memory
 *
 * @param pInputData pointer to input(source) data to compress
 * @param pOutBuffer buffer for compressed data
 * @param nInputSize input(source) size in bytes
 * @param nMaxOutBufferSize maximum capacity of compression buffer
 * @param nFlags compression flags (set to 0)
 * @param nMaxWindowSize maximum window size to use (0 for default)
 * @param nDictionarySize size of dictionary in front of input data (0 for none)
 * @param progress progress function, called after compressing each block, or NULL for none
 * @param pStats pointer to compression stats that are filled if this function is successful, or NULL
 *
 * @return actual compressed size, or -1 for error
 */
size_t salvador_compress(const unsigned char *pInputData, unsigned char *pOutBuffer, size_t nInputSize, size_t nMaxOutBufferSize,
      const unsigned int nFlags, size_t nMaxWindowSize, size_t nDictionarySize, void(*progress)(long long nOriginalSize, long long nCompressedSize), salvador_stats *pStats) {
   salvador_compressor compressor;
   size_t nOriginalSize = 0;
   size_t nCompressedSize = 0L;
   int nResult;
   int nMaxArrivals = NARRIVALS_PER_POSITION;
   int nError = 0;
   const int nBlockSize = (nInputSize < BLOCK_SIZE) ? ((nInputSize < 1024) ? 1024 : (int)nInputSize) : BLOCK_SIZE;
   const int nMaxOutBlockSize = (int)salvador_get_max_compressed_size(nBlockSize);

   if (nDictionarySize < nInputSize) {
      int nInDataSize = (int)(nInputSize - nDictionarySize);
      if (nInDataSize > nBlockSize)
         nInDataSize = nBlockSize;
   }

   nResult = salvador_compressor_init(&compressor, nBlockSize, nBlockSize * 2, nMaxArrivals, nFlags);
   if (nResult != 0) {
      return -1;
   }

   compressor.max_offset = nMaxWindowSize ? (int)nMaxWindowSize : MAX_OFFSET;

   int nPreviousBlockSize = 0;
   int nNumBlocks = 0;
   int nCurBitsOffset = INT_MIN, nCurBitShift = 0, nCurFinalLiterals = 0;
   int nBlockFlags = 1;
   int nCurRepMatchOffset = 1;

   if (nDictionarySize) {
      nOriginalSize = (int)nDictionarySize;
      nPreviousBlockSize = (int)nDictionarySize;
   }

   while (nOriginalSize < nInputSize && !nError) {
      int nInDataSize;

      nInDataSize = (int)(nInputSize - nOriginalSize);
      if (nInDataSize > nBlockSize)
         nInDataSize = nBlockSize;

      if (nInDataSize > 0) {
         int nOutDataSize;
         int nOutDataEnd = (int)(nMaxOutBufferSize - nCompressedSize);

         if (nOutDataEnd > nMaxOutBlockSize)
            nOutDataEnd = nMaxOutBlockSize;

         if ((nOriginalSize + nInDataSize) >= nInputSize)
            nBlockFlags |= 2;
         nOutDataSize = salvador_compressor_shrink_block(&compressor, pInputData + nOriginalSize - nPreviousBlockSize, nPreviousBlockSize, nInDataSize, pOutBuffer + nCompressedSize, nOutDataEnd,
            &nCurBitsOffset, &nCurBitShift, &nCurFinalLiterals, &nCurRepMatchOffset, nBlockFlags);
         nBlockFlags &= (~1);

         if (nOutDataSize >= 0 && nCurFinalLiterals >= 0 && nCurFinalLiterals < nInDataSize) {
            /* Write compressed block */

            if (!nError) {
               nInDataSize -= nCurFinalLiterals;
               nOriginalSize += nInDataSize;
               nCurFinalLiterals = 0;
               nCompressedSize += nOutDataSize;
               if (nCurBitsOffset != INT_MIN)
                  nCurBitsOffset -= nOutDataSize;
            }
         }
         else {
            nError = -1;
         }

         nPreviousBlockSize = nInDataSize;
         nNumBlocks++;
      }

      if (!nError && nOriginalSize < nInputSize) {
         if (progress)
            progress(nOriginalSize, nCompressedSize);
      }
   }

   if (progress)
      progress(nOriginalSize, nCompressedSize);
   if (pStats)
      *pStats = compressor.stats;

   salvador_compressor_destroy(&compressor);

   if (nError) {
      return -1;
   }
   else {
      return nCompressedSize;
   }
}