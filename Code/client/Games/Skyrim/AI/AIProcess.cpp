#include "AIProcess.h"

bhkCharacterController* AIProcess::GetCharController() noexcept
{
    TP_THIS_FUNCTION(TGetCharController, bhkCharacterController*, AIProcess);
    POINTER_SKYRIMSE(TGetCharController, getCharController, 39856, 39856);
    // No VR address for this one yet. Without the controller the caller keeps the movement path it used before.
    if (!getCharController.Get())
        return nullptr;
    return TiltedPhoques::ThisCall(getCharController, this);
}

void AIProcess::KnockExplosion(Actor* apActor, const NiPoint3* aSourceLocation, float afMagnitude)
{
    TP_THIS_FUNCTION(TKnockExplosion, void, AIProcess, Actor*, const NiPoint3*, float);
    POINTER_SKYRIMSE(TKnockExplosion, knockExplosion, 39895, 38858);
    TiltedPhoques::ThisCall(knockExplosion, this, apActor, aSourceLocation, afMagnitude);
}
