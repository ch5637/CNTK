//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "DistributionTestCommon.h"

// Mock communicator to simulate MPI run
class MockCommunicator : public DistributedCommunicator
{
private:
    std::unordered_set<DistributedWorkerDescriptor> m_workers;
    DistributedWorkerDescriptor m_self;

public:
    virtual const std::unordered_set<DistributedWorkerDescriptor>& Workers() const override
    {
        return m_workers;
    }

    virtual const DistributedWorkerDescriptor& CurrentWorker() const override
    {
        return m_self;
    }

    virtual DistributedCommunicatorPtr SubGroup(const std::unordered_set<DistributedWorkerDescriptor>&) const override
    {
        return nullptr;
    }

    virtual void Concatenate(
        const std::vector<ValuePtr>&,
        std::vector<ValuePtr>&,
        const std::unordered_set<DistributedWorkerDescriptor>&) override
    {}

    virtual void AggregateInPlace(
        const std::vector<NDArrayViewPtr>&,
        const std::unordered_set<DistributedWorkerDescriptor>&) override
    {}

    virtual void Aggregate(
        const std::vector<NDArrayViewPtr>&,
        std::vector<NDArrayViewPtr>&,
        const std::unordered_set<DistributedWorkerDescriptor>&) override
    {}
    
    virtual void Barrier() override
    {}

    MockCommunicator(size_t numWorkers)
    {
        for (size_t i = 0; i < numWorkers; i++)
        {
            DistributedWorkerDescriptor desc;
            desc.m_hostId = L"MockCommunicator";
            desc.m_globalRank = i;

            m_workers.insert(desc);
        }
        MockRank(0);
    }

    void MockRank(size_t rank)
    {
        m_self.m_hostId = L"MockCommunicator";
        m_self.m_globalRank = rank;
    }
};

MinibatchSourcePtr TextFormatMinibatchSourceWithMockCommunicator(const std::wstring& dataFilePath, const std::vector<StreamConfiguration>& streamConfigs, size_t epochSize = MinibatchSource::InfinitelyRepeat, bool randomize = true, size_t distributedAfterSampleCount = MinibatchSource::InfiniteSamples, DistributedCommunicatorPtr* pMockCommunicatoryPtr = nullptr)
{
    ::CNTK::Dictionary minibatchSourceConfiguration;
    minibatchSourceConfiguration[L"epochSize"] = epochSize;

    if (randomize)
        minibatchSourceConfiguration[L"randomize"] = true;

    ::CNTK::Dictionary deserializerConfiguration;
    deserializerConfiguration[L"type"] = L"CNTKTextFormatDeserializer";
    deserializerConfiguration[L"file"] = dataFilePath;

    ::CNTK::Dictionary inputStreamsConfig;
    for (auto streamConfig : streamConfigs)
    {
        std::wstring streamName = streamConfig.m_streamName;
        size_t streamDim = streamConfig.m_dim;
        bool isSparse = streamConfig.m_isSparse;
        std::wstring streamAlias = streamConfig.m_streamAlias;

        ::CNTK::Dictionary inputStreamConfig;
        inputStreamConfig[L"dim"] = streamDim;
        inputStreamConfig[L"format"] = isSparse ? L"sparse" : L"dense";
        if (!streamAlias.empty())
            inputStreamConfig[L"alias"] = streamAlias;

        inputStreamsConfig[streamName] = inputStreamConfig;
    }

    deserializerConfiguration[L"input"] = inputStreamsConfig;
    minibatchSourceConfiguration[L"deserializers"] = std::vector<::CNTK::DictionaryValue>({ deserializerConfiguration });

    minibatchSourceConfiguration[L"distributedAfterSampleCount"] = distributedAfterSampleCount;

    minibatchSourceConfiguration[L"mockCommunicator"] = reinterpret_cast<size_t>(pMockCommunicatoryPtr);

    return CreateCompositeMinibatchSource(minibatchSourceConfiguration);
}

void TestMinibatchSourceWarmStart(size_t numMBs, size_t minibatchSize, size_t warmStartSamples)
{
    const size_t inputDim = 2;
    const size_t numOutputClasses = 2;
    auto featureStreamName = L"features";
    auto labelsStreamName = L"labels";
    const size_t numWorkers = 2;
    DistributedCommunicatorPtr mockCommunicator = std::make_shared<MockCommunicator>(numWorkers);

    auto minibatchSource = TextFormatMinibatchSourceWithMockCommunicator(
        L"SimpleDataTrain_cntk_text.txt",
        { { featureStreamName, inputDim }, { labelsStreamName, numOutputClasses } },
        MinibatchSource::InfinitelyRepeat,
        false,
        warmStartSamples,
        &mockCommunicator);

    auto featureStreamInfo = minibatchSource->StreamInfo(featureStreamName);
    auto labelStreamInfo = minibatchSource->StreamInfo(labelsStreamName);

    size_t totalSamples = 0;
    for (size_t i = 0; i < numMBs; ++i)
    {
        bool distributed = minibatchSource->IsDistributed();
        if (distributed != (totalSamples >= warmStartSamples))
        {
            ReportFailure("TestMinibatchSourceWarmStart failed in distributed state: expected %d, actual %d",
                totalSamples >= warmStartSamples, distributed);
        }

        auto minibatchData = minibatchSource->GetNextMinibatch(minibatchSize);

        size_t expectedNumSamples = distributed ? minibatchSize / numWorkers : minibatchSize;

        if (minibatchData[featureStreamInfo].m_numSamples != expectedNumSamples)
        {
            ReportFailure("TestMinibatchSourceWarmStart failed in sample count: expected %lu, actual %lu",
                expectedNumSamples, minibatchData[featureStreamInfo].m_numSamples);
        }

        totalSamples += minibatchSize;
    }
}