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
struct NotifyDroppedItemMove;
struct UpdateEvent;
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
 *  - It passes movement on: an item held, thrown or pushed here is sent where it is ten times a second, and one
 *    moved over there is held still by this side's physics and put where the other player has it.
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
    void OnNotifyDroppedItemMove(const NotifyDroppedItemMove&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;

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

    //! How each remembered item has been moving here, by server id.
    //!
    //! Only one side should be sending an item's position at a time, or the two chase each other: this side moves it,
    //! the other side's copy follows and is reported back as moving, and so on. So an item being moved by the other
    //! side is held (keyframed) here and not watched, and after anything this side does to an item on the other's word
    //! -- placing it, moving it, letting it go -- its own settling is ignored for a moment.
    struct Motion
    {
        glm::vec3 LastPosition{};
        glm::vec3 LastRotation{};
        std::chrono::steady_clock::time_point LastLocalMove{};
        std::chrono::steady_clock::time_point LastRemote{};
        std::chrono::steady_clock::time_point QuietUntil{};
        //! The item's base form when it was mapped. A dropped item's FF id is handed to another object once the game
        //! deletes the item, and whatever has it now must not be moved -- it could be an actor.
        uint32_t BaseFormId{};
        bool Moving{};
        bool Held{};
        //! The last word from the other side said it was put down. Still held for a moment: a hand kept still reads
        //! the same as a table, and letting go at once dropped the item out of a still hand on this side.
        bool RemoteAtRest{};
        //! Its 3D was there on the last pass. A cell loading settles every item in it, which is not a move.
        bool Loaded{};
    };
    std::unordered_map<uint32_t, Motion> m_motion;

    //! Old drops already announced this session, so a cell change does not send them again.
    std::unordered_set<uint32_t> m_announced;
    //! Whether this connection has said what it found on the player, so an empty result is reported once, not never.
    bool m_reportedOldDrops{false};

    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_droppedConnection;
    entt::scoped_connection m_pickedUpConnection;
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_removedConnection;
    entt::scoped_connection m_moveConnection;
    entt::scoped_connection m_updateConnection;
};
