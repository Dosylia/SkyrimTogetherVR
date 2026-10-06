#pragma once

#include "Message.h"

#include <Structs/Vector3_NetQuantize.h>

//! One of this player's hands or weapons met another player's weapon here (VRBodySync: a Havok contact between that
//! weapon's body in this game and one of this player's HIGGS bodies). The server passes it on (NotifyClash), so the
//! other player feels it too -- on the hand holding the weapon that was met.
struct ClashRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kClashRequest;

    ClashRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const ClashRequest& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && OtherId == acRhs.OtherId && OtherSide == acRhs.OtherSide && OwnSide == acRhs.OwnSide && Point == acRhs.Point &&
               Speed == acRhs.Speed && Tick == acRhs.Tick;
    }

    //! Server id of the character whose weapon it was.
    uint32_t OtherId{};
    //! Which of that character's weapons: 0 left, 1 right.
    uint8_t OtherSide{};
    //! Which of this player's hands it was: 0 left, 1 right, 2 not known.
    uint8_t OwnSide{2};
    //! Where, in the world.
    Vector3_NetQuantize Point{};
    //! How fast the two met, in game units a second.
    float Speed{};
    //! When, on the server's clock (TransportService::GetClock), in the game that saw it.
    uint64_t Tick{};
};
