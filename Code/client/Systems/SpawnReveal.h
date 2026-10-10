#pragma once

struct Actor;

// A copy is not drawn until it has been put where its owner is (Emma, 2026-10-10: copies "fade in" instead of popping).
// A copy arrives at the position the server stored, which can be wrong in height (SinkDiag: Seen's copy 4,112 units
// below its place right after it spawned); the first movement update then moved it, and both players saw it jump.
// Now a player's copy has its 3D hidden from the moment it is applied until the interpolation first places it from a
// movement update (13 to 27 ms in the rig), or 1.5 s at most. Only players' copies (the case seen): an NPC standing
// still sends no movement, and hidden NPC copies all appeared 1.5 s late (rig, 2026-10-10). Dead copies never. Not a
// gradual fade: papyrus Actor.SetAlpha could do one (PAPYRUS_FUNCTION reaches it), but a copy left at alpha 0 by a
// failed fade is the invisible-player bug again; the hidden flag cannot outlive Show or the 1.5 s timeout.
namespace SpawnReveal
{
void HideUntilPlaced(Actor* apActor) noexcept;
void Placed(Actor* apActor) noexcept;
void Tick() noexcept;
} // namespace SpawnReveal
