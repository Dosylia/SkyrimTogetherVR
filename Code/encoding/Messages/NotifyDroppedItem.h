#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>

//! An item lying in the world: the receiver adopts the one already on its floor if there is one, and
//! places it otherwise. Sent when an item is dropped, and again to anyone entering its cell.
struct NotifyDroppedItem final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDroppedItem;

    NotifyDroppedItem()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDroppedItem& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && Item == acRhs.Item && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position && Rotation == acRhs.Rotation;
    }

    uint32_t Id{};
    Inventory::Entry Item{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    //! Euler angles, radians, as the reference holds them. Plain floats: the packed position type is ~1 unit
    //! precise, which is fine for a place and useless for an angle.
    glm::vec3 Rotation{};
};
