#pragma once

#include <chrono>

struct ServerMessage;
struct Player
{
    Player(ConnectionId_t aConnectionId);
    ~Player() noexcept = default;

    Player(Player&&) noexcept;
    Player& operator=(Player&&) noexcept = default;

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    [[nodiscard]] uint32_t GetId() const noexcept { return m_id; }
    [[nodiscard]] ConnectionId_t GetConnectionId() const noexcept { return m_connectionId; }
    [[nodiscard]] std::optional<entt::entity> GetCharacter() const noexcept { return m_character; }
    [[nodiscard]] PartyComponent& GetParty() noexcept { return m_party; }
    [[nodiscard]] const PartyComponent& GetParty() const noexcept { return m_party; }
    [[nodiscard]] const String& GetUsername() const noexcept { return m_username; }
    [[nodiscard]] const String& GetEndPoint() const noexcept { return m_endpoint; }
    [[nodiscard]] const uint64_t GetDiscordId() const noexcept { return m_discordId; }
    [[nodiscard]] const uint32_t GetStringCacheId() const noexcept { return m_stringCacheId; }
    [[nodiscard]] const uint16_t GetLevel() const noexcept { return m_level; }

    [[nodiscard]] CellIdComponent& GetCellComponent() noexcept;
    [[nodiscard]] const CellIdComponent& GetCellComponent() const noexcept;
    [[nodiscard]] QuestLogComponent& GetQuestLogComponent() noexcept;
    [[nodiscard]] const QuestLogComponent& GetQuestLogComponent() const noexcept;

    void SetDiscordId(uint64_t aDiscordId) noexcept;
    void SetEndpoint(String aEndpoint) noexcept;
    void SetUsername(String aUsername) noexcept;
    void SetMods(Vector<String> aMods) noexcept;
    [[nodiscard]] const Vector<String>& GetMods() const noexcept { return m_mods; }
    void SetModIds(Vector<uint16_t> aModIds) noexcept;
    void SetCharacter(entt::entity aCharacter) noexcept;
    void SetStringCacheId(uint32_t aStringCacheId) noexcept;
    // TODO(cosideci): update on level up
    void SetLevel(uint16_t aLevel) noexcept;

    void SetCellComponent(const CellIdComponent& aCellComponent) noexcept;

    void Send(const ServerMessage& acServerMessage) const;

    // When the last movement snapshot came from this client. A paused client sends none: the mod's update runs off
    // the Papyrus VM, which the game suspends. Its actors are handed to whoever is near them meanwhile.
    [[nodiscard]] std::chrono::steady_clock::time_point GetLastMovementAt() const noexcept { return m_lastMovementAt; }
    void MarkMovement() noexcept { m_lastMovementAt = std::chrono::steady_clock::now(); }

private:
    uint32_t m_id{0};
    ConnectionId_t m_connectionId;
    std::optional<entt::entity> m_character;
    Vector<String> m_mods;
    Vector<uint16_t> m_modIds;
    uint64_t m_discordId{0};
    String m_endpoint;
    String m_username;
    PartyComponent m_party;
    QuestLogComponent m_questLog;
    CellIdComponent m_cell;
    uint32_t m_stringCacheId{0};
    uint16_t m_level{0};
    std::chrono::steady_clock::time_point m_lastMovementAt{};
};
