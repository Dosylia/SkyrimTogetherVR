#pragma once

#include <cstdint>

//! Controller haptics, straight to OpenVR.
//!
//! Emma, 2026-09-26: "when my hand goes against Lydia's axe I do not feel it, even when I have a sword". Nothing
//! in her list does that. PLANCK gives her an NPC's *body*; the parry mod only fires on an incoming attack and
//! scales its pulse by the stamina a parry costs, so resting a blade on a held axe is silent by design; HIGGS is
//! hands against grabbable objects. Weapon-touching-weapon is a gap, and for another *player's* weapon nothing
//! but this mod could fill it anyway, because only this mod knows where that weapon is.
//!
//! The probe that asked whether a pulse is felt at all was answered on 2026-09-26 -- it is, on both controllers,
//! so SkyrimVR is on OpenVR's legacy input -- and has been removed. VRBodySync's weapon touch is what calls this
//! now.
namespace VRHaptics
{
//! One pulse on one controller. Duration is clamped to what OpenVR accepts in a single call (about 4 ms); a
//! longer buzz is made by asking again on the next frame.
//! @param aRightHand Which controller.
//! @param aDurationMicroSec 0 to 3999.
//! @return false if OpenVR is not up, or that controller is not tracked.
bool Pulse(bool aRightHand, uint16_t aDurationMicroSec) noexcept;

} // namespace VRHaptics
