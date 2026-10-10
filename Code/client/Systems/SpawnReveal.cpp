#include <TiltedOnlinePCH.h>

#include <Systems/SpawnReveal.h>

#include <Actor.h>
#include <Forms/TESForm.h>
#include <NetImmerse/NiAVObject.h>
#include <NetImmerse/NiNode.h>

namespace
{
// NiAVObject::flags, bit 0 is kHidden (CommonLibVR-NG, `RE/N/NiAVObject.h`: 0x10C on VR, 0xF4 on SE).
#ifdef SKYRIMVR
constexpr size_t kFlagsOffset = 0x10C;
#else
constexpr size_t kFlagsOffset = 0xF4;
#endif
constexpr uint32_t kHidden = 1u << 0;
constexpr auto kLongest = std::chrono::milliseconds(1500);

struct Hidden
{
    std::chrono::steady_clock::time_point Since;
};
TiltedPhoques::Map<uint32_t, Hidden> s_hidden;

uint32_t& FlagsOf(NiAVObject* apObject) noexcept
{
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(apObject) + kFlagsOffset);
}

void Show(uint32_t aFormId, const char* acpWhy) noexcept
{
    const auto it = s_hidden.find(aFormId);
    if (it == s_hidden.end())
        return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - it->second.Since).count();
    s_hidden.erase(it);

    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    NiAVObject* p3D = pActor ? pActor->GetNiNode() : nullptr;
    if (!p3D)
        return;
    FlagsOf(p3D) &= ~kHidden;
    spdlog::info("SpawnReveal: copy {:X} shown {} ms after its 3D arrived ({})", aFormId, ms, acpWhy);
}
} // namespace

void SpawnReveal::HideUntilPlaced(Actor* apActor) noexcept
{
    if (!apActor || apActor->IsDead())
        return;
    NiAVObject* p3D = apActor->GetNiNode();
    if (!p3D)
        return;
    FlagsOf(p3D) |= kHidden;
    s_hidden[apActor->formID] = {std::chrono::steady_clock::now()};
}

void SpawnReveal::Placed(Actor* apActor) noexcept
{
    if (apActor && !s_hidden.empty())
        Show(apActor->formID, "placed from a movement update");
}

void SpawnReveal::Tick() noexcept
{
    if (s_hidden.empty())
        return;
    const auto now = std::chrono::steady_clock::now();
    TiltedPhoques::Vector<uint32_t> late;
    for (const auto& [formId, hidden] : s_hidden)
        if (now - hidden.Since >= kLongest)
            late.push_back(formId);
    for (const uint32_t formId : late)
        Show(formId, "no movement update in 1.5 s");
}
