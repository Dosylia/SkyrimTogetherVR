#include "TESWorldSpace.h"

TESObjectCELL* TESWorldSpace::LoadCell(int32_t aXCoordinate, int32_t aYCoordinate) noexcept
{
    TP_THIS_FUNCTION(TLoadCell, TESObjectCELL*, TESWorldSpace, int32_t aXCoordinate, int32_t aYCoordinate);
    // VR: AE 20460 -> SE 20026 (13 consecutive function sizes match at id offset -434, incl.
    // SE 20022 TESFile::SeekCell), VR 0x2c3850 (in the VR CSV). The old override 0x2d6460 was
    // an unrelated function; it freed a bogus pointer when a remote actor was placed.
    POINTER_SKYRIMSE(TLoadCell, s_loadCell, 20460, 20026);
    return TiltedPhoques::ThisCall(s_loadCell, this, aXCoordinate, aYCoordinate);
}

