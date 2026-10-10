#pragma once

#include "Message.h"

//! This player turned voice on or off in the urSovngarde menu. Off (the default) means they neither speak nor hear:
//! the server sends voice only to the players who have it on.
struct VoiceStateRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kVoiceStateRequest;

    VoiceStateRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const VoiceStateRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Enabled == acRhs.Enabled; }

    bool Enabled{};
};
