#pragma once

#include <Events/PacketEvent.h>

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>

struct World;
struct Player;
struct CellIdComponent;
struct RequestDroppedItemAdd;
struct RequestDroppedItemRemove;
struct EnterInteriorCellRequest;
struct EnterExteriorCellRequest;
struct ShiftGridCellRequest;

/**
 * @brief Remembers items players drop, and gives them to everyone who comes near.
 *
 * A drop used to be replayed on the other side only at the moment it happened, by making the dropper's copy drop the
 * same item. The server kept nothing, and every player loads their own save, so an item dropped before the other
 * player connected, out of their range, or in another session existed in one world only -- Emma's report of
 * 2026-09-30, items dropped sessions earlier that Seen could not see. (That live path had also never run: the setting
 * that enabled it was false in every STServer.ini, and an ini beats a new default.)
 *
 * So the server is now the record. Each item is kept with where it lies, sent to everyone in range when it is dropped
 * and again to anyone entering its cell, and forgotten when somebody picks it up. The list is written to disk, because
 * the whole complaint was about items outliving a session.
 */
class DroppedItemService
{
public:
    DroppedItemService(World& aWorld, entt::dispatcher& aDispatcher);

    struct Item
    {
        uint32_t Id{};
        Inventory::Entry Entry{};
        GameId CellId{};
        GameId WorldSpaceId{};
        Vector3_NetQuantize Position{};
    };

    //! The saved list, for tests and diagnostics.
    [[nodiscard]] const std::map<uint32_t, Item>& Items() const noexcept { return m_items; }

private:
    void OnAdd(const PacketEvent<RequestDroppedItemAdd>& acMessage) noexcept;
    void OnRemove(const PacketEvent<RequestDroppedItemRemove>& acMessage) noexcept;
    void OnEnterInteriorCell(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept;
    void OnEnterExteriorCell(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept;
    void OnShiftGridCell(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept;

    //! Everything within range of where a player now stands, to that player.
    void SendInRange(Player* apPlayer, const CellIdComponent& acViewer) const noexcept;
    void Send(Player* apPlayer, const Item& acItem) const noexcept;

    const Item* FindSame(const Inventory::Entry& acEntry, const GameId& acCellId, const GameId& acWorldSpaceId,
                         const Vector3_NetQuantize& acPosition) const noexcept;

    void Load() noexcept;
    void Save() const noexcept;

    World& m_world;
    std::map<uint32_t, Item> m_items;
    uint32_t m_nextId{1};

    entt::scoped_connection m_addConnection;
    entt::scoped_connection m_removeConnection;
    entt::scoped_connection m_interiorConnection;
    entt::scoped_connection m_exteriorConnection;
    entt::scoped_connection m_gridConnection;
};
