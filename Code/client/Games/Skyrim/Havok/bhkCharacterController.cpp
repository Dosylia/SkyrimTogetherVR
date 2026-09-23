#include <Havok/bhkCharacterController.h>

bool bhkCharacterController::UpdateStepTiming(float aMovementDeltaTime) noexcept
{
    // The same physics timestep the native rigid-body controller movement path uses. The VR address arrived on
    // 2026-09-23 (VR 0x141ec8278, SE 0x141e083a8, SE id 512261) and is in VRAddressOverrides under AE id 389089,
    // so both builds read the real global now.
    //
    // If it ever fails to resolve, zero is passed instead, which UpdateDeltaTime rejects before falling back to
    // the movement delta the caller passes and then to the step already stored. HookActorProcess passes the frame
    // delta, so the controller still gets a usable step either way. Dereferencing the pointer blindly would not
    // be safe: an unresolved id reads address zero.
    POINTER_SKYRIMSE(float, s_physicsDeltaTime, 389089, 389089);
    const float* pPhysicsDeltaTime = s_physicsDeltaTime.Get();
    return stepInfo.UpdateDeltaTime(pPhysicsDeltaTime ? *pPhysicsDeltaTime : 0.f, aMovementDeltaTime);
}
