#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

using TiltedPhoques::String;
using TiltedPhoques::Vector;

//! Where every player is, every two seconds, for the markers on the map and the compass (VR_TODO, "Players on the
//! map"): the world (empty indoors) and cell their character is in, their exact position, and the place name their
//! game reports (PlayerPlaceRequest: the room indoors, the town, dungeon or hold outdoors). The receiver skips itself.
struct NotifyPlayerWhereabouts final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerWhereabouts;

    struct Entry
    {
        bool operator==(const Entry& acRhs) const noexcept
        {
            return PlayerId == acRhs.PlayerId && WorldSpaceId == acRhs.WorldSpaceId && CellId == acRhs.CellId && Position == acRhs.Position && Place == acRhs.Place;
        }

        uint32_t PlayerId{};
        GameId WorldSpaceId{};
        GameId CellId{};
        Vector3_NetQuantize Position{};
        String Place{};
    };

    NotifyPlayerWhereabouts()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPlayerWhereabouts& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Players == acRhs.Players; }

    Vector<Entry> Players{};
};
