#pragma once

#include <cstddef>
#include <cstdint>

//! When a remote copy we have been told to remove may actually be freed.
//!
//! This is deliberately a pure function of numbers, with no game types in it, because the reasoning is what
//! kept going wrong and reasoning is the part that can be tested. See CopyRemovalPolicy tests.
//!
//! The history it encodes, from 2026-09-27:
//!
//!   - Deleting immediately is a use-after-free. Every copy is reported as "still in the AI process lists,
//!     dead or dying, 3 outstanding handle(s)", and the game then makes a virtual call on it -- 567 ms later in
//!     one dump, 24 s later in another.
//!   - Waiting for the handle count to reach zero never frees anything at all: the holders are the game's own
//!     lists, and they do not let go while the cell is loaded. Twice this filled the world with live copies and
//!     collapsed Emma's frame rate in VR.
//!
//! So waiting is bounded by construction rather than by hoping. At most kMaxWaiting copies are ever held back;
//! the moment a new one arrives at that limit, the oldest is freed regardless of its handles. The worst case is
//! therefore exactly the old immediate-delete behaviour -- which crashes Seen and costs Emma nothing -- and the
//! ordinary case frees copies cleanly once the game has let go. It cannot degrade into an unbounded pile-up,
//! because the bound does not depend on the game releasing anything.
//!
//! Held-back copies are disabled while they wait, so their cost is a slot in a list rather than sound, AI and
//! draw calls.
namespace CopyRemovalPolicy
{
//! Never hold back more than this many copies. Small on purpose: the bound is the safety property, and a
//! handful of disabled corpses is the entire budget.
constexpr size_t kMaxWaiting = 24;

//! Give up waiting after this long and free it anyway. A copy nothing has released in a minute is one whose
//! holder is not going to release it while this cell is loaded.
constexpr uint32_t kMaxWaitMs = 60000;

enum class Action
{
    DeleteNow,    //!< free it here: either nothing holds it, or the waiting list is full
    WaitDisabled, //!< disable it and try again when its handles clear
};

//! What to do with a copy the moment we are told to remove it.
//! @param aHandles Outstanding BSPointerHandles: how many things still have a claim on the actor.
//! @param aWaitingCount How many copies are already being held back.
inline Action OnRemoved(const uint32_t aHandles, const size_t aWaitingCount) noexcept
{
    if (aHandles == 0)
        return Action::DeleteNow;

    if (aWaitingCount >= kMaxWaiting)
        return Action::DeleteNow;

    return Action::WaitDisabled;
}

//! Whether a copy that is already waiting should be freed now.
//! @param aHandles Its outstanding handles as of this check.
//! @param aWaitedMs How long it has been waiting.
//! @param aOverBound True when the list is over its limit and this is among the oldest that must go.
inline bool ShouldRelease(const uint32_t aHandles, const uint32_t aWaitedMs, const bool aOverBound) noexcept
{
    return aHandles == 0 || aWaitedMs >= kMaxWaitMs || aOverBound;
}
} // namespace CopyRemovalPolicy
