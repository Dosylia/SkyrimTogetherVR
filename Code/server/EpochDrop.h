#pragma once

#include <chrono>
#include <string>

//! Say when a message is thrown away for a bad ownership epoch, instead of dropping it in silence.
//!
//! This is the failure mode that hid four bot bugs and one real client bug through September 2026: the send
//! succeeds, the server discards the message, and the only symptom is that the world quietly disagrees. The
//! sender cannot detect it -- there is no negative acknowledgement anywhere in the protocol -- so the server has
//! to be the one to say so, or nobody ever finds out.
//!
//! Rate limited per kind of message, because a sender with a stale epoch sends a great many of them.
inline void ReportEpochDrop(const char* apWhat, uint32_t aServerId, uint32_t aSent, uint32_t aHeld) noexcept
{
    static TiltedPhoques::Map<std::string, std::chrono::steady_clock::time_point> s_nextByKind;
    const auto now = std::chrono::steady_clock::now();
    auto& next = s_nextByKind[apWhat];
    if (now < next)
        return;

    next = now + std::chrono::seconds(5);
    spdlog::warn("Dropped {} for actor {:X}: the sender's ownership epoch is {} and this server holds {}. Nothing was applied and the sender was not told.", apWhat, aServerId, aSent, aHeld);
}
