#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>

struct NotifyEquipmentChanges final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyEquipmentChanges;

    NotifyEquipmentChanges()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyEquipmentChanges& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && ItemId == acRhs.ItemId && EquipSlotId == acRhs.EquipSlotId && Count == acRhs.Count && Unequip == acRhs.Unequip && IsSpell == acRhs.IsSpell && IsShout == acRhs.IsShout && CurrentInventory == acRhs.CurrentInventory;
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    GameId ItemId{};
    GameId EquipSlotId{};
    uint32_t Count{};
    bool Unequip = false;
    bool IsSpell = false;
    bool IsShout = false;
    // Worn equipment reported by the owner. An empty ItemId means this is only an equipment snapshot.
    Inventory CurrentInventory{};
};
