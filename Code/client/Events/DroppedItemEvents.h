#pragma once

#include <Structs/Inventory.h>

//! The local player dropped an item, and the game has made the reference that lies on the floor.
//!
//! Raised from the drop hook after the real drop has run, because only then does the reference exist: its position
//! is what the other players need, not the player's. See DroppedItemService.
struct ItemDroppedEvent
{
    uint32_t RefFormId{};
    Inventory::Entry Item{};
};

//! The local player is picking up a reference that lies on the floor. Raised before the pick-up runs, while the
//! reference still exists to be named.
struct ItemPickedUpEvent
{
    uint32_t RefFormId{};
};
