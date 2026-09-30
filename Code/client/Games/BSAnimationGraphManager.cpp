#include <TiltedOnlinePCH.h>

#include <BSAnimationGraphManager.h>
#include <Havok/BShkbAnimationGraph.h>
#include <Havok/BShkbHkxDB.h>
#include <Havok/hkbBehaviorGraph.h>

#include <TiltedCore/Hash.hpp>

SortedMap<uint32_t, String> BSAnimationGraphManager::DumpAnimationVariables(bool aPrintVariables, int aForceIndex)
{
    SortedMap<uint32_t, String> variables;

    if (const uint32_t index = ResolveGraphIndex(aForceIndex); index < animationGraphs.size)
    {
        const auto pGraph = animationGraphs.Get(index);
        if (pGraph)
        {
            const auto pDb = pGraph->hkxDB;
            const auto pBuckets = pDb->animationVariables.buckets;
            const auto pVariableSet = pGraph->behaviorGraph->animationVariables;

            if (pBuckets && pVariableSet)
            {
                for (decltype(pDb->animationVariables.bucketCount) i = 0; i < pDb->animationVariables.bucketCount; ++i)
                {
                    auto pBucket = &pBuckets[i];
                    if (!pBucket->next)
                        continue;

                    while (pBucket != pDb->animationVariables.end)
                    {
                        const auto variableIndex = pBucket->value;
                        if (pVariableSet->size > static_cast<uint32_t>(variableIndex))
                        {
                            variables[variableIndex] = pBucket->key.AsAscii();
                        }

                        pBucket = pBucket->next;
                    }
                }

                if (aPrintVariables)
                {
                    for (auto& [id, name] : variables)
                    {
                        std::cout << "k" << name << " = " << id << "," << std::endl;
                    }
                }
            }
        }
    }

    return variables;
}

uint32_t BSAnimationGraphManager::ResolveGraphIndex(const int aForceIndex) const noexcept
{
    if (aForceIndex >= 0)
        return static_cast<uint32_t>(aForceIndex);

    if (animationGraphIndex < animationGraphs.size)
        return animationGraphIndex;

    // Out of range. With one graph in the list there is nothing else it could mean; with more than one there is
    // no way to guess, so the value is handed back unchanged and the caller's own bounds check rejects it.
    if (animationGraphs.size == 1)
        return 0;

    return animationGraphIndex;
}

uint64_t BSAnimationGraphManager::GetDescriptorKey(int aForceIndex)
{
    using TiltedPhoques::FHash::Crc64;

    String variableNames{};
    variableNames.reserve(8192);
    std::map<uint32_t, const char*> variables;

    if (const uint32_t index = ResolveGraphIndex(aForceIndex); index < animationGraphs.size)
    {
        const auto pGraph = animationGraphs.Get(index);

        if (pGraph)
        {
            const auto pDb = pGraph->hkxDB;
            const auto pBuckets = pDb->animationVariables.buckets;
            const auto pVariableSet = pGraph->behaviorGraph->animationVariables;

            if (pBuckets && pVariableSet)
            {
                for (decltype(pDb->animationVariables.bucketCount) i = 0; i < pDb->animationVariables.bucketCount; ++i)
                {
                    auto pBucket = &pBuckets[i];
                    if (!pBucket->next)
                        continue;

                    while (pBucket != pDb->animationVariables.end)
                    {
                        const auto variableIndex = pBucket->value;
                        if (pVariableSet->size > static_cast<uint32_t>(variableIndex))
                        {
                            variables[variableIndex] = pBucket->key.AsAscii();
                        }

                        pBucket = pBucket->next;
                    }
                }

                for (auto& [id, name] : variables)
                {
                    variableNames += name;
                }
            }
        }
    }

    std::transform(variableNames.begin(), variableNames.end(), variableNames.begin(), [](unsigned char c) { return std::tolower(c); });

    return Crc64(reinterpret_cast<const unsigned char*>(variableNames.c_str()), variableNames.size());
}
