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
#include <Structs/ReferenceUpdate.h>
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
    //! The worldspace to look in, as the game's form id (Solstheim is 0x02000800 in a load order where
    //! Dragonborn.esm is index 2). Empty means Tamriel, which is where this used to be nailed down: the bot
    //! could not see a host anywhere else, and 2026-09-24 was spent discovering that rather than testing.
    std::optional<uint32_t> WorldSpaceFormId;
    //! The plugin that form id belongs to. The index inside the form id is this machine's load order, which the
    //! server does not share, so the plugin has to be named.
    std::string WorldSpacePlugin = "Skyrim.esm";
    //! Enter the world without a human to copy. The bot normally clones its appearance and inventory from a host
    //! player, which means no test can run unless someone is in a headset; with this it gives up waiting and goes
    //! in with an empty body. Nothing renders it, so it is only good for protocol behaviour -- health, death,
    //! ownership, party, spawns -- but that is most of what the bot is for, and two of these can test each other
    //! with nobody present.
    bool Standalone = false;
    float HostTimeout = 20.f; // seconds of looking for a host before giving up, with Standalone
    //! Hard ceiling on a whole run. A bot that cannot connect retries for ever and its script never advances, so
    //! no waitfor timeout can rescue it; unattended, that is a hang rather than a failed test. 0 disables it.
    float MaxRuntime = 300.f;
};

//! Any character the server told us about, player or NPC. The bot cannot see a game, so this is the whole of
//! what it knows: it is what `expect` and `waitfor` are checked against.
struct KnownActor
{
    uint32_t ServerId{};
    //! The base form the server sent with an NPC's spawn. Empty for players and for NPCs this bot registered itself.
    GameId Base{};
    //! The placed reference the server sent with an NPC's spawn (a bandit placed in Mistwatch, say). Empty for
    //! temporary actors. What `ref:<hex>` in a script names.
    GameId Ref{};
    std::string Name;      // when the server named it
    float Health{};
    bool HealthKnown{};
    bool Dead{};
    bool IsPlayer{};
    bool OwnedByUs{};
    //! The epoch the server last told us this actor's ownership was at.
    //!
    //! Kept because every message in the ownership family is refused unless it carries the current one, and each
    //! transfer advances it. Handing an actor back with a stale epoch is silently ignored -- three separate bugs
    //! in this family were the same mistake (2026-09-26) -- so the number has to be stored, not guessed.
    uint32_t OwnershipEpoch{};
    std::chrono::steady_clock::time_point LastChange{};
    //! What this actor was last seen equipping, by base form id. Empty once it unequips. Last, so that
    //! KnownActor{serverId} still initialises the id.
    std::vector<uint32_t> Equipped;
};

//! What a run is allowed to record. A test that collects everything drowns in movement, so a script says what it
//! cares about and the rest is counted but not written.
enum class Collect : uint32_t
{
    None = 0,
    Health = 1 << 0,
    Death = 1 << 1,
    Ownership = 1 << 2,
    Spawn = 1 << 3,
    Party = 1 << 4,
    Movement = 1 << 5,
    Equipment = 1 << 6,
    All = 0xFFFFFFFF
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
    //! How many times the server has sent this character to us. The server re-sends a character to put a stale or
    //! missing copy right, so this counts repairs -- and a repair that never arrives is a copy left wrong, which
    //! is the whole invisible-player family of bugs. Assertable as `spawns other >= 2`.
    uint32_t Spawns{};
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
    //! Fills in what a host would have provided, so Assign can run with nobody else in the world.
    void UseEmptyBody() noexcept;
    void Assign() noexcept;
    void SendCellEntry() noexcept;
    void SendMovement() noexcept;
    void SendHealth(float aHealth) noexcept;
    void SendDeath(bool aDead) noexcept;
    void SendHit(uint32_t aTargetId, float aDelta, uint64_t aTick = 0) noexcept;
    void SendEquip(uint32_t aBaseId, uint32_t aSlot, bool aUnequip, bool aSpell) noexcept;

    void HandleMessage(const ServerMessage& acMessage) noexcept;

    bool RunScript() noexcept;
    //! One line of the report, kept only while recording and only for an enabled kind.
    void Record(Collect aKind, const std::string& acText) noexcept;
    KnownActor& Actor(uint32_t aServerId) noexcept;
    //! Update-only: never invents a character. Only a spawn may do that.
    KnownActor* FindActor(uint32_t aServerId) noexcept;
    //! Shared by waitfor and expect. Returns nothing when the condition cannot be parsed.
    std::optional<bool> Evaluate(const std::vector<std::string>& acArgs, std::string& aOutWhy) const noexcept;
    void WriteReport(const std::string& acPath) const noexcept;
    bool StepCommand(const Command& acCommand, bool aFirstTick) noexcept;
    bool MoveTowards(const glm::vec3& acTarget, float aSpeed, float aDt) noexcept;

    const KnownPlayer* Host() const noexcept;
    //! Move into the cell the host just entered, so a load door does not leave the bot outside.
    void FollowHostIntoCell(const GameId& acCell, const GameId& acWorldSpace) noexcept;
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
    // The server checks this on every change we ask for and silently drops the message when it does not match,
    // including when it is left at zero. Without it the bot can ask for anything and nothing ever happens.
    uint32_t m_ownershipEpoch{};
    // Whether this bot has a character at all. It cannot be inferred from m_serverId: the server hands out entity
    // ids from zero, so the first character on a freshly started server legitimately has id 0.
    bool m_hasCharacter{false};
    //! The grid square last reported to the server, so a walk that crosses one says so.
    int32_t m_reportedGridX{};
    int32_t m_reportedGridY{};
    bool m_gridReported{false};
    //! True when the cell id is one this bot invented for its grid square rather than a real one from a host.
    bool m_standaloneCell{false};
    //! Whether announcing a cell also sends the grid shift. Off makes the bot behave like a real client whose
    //! *current* grid changed while its *centre* grid did not -- walking from one outdoor cell into the next,
    //! which sends the exterior-enter alone. The bot always sent both, so the server's handling of that message
    //! on its own could not be tested: whatever the exterior-enter failed to do, the grid shift did a moment
    //! later. See the cellwalk pair.
    bool m_sendGridShift{true};
    //! Whether this bot keeps an actor the server hands it, instead of handing it straight back.
    //!
    //! Off by default, because a bot has no game behind it and owning an actor means being asked to drive
    //! something it cannot drive -- every existing test relies on that. The ownership-churn pair turns it on,
    //! because handing an actor back and forth on purpose is the thing it measures.
    bool m_acceptOwnership{false};
    bool m_followHostCell{true}; // false after `stay`
    //! The cookie of an NPC this bot asked the server to register, kept apart from m_cookie so the response for
    //! its own character and the response for the NPC can be told apart.
    uint32_t m_npcCookie{};
    std::string m_serverVersion;

    glm::vec3 m_position{};
    float m_yaw{};
    GameId m_cell{};
    // Where "teleport away" left from, for "teleport back".
    glm::vec3 m_awayFrom{};
    GameId m_awayFromCell{};
    bool m_awayFromStandalone{false};
    bool m_isAway{false};
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
    //! Items lying in the world as the server has told this bot, by the server's id: what `drops` counts and
    //! `pickup any` takes. The server sends one when an item is dropped in range and again on entering its cell.
    std::map<uint32_t, GameId> m_drops;
    // This bot's own drop: the first new item of the base it last dropped. Items already lying there (a save with drops
    // of its own) have lower ids or come first, so "the first known item" is not necessarily this bot's.
    GameId m_pendingDropBase{};
    uint32_t m_ownDrop = UINT32_MAX;
    uint32_t DropToHandle() const noexcept { return m_drops.count(m_ownDrop) ? m_ownDrop : m_drops.begin()->first; }
    uint32_t m_dropRemovals{};
    //! Where each of them lies now as far as this bot knows, and where it first heard of it -- what `dropmoved`
    //! measures. The first place outlives a pick-up, so a late re-send can still be measured against it.
    std::map<uint32_t, glm::vec3> m_dropPlaces;
    std::map<uint32_t, glm::vec3> m_dropFirstPlaces;
    //! Moves relayed to this bot by the server, which only sends them to players in range.
    uint32_t m_dropMoves{};
    uint32_t m_clashes{}; //!< clashes relayed to this bot (NotifyClash)
    uint64_t m_lastClashTick{}; //!< when the last of them was seen, on the server's clock
    uint64_t m_firstClashTick{}; //!< and the first
    uint32_t m_objectMoves{}; //!< world-object moves relayed to this bot
    uint32_t m_activationsBack{}; //!< activations relayed to this bot in its own name: its own, sent back by a game
    std::vector<std::pair<uint32_t, std::string>> m_names; // player id -> username

    size_t m_pc = 0;
    Clock::time_point m_commandStart{};
    bool m_commandFresh = true;
    glm::vec3 m_walkTarget{};
    float m_walkSpeed{};
    //! `capture` and `replay`: the movement stream the server relayed for one character -- positions, animation
    //! variables, actions and VR pose, exactly as any client is given them -- and when each piece arrived. Replayed
    //! as this bot's own character (or an NPC it owns), it is a remote body that moves the way a real one does,
    //! which a bot walking in straight lines with no animation data never was.
    struct CapturedUpdate
    {
        uint32_t AtMs{};
        ReferenceUpdate Update{};
    };
    std::vector<CapturedUpdate> m_capture;
    uint32_t m_captureId{};
    bool m_capturing{false};
    Clock::time_point m_captureStart{};
    size_t m_replayIndex{};
    uint32_t m_replayTarget{};
    int m_replayRound{};
    bool m_replaying{false};
    glm::vec3 m_replayBase{};
    //! `handtargets`: hand positions (root space, left then right) written into every replayed VR pose instead of the
    //! recorded ones -- the rig's headset has no real controllers, so a recorded pose has the hands on the floor.
    bool m_hasHandTargets{false};
    glm::vec3 m_handTargets[2]{};
    //! `holdpose on`: the last VR pose a replay sent as this bot's own goes on being sent with every movement after the
    //! replay ends, so the copy stays posed (a stream without a pose hands the copy back to its animation).
    bool m_holdPose{false};
    VRPose m_lastReplayedPose{};
    //! Where the NPC this bot registered stands, which is where a replay for it starts from.
    glm::vec3 m_npcPosition{};
    //! The base form of the last character captured, as the server names it: what "npc captured" registers a
    //! creature of. The bot only knows Skyrim.esm's mod id itself, and a Seeker is not in Skyrim.esm.
    GameId m_captureBase{};

    //! `burn`: who is being hurt and how many quarter-second batches have gone out.
    uint32_t m_burnTarget{};
    int m_burnSent{};
    Clock::time_point m_healthRestoreAt{};
    bool m_healthRestorePending = false;
    uint32_t m_reconnects{};
    bool m_leftParty = false;

    // Testing controls. The point of these is that a run says pass or fail by itself, and collects only what the
    // script asked for, instead of leaving someone to read a log afterwards and guess.
    std::vector<KnownActor> m_actors;
    uint32_t m_collect = static_cast<uint32_t>(Collect::All);
    bool m_recording = false;
    std::string m_recordName;
    Clock::time_point m_recordStart{};
    std::vector<std::string> m_events;
    uint64_t m_suppressed = 0;
    uint32_t m_checksPassed = 0;
    std::vector<std::string> m_failures;
};
