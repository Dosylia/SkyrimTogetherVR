#pragma once

struct World;
struct TransportService;

struct UpdateEvent;
struct NotifyObjectInventoryChanges;
struct NotifyInventoryChanges;
struct InventoryChangeEvent;
struct EquipmentChangeEvent;
struct NotifyEquipmentChanges;
struct Actor;

#include <Structs/Inventory.h>

/**
 * @brief Manages inventories of actors and containers.
 *
 * The initial contents of actor inventories are synced through spawn messages through CharacterService
 * @see CharacterService
 * The initial contents of container inventories are synced through the ObjectService
 * @see ObjectService
 * The InventoryService manages any changes to both the current equipment of actors,
 * and the contents of both actor and object inventories.
 */
struct InventoryService
{
    InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~InventoryService() noexcept = default;

    TP_NOCOPYMOVE(InventoryService);

    /**
     * @brief Check weapon state.
     * @see RunWeaponStateUpdates
     */
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;
    /**
     * @brief Sends out inventory changes made by the local client.
     */
    void OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept;
    /**
     * @brief Sends out equipment changes made by the local client.
     */
    void OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept;

    /**
     * @brief Applies inventory changes sent by the server.
     */
    void OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept;
    /**
     * @brief Applies equipment changes sent by the server.
     */
    void OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept;

    /**
     * Makes a remote actor's hand items (weapons, torches, spells) match an equipment snapshot.
     */
    static void ApplyHandEquipment(Actor* apActor, const Inventory& acEquipment) noexcept;

private:
    /**
     * Checks whether local actors their weapon draw states have changed,
     * and if so, send the new states to the server.
     */
    void RunWeaponStateUpdates() noexcept;
    /**
    * Checks whether an NPC's (local or remote) equipment is bugged (i.e. naked NPCS)
    * and resets their inventory.
    */
    void RunNakedNPCBugChecks() noexcept;
    /**
     * Sends the local player's worn equipment when it changed. Single equip events get lost or arrive
     * out of date on VR; the snapshot corrects the other clients within a second.
     */
    void RunEquipmentSnapshotUpdates() noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    uint32_t m_lastSnapshotServerId = 0;
    Inventory m_lastEquipmentSnapshot{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_inventoryConnection;
    entt::scoped_connection m_equipmentConnection;
    entt::scoped_connection m_inventoryChangeConnection;
    entt::scoped_connection m_equipmentChangeConnection;
};
