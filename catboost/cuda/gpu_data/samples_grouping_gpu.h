#pragma once

#include "samples_grouping.h"
#include <catboost/cuda/cuda_lib/cuda_buffer.h>
#include <util/system/types.h>
#include <util/generic/vector.h>

namespace NCatboostCuda {
    //TODO(noxoomo): account to group sizes during splitting.
    //TODO(noxoomo): write only gids on GPU (and gids pairs/pairSize/pairCount), all index should be made on GPU for efficien sampling scheme support
    template <class TMapping>
    class TGpuSamplesGrouping: public TGuidHolder {
    public:
        /*
         * Biased offset are query offset in single mapping
         * offsets bias if first query offset on device
         * query size are just size
         * introduction of offsetsBias allows to reuse mirror-buffers in stripe leaves estimation
         * Pairs are also global index
         */
        const NCudaLib::TCudaBuffer<const ui32, TMapping>& GetBiasedOffsets() const {
            return Offsets;
        };

        const NCudaLib::TDistributedObject<ui32>& GetOffsetsBias() const {
            return OffsetBiases;
        }

        const NCudaLib::TCudaBuffer<const ui32, TMapping>& GetSizes() const {
            return Sizes;
        };

        const NCudaLib::TCudaBuffer<const uint2, TMapping>& GetPairs() const {
            return Pairs;
        };

        const NCudaLib::TCudaBuffer<const float, TMapping>& GetPairsWeights() const {
            return PairsWeights;
        };

        //TODO(noxoomo): get rid of this, we need sampling for this structure without CPU structs rebuilding
        ui32 GetQuerySize(ui32 localQueryId) const {
            return Grouping->GetQuerySize(GetQid(localQueryId));
        }

        ui32 GetQueryOffset(ui32 localQueryId) const {
            return Grouping->GetQueryOffset(GetQid(localQueryId)) - Grouping->GetQueryOffset(GetQid(0));
        }

        ui32 GetQueryCount() const {
            return Grouping->GetQueryId(CurrentDocsSlice.Right) - Grouping->GetQueryId(CurrentDocsSlice.Left);
        }

        bool HasSubgroupIds() const {
            const auto queriesGrouping = dynamic_cast<const TQueriesGrouping*>(Grouping);
            return queriesGrouping && queriesGrouping->HasSubgroupIds();
        }

        bool HasPairs() const {
            return Pairs.GetObjectsSlice().Size();
        }

        TVector<TVector<TCompetitor>> CreateQueryCompetitors(ui32 localQid) const {
            const auto queriesGrouping = dynamic_cast<const TQueriesGrouping*>(Grouping);
            CB_ENSURE(queriesGrouping && queriesGrouping->GetFlatQueryPairs().size());
            const ui32 querySize = GetQuerySize(localQid);

            TVector<TVector<TCompetitor>> competitors(querySize);
            const uint2* pairs = queriesGrouping->GetFlatQueryPairs().data();
            const float* pairWeights = queriesGrouping->GetQueryPairWeights().data();
            const ui32 queryOffset = Grouping->GetQueryOffset(GetQid(localQid));

            const ui32 firstPair = GetQueryPairOffset(localQid);
            const ui32 lastPair = GetQueryPairOffset(localQid + 1);

            for (ui32 pairId = firstPair; pairId < lastPair; ++pairId) {
                uint2 pair = pairs[pairId];
                TCompetitor competitor(pair.y - queryOffset,
                                       pairWeights[pairId]);
                competitor.SampleWeight = 0;
                competitors[pair.x - queryOffset].push_back(competitor);
            }
            return competitors;
        };

        const ui32* GetSubgroupIds(ui32 localQueryId) const {
            const auto queriesGrouping = dynamic_cast<const TQueriesGrouping*>(Grouping);
            CB_ENSURE(queriesGrouping && queriesGrouping->HasSubgroupIds());
            return queriesGrouping->GetSubgroupIds(GetQid(localQueryId));
        }

        TGpuSamplesGrouping CopyView() const {
            TGpuSamplesGrouping copy;
            copy.Grouping = Grouping;
            copy.CurrentDocsSlice = CurrentDocsSlice;
            copy.Offsets = Offsets.ConstCopyView();
            copy.Sizes = Sizes.ConstCopyView();
            copy.OffsetBiases = OffsetBiases;
            if (Pairs.GetObjectsSlice().Size()) {
                copy.Pairs = Pairs.ConstCopyView();
                copy.PairsWeights = PairsWeights.ConstCopyView();
            }
            return copy;
        }

    protected:
        TGpuSamplesGrouping(const IQueriesGrouping* owner,
                            TSlice docSlices,
                            NCudaLib::TCudaBuffer<const ui32, TMapping>&& offsets,
                            NCudaLib::TCudaBuffer<const ui32, TMapping>&& sizes,
                            NCudaLib::TDistributedObject<ui32>&& biases)
            : Grouping(owner)
            , CurrentDocsSlice(docSlices)
            , Offsets(std::move(offsets))
            , Sizes(std::move(sizes))
            , OffsetBiases(std::move(biases))
        {
        }

        TGpuSamplesGrouping()
            : OffsetBiases(NCudaLib::GetCudaManager().CreateDistributedObject<ui32>(0))
        {
        }

        ui32 GetQid(ui32 localQueryId) const {
            return Grouping->GetQueryOffset(CurrentDocsSlice.Left) + localQueryId;
        }

        ui32 GetQueryPairOffset(ui32 localQueryId) const {
            if (dynamic_cast<const TQueriesGrouping*>(Grouping) != nullptr) {
                return dynamic_cast<const TQueriesGrouping*>(Grouping)->GetQueryPairOffset(GetQid(localQueryId));
            } else {
                CB_ENSURE(false, "Error: don't have pairs thus pairwise metrics/learning can't be used");
            }
        }

    private:
        template <class>
        friend class TGpuSamplesGroupingHelper;

        const IQueriesGrouping* Grouping;
        TSlice CurrentDocsSlice;

        NCudaLib::TCudaBuffer<const ui32, TMapping> Offsets;
        NCudaLib::TCudaBuffer<const ui32, TMapping> Sizes;
        NCudaLib::TDistributedObject<ui32> OffsetBiases;

        //if we have them
        NCudaLib::TCudaBuffer<const uint2, TMapping> Pairs;
        NCudaLib::TCudaBuffer<const float, TMapping> PairsWeights;
    };

    template <class>
    class TGpuSamplesGroupingHelper;

    template <>
    class TGpuSamplesGroupingHelper<NCudaLib::TMirrorMapping> {
    public:
        template <class TDataSet>
        static TGpuSamplesGrouping<NCudaLib::TMirrorMapping> CreateGpuGrouping(const TDataSet& dataSet,
                                                                               const TSlice& slice) {
            const IQueriesGrouping& grouping = dataSet.GetSamplesGrouping();

            const ui32 rightGroupId = grouping.GetQueryId(slice.Right);
            const ui32 leftGroupId = grouping.GetQueryId(slice.Left);
            const ui32 groupCount = rightGroupId - leftGroupId;
            TVector<ui32> offsets(groupCount);
            TVector<ui32> sizes(groupCount);
            const ui32 offset = grouping.GetQueryOffset(leftGroupId);

            for (ui32 i = leftGroupId; i < rightGroupId; ++i) {
                offsets[i - leftGroupId] = grouping.GetQueryOffset(i) - offset;
                sizes[i - leftGroupId] = grouping.GetQuerySize(i);
            }
            TMirrorBuffer<ui32> offsetsGpu = TMirrorBuffer<ui32>::Create(NCudaLib::TMirrorMapping(offsets.size()));
            TMirrorBuffer<ui32> sizesGpu = TMirrorBuffer<ui32>::CopyMapping(offsetsGpu);
            offsetsGpu.Write(offsets);
            sizesGpu.Write(sizes);
            TGpuSamplesGrouping<NCudaLib::TMirrorMapping> samplesGrouping;

            samplesGrouping.Grouping = &grouping;
            samplesGrouping.CurrentDocsSlice = slice;
            samplesGrouping.Offsets = offsetsGpu.ConstCopyView();
            samplesGrouping.Sizes = sizesGpu.ConstCopyView();
            samplesGrouping.OffsetBiases = NCudaLib::GetCudaManager().CreateDistributedObject<ui32>(0);

            {
                const TQueriesGrouping* pointwiseSamplesGrouping = dynamic_cast<const TQueriesGrouping*>(&grouping);
                if (pointwiseSamplesGrouping && pointwiseSamplesGrouping->GetFlatQueryPairs().size()) {
                    const auto& pairs = pointwiseSamplesGrouping->GetFlatQueryPairs();
                    const auto& pairsWeights = pointwiseSamplesGrouping->GetQueryPairWeights();

                    auto pairsGpu = TMirrorBuffer<uint2>::Create(NCudaLib::TMirrorMapping(pairs.size()));
                    auto pairsWeightsGpu = TMirrorBuffer<float>::Create(NCudaLib::TMirrorMapping(pairs.size()));

                    pairsGpu.Write(pairs);
                    pairsWeightsGpu.Write(pairsWeights);

                    samplesGrouping.Pairs = pairsGpu.ConstCopyView();
                    samplesGrouping.PairsWeights = pairsWeightsGpu.ConstCopyView();
                }
            }
            return samplesGrouping;
        }

        static inline TGpuSamplesGrouping<NCudaLib::TMirrorMapping> SliceGrouping(const TGpuSamplesGrouping<NCudaLib::TMirrorMapping>& grouping,
                                                                                  const TSlice& localSlice) {
            CB_ENSURE(localSlice.Size() <= grouping.CurrentDocsSlice.Size());
            TSlice globalSlice;
            globalSlice.Left = localSlice.Left + grouping.CurrentDocsSlice.Left;
            globalSlice.Right = localSlice.Right + grouping.CurrentDocsSlice.Left;

            const ui32 firstGroupId = grouping.Grouping->GetQueryId(globalSlice.Left);
            const ui32 lastGroupId = grouping.Grouping->GetQueryId(globalSlice.Right);

            const ui32 firstGroupDoc = grouping.Grouping->GetQueryOffset(firstGroupId);
            const ui32 lastGroupDoc = grouping.Grouping->GetQueryOffset(lastGroupId);

            CB_ENSURE(firstGroupDoc == globalSlice.Left, "Error: slice should be group-consistent");
            CB_ENSURE(lastGroupDoc == globalSlice.Right, "Error: slice should be group-consistent");

            auto biases = NCudaLib::GetCudaManager().CreateDistributedObject<ui32>(0);
            for (ui32 dev = 0; dev < biases.DeviceCount(); ++dev) {
                biases.Set(dev, firstGroupDoc);
            }

            TSlice groupsSlice;
            const ui32 groupOffset = grouping.Grouping->GetQueryId(grouping.CurrentDocsSlice.Left);
            CB_ENSURE(firstGroupId >= groupOffset);
            groupsSlice.Left = firstGroupId - groupOffset;
            groupsSlice.Right = lastGroupId - groupOffset;
            CB_ENSURE(grouping.Offsets.GetObjectsSlice() == TSlice(0, grouping.Grouping->GetQueryId(
                                                                          grouping.CurrentDocsSlice.Right) -
                                                                          groupOffset));

            TGpuSamplesGrouping<NCudaLib::TMirrorMapping> sliceGrouping(grouping.Grouping,
                                                                        globalSlice,
                                                                        grouping.Offsets.SliceView(groupsSlice),
                                                                        grouping.Sizes.SliceView(groupsSlice),
                                                                        std::move(biases));
            {
                const TQueriesGrouping* pointwiseSamplesGrouping = dynamic_cast<const TQueriesGrouping*>(grouping.Grouping);
                if (pointwiseSamplesGrouping && pointwiseSamplesGrouping->GetFlatQueryPairs().size()) {
                    TSlice pairsSlice;
                    CB_ENSURE(firstGroupId >= groupOffset);
                    const ui32 shift = pointwiseSamplesGrouping->GetQueryPairOffset(groupOffset);
                    pairsSlice.Left = pointwiseSamplesGrouping->GetQueryPairOffset(firstGroupId) - shift;
                    pairsSlice.Right = pointwiseSamplesGrouping->GetQueryPairOffset(lastGroupId) - shift;

                    sliceGrouping.Pairs = grouping.Pairs.SliceView(pairsSlice);
                    sliceGrouping.PairsWeights = grouping.PairsWeights.SliceView(pairsSlice);
                }
            }

            return sliceGrouping;
        }

        static TGpuSamplesGrouping<NCudaLib::TStripeMapping> MakeStripeGrouping(const TGpuSamplesGrouping<NCudaLib::TMirrorMapping>& mirrorMapping,
                                                                                const TCudaBuffer<const ui32, NCudaLib::TStripeMapping>& indices) {
            CB_ENSURE(indices.GetObjectsSlice() == mirrorMapping.CurrentDocsSlice);
            TSlice docsSlice = indices.GetObjectsSlice();
            docsSlice.Left += mirrorMapping.CurrentDocsSlice.Left;
            docsSlice.Right += mirrorMapping.CurrentDocsSlice.Left;
            const IQueriesGrouping& grouping = *mirrorMapping.Grouping;
            const ui32 queryIdOffset = grouping.GetQueryId(docsSlice.Left);

            const NCudaLib::TDistributedObject<ui32>& baseBias = mirrorMapping.GetOffsetsBias();
            NCudaLib::TDistributedObject<ui32> deviceOffsetsBiases = NCudaLib::GetCudaManager().CreateDistributedObject<ui32>(0);

            TVector<TSlice> groupMetaSlices(deviceOffsetsBiases.DeviceCount());

            ui32 offset = 0;
            for (ui32 dev = 0; dev < deviceOffsetsBiases.DeviceCount(); ++dev) {
                TSlice deviceSlice = indices.GetMapping().DeviceSlice(dev);
                const ui32 localBias = baseBias.At(dev) + offset;
                deviceOffsetsBiases.Set(dev, localBias);
                ui32 firstGroupId = grouping.GetQueryId(docsSlice.Left + offset) - queryIdOffset;
                ui32 lastGroupId = grouping.GetQueryId(docsSlice.Left + offset + deviceSlice.Size()) - queryIdOffset;
                offset += deviceSlice.Size();
                groupMetaSlices[dev] = TSlice(firstGroupId, lastGroupId);
            }

            NCudaLib::TStripeMapping groupMetaMapping(std::move(groupMetaSlices));
            CB_ENSURE(groupMetaMapping.GetObjectsSlice() == TSlice(0, grouping.GetQueryId(docsSlice.Right) - queryIdOffset));
            NCudaLib::TCudaBuffer<const ui32, NCudaLib::TStripeMapping> offsets = NCudaLib::StripeView(mirrorMapping.Offsets, groupMetaMapping);
            NCudaLib::TCudaBuffer<const ui32, NCudaLib::TStripeMapping> sizes = NCudaLib::StripeView(mirrorMapping.Sizes, groupMetaMapping);

            TGpuSamplesGrouping<NCudaLib::TStripeMapping> samplesGrouping(&grouping,
                                                                          mirrorMapping.CurrentDocsSlice,
                                                                          std::move(offsets),
                                                                          std::move(sizes),
                                                                          std::move(deviceOffsetsBiases));

            {
                const TQueriesGrouping* pointwiseSamplesGrouping = dynamic_cast<const TQueriesGrouping*>(&grouping);
                if (pointwiseSamplesGrouping && pointwiseSamplesGrouping->GetFlatQueryPairs().size()) {
                    NCudaLib::TStripeMapping pairsMapping = groupMetaMapping.Transform([&](const TSlice& groupsSlice) -> ui64 {
                        ui32 firstGroupId = groupsSlice.Left + queryIdOffset;
                        ui32 lastGroupId = groupsSlice.Right + queryIdOffset;
                        return pointwiseSamplesGrouping->GetQueryPairOffset(lastGroupId) - pointwiseSamplesGrouping->GetQueryPairOffset(firstGroupId);
                    });
                    samplesGrouping.Pairs = NCudaLib::StripeView(mirrorMapping.Pairs, pairsMapping);
                    samplesGrouping.PairsWeights = NCudaLib::StripeView(mirrorMapping.PairsWeights, pairsMapping);
                }
            }
            return samplesGrouping;
        }
    };

    template <>
    class TGpuSamplesGroupingHelper<NCudaLib::TStripeMapping> {
    public:
        template <class TDataSet>
        static TGpuSamplesGrouping<NCudaLib::TStripeMapping> CreateGpuGrouping(const TDataSet& dataSet) {
            const IQueriesGrouping& grouping = dataSet.GetSamplesGrouping();
            const NCudaLib::TStripeMapping& samplesMapping = dataSet.GetSamplesMapping();
            const ui32 groupCount = samplesMapping.GetObjectsSlice().Size();
            TGpuSamplesGrouping<NCudaLib::TStripeMapping> samplesGrouping;

            TVector<ui32> offsets(groupCount);
            TVector<ui32> sizes(groupCount);

            NCudaLib::TDistributedObject<ui32> offsetsBias = CreateDistributedObject<ui32>(0);
            NCudaLib::TStripeMapping queriesMapping;

            {
                TVector<TSlice> queriesSlices;
                for (ui32 dev = 0; dev < NCudaLib::GetCudaManager().GetDeviceCount(); ++dev) {
                    auto slice = samplesMapping.DeviceSlice(dev);
                    const ui32 rightGroupId = grouping.GetQueryId(slice.Right);
                    const ui32 leftGroupId = grouping.GetQueryId(slice.Left);
                    const ui32 offset = grouping.GetQueryOffset(leftGroupId);

                    for (ui32 i = leftGroupId; i < rightGroupId; ++i) {
                        offsets[i] = grouping.GetQueryOffset(i);
                        sizes[i] = grouping.GetQuerySize(i);
                    }
                    queriesSlices.push_back(TSlice(leftGroupId, rightGroupId));
                    offsetsBias.Set(dev, offset);
                }
                queriesMapping = NCudaLib::TStripeMapping(std::move(queriesSlices));
            }

            TStripeBuffer<ui32> offsetsGpu = TStripeBuffer<ui32>::Create(queriesMapping);
            TStripeBuffer<ui32> sizesGpu = TStripeBuffer<ui32>::CopyMapping(offsetsGpu);
            offsetsGpu.Write(offsets);
            sizesGpu.Write(sizes);

            samplesGrouping.Grouping = &grouping;
            samplesGrouping.CurrentDocsSlice = samplesMapping.GetObjectsSlice();

            samplesGrouping.Offsets = offsetsGpu.ConstCopyView();
            samplesGrouping.Sizes = sizesGpu.ConstCopyView();
            samplesGrouping.OffsetBiases = offsetsBias;

            {
                const TQueriesGrouping* pointwiseSamplesGrouping = dynamic_cast<const TQueriesGrouping*>(&grouping);
                if (pointwiseSamplesGrouping && pointwiseSamplesGrouping->GetFlatQueryPairs().size()) {
                    const auto& pairs = pointwiseSamplesGrouping->GetFlatQueryPairs();
                    const auto& pairsWeights = pointwiseSamplesGrouping->GetQueryPairWeights();

                    NCudaLib::TStripeMapping pairsMapping = queriesMapping.Transform([&](const TSlice& groupsSlice) -> ui64 {
                        return pointwiseSamplesGrouping->GetQueryPairOffset(groupsSlice.Right) - pointwiseSamplesGrouping->GetQueryPairOffset(groupsSlice.Left);
                    });

                    auto pairsGpu = TStripeBuffer<uint2>::Create(pairsMapping);
                    auto pairsWeightsGpu = TStripeBuffer<float>::Create(pairsMapping);

                    pairsGpu.Write(pairs);
                    pairsWeightsGpu.Write(pairsWeights);

                    samplesGrouping.Pairs = pairsGpu.ConstCopyView();
                    samplesGrouping.PairsWeights = pairsWeightsGpu.ConstCopyView();
                }
            }
            return samplesGrouping;
        }
    };

    extern template class TGpuSamplesGroupingHelper<NCudaLib::TStripeMapping>;
    extern template class TGpuSamplesGroupingHelper<NCudaLib::TMirrorMapping>;

    template <class TDataSet>
    inline TGpuSamplesGrouping<NCudaLib::TStripeMapping> CreateGpuGrouping(const TDataSet& dataSet) {
        return TGpuSamplesGroupingHelper<NCudaLib::TStripeMapping>::CreateGpuGrouping(dataSet);
    }

    template <class TDataSet>
    inline TGpuSamplesGrouping<NCudaLib::TMirrorMapping> CreateGpuGrouping(const TDataSet& dataSet,
                                                                           const TSlice& slice) {
        return TGpuSamplesGroupingHelper<NCudaLib::TMirrorMapping>::CreateGpuGrouping(dataSet, slice);
    }

    inline TGpuSamplesGrouping<NCudaLib::TMirrorMapping> SliceGrouping(const TGpuSamplesGrouping<NCudaLib::TMirrorMapping>& grouping,
                                                                       const TSlice& localSlice) {
        return TGpuSamplesGroupingHelper<NCudaLib::TMirrorMapping>::SliceGrouping(grouping, localSlice);
    }

    inline TGpuSamplesGrouping<NCudaLib::TStripeMapping> MakeStripeGrouping(const TGpuSamplesGrouping<NCudaLib::TMirrorMapping>& mirrorMapping,
                                                                            const TCudaBuffer<const ui32, NCudaLib::TStripeMapping>& indices) {
        return TGpuSamplesGroupingHelper<NCudaLib::TMirrorMapping>::MakeStripeGrouping(mirrorMapping, indices);
    }
}
