#include <Havok/bhkCharacterController.h>

bool bhkCharacterController::UpdateStepTiming(float aMovementDeltaTime) noexcept
{
#ifdef SKYRIMVR
    // AE id 389089 (the physics timestep global) has no VR address, and an AE id cannot be passed through as a VR
    // one: VR ids are looked up as Special Edition ones, so it would read a stranger's memory. It is not needed.
    // UpdateDeltaTime only *prefers* the physics timestep: it falls back to the movement delta the caller passes
    // and then to the step already stored, rejecting anything non-finite or <= 0.0001s. HookActorProcess passes
    // the frame delta, so the controller still ends up with a valid step and upstream's #901 fix does its job
    // here. A zero first argument is simply rejected and skipped.
    // Note this no longer always reports success: ForcePosition calls it with no movement delta, so a freshly
    // created controller with no stored step now correctly reports "no usable step yet" and the caller takes its
    // own branch, which is what upstream intends.
    return stepInfo.UpdateDeltaTime(0.f, aMovementDeltaTime);
#else
    // Same physics timestep used by the native rigid-body controller movement path
    POINTER_SKYRIMSE(float, s_physicsDeltaTime, 389089, 389089);
    return stepInfo.UpdateDeltaTime(*s_physicsDeltaTime.Get(), aMovementDeltaTime);
#endif
}
