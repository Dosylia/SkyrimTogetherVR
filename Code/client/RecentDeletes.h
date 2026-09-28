#pragma once

#include <atomic>
#include <cstdint>

//! The last few actors this client deleted, so a crash dump can be matched against them by address.
//!
//! Seen's dump of 2026-09-27 11:45 established that the crash is a use-after-free: the game makes a virtual call
//! on an object whose first vtable has been overwritten with heap garbage, ~0.5 s after we tear down a batch of
//! remote copies. What it could not establish is whether the dead object *is* one of those copies -- the link was
//! inferred from timing, and inferring from timing has been wrong repeatedly this week.
//!
//! An address settles it. Every temporary remote deletion records the actor's pointer here, and the crash handler
//! prints the ring. If the faulting object's address appears in that list, the deletion path is the cause and
//! there is nothing left to argue about; if it never appears, the path is innocent and the search moves elsewhere.
//!
//! Deliberately trivial: a fixed array, one atomic counter, no allocation and no lock. It is written from the
//! actor threads and read from a crashing one, where a torn entry is a cosmetic problem and a lock is a real one.
namespace RecentDeletes
{
struct Entry
{
    uint64_t Address{};
    uint32_t FormId{};
    uint32_t Handles{};   // outstanding BSPointerHandles: who else still has a claim on this actor
    uint32_t RefCount{};  // the whole refCount word, handles are its low 10 bits
    uint32_t Flags{};     // see Claim below
    uint64_t Stamp{};     // steady_clock ticks
};

//! What still had a hold on an actor at the moment it was deleted.
//!
//! Twenty-four seconds passed between deleting FF001199 and the game calling a virtual function on it
//! (2026-09-27 16:05). Nothing survives that long by accident, so something was holding a handle. These are the
//! holders worth naming, recorded at the moment of the delete because afterwards the evidence is gone.
enum Claim : uint32_t
{
    kNone = 0,
    kPlayerCombatTarget = 1 << 0, //!< the player is fighting it -- Emma shot the Seeker that then went wrong
    kOtherCombatTarget = 1 << 1,  //!< some other actor is fighting it
    kHasProcess = 1 << 2,         //!< still in the AI process lists
    kHas3D = 1 << 3,              //!< still has a loaded model, so "unloaded" was wrong about it
    kDeadOrDying = 1 << 4,
    kIsPlayerTeammate = 1 << 5,
};

constexpr size_t kCount = 32;

//! Note that an actor is about to be deleted, and what still had a claim on it.
void Record(const void* apActor, uint32_t aFormId, uint32_t aHandles, uint32_t aRefCount, uint32_t aFlags) noexcept;

//! Write the ring to the log, newest first, flagging any entry that appears in the crashing registers.
//!
//! The first version compared only Rcx and reported "not our actor deletion" for the crash of 2026-09-27
//! 12:33 -- while Rdi held 0x9b30f4f0, an actor deleted 1930 ms earlier and printed three lines below. A dead
//! pointer is just as dead in Rdi as in Rcx.
void Report(const uint64_t* apRegisters, size_t aRegisterCount) noexcept;
} // namespace RecentDeletes
