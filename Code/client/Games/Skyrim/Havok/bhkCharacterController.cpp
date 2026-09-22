#include <Havok/bhkCharacterController.h>

bool bhkCharacterController::UpdateStepTiming(float aMovementDeltaTime) noexcept
{
    // Same physics timestep used by the native rigid-body controller movement path
    POINTER_SKYRIMSE(float, s_physicsDeltaTime, 389089, 389089);
    // No VR address for the timestep global yet. Reporting the step as usable keeps the pre-merge behaviour, which
    // is to move the controller along with the reference.
    if (!s_physicsDeltaTime.Get())
        return true;
    return stepInfo.UpdateDeltaTime(*s_physicsDeltaTime.Get(), aMovementDeltaTime);
}
