
#include <Games/Skyrim/BSRandom/BSRandom.h>

namespace BSRandom
{
static void* (*GetGenerator)();
static uint32_t (*Real_UnsignedInt)(void*, uint32_t);

uint32_t UnsignedInt(uint32_t aMin, uint32_t aMax)
{
    // Both ids are unresolved on VR - calling through the null pointers would
    // crash, so fall back to a fixed value rather than the game's RNG.
    if (!Real_UnsignedInt || !GetGenerator)
        return aMin;

    return Real_UnsignedInt(GetGenerator(), aMax - aMin);
}

static TiltedPhoques::Initializer s_randomInit(
    []()
    {
        #ifndef SKYRIMVR
        const VersionDbPtr<void> unsignedInt(68276);
        #else
        const VersionDbPtr<void> unsignedInt(0);
        #endif
        Real_UnsignedInt = static_cast<decltype(Real_UnsignedInt)>(unsignedInt.GetPtr());
        #ifndef SKYRIMVR
        const VersionDbPtr<void> getGenerator(14774);
        #else
        const VersionDbPtr<void> getGenerator(0);
        #endif
        GetGenerator = static_cast<decltype(GetGenerator)>(getGenerator.GetPtr());
    });

} // namespace BSRandom
