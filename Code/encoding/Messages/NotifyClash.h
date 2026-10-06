#pragma once

#include "Message.h"

#include <Structs/Vector3_NetQuantize.h>

//! Another player's hand or weapon met a weapon in their game (ClashRequest, passed on by the server to the players
//! near it). The receiver whose weapon it was feels it on the hand holding it.
struct NotifyClash final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyClash;

    NotifyClash()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyClash& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && FromId == acRhs.FromId && OtherId == acRhs.OtherId && OtherSide == acRhs.OtherSide && OwnSide == acRhs.OwnSide &&
               Point == acRhs.Point && Speed == acRhs.Speed && Tick == acRhs.Tick;
    }

    //! Server id of the character of the player who felt it first.
    uint32_t FromId{};
    //! Server id of the character whose weapon was met, and which one: 0 left, 1 right.
    uint32_t OtherId{};
    uint8_t OtherSide{};
    //! Which of the first player's hands met it: 0 left, 1 right, 2 not known.
    uint8_t OwnSide{2};
    Vector3_NetQuantize Point{};
    //! Game units a second.
    float Speed{};
    //! When, on the server's clock (TransportService::GetClock), in the game that saw it.
    uint64_t Tick{};
};
