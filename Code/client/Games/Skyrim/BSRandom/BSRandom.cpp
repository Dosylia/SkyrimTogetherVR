
#include <Games/Skyrim/BSRandom/BSRandom.h>

namespace BSRandom
{
static void* (*GetGenerator)();
static uint32_t (*Real_UnsignedInt)(void*, uint32_t);

uint32_t UnsignedInt(uint32_t aMin, uint32_t aMax)
{
    return Real_UnsignedInt(GetGenerator(), aMax - aMin);
}

static TiltedPhoques::Initializer s_randomInit(
    []()
    {
        #ifndef SKYRIMVR
        const VersionDbPtr<void> unsignedInt(68276);
        #else
        const VersionDbPtr<void> unsignedInt(0); // TODOVR : find the correct id for VR
        #endif
        Real_UnsignedInt = static_cast<decltype(Real_UnsignedInt)>(unsignedInt.GetPtr());
        #ifndef SKYRIMVR
        const VersionDbPtr<void> getGenerator(14774);
        #else
        const VersionDbPtr<void> getGenerator(0); // TODOVR: find the correct id for VR
        #endif
        GetGenerator = static_cast<decltype(GetGenerator)>(getGenerator.GetPtr());
    });

} // namespace BSRandom
