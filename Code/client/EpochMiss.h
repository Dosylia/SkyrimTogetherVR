#pragma once

#include <chrono>

//! Say when a message for a character we *do* have is thrown away because the epoch does not match.
//!
//! Six handlers -- health, the health broadcast, death, equipment and inventory -- accept a message only when the
//! copy's ownership epoch equals the message's. The server has already checked the sender's ownership before
//! broadcasting, so the arrival-side check mostly provides a way to discard valid state. It can be reached:
//! `NotifyOwnershipTransfer` is range-filtered, so a player who is away when an actor changes hands never learns
//! the new epoch and silently drops everything about that character until a re-sent spawn refreshes it.
//!
//! Whether that is a rare corner or a constant drip decides whether the equality test should become "not older".
//! Until a session says, the drops are counted rather than argued about. Rate limited per kind.
inline void ReportEpochMiss(const char* apWhat, uint32_t aServerId, uint32_t aMessageEpoch, uint32_t aCopyEpoch) noexcept
{
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_nextByKind;
    static std::unordered_map<std::string, uint32_t> s_sinceByKind;

    const auto now = std::chrono::steady_clock::now();
    auto& count = s_sinceByKind[apWhat];
    ++count;

    auto& next = s_nextByKind[apWhat];
    if (now < next)
        return;

    next = now + std::chrono::seconds(10);
    spdlog::warn("EpochMiss: dropped {} for character {:X} -- the message is epoch {} and this copy is epoch {}. {} of these since the last line.", apWhat, aServerId, aMessageEpoch, aCopyEpoch, count);
    count = 0;
}
