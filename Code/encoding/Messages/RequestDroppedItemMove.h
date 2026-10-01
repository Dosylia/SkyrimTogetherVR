#pragma once

#include "Message.h"

#include <Structs/Vector3_NetQuantize.h>

//! A remembered item is moving here -- held, thrown, pushed -- and this is where it is now.
struct RequestDroppedItemMove final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDroppedItemMove;

    RequestDroppedItemMove()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDroppedItemMove& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && Position == acRhs.Position && Rotation == acRhs.Rotation && AtRest == acRhs.AtRest;
    }

    //! The server's id for the item, as NotifyDroppedItem gave it.
    uint32_t Id{};
    Vector3_NetQuantize Position{};
    glm::vec3 Rotation{};
    //! The last of a movement: the item has stopped. The server keeps this place for anyone who arrives later, and the
    //! receiver hands the item back to its own physics.
    bool AtRest{};
};
