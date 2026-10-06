#pragma once

#include "Message.h"

struct RequestHealthChangeBroadcast final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestHealthChangeBroadcast;

    RequestHealthChangeBroadcast()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestHealthChangeBroadcast& acRhs) const noexcept
    {
        return Id == acRhs.Id && DeltaHealth == acRhs.DeltaHealth && Tick == acRhs.Tick && GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t Id;
    float DeltaHealth;
    //! When it happened in the sender's game, on the server's clock (TransportService::GetClock); 0 when not known.
    uint64_t Tick{};
};
