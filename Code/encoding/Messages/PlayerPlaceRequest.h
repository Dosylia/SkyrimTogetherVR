#pragma once

#include "Message.h"

//! Where the player is, by name: the room's name indoors, the location's outdoors (a town, a dungeon, the hold in the
//! wilds). Sent when it changes; the server shows it on the public server page and nowhere else.
struct PlayerPlaceRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPlayerPlaceRequest;

    PlayerPlaceRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const PlayerPlaceRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Place == acRhs.Place; }

    String Place{};
};
