#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

//! An object placed in the world (a bottle on a shelf, a cart, a bucket) is moving here -- held, thrown, pushed -- and
//! this is where it is now. Unlike a dropped item it exists in every game already, under the same id.
struct RequestWorldObjectMove final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWorldObjectMove;

    RequestWorldObjectMove()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestWorldObjectMove& acRhs) const noexcept
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
