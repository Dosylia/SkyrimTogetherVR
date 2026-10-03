#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

//! An object placed in the world moved on another player's side, or was left there before this player arrived: put it
//! where they have it.
struct NotifyWorldObjectMove final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyWorldObjectMove;

    NotifyWorldObjectMove()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyWorldObjectMove& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ObjectId == acRhs.ObjectId && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId &&
               Position == acRhs.Position && Rotation == acRhs.Rotation && AtRest == acRhs.AtRest;
    }

    //! The object's own reference id, the same in every game that has the plugin it comes from.
    GameId ObjectId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    glm::vec3 Rotation{};
    //! The last of a movement: the object has stopped, and the receiver hands it back to its own physics.
    bool AtRest{};
};
