#include "AIProcess.h"

#include <Misc/MiddleProcess.h>

bhkCharacterController* AIProcess::GetCharController() noexcept
{
    TP_THIS_FUNCTION(TGetCharController, bhkCharacterController*, AIProcess);

    // A VR address for this arrived on 2026-09-23 (VR 0x140685270, SE 0x14067bdf0) and is in
    // VRAddressOverrides under AE id 39856, so the engine's own function is what answers now.
    //
    // The field read below stays as the fallback, and as a check on it. CommonLibVR implements this as
    // `middleHigh ? middleHigh->charController.get() : nullptr` with middleHigh at +0x08 and charController at
    // +0x250, which is what this struct's middleProcess says. If the two ever disagree, the offset is wrong and
    // that is worth knowing, because nothing else here would notice: it is said once and then left alone.
    bhkCharacterController* pFromField = middleProcess ? middleProcess->charController : nullptr;

    POINTER_SKYRIMSE(TGetCharController, getCharController, 39856, 39856);
    if (!getCharController.Get())
        return pFromField;

    bhkCharacterController* pFromGame = TiltedPhoques::ThisCall(getCharController, this);

    static bool s_disagreed = false;
    if (!s_disagreed && pFromGame != pFromField)
    {
        s_disagreed = true;
        spdlog::warn("AIProcess::GetCharController: the engine says {} and MiddleProcess+0x250 says {}; the charController offset is wrong",
                     fmt::ptr(pFromGame), fmt::ptr(pFromField));
    }

    return pFromGame;
}

void AIProcess::KnockExplosion(Actor* apActor, const NiPoint3* aSourceLocation, float afMagnitude)
{
    TP_THIS_FUNCTION(TKnockExplosion, void, AIProcess, Actor*, const NiPoint3*, float);
    POINTER_SKYRIMSE(TKnockExplosion, knockExplosion, 39895, 38858);
    TiltedPhoques::ThisCall(knockExplosion, this, apActor, aSourceLocation, afMagnitude);
}
