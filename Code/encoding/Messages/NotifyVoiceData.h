#pragma once

#include "Message.h"

using TiltedPhoques::Vector;

//! A piece of another player's voice (VoiceDataRequest), sent unreliable to the players near enough to hear it and to
//! the speaker's party at any distance.
struct NotifyVoiceData final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyVoiceData;

    NotifyVoiceData()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyVoiceData& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && PlayerId == acRhs.PlayerId && Sequence == acRhs.Sequence && Data == acRhs.Data;
    }

    //! The speaker's player id (as in NotifyPlayerJoined).
    uint32_t PlayerId{};
    uint32_t Sequence{};
    Vector<uint8_t> Data{};
};
