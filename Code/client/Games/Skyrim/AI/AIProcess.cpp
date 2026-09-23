#include "AIProcess.h"

#include <Misc/MiddleProcess.h>

bhkCharacterController* AIProcess::GetCharController() noexcept
{
    // No address needed: in CommonLibVR this function is a field read, not a call
    // (`return middleHigh ? middleHigh->charController.get() : nullptr;`), and middleHigh is this struct's
    // middleProcess at +0x08. That sidesteps AE id 39856, which has no VR address and could not simply be passed
    // through as one: VR ids are looked up as Special Edition ones, so an AE id resolves to whatever SE symbol
    // carries that number rather than to nothing. That is how GarbageCollector::Add crashed Seen on 2026-09-22.
    return middleProcess ? middleProcess->charController : nullptr;
}

void AIProcess::KnockExplosion(Actor* apActor, const NiPoint3* aSourceLocation, float afMagnitude)
{
    TP_THIS_FUNCTION(TKnockExplosion, void, AIProcess, Actor*, const NiPoint3*, float);
    POINTER_SKYRIMSE(TKnockExplosion, knockExplosion, 39895, 38858);
    TiltedPhoques::ThisCall(knockExplosion, this, apActor, aSourceLocation, afMagnitude);
}
