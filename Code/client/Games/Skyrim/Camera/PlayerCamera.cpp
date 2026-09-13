
#include <Camera/PlayerCamera.h>
#include <NetImmerse/NiCamera.h>
#include <TiltedOnlinePCH.h>

PlayerCamera* PlayerCamera::Get() noexcept
{
    POINTER_SKYRIMSE(PlayerCamera*, s_instance, 400802, 514642);
    return *(s_instance.Get());
}

bool PlayerCamera::IsFirstPerson() noexcept
{
#ifdef SKYRIMVR
    // VR has no flat first-person camera state or first-person behavior graph: the player always
    // runs the full-body graph that remote clients replay. AE 21600 was an unresolved crosswalk
    // value that crashed in BehaviorVar::Patch (via SaveAnimationVariables); before that crash it
    // effectively reported "not first person", which is the behaviour sync relies on.
    return false;
#endif
    TP_THIS_FUNCTION(TIsFirstPerson, void, PlayerCamera, void*, void*, double*);
    POINTER_SKYRIMSE(TIsFirstPerson, isFirstPerson, 21600, 21600);

    double firstPerson = 0.0;
    TiltedPhoques::ThisCall(isFirstPerson, this, nullptr, nullptr, &firstPerson);

    return firstPerson == 1.0;
}

bool PlayerCamera::WorldPtToScreenPt3(const NiPoint3& in, NiPoint3& out, float zeroTolerance /* = 1e-5f */)
{
    auto* pCam = GetNiCamera();
    if (cameraNode && pCam)
    {
        return pCam->WorldPtToScreenPt3(in, out, zeroTolerance);
    }

    return false;
}

void PlayerCamera::ForceFirstPerson() noexcept
{
#ifdef SKYRIMVR
    // CommonLibVR-NG's own PlayerCamera::ForceFirstPerson explicitly refuses
    // to run on VR ("if (REL::Module::IsVR()) return false;") even though the
    // id (49858) does resolve to a real VR address - forcing a first/third
    // person switch doesn't map cleanly onto VR's own camera/view handling.
    // Matching that judgment call rather than trusting "the address resolves"
    // as proof it's safe to call.
    return;
#else
    TP_THIS_FUNCTION(TForceFirstPerson, void, PlayerCamera);
    POINTER_SKYRIMSE(TForceFirstPerson, forceFirstPerson, 50790, 49858);
    TiltedPhoques::ThisCall(forceFirstPerson, this);
#endif
}

void PlayerCamera::ForceThirdPerson() noexcept
{
#ifdef SKYRIMVR
    // See ForceFirstPerson above - CommonLibVR-NG deliberately no-ops this on
    // VR despite the id (49863) resolving to a real address.
    return;
#else
    TP_THIS_FUNCTION(TForceThirdPerson, void, PlayerCamera);
    POINTER_SKYRIMSE(TForceThirdPerson, forceThirdPerson, 50796, 49863);
    TiltedPhoques::ThisCall(forceThirdPerson, this);
#endif
}

