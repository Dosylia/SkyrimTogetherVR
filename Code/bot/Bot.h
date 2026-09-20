#pragma once

// A headless Skyrim Together player. It speaks the client protocol (authenticate, assign a player character, send
// movement at 20 Hz, equip, take damage, die, respawn, reconnect) without a game behind it, so one person with a
// headset can test what a second player does to their screen. Phase 1: positions only, so the copy slides rather
// than walks; the appearance and outfit are cloned from the host so the receiving game builds the body from data it
// already accepts.

#include <EncodingPch.h>

// The transport's headers expect the standard clocks to be declared already.
#include <chrono>
#include <string>

#include <Client.hpp>

#include <Messages/AssignCharacterResponse.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/Tints.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

struct ServerMessage;

struct Command
{
    std::string Name;
    std::vector<std::string> Args;
    int Line{};
};

struct BotOptions
{
    std::string Server = "127.0.0.1:10578";
    std::string Password;
    std::string Name = "Bot";
    std::optional<glm::vec2> Start; // where to look for the host first; else read from HostLog
    std::string HostLog;            // the host's tp_client.log, for the position guess
    float Spacing = 200.f;          // spawn this far from the host (game units)
};

//! A character the server told us about; players only.
struct KnownPlayer
{
    uint32_t ServerId{};
    uint32_t PlayerId{};
    std::string Name;
    glm::vec3 Position{};
    glm::vec2 Rotation{};
    GameId CellId{};
    GameId WorldSpaceId{};
    TiltedPhoques::String AppearanceBuffer;
    uint32_t ChangeFlags{};
    Tints FaceTints{};
    Inventory InventoryContent{};
    float Health{};
};

class Bot final : public TiltedPhoques::Client
{
public:
    Bot(BotOptions aOptions, std::vector<Command> aScript) noexcept;
    ~Bot() override = default;

    //! Pumps the network and the script until the script says stop or the process is interrupted.
    int Run() noexcept;

    // TiltedPhoques::Client
    void OnConsume(const void* apData, uint32_t aSize) override;
    void OnConnected() override;
    void OnDisconnected(EDisconnectReason aReason) override;
    void OnUpdate() override {}

private:
    enum class Phase
    {
        Idle,           // will connect on the next tick
        Connecting,
        Authenticating, // AuthenticationRequest sent
        Scouting,       // looking for the host's character around the start position
        Assigning,      // AssignCharacterRequest sent
        InWorld,        // has a character; movement flows, the script runs
        Detached,       // disconnected on purpose by the script; waits for "reconnect"
        Stopped
    };

    using Clock = std::chrono::steady_clock;

    template <class T> bool SendMsg(const T& acMessage) noexcept;

    void Tick() noexcept;
    void SendAuthentication() noexcept;
    void Scout() noexcept;
    void Assign() noexcept;
    void SendCellEntry() noexcept;
    void SendMovement() noexcept;
    void SendHealth(float aHealth) noexcept;
    void SendEquip(uint32_t aBaseId, uint32_t aSlot, bool aUnequip, bool aSpell) noexcept;

    void HandleMessage(const ServerMessage& acMessage) noexcept;

    bool RunScript() noexcept;
    bool StepCommand(const Command& acCommand, bool aFirstTick) noexcept;
    bool MoveTowards(const glm::vec3& acTarget, float aSpeed, float aDt) noexcept;

    const KnownPlayer* Host() const noexcept;
    KnownPlayer* FindPlayerCharacter(uint32_t aServerId) noexcept;
    std::optional<glm::vec2> GuessStart() const noexcept;
    GameId Skyrim(uint32_t aBaseId) const noexcept { return GameId(m_skyrimModId, aBaseId); }

    BotOptions m_options;
    std::vector<Command> m_script;

    Phase m_phase = Phase::Idle;
    Clock::time_point m_phaseStart{};
    Clock::time_point m_lastMovement{};
    Clock::time_point m_lastTick{};
    Clock::time_point m_connectAt{};

    uint32_t m_playerId{};
    uint32_t m_skyrimModId{};
    uint32_t m_cookie{};
    uint32_t m_serverId{};
    std::string m_serverVersion;

    glm::vec3 m_position{};
    float m_yaw{};
    GameId m_cell{};
    GameId m_worldSpace{};
    float m_health = 100.f;
    float m_maxHealth = 100.f;
    Inventory m_inventory{};
    TiltedPhoques::String m_appearance;
    uint32_t m_changeFlags{};
    Tints m_faceTints{};
    bool m_hasBody = false;
    std::optional<glm::vec2> m_start;

    std::vector<KnownPlayer> m_players;
    std::vector<std::pair<uint32_t, std::string>> m_names; // player id -> username

    size_t m_pc = 0;
    Clock::time_point m_commandStart{};
    bool m_commandFresh = true;
    glm::vec3 m_walkTarget{};
    float m_walkSpeed{};
    Clock::time_point m_healthRestoreAt{};
    bool m_healthRestorePending = false;
    uint32_t m_reconnects{};
    bool m_leftParty = false;
};
