#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>

//! Somebody picked the item up: the receiver takes its own copy of it off the floor.
struct NotifyDroppedItemRemoved final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDroppedItemRemoved;

    NotifyDroppedItemRemoved()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDroppedItemRemoved& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id;
    }

    uint32_t Id{};
};
