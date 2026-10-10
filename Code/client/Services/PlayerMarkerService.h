#pragma once

#include <Structs/GameId.h>

struct World;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NotifyPlayerJoined;
struct NotifyPlayerLeft;
struct NotifyPlayerWhereabouts;
struct TESObjectREFR;
struct TESObjectCELL;
struct TESQuest;

/**
 * @brief The other players on the map and the compass (VR_TODO, "Players on the map", decided by Emma on 2026-10-10).
 *
 * Party members are objectives of the "Fellow travellers" misc quest (SkyrimTogether.esp, Tools/VR/player-markers.py):
 * a quest marker on the map and the compass, named "Seen, near Whiterun", hidden while their body is in our game.
 * Other players are a world-map icon each, never on the compass. Both follow the server's NotifyPlayerWhereabouts,
 * sent every two seconds with each player's exact position and place name.
 */
struct PlayerMarkerService
{
    PlayerMarkerService(World& aWorld, entt::dispatcher& aDispatcher);
    ~PlayerMarkerService() noexcept = default;

    TP_NOCOPYMOVE(PlayerMarkerService);

    static constexpr size_t kPartyMarkers = 7;
    static constexpr size_t kMapMarkers = 32;

protected:
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept;
    void OnWhereabouts(const NotifyPlayerWhereabouts& acMessage) noexcept;

private:
    // One marker and the player it shows, if any.
    struct Slot
    {
        TESObjectREFR* pMarker{nullptr};
        uint32_t PlayerId{0}; // 0: free
        String Text;
        GameId CellId{};
        glm::vec3 Position{};
        bool Parked{true}; // waiting in the holding cell, on no map
        bool Shown{false}; // party slots: the quest objective is displayed
    };

    bool FindForms() noexcept;
    void StartQuest() noexcept;
    void StopQuest() noexcept;
    void Follow(Slot& aSlot, size_t aIndex, const GameId& acWorldSpaceId, const GameId& acCellId, const glm::vec3& acPosition) noexcept;
    void Park(Slot& aSlot, size_t aIndex) noexcept;
    void Free(Slot& aSlot, size_t aIndex, bool aParty) noexcept;
    void SetObjectiveShown(Slot& aSlot, size_t aIndex, bool aShown) noexcept;
    bool IsObjectiveDisplayed(size_t aIndex) const noexcept;
    void SetPartyMarkerName(size_t aIndex, const String& acText) noexcept;
    void SetMapMarkerName(TESObjectREFR* apMarker, const String& acText) noexcept;
    void FreeAll() noexcept;

    World& m_world;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_joinedConnection;
    entt::scoped_connection m_leftConnection;
    entt::scoped_connection m_whereaboutsConnection;

    bool m_formsLooked{false};
    bool m_formsFound{false};
    bool m_connected{false};
    bool m_questStarted{false};
    uint32_t m_modBase{0};
    TESQuest* m_pQuest{nullptr};
    TESObjectCELL* m_pHoldingCell{nullptr};
    std::array<Slot, kPartyMarkers> m_partySlots{};
    std::array<Slot, kMapMarkers> m_mapSlots{};
    std::unordered_map<uint32_t, String> m_names;
};
