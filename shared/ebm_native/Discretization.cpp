// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "PrecompiledHeader.h"

#include <stddef.h> // size_t, ptrdiff_t
#include <limits> // numeric_limits
#include <algorithm> // sort
#include <cmath> // std::round
#include <vector>

#include "ebm_native.h"

#include "EbmInternal.h"
// very independent includes
#include "Logging.h" // EBM_ASSERT & LOG
#include "RandomStream.h"

constexpr unsigned int k_MiddleSplittingRange = 0x0;
constexpr unsigned int k_FirstSplittingRange = 0x1;
constexpr unsigned int k_LastSplittingRange = 0x2;

struct SplittingRange {
   // we divide the space into long segments of unsplittable equal values separated by zero or more items that we call SplittingRanges.  SplittingRange are
   // where we put the splitting points.  If there are long unsplittable segments at either the start or the end, we can't put split points at those ends, 
   // so these are left out.  SplittingRange can always have a split point, even in the length is zero, since they sit between long ranges.  If there
   // are non-equal values at either end, but not enough items to put a split point, we put those values at the ends into the Unsplittable category

   FloatEbmType * m_pSplittableValuesStart;
   size_t         m_cSplittableItems; // this can be zero
   size_t         m_cUnsplittablePriorItems;
   size_t         m_cUnsplittableSubsequentItems;

   size_t         m_cUnsplittableEitherSideMax;
   size_t         m_cUnsplittableEitherSideMin;

   size_t         m_cSplitsAssigned;
   unsigned int   m_flags;
};

// PK VERIFIED!
void SortSplittingRangesByCountItemsAscending(RandomStream * const pRandomStream, const size_t cSplittingRanges, SplittingRange ** const apSplittingRange) {
   EBM_ASSERT(1 <= cSplittingRanges);

   // sort in ascending order for m_cSplittableItems
   //
   // But some items can have the same primary sort key, so sort secondarily on the pointer to the original object, thus putting them secondarily in index
   // order, which is guaranteed to be a unique ordering. We'll later randomize the order of items that have the same primary sort index, BUT we want our 
   // initial sort order to be replicatable with the same random seed, so we need the initial sort to be stable.
   std::sort(apSplittingRange, apSplittingRange + cSplittingRanges, [](SplittingRange * & pSplittingRange1, SplittingRange * & pSplittingRange2) {
      if(PREDICTABLE(pSplittingRange1->m_cSplittableItems == pSplittingRange2->m_cSplittableItems)) {
         return UNPREDICTABLE(pSplittingRange1->m_pSplittableValuesStart < pSplittingRange2->m_pSplittableValuesStart);
      } else {
         return UNPREDICTABLE(pSplittingRange1->m_cSplittableItems < pSplittingRange2->m_cSplittableItems);
      }
      });

   // find sections that have the same number of items and randomly shuffle the sections with equal numbers of items so that there is no directional preference
   size_t iStartEqualLengthRange = 0;
   size_t cItems = apSplittingRange[0]->m_cSplittableItems;
   for(size_t i = 1; LIKELY(i < cSplittingRanges); ++i) {
      const size_t cNewItems = apSplittingRange[i]->m_cSplittableItems;
      if(PREDICTABLE(cItems != cNewItems)) {
         // we have a real range
         size_t cRemainingItems = i - iStartEqualLengthRange;
         EBM_ASSERT(1 <= cRemainingItems);
         while(PREDICTABLE(1 != cRemainingItems)) {
            const size_t iSwap = pRandomStream->Next(cRemainingItems);
            SplittingRange * pTmp = apSplittingRange[iStartEqualLengthRange];
            apSplittingRange[iStartEqualLengthRange] = apSplittingRange[iStartEqualLengthRange + iSwap];
            apSplittingRange[iStartEqualLengthRange + iSwap] = pTmp;
            ++iStartEqualLengthRange;
            --cRemainingItems;
         }
         iStartEqualLengthRange = i;
         cItems = cNewItems;
      }
   }
   size_t cRemainingItemsOuter = cSplittingRanges - iStartEqualLengthRange;
   EBM_ASSERT(1 <= cRemainingItemsOuter);
   while(PREDICTABLE(1 != cRemainingItemsOuter)) {
      const size_t iSwap = pRandomStream->Next(cRemainingItemsOuter);
      SplittingRange * pTmp = apSplittingRange[iStartEqualLengthRange];
      apSplittingRange[iStartEqualLengthRange] = apSplittingRange[iStartEqualLengthRange + iSwap];
      apSplittingRange[iStartEqualLengthRange + iSwap] = pTmp;
      ++iStartEqualLengthRange;
      --cRemainingItemsOuter;
   }
}

// PK VERIFIED!
void SortSplittingRangesByUnsplittableDescending(RandomStream * const pRandomStream, const size_t cSplittingRanges, SplittingRange ** const apSplittingRange) {
   EBM_ASSERT(1 <= cSplittingRanges);

   // sort in descending order for m_cUnsplittableEitherSideMax, m_cUnsplittableEitherSideMin
   //
   // But some items can have the same primary sort key, so sort secondarily on the pointer to the original object, thus putting them secondarily in index
   // order, which is guaranteed to be a unique ordering. We'll later randomize the order of items that have the same primary sort index, BUT we want our 
   // initial sort order to be replicatable with the same random seed, so we need the initial sort to be stable.
   std::sort(apSplittingRange, apSplittingRange + cSplittingRanges, [](SplittingRange * & pSplittingRange1, SplittingRange * & pSplittingRange2) {
      if(PREDICTABLE(pSplittingRange1->m_cUnsplittableEitherSideMax == pSplittingRange2->m_cUnsplittableEitherSideMax)) {
         if(PREDICTABLE(pSplittingRange1->m_cUnsplittableEitherSideMin == pSplittingRange2->m_cUnsplittableEitherSideMin)) {
            return UNPREDICTABLE(pSplittingRange1->m_pSplittableValuesStart > pSplittingRange2->m_pSplittableValuesStart);
         } else {
            return UNPREDICTABLE(pSplittingRange1->m_cUnsplittableEitherSideMin > pSplittingRange2->m_cUnsplittableEitherSideMin);
         }
      } else {
         return UNPREDICTABLE(pSplittingRange1->m_cUnsplittableEitherSideMax > pSplittingRange2->m_cUnsplittableEitherSideMax);
      }
   });

   // find sections that have the same number of items and randomly shuffle the sections with equal numbers of items so that there is no directional preference
   size_t iStartEqualLengthRange = 0;
   size_t cItemsMax = apSplittingRange[0]->m_cUnsplittableEitherSideMax;
   size_t cItemsMin = apSplittingRange[0]->m_cUnsplittableEitherSideMin;
   for(size_t i = 1; LIKELY(i < cSplittingRanges); ++i) {
      const size_t cNewItemsMax = apSplittingRange[i]->m_cUnsplittableEitherSideMax;
      const size_t cNewItemsMin = apSplittingRange[i]->m_cUnsplittableEitherSideMin;
      if(PREDICTABLE(PREDICTABLE(cNewItemsMax != cItemsMax) || PREDICTABLE(cNewItemsMin != cItemsMin))) {
         // we have a real range
         size_t cRemainingItems = i - iStartEqualLengthRange;
         EBM_ASSERT(1 <= cRemainingItems);
         while(PREDICTABLE(1 != cRemainingItems)) {
            const size_t iSwap = pRandomStream->Next(cRemainingItems);
            SplittingRange * pTmp = apSplittingRange[iStartEqualLengthRange];
            apSplittingRange[iStartEqualLengthRange] = apSplittingRange[iStartEqualLengthRange + iSwap];
            apSplittingRange[iStartEqualLengthRange + iSwap] = pTmp;
            ++iStartEqualLengthRange;
            --cRemainingItems;
         }
         iStartEqualLengthRange = i;
         cItemsMax = cNewItemsMax;
         cItemsMin = cNewItemsMin;
      }
   }
   size_t cRemainingItemsOuter = cSplittingRanges - iStartEqualLengthRange;
   EBM_ASSERT(1 <= cRemainingItemsOuter);
   while(PREDICTABLE(1 != cRemainingItemsOuter)) {
      const size_t iSwap = pRandomStream->Next(cRemainingItemsOuter);
      SplittingRange * pTmp = apSplittingRange[iStartEqualLengthRange];
      apSplittingRange[iStartEqualLengthRange] = apSplittingRange[iStartEqualLengthRange + iSwap];
      apSplittingRange[iStartEqualLengthRange + iSwap] = pTmp;
      ++iStartEqualLengthRange;
      --cRemainingItemsOuter;
   }
}

EBM_INLINE size_t GetCountBinsMax(const bool bMissing, const IntEbmType countMaximumBins) {
   EBM_ASSERT(IntEbmType { 2 } <= countMaximumBins);
   size_t cMaximumBins = static_cast<size_t>(countMaximumBins);
   if(PREDICTABLE(bMissing)) {
      // if there is a missing value, then we use 0 for the missing value bin, and bump up all other values by 1.  This creates a semi-problem
      // if the number of bins was specified as a power of two like 256, because we now have 257 possible values, and instead of consuming 8
      // bits per value, we're consuming 9.  If we're told to have a maximum of a power of two bins though, in most cases it won't hurt to
      // have one less bin so that we consume less data.  Our countMaximumBins is just a maximum afterall, so we can choose to have less bins.
      // BUT, if the user requests 8 bins or less, then don't reduce the number of bins since then we'll be changing the bin size significantly

      size_t cBits = ((~size_t { 0 }) >> 1) + size_t { 1 };
      do {
         // if cMaximumBins is a power of two equal to or greater than 16, then reduce the number of bins (it's a maximum after all) to one less so that
         // it's more compressible.  If we have 256 bins, we really want 255 bins and 0 to be the missing value, using 256 values and 1 byte of storage
         // some powers of two aren't compressible, like 2^34, which needs to fit into a 64 bit storage, but we don't want to take a dependency
         // on the size of the storage system, which is system dependent, so we just exclude all powers of two
         if(UNLIKELY(cBits == cMaximumBins)) {
            --cMaximumBins;
            break;
         }
         cBits >>= 1;
         // don't allow shrinkage below 16 bins (8 is the first power of two below 16).  By the time we reach 8 bins, we don't want to reduce this
         // by a complete bin.  We can just use the extra bit for the missing bin
         // if we had shrunk down to 7 bits for non-missing, we would have been able to fit in 16 items per data item instead of 21 for 64 bit systems
      } while(UNLIKELY(0x8 != cBits));
   }
   return cMaximumBins;
}

EBM_INLINE size_t GetAvgLength(const size_t cInstances, const size_t cMaximumBins, const size_t cMinimumInstancesPerBin) {
   EBM_ASSERT(size_t { 2 } <= cMaximumBins);
   EBM_ASSERT(size_t { 1 } <= cMinimumInstancesPerBin);

   const FloatEbmType avgLengthFloat = std::ceil(static_cast<FloatEbmType>(cInstances) / static_cast<FloatEbmType>(cMaximumBins));
   // we take the ceiling so that we have a gurantee that each and every SplittingRange is GUARANTEED to be able to have one split point
   // at the worst case, each long range has 1/N items, but if we rounded down, then perahps it might be possible for a long number of these
   // to steal fractional items until there is one or more SplittingRange that don't have splits available.  Taking the ceil removes that remote
   // possibility
   size_t avgLength = static_cast<size_t>(avgLengthFloat);
   while(UNLIKELY(static_cast<FloatEbmType>(avgLength) < avgLengthFloat)) {
      // for very large numbers there are integers that aren't representable by floating point numbers.  This loop forces us upwards until we
      // are larger than the floating point avgLengthFloatDontUse if this occurs
      ++avgLength;
   }
   if(PREDICTABLE(avgLength < cMinimumInstancesPerBin)) {
      avgLength = cMinimumInstancesPerBin;
   }
   return avgLength;
}

EBM_INLINE FloatEbmType * RemoveMissingValues(FloatEbmType * const aValues, FloatEbmType * const pValuesEnd) {
   FloatEbmType * pCopyFrom = aValues;
   FloatEbmType * pCopyTo = pValuesEnd;
   const FloatEbmType * const pCopyFromEnd = pCopyTo;
   do {
      FloatEbmType val = *pCopyFrom;
      if(std::isnan(val)) {
         pCopyTo = pCopyFrom;
         goto skip_val;
         do {
            val = *pCopyFrom;
            if(!std::isnan(val)) {
               *pCopyTo = val;
               ++pCopyTo;
            }
         skip_val:
            ++pCopyFrom;
         } while(pCopyFromEnd != pCopyFrom);
         break;
      }
      ++pCopyFrom;
   } while(pValuesEnd != pCopyFrom);
   return pCopyTo;
}

EBM_NATIVE_IMPORT_EXPORT_BODY IntEbmType EBM_NATIVE_CALLING_CONVENTION GenerateQuantileCutPoints(
   IntEbmType randomSeed,
   IntEbmType countInstances,
   FloatEbmType * singleFeatureValues,
   IntEbmType countMaximumBins,
   IntEbmType countMinimumInstancesPerBin,
   FloatEbmType * cutPointsLowerBoundInclusive,
   IntEbmType * countCutPoints,
   IntEbmType * isMissing,
   FloatEbmType * minValue,
   FloatEbmType * maxValue
) {
   EBM_ASSERT(0 <= countInstances);
   EBM_ASSERT(0 == countInstances || nullptr != singleFeatureValues);
   EBM_ASSERT(0 <= countMaximumBins);
   EBM_ASSERT(0 == countInstances || 0 < countMaximumBins); // countMaximumBins can only be zero if there are no instances, because otherwise you need a bin
   EBM_ASSERT(0 <= countMinimumInstancesPerBin);
   EBM_ASSERT(0 == countInstances || countMaximumBins <= 1 || nullptr != cutPointsLowerBoundInclusive);
   EBM_ASSERT(nullptr != countCutPoints);
   EBM_ASSERT(nullptr != isMissing);
   EBM_ASSERT(nullptr != minValue);
   EBM_ASSERT(nullptr != maxValue);

   LOG_N(TraceLevelInfo, "Entered GenerateQuantileCutPoints: randomSeed=%" IntEbmTypePrintf ", countInstances=%" IntEbmTypePrintf 
      ", singleFeatureValues=%p, countMaximumBins=%" IntEbmTypePrintf ", countMinimumInstancesPerBin=%" IntEbmTypePrintf 
      ", cutPointsLowerBoundInclusive=%p, countCutPoints=%p, isMissing=%p, minValue=%p, maxValue=%p", 
      randomSeed, 
      countInstances, 
      static_cast<void *>(singleFeatureValues), 
      countMaximumBins, 
      countMinimumInstancesPerBin, 
      static_cast<void *>(cutPointsLowerBoundInclusive), 
      static_cast<void *>(countCutPoints),
      static_cast<void *>(isMissing),
      static_cast<void *>(minValue),
      static_cast<void *>(maxValue)
   );

   if(!IsNumberConvertable<size_t, IntEbmType>(countInstances)) {
      LOG_0(TraceLevelWarning, "WARNING GenerateQuantileCutPoints !IsNumberConvertable<size_t, IntEbmType>(countInstances)");
      return 1;
   }

   if(!IsNumberConvertable<size_t, IntEbmType>(countMaximumBins)) {
      LOG_0(TraceLevelWarning, "WARNING GenerateQuantileCutPoints !IsNumberConvertable<size_t, IntEbmType>(countMaximumBins)");
      return 1;
   }

   if(!IsNumberConvertable<size_t, IntEbmType>(countMinimumInstancesPerBin)) {
      LOG_0(TraceLevelWarning, "WARNING GenerateQuantileCutPoints !IsNumberConvertable<size_t, IntEbmType>(countMinimumInstancesPerBin)");
      return 1;
   }

   const size_t cInstancesIncludingMissingValues = static_cast<size_t>(countInstances);

   IntEbmType ret = 0;
   if(0 == cInstancesIncludingMissingValues) {
      *countCutPoints = 0;
      *isMissing = EBM_FALSE;
      *minValue = 0;
      *maxValue = 0;
   } else {
      FloatEbmType * const pValuesOriginalEnd = singleFeatureValues + cInstancesIncludingMissingValues;
      FloatEbmType * const pValuesEnd = RemoveMissingValues(singleFeatureValues, pValuesOriginalEnd);

      const bool bMissing = pValuesOriginalEnd != pValuesEnd;
      *isMissing = bMissing ? EBM_TRUE : EBM_FALSE;

      const size_t cInstances = pValuesEnd - singleFeatureValues;
      if(0 == cInstances) {
         *countCutPoints = 0;
         *minValue = 0;
         *maxValue = 0;
      } else {
         std::sort(singleFeatureValues, pValuesEnd);
         *minValue = singleFeatureValues[0];
         *maxValue = pValuesEnd[-1];
         if(countMaximumBins <= 1) {
            // if there is only 1 bin, then there can be no cut points, and no point doing any more work here
            *countCutPoints = 0;
         } else {
            const size_t cMinimumInstancesPerBin =
               countMinimumInstancesPerBin <= IntEbmType { 0 } ? size_t { 1 } : static_cast<size_t>(countMinimumInstancesPerBin);
            if(cInstances < (cMinimumInstancesPerBin << 1)) {
               // we don't have enough to make even a single cut.  We would need cMinimumInstancesPerBin on each side, so that's twice the minimum needed
               // in total instances to make a single cut
               *countCutPoints = 0;
            } else {
               const size_t cMaximumBins = GetCountBinsMax(bMissing, countMaximumBins);
               const size_t avgLength = GetAvgLength(cInstances, cMaximumBins, cMinimumInstancesPerBin);
               EBM_ASSERT(1 <= avgLength);

               FloatEbmType rangeValue = *singleFeatureValues;
               FloatEbmType * pSplittableValuesStart = singleFeatureValues;
               FloatEbmType * pStartEqualRange = singleFeatureValues;
               FloatEbmType * pScan = singleFeatureValues + 1;
               size_t cSplittingRanges = 0;
               while(pValuesEnd != pScan) {
                  const FloatEbmType val = *pScan;
                  if(val != rangeValue) {
                     size_t cEqualRangeItems = pScan - pStartEqualRange;
                     if(avgLength <= cEqualRangeItems) {
                        if(singleFeatureValues != pSplittableValuesStart || cMinimumInstancesPerBin <= static_cast<size_t>(pStartEqualRange - pSplittableValuesStart)) {
                           ++cSplittingRanges;
                        }
                        pSplittableValuesStart = pScan;
                     }
                     rangeValue = val;
                     pStartEqualRange = pScan;
                  }
                  ++pScan;
               }
               if(singleFeatureValues == pSplittableValuesStart) {
                  EBM_ASSERT(0 == cSplittingRanges);
                  cSplittingRanges = 1; // we'll either exit completely, or have 1

                  // we're still on the first splitting range.  We need to make sure that there is at least one possible cut
                  // if we require 3 items for a cut, a problematic range like 0 1 3 3 4 5 could look ok, but we can't cut it in the middle!
                  FloatEbmType * pCheckForSplitPoint = singleFeatureValues + cMinimumInstancesPerBin;
                  EBM_ASSERT(pCheckForSplitPoint <= pValuesEnd);
                  FloatEbmType * pCheckForSplitPointLast = pValuesEnd - cMinimumInstancesPerBin;
                  EBM_ASSERT(singleFeatureValues <= pCheckForSplitPointLast);
                  EBM_ASSERT(singleFeatureValues < pCheckForSplitPoint);
                  FloatEbmType checkValue = *(pCheckForSplitPoint - 1);
                  while(pCheckForSplitPoint <= pCheckForSplitPointLast) {
                     if(checkValue != *pCheckForSplitPoint) {
                        goto continue_working;
                     }
                     ++pCheckForSplitPoint;
                  }
                  // there's no possible place to split, so return
                  *countCutPoints = 0;
                  goto done;
               } else {
                  const size_t cItemsLast = static_cast<size_t>(pValuesEnd - pSplittableValuesStart);
                  if(cMinimumInstancesPerBin <= cItemsLast) {
                     ++cSplittingRanges;
                  } else if(0 == cSplittingRanges) {
                     *countCutPoints = 0;
                     goto done;
                  }
               }

            continue_working:

               try {
                  RandomStream randomStream(randomSeed);
                  if(!randomStream.IsSuccess()) {
                     goto exit_error;
                  }

                  const size_t cBytesCombined = sizeof(SplittingRange) + sizeof(SplittingRange *);
                  if(IsMultiplyError(cSplittingRanges, cBytesCombined)) {
                     goto exit_error;
                  }
                  // use the same memory allocation for both the Junction items and the pointers to the junctions that we'll use for sorting
                  SplittingRange ** const apSplittingRange = static_cast<SplittingRange **>(malloc(cSplittingRanges * cBytesCombined));
                  if(nullptr == apSplittingRange) {
                     goto exit_error;
                  }
                  SplittingRange * const aSplittingRange = reinterpret_cast<SplittingRange *>(apSplittingRange + cSplittingRanges);

                  rangeValue = *singleFeatureValues;
                  pSplittableValuesStart = singleFeatureValues;
                  pStartEqualRange = singleFeatureValues;
                  pScan = singleFeatureValues + 1;
                  SplittingRange * pSplittingRange = aSplittingRange;
                  SplittingRange ** ppSplittingRange = apSplittingRange;
                  size_t cRemainingSplittingRanges = cMaximumBins - 1;
                  size_t cTotalSplittable = 0;
                  size_t cEqualRangeItemsPrior = 0;
                  while(pValuesEnd != pScan) {
                     const FloatEbmType val = *pScan;
                     if(val != rangeValue) {
                        size_t cEqualRangeItems = pScan - pStartEqualRange;
                        if(avgLength <= cEqualRangeItems) {
                           if(singleFeatureValues != pSplittableValuesStart || cMinimumInstancesPerBin <= static_cast<size_t>(pStartEqualRange - pSplittableValuesStart)) {
                              EBM_ASSERT(pSplittingRange < aSplittingRange + cSplittingRanges);
                              const size_t cSplittable = pStartEqualRange - pSplittableValuesStart;
                              cTotalSplittable += cSplittable;

                              pSplittingRange->m_pSplittableValuesStart = pSplittableValuesStart;
                              pSplittingRange->m_cSplittableItems = cSplittable;
                              pSplittingRange->m_cUnsplittablePriorItems = cEqualRangeItemsPrior;
                              pSplittingRange->m_cUnsplittableSubsequentItems = cEqualRangeItems;

                              pSplittingRange->m_cUnsplittableEitherSideMax = std::max(cEqualRangeItemsPrior, cEqualRangeItems);
                              pSplittingRange->m_cUnsplittableEitherSideMin = std::min(cEqualRangeItemsPrior, cEqualRangeItems);

                              pSplittingRange->m_cSplitsAssigned = 1; // we can 100% guarantee that we have a split assinged to this range
                              pSplittingRange->m_flags = singleFeatureValues == pSplittableValuesStart ? k_FirstSplittingRange : k_MiddleSplittingRange;

                              cEqualRangeItemsPrior = cEqualRangeItems;

                              EBM_ASSERT(ppSplittingRange < apSplittingRange + cSplittingRanges);
                              *ppSplittingRange = pSplittingRange;

                              ++pSplittingRange;
                              ++ppSplittingRange;
                              EBM_ASSERT(1 <= cRemainingSplittingRanges);
                              --cRemainingSplittingRanges;
                           }
                           pSplittableValuesStart = pScan;
                        }
                        rangeValue = val;
                        pStartEqualRange = pScan;
                     }
                     ++pScan;
                  }
                  size_t cEqualRangeItems = pValuesEnd - pStartEqualRange;
                  if(avgLength <= cEqualRangeItems) {
                     if(singleFeatureValues != pSplittableValuesStart || cMinimumInstancesPerBin <= static_cast<size_t>(pStartEqualRange - pSplittableValuesStart)) {
                        EBM_ASSERT(pSplittingRange == aSplittingRange + cSplittingRanges - 1);
                        const size_t cSplittable = pStartEqualRange - pSplittableValuesStart;
                        cTotalSplittable += cSplittable;

                        pSplittingRange->m_pSplittableValuesStart = pSplittableValuesStart;
                        pSplittingRange->m_cSplittableItems = cSplittable;
                        pSplittingRange->m_cUnsplittablePriorItems = cEqualRangeItemsPrior;
                        pSplittingRange->m_cUnsplittableSubsequentItems = cEqualRangeItems;

                        pSplittingRange->m_cUnsplittableEitherSideMax = std::max(cEqualRangeItemsPrior, cEqualRangeItems);
                        pSplittingRange->m_cUnsplittableEitherSideMin = std::min(cEqualRangeItemsPrior, cEqualRangeItems);

                        pSplittingRange->m_cSplitsAssigned = 1; // we can 100% guarantee that we have a split assinged to this range
                        pSplittingRange->m_flags = singleFeatureValues == pSplittableValuesStart ? k_FirstSplittingRange : k_MiddleSplittingRange;

                        cEqualRangeItemsPrior = cEqualRangeItems;

                        EBM_ASSERT(ppSplittingRange == apSplittingRange + cSplittingRanges - 1);
                        *ppSplittingRange = pSplittingRange;

                        EBM_ASSERT(1 <= cRemainingSplittingRanges);
                        --cRemainingSplittingRanges;
                     } else {
                        EBM_ASSERT(pSplittingRange == aSplittingRange + cSplittingRanges);
                        EBM_ASSERT(ppSplittingRange == apSplittingRange + cSplittingRanges);
                     }
                  } else {
                     if(pSplittingRange != aSplittingRange + cSplittingRanges) {
                        // we're not done, so we have one more to go.. this last one
                        EBM_ASSERT(pSplittingRange == aSplittingRange + cSplittingRanges - 1);
                        const size_t cSplittable = pValuesEnd - pSplittableValuesStart;
                        cTotalSplittable += cSplittable;

                        pSplittingRange->m_pSplittableValuesStart = pSplittableValuesStart;
                        pSplittingRange->m_cSplittableItems = cSplittable;
                        pSplittingRange->m_cUnsplittablePriorItems = cEqualRangeItemsPrior;
                        pSplittingRange->m_cUnsplittableSubsequentItems = cEqualRangeItems;

                        pSplittingRange->m_cUnsplittableEitherSideMax = std::max(cEqualRangeItemsPrior, cEqualRangeItems);
                        pSplittingRange->m_cUnsplittableEitherSideMin = std::min(cEqualRangeItemsPrior, cEqualRangeItems);

                        pSplittingRange->m_cSplitsAssigned = 1; // we can 100% guarantee that we have a split assinged to this range
                        pSplittingRange->m_flags = singleFeatureValues == pSplittableValuesStart ? k_FirstSplittingRange : k_MiddleSplittingRange;

                        cEqualRangeItemsPrior = cEqualRangeItems;

                        EBM_ASSERT(ppSplittingRange == apSplittingRange + cSplittingRanges - 1);
                        *ppSplittingRange = pSplittingRange;

                        EBM_ASSERT(1 <= cRemainingSplittingRanges);
                        --cRemainingSplittingRanges;
                     }
                  }
                  (aSplittingRange + cSplittingRanges - 1)->m_flags |= k_LastSplittingRange;








                  //// let's assign how many 
                  //SortSplittingRangesByCountItemsAscending(&randomStream, cSplittingRanges, apSplittingRange);
                  //ppSplittingRange = apSplittingRange;
                  //const SplittingRange * const * const apSplittingRangesEnd = apSplittingRange + cSplittingRanges;
                  //const SplittingRange * const pSplittingRangesLast = aSplittingRange + cSplittingRanges - 1;
                  //size_t cCutsRemaining = cMaximumBins - 1;
                  //do {
                  //   SplittingRange * pSplittingRange = *ppSplittingRange;
                  //   if(UNLIKELY(cMinimumInstancesPerBin <= pSplittingRange->m_cItemsSplittableBefore)) {
                  //      // of, we've assigned everything for which we basically had no choices.  Now let's take stock of where we are by counting the number

                  //      do {
                  //         const SplittingRange * pSplittingRange = *ppSplittingRange;
                  //         EBM_ASSERT(cMinimumInstancesPerBin <= pSplittingRange->m_cItemsSplittableBefore);




                  //         if(pSplittingRange == aSplittingRange || pSplittingRange == pSplittingRangeLast && 0 == pSplittingRange->m_cItemsUnsplittableAfter) {
                  //            // if we're at the first item, we can't put a cut on the left
                  //            // if we're at the last item, we can't put a cut on the right, unless there is a long string of items on the right, in which case we can
                  //         }
                  //         if(pSplittingRange != aSplittingRange && (pSplittingRange != pSplittingRangeLast || 0 != pSplittingRange->m_cItemsUnsplittableAfter)) {
                  //            // if we're at the first item, we can't put a cut on the left
                  //            // if we're at the last item, we can't put a cut on the right, unless there is a long string of items on the right, in which case we can
                  //         }
                  //         ++ppSplittingRange;
                  //      } while(apSplittingRangeEnd != ppSplittingRange);
                  //      break;
                  //   }
                  //   if(LIKELY(LIKELY(pJunction != aJunctions) && 
                  //      LIKELY(LIKELY(pJunction != pJunctionsLast) || LIKELY(0 != pJunction->m_cItemsUnsplittableAfter))))
                  //   {
                  //      // if we're at the first item, we can't put a cut on the left
                  //      // if we're at the last item, we can't put a cut on the right, unless there is a long string of items on the right, in which case we can
                  //      
                  //      
                  //      
                  //      //pSplittingRange->m_cSplits = 1;
                  //      
                  //      
                  //      
                  //      --cCutsRemaining;
                  //      if(UNLIKELY(0 == cCutsRemaining)) {
                  //         // this shouldn't be possible, but maybe some terrible dataset might achieve this bad result
                  //         // let's not handle this case, since I think it's impossible
                  //         break;
                  //      }
                  //   }
                  //   ++ppSplittingRange;
                  //} while(apSplittingRangeEnd != ppSplittingRange);




                  //SortSplittingRangesByUnsplittableDescending(&randomStream, cSplittingRanges, apSplittingRange);



                  free(apSplittingRange); // both the junctions and the pointers to the junctions are in the same memory allocation

                  // first let's tackle the short ranges between big ranges (or at the tails) where we know there will be a split to separate the big ranges to either
                  // side, but the short range isn't big enough to split.  In otherwords, there are less than cMinimumInstancesPerBin items
                  // we start with the biggest long ranges and essentially try to push whatever mass there is away from them and continue down the list

                  *countCutPoints = 0;
               } catch(...) {
                  ret = 1;
               }
            }
         }
      }
   }
   if(0 != ret) {
      LOG_N(TraceLevelWarning, "WARNING GenerateQuantileCutPoints returned %" IntEbmTypePrintf, ret);
   } else {
   done:
      LOG_N(TraceLevelInfo, "Exited GenerateQuantileCutPoints countCutPoints=%" IntEbmTypePrintf ", isMissing=%" IntEbmTypePrintf,
         *countCutPoints,
         *isMissing
      );
   }
   return ret;

exit_error:;
   ret = 1;
   LOG_N(TraceLevelWarning, "WARNING GenerateQuantileCutPoints returned %" IntEbmTypePrintf, ret);
   return ret;
}

EBM_NATIVE_IMPORT_EXPORT_BODY IntEbmType EBM_NATIVE_CALLING_CONVENTION GenerateImprovedEqualWidthCutPoints(
   IntEbmType countInstances,
   FloatEbmType * singleFeatureValues,
   IntEbmType countMaximumBins,
   FloatEbmType * cutPointsLowerBoundInclusive,
   IntEbmType * countCutPoints,
   IntEbmType * isMissing,
   FloatEbmType * minValue,
   FloatEbmType * maxValue
) {
   UNUSED(countInstances);
   UNUSED(singleFeatureValues);
   UNUSED(countMaximumBins);
   UNUSED(cutPointsLowerBoundInclusive);
   UNUSED(countCutPoints);
   UNUSED(isMissing);
   UNUSED(minValue);
   UNUSED(maxValue);

   // TODO: IMPLEMENT

   return 0;
}

EBM_NATIVE_IMPORT_EXPORT_BODY IntEbmType EBM_NATIVE_CALLING_CONVENTION GenerateEqualWidthCutPoints(
   IntEbmType countInstances,
   FloatEbmType * singleFeatureValues,
   IntEbmType countMaximumBins,
   FloatEbmType * cutPointsLowerBoundInclusive,
   IntEbmType * countCutPoints,
   IntEbmType * isMissing,
   FloatEbmType * minValue,
   FloatEbmType * maxValue
) {
   UNUSED(countInstances);
   UNUSED(singleFeatureValues);
   UNUSED(countMaximumBins);
   UNUSED(cutPointsLowerBoundInclusive);
   UNUSED(countCutPoints);
   UNUSED(isMissing);
   UNUSED(minValue);
   UNUSED(maxValue);

   // TODO: IMPLEMENT

   return 0;
}

EBM_NATIVE_IMPORT_EXPORT_BODY void EBM_NATIVE_CALLING_CONVENTION Discretize(
   IntEbmType isMissing,
   IntEbmType countCutPoints,
   const FloatEbmType * cutPointsLowerBoundInclusive,
   IntEbmType countInstances,
   const FloatEbmType * singleFeatureValues,
   IntEbmType * singleFeatureDiscretized
) {
   EBM_ASSERT(EBM_FALSE == isMissing || EBM_TRUE == isMissing);
   EBM_ASSERT(0 <= countCutPoints);
   EBM_ASSERT((IsNumberConvertable<size_t, IntEbmType>(countCutPoints))); // this needs to point to real memory, otherwise it's invalid
   EBM_ASSERT(0 == countInstances || 0 == countCutPoints || nullptr != cutPointsLowerBoundInclusive);
   EBM_ASSERT(0 <= countInstances);
   EBM_ASSERT((IsNumberConvertable<size_t, IntEbmType>(countInstances))); // this needs to point to real memory, otherwise it's invalid
   EBM_ASSERT(0 == countInstances || nullptr != singleFeatureValues);
   EBM_ASSERT(0 == countInstances || nullptr != singleFeatureDiscretized);

   if(IntEbmType { 0 } < countInstances) {
      const size_t cCutPoints = static_cast<size_t>(countCutPoints);
#ifndef NDEBUG
      for(size_t iDebug = 1; iDebug < cCutPoints; ++iDebug) {
         EBM_ASSERT(cutPointsLowerBoundInclusive[iDebug - 1] < cutPointsLowerBoundInclusive[iDebug]);
      }
# endif // NDEBUG
      const size_t cInstances = static_cast<size_t>(countInstances);
      const FloatEbmType * pValue = singleFeatureValues;
      const FloatEbmType * const pValueEnd = singleFeatureValues + cInstances;
      IntEbmType * pDiscretized = singleFeatureDiscretized;

      if(size_t { 0 } == cCutPoints) {
         const IntEbmType missingVal = EBM_FALSE != isMissing ? IntEbmType { 0 } : IntEbmType { -1 };
         const IntEbmType nonMissingVal = EBM_FALSE != isMissing ? IntEbmType { 1 } : IntEbmType { 0 };
         do {
            const FloatEbmType val = *pValue;
            const IntEbmType result = UNPREDICTABLE(std::isnan(val)) ? missingVal : nonMissingVal;
            *pDiscretized = result;
            ++pDiscretized;
            ++pValue;
         } while(LIKELY(pValueEnd != pValue));
      } else {
         const ptrdiff_t highStart = static_cast<ptrdiff_t>(cCutPoints - size_t { 1 });
         if(EBM_FALSE != isMissing) {
            // there are missing values.  We need to bump up all indexes by 1 and make missing zero
            do {
               ptrdiff_t middle = ptrdiff_t { 0 };
               const FloatEbmType val = *pValue;
               if(!std::isnan(val)) {
                  ptrdiff_t high = highStart;
                  ptrdiff_t low = 0;
                  FloatEbmType midVal;
                  do {
                     middle = (low + high) >> 1;
                     EBM_ASSERT(0 <= middle && middle <= highStart);
                     midVal = cutPointsLowerBoundInclusive[static_cast<size_t>(middle)];
                     high = UNPREDICTABLE(midVal <= val) ? high : middle - ptrdiff_t { 1 };
                     low = UNPREDICTABLE(midVal <= val) ? middle + ptrdiff_t { 1 } : low;
                  } while(LIKELY(low <= high));
                  // we bump up all indexes to allow missing to be 0
                  middle = UNPREDICTABLE(midVal <= val) ? middle + ptrdiff_t { 2 } : middle + ptrdiff_t { 1 };
                  EBM_ASSERT(ptrdiff_t { 0 } <= middle - ptrdiff_t { 1 } && middle - ptrdiff_t { 1 } <= static_cast<ptrdiff_t>(cCutPoints));
               }
               *pDiscretized = static_cast<IntEbmType>(middle);
               ++pDiscretized;
               ++pValue;
            } while(LIKELY(pValueEnd != pValue));
         } else {
            // there are no missing values, but check anyways. If there are missing anyways, then make them -1
            do {
               ptrdiff_t middle = ptrdiff_t { -1 };
               const FloatEbmType val = *pValue;
               if(!std::isnan(val)) {
                  ptrdiff_t high = highStart;
                  ptrdiff_t low = 0;
                  FloatEbmType midVal;
                  do {
                     middle = (low + high) >> 1;
                     EBM_ASSERT(0 <= middle && middle <= highStart);
                     midVal = cutPointsLowerBoundInclusive[static_cast<size_t>(middle)];
                     high = UNPREDICTABLE(midVal <= val) ? high : middle - ptrdiff_t { 1 };
                     low = UNPREDICTABLE(midVal <= val) ? middle + ptrdiff_t { 1 } : low;
                  } while(LIKELY(low <= high));
                  middle = UNPREDICTABLE(midVal <= val) ? middle + ptrdiff_t { 1 } : middle;
                  EBM_ASSERT(ptrdiff_t { 0 } <= middle && middle <= static_cast<ptrdiff_t>(cCutPoints));
               }
               *pDiscretized = static_cast<IntEbmType>(middle);
               ++pDiscretized;
               ++pValue;
            } while(LIKELY(pValueEnd != pValue));
         }
      }
   }
}