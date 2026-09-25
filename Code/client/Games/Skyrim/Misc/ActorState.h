#pragma once

#include <Misc/IMovementState.h>

struct ActorState : IMovementState
{
    virtual ~ActorState();

    uint32_t flags1;
    uint32_t flags2;

    bool IsWeaponDrawn() const noexcept { return (flags2 >> 5 & 7) >= 3; }

    bool IsWeaponFullyDrawn() const noexcept { return (flags2 >> 5 & 7) == 3; }

    bool IsBleedingOut() const noexcept { return (flags1 & 0x1E00000) == 0x1000000 || (flags1 & 0x1E00000) == 0xE00000; }

    // Life state (bits 21-24): 1 dying, 2 dead.
    bool IsDying() const noexcept { return (flags1 & 0x1E00000) == 0x200000; }
    bool IsDead() const noexcept { return (flags1 & 0x1E00000) == 0x400000; }
    bool IsDeadOrDying() const noexcept { return IsDying() || IsDead(); }

    //! ATTACK_STATE_ENUM, bits 28-31 of flags1. The bow values are the interesting ones: 9 kBowDraw,
    //! 10 kBowAttached (the arrow is on the string), 11 kBowDrawn, 12 kBowReleasing, 13 kBowReleased.
    uint32_t AttackState() const noexcept { return (flags1 >> 28) & 0xF; }

    bool SetWeaponDrawn(bool aDraw) noexcept;
};
