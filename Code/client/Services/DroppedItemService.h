#pragma once

#include <Events/EventDispatcher.h>

#include <Structs/Inventory.h>

struct World;
struct TransportService;
struct ConnectedEvent;
struct DisconnectedEvent;
struct CellChangeEvent;
struct ItemDroppedEvent;
struct ItemPickedUpEvent;
struct NotifyDroppedItem;
struct NotifyDroppedItemRemoved;
struct TESObjectREFR;

/**
 * @brief Keeps items players drop lying in every player's world, including items dropped in earlier sessions.
 *
 * The server remembers each dropped item (see the server's DroppedItemService); this side does three things with that.
 *
 *  - It reports the local player's drops, from the reference the game actually made, so the position is where the
 *    item lies rather than where the player stood.
 *  - It puts the server's items in this world. Each one is first *adopted* if it is already here -- the player's own
 *    drop, or one this save kept from an earlier session -- and only *placed* if it is missing. Adopting is what stops
 *    the same item turning up twice.
 *  - It passes pick-ups on, so everybody's copy leaves the floor when anybody takes it.
 *
 * And once per session and cell it announces the items the player dropped before any of this existed, read from the
 * list the game itself keeps on the player (ExtraDroppedItemList). That list is exact: it is what this player dropped,
 * and nothing else -- not a dead NPC's weapon, not an arrow in the ground.
 */
class DroppedItemService
{
public:
    DroppedItemService(World&, entt::dispatcher&, TransportService&);

private:
    void OnConnected(const ConnectedEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnCellChange(const CellChangeEvent&) noexcept;
    void OnItemDropped(const ItemDroppedEvent&) noexcept;
    void OnItemPickedUp(const ItemPickedUpEvent&) noexcept;
    void OnNotifyDroppedItem(const NotifyDroppedItem&) noexcept;
    void OnNotifyDroppedItemRemoved(const NotifyDroppedItemRemoved&) noexcept;

    //! Send a reference that lies on the floor to the server. False when its place cannot be named to the server.
    bool SendAdd(TESObjectREFR* apRef, const Inventory::Entry& acItem, bool aAnnouncement) noexcept;
    void AnnounceOldDrops() noexcept;

    void Map(uint32_t aId, uint32_t aRefFormId) noexcept;

    World& m_world;
    TransportService& m_transport;

    //! Server id to the reference that stands for it here, and back.
    std::unordered_map<uint32_t, uint32_t> m_refById;
    std::unordered_map<uint32_t, uint32_t> m_idByRef;

    //! This player's own drops and announcements, waiting for the id the server gives them.
    struct Pending
    {
        uint32_t RefFormId{};
        GameId BaseId{};
    };
    std::vector<Pending> m_pending;

    //! Old drops already announced this session, so a cell change does not send them again.
    std::unordered_set<uint32_t> m_announced;

    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_droppedConnection;
    entt::scoped_connection m_pickedUpConnection;
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_removedConnection;
};
