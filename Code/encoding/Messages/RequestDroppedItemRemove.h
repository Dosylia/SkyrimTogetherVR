#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>

//! A player picked up an item the server was remembering, by the id the server gave it.
struct RequestDroppedItemRemove final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDroppedItemRemove;

    RequestDroppedItemRemove()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDroppedItemRemove& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id;
    }

    uint32_t Id{};
};
