#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>

//! A player dropped an item, or announces one it dropped before: where it lies, so the server can remember it
//! and give it to everyone who comes near, now or in a later session.
struct RequestDroppedItemAdd final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDroppedItemAdd;

    RequestDroppedItemAdd()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDroppedItemAdd& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Announcement == acRhs.Announcement && Item == acRhs.Item && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position && Rotation == acRhs.Rotation;
    }

    Inventory::Entry Item{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    //! Euler angles, radians, as the reference holds them. Plain floats: the packed position type is ~1 unit
    //! precise, which is fine for a place and useless for an angle.
    glm::vec3 Rotation{};

    //! False for a drop that just happened: always a new item, even beside an identical one, or dropping two
    //! iron daggers in one spot would sync only the first. True for an item the player dropped earlier and is
    //! announcing now (on connecting, or entering its cell): the server may already know it, from this
    //! player's last session or from the other player's save, so it matches by item and place first.
    bool Announcement{};
};
