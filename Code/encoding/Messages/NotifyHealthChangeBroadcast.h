#pragma once

#include "Message.h"

struct NotifyHealthChangeBroadcast final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyHealthChangeBroadcast;

    NotifyHealthChangeBroadcast()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyHealthChangeBroadcast& acRhs) const noexcept
    {
        return Id == acRhs.Id && DeltaHealth == acRhs.DeltaHealth && AttackerPlayerId == acRhs.AttackerPlayerId && GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t Id;
    float DeltaHealth;
    //! The player whose hit this was, filled in by the server, which is the only side that knows. The owner of the
    //! actor uses it to put that player in its combat, so an NPC fights everyone hitting it and not only whoever
    //! angered it first.
    uint32_t AttackerPlayerId{};
};
