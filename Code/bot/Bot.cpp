#include "Bot.h"

#include <Messages/AssignCharacterRequest.h>
#include <Messages/AuthenticationRequest.h>
#include <Messages/AuthenticationResponse.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/Message.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/PartyLeaveRequest.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyRelinquishControl.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/PlayerRespawnRequest.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/RequestRespawn.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ShiftGridCellRequest.h>
#include <Messages/StringCacheUpdate.h>
#include <StringCache.h>
#include <Structs/GridCellCoords.h>

#include <Packet.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>

#include <spdlog/spdlog.h>

#include <cmath>
#include <fstream>
#include <random>
#include <regex>
#include <thread>

using namespace std::chrono_literals;

namespace
{
// Actor value ids (ActorValueInfo on the client).
constexpr uint32_t kHealth = 24;
constexpr uint32_t kMagicka = 25;
constexpr uint32_t kStamina = 26;

// Skyrim.esm forms.
constexpr uint32_t kTamriel = 0x3C;
constexpr uint32_t kSlotRight = 0x13F42;
constexpr uint32_t kSlotLeft = 0x13F43;
constexpr uint32_t kSlotBoth = 0x13F45;

constexpr auto kMovementPeriod = 50ms; // 20 Hz, like the client's full snapshots
constexpr float kPi = 3.14159265f;

const char* ReasonName(TiltedPhoques::Client::EDisconnectReason aReason) noexcept
{
    switch (aReason)
    {
    case TiltedPhoques::Client::kTimeout: return "timeout";
    case TiltedPhoques::Client::kLocalProblem: return "local problem";
    case TiltedPhoques::Client::kKicked: return "kicked";
    case TiltedPhoques::Client::kCannotResolve: return "cannot resolve";
    case TiltedPhoques::Client::kAborted: return "closed by us";
    case TiltedPhoques::Client::kNormal: return "normal";
    }
    return "?";
}

Vector3_NetQuantize ToNet(const glm::vec3& acValue) noexcept
{
    Vector3_NetQuantize result;
    result.x = acValue.x;
    result.y = acValue.y;
    result.z = acValue.z;
    return result;
}

Rotator2_NetQuantize ToNet(const glm::vec2& acValue) noexcept
{
    Rotator2_NetQuantize result;
    result.x = acValue.x;
    result.y = acValue.y;
    return result;
}

glm::vec3 FromNet(const Vector3_NetQuantize& acValue) noexcept
{
    return glm::vec3(acValue.x, acValue.y, acValue.z);
}

float Seconds(std::chrono::steady_clock::duration aDuration) noexcept
{
    return std::chrono::duration<float>(aDuration).count();
}

std::string Join(const std::vector<std::string>& acArgs)
{
    std::string result;
    for (const auto& arg : acArgs)
    {
        if (!result.empty())
            result += ' ';
        result += arg;
    }
    return result;
}

bool ParseFloat(const std::string& acText, float& aOut) noexcept
{
    try
    {
        aOut = std::stof(acText);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ParseHex(const std::string& acText, uint32_t& aOut) noexcept
{
    try
    {
        aOut = static_cast<uint32_t>(std::stoul(acText, nullptr, 16));
        return true;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace

Bot::Bot(BotOptions aOptions, std::vector<Command> aScript) noexcept
    : m_options(std::move(aOptions))
    , m_script(std::move(aScript))
{
    m_start = m_options.Start;
}

int Bot::Run() noexcept
{
    m_lastTick = Clock::now();
    m_connectAt = m_lastTick;

    while (m_phase != Phase::Stopped)
    {
        Update(); // network: OnConnected, OnConsume and OnDisconnected fire from here
        Tick();
        std::this_thread::sleep_for(10ms);
    }

    return 0;
}

template <class T> bool Bot::SendMsg(const T& acMessage) noexcept
{
    if (!IsConnected())
        return false;

    TiltedPhoques::Buffer buffer(1 << 16);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    writer.WriteBits(0, 8); // the packet layer uses the first byte

    acMessage.Serialize(writer);
    TiltedPhoques::PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), static_cast<uint32_t>(writer.Size()));

    Client::Send(&packet);
    return true;
}

void Bot::OnConsume(const void* apData, uint32_t aSize)
{
    ServerMessageFactory factory;
    TiltedPhoques::ViewBuffer buf(reinterpret_cast<uint8_t*>(const_cast<void*>(apData)), aSize);
    TiltedPhoques::Buffer::Reader reader(&buf);

    auto pMessage = factory.Extract(reader);
    if (!pMessage)
    {
        spdlog::warn("Could not parse a {} byte packet from the server", aSize);
        return;
    }

    HandleMessage(*pMessage);
}

void Bot::OnConnected()
{
    spdlog::info("Connected to {}; authenticating as '{}', version {}", m_options.Server, m_options.Name, BUILD_COMMIT);
    StringCache::Get().Clear();
    SendAuthentication();
    m_phase = Phase::Authenticating;
    m_phaseStart = Clock::now();
}

void Bot::OnDisconnected(EDisconnectReason aReason)
{
    spdlog::warn("Disconnected ({})", ReasonName(aReason));
    m_serverId = 0;
    m_players.clear();
    m_names.clear();
    m_leftParty = false;

    if (m_phase == Phase::Stopped || m_phase == Phase::Detached)
        return;

    // Like the client: try again in a few seconds, and carry on with the script once back in the world.
    m_phase = Phase::Idle;
    m_connectAt = Clock::now() + 5s;
    spdlog::info("Reconnecting in 5 s");
}

void Bot::Tick() noexcept
{
    const auto now = Clock::now();
    const float dt = Seconds(now - m_lastTick);
    m_lastTick = now;

    switch (m_phase)
    {
    case Phase::Idle:
        if (now >= m_connectAt)
        {
            spdlog::info("Connecting to {}", m_options.Server);
            if (Connect(m_options.Server))
            {
                m_phase = Phase::Connecting;
                m_phaseStart = now;
            }
            else
            {
                spdlog::error("Connect() refused '{}'; retrying in 5 s", m_options.Server);
                m_connectAt = now + 5s;
            }
        }
        break;
    case Phase::Connecting:
        if (now - m_phaseStart > 15s)
        {
            spdlog::error("No connection after 15 s (is the server up, is the address right?)");
            Close();
            m_phase = Phase::Idle;
            m_connectAt = now + 5s;
        }
        break;
    case Phase::Authenticating:
        if (now - m_phaseStart > 10s)
        {
            spdlog::error("No answer to the authentication after 10 s");
            Close();
            m_phase = Phase::Idle;
            m_connectAt = now + 5s;
        }
        break;
    case Phase::Scouting:
        if (Host())
            Assign();
        else if (now - m_phaseStart > 5s)
        {
            spdlog::info("No player character seen around ({:.0f}, {:.0f}) yet; is the host in game there? Asking again", m_start ? m_start->x : 0.f, m_start ? m_start->y : 0.f);
            Scout();
        }
        break;
    case Phase::Assigning:
        if (now - m_phaseStart > 5s)
        {
            spdlog::warn("No answer to the character assignment after 5 s; asking again");
            Assign();
        }
        break;
    case Phase::InWorld:
        if (now - m_lastMovement >= kMovementPeriod)
            SendMovement();
        if (m_healthRestorePending && now >= m_healthRestoreAt)
        {
            m_healthRestorePending = false;
            SendHealth(m_maxHealth);
        }
        RunScript();
        break;
    case Phase::Detached:
        RunScript();
        break;
    case Phase::Stopped:
        break;
    }

    (void)dt;
}

void Bot::SendAuthentication() noexcept
{
    AuthenticationRequest request{};
    request.DiscordId = 0;
    request.SKSEActive = false;
    request.MO2Active = false;
    request.Token = m_options.Password.c_str();
    request.Version = BUILD_COMMIT;
    request.Username = m_options.Name.c_str();
    request.Level = 1;

    Mods::Entry skyrim{};
    skyrim.Filename = "Skyrim.esm";
    skyrim.Id = 0;
    skyrim.IsLite = false;
    request.UserMods.ModList.push_back(skyrim);

    SendMsg(request);
}

void Bot::Scout() noexcept
{
    if (!m_start)
        m_start = GuessStart();
    if (!m_start)
    {
        spdlog::warn("No idea where the host is: pass --x and --y, or --hostlog with the host's tp_client.log. Trying the world origin");
        m_start = glm::vec2(0.f, 0.f);
    }

    const auto grid = GridCellCoords::CalculateGridCellCoords(m_start->x, m_start->y);

    ShiftGridCellRequest request{};
    request.WorldSpaceId = m_worldSpace;
    request.PlayerCell = GameId{};
    request.CenterCoords = grid;
    SendMsg(request);

    spdlog::info("Looking for a player character around ({:.0f}, {:.0f}), grid ({}, {})", m_start->x, m_start->y, grid.X, grid.Y);
    m_phase = Phase::Scouting;
    m_phaseStart = Clock::now();
}

void Bot::Assign() noexcept
{
    const KnownPlayer* pHost = Host();
    if (!pHost)
    {
        m_phase = Phase::Scouting;
        return;
    }

    if (!m_hasBody)
    {
        // Cloned from the host: the receiving game builds this body from data it has already accepted once.
        m_appearance = pHost->AppearanceBuffer;
        m_changeFlags = pHost->ChangeFlags;
        m_faceTints = pHost->FaceTints;
        m_inventory = Inventory{};
        for (const auto& entry : pHost->InventoryContent.Entries)
            if (entry.BaseId.ModId == m_skyrimModId)
                m_inventory.Entries.push_back(entry);

        m_position = pHost->Position + glm::vec3(m_options.Spacing, 0.f, 0.f);
        m_yaw = pHost->Rotation.y;
        m_cell = pHost->CellId;
        m_hasBody = true;

        spdlog::info("Body cloned from player {} '{}': {} bytes of appearance, {} face tints, {} of {} inventory entries kept (Skyrim.esm only)", pHost->PlayerId, pHost->Name, m_appearance.size(),
                     m_faceTints.Entries.size(), m_inventory.Entries.size(), pHost->InventoryContent.Entries.size());
    }

    std::random_device device;
    m_cookie = device();

    AssignCharacterRequest request{};
    request.Cookie = m_cookie;
    request.ReferenceId = GameId(0, 0x14); // "the player": the server keys a player character on exactly this id
    request.FormId = GameId{};
    request.CellId = m_cell;
    request.WorldSpaceId = m_worldSpace;
    request.Position = ToNet(m_position);
    request.Rotation = ToNet(glm::vec2(0.f, m_yaw));
    request.ChangeFlags = m_changeFlags;
    request.AppearanceBuffer = m_appearance;
    request.FaceTints = m_faceTints;
    request.CurrentActorData.InitialInventory = m_inventory;
    request.CurrentActorData.InitialActorValues.ActorValuesList[kHealth] = m_health;
    request.CurrentActorData.InitialActorValues.ActorValuesList[kMagicka] = 100.f;
    request.CurrentActorData.InitialActorValues.ActorValuesList[kStamina] = 100.f;
    request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kHealth] = m_maxHealth;
    request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kMagicka] = 100.f;
    request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kStamina] = 100.f;
    request.CurrentActorData.IsDead = false;
    request.CurrentActorData.IsWeaponDrawn = false;
    SendMsg(request);

    spdlog::info("Asked for a character at ({:.0f}, {:.0f}, {:.0f}), {:.0f} units from player {}", m_position.x, m_position.y, m_position.z, m_options.Spacing, pHost->PlayerId);
    m_phase = Phase::Assigning;
    m_phaseStart = Clock::now();
}

void Bot::SendCellEntry() noexcept
{
    const auto grid = GridCellCoords::CalculateGridCellCoords(m_position.x, m_position.y);

    EnterExteriorCellRequest enter{};
    enter.WorldSpaceId = m_worldSpace;
    enter.CellId = m_cell;
    enter.CurrentCoords = grid;
    SendMsg(enter);

    ShiftGridCellRequest shift{};
    shift.WorldSpaceId = m_worldSpace;
    shift.PlayerCell = m_cell;
    shift.CenterCoords = grid;
    SendMsg(shift);

    spdlog::info("Cell entry sent: grid ({}, {})", grid.X, grid.Y);
}

void Bot::SendMovement() noexcept
{
    if (!m_serverId)
        return;

    ClientReferencesMoveRequest message{};
    message.Tick = GetClock().GetCurrentTick();

    auto& update = message.Updates[m_serverId];
    auto& movement = update.UpdatedMovement;
    movement.Position = ToNet(m_position);
    movement.Rotation = ToNet(glm::vec2(0.f, m_yaw));
    movement.CellId = m_cell;
    movement.WorldSpaceId = m_worldSpace;
    movement.Direction = 0.f;

    SendMsg(message);
    m_lastMovement = Clock::now();
}

void Bot::SendHealth(const float aHealth) noexcept
{
    if (!m_serverId)
        return;

    RequestActorValueChanges request{};
    request.Id = m_serverId;
    request.Values[kHealth] = aHealth;
    SendMsg(request);

    spdlog::info("Health {:.0f} -> {:.0f} sent", m_health, aHealth);
    m_health = aHealth;
}

void Bot::SendEquip(const uint32_t aBaseId, const uint32_t aSlot, const bool aUnequip, const bool aSpell) noexcept
{
    if (!m_serverId)
        return;

    if (!aSpell)
    {
        Inventory::Entry* pEntry = nullptr;
        for (auto& entry : m_inventory.Entries)
            if (entry.BaseId == Skyrim(aBaseId))
                pEntry = &entry;
        if (!pEntry && !aUnequip)
        {
            Inventory::Entry entry{};
            entry.BaseId = Skyrim(aBaseId);
            entry.Count = 1;
            m_inventory.Entries.push_back(entry);
            pEntry = &m_inventory.Entries.back();
        }
        if (pEntry)
        {
            pEntry->ExtraWorn = !aUnequip && aSlot != kSlotLeft;
            pEntry->ExtraWornLeft = !aUnequip && aSlot == kSlotLeft;
        }
    }

    RequestEquipmentChanges request{};
    request.ServerId = m_serverId;
    request.ItemId = Skyrim(aBaseId);
    request.EquipSlotId = Skyrim(aSlot);
    request.Count = 1;
    request.Unequip = aUnequip;
    request.IsSpell = aSpell;
    request.IsShout = false;
    request.IsAmmo = false;
    request.CurrentInventory = m_inventory;
    SendMsg(request);

    spdlog::info("{} {:X} in slot {:X}{} sent", aUnequip ? "Unequip" : "Equip", aBaseId, aSlot, aSpell ? " (spell)" : "");
}

void Bot::HandleMessage(const ServerMessage& acMessage) noexcept
{
    const auto opcode = acMessage.GetOpcode();

    if (opcode == AuthenticationResponse::Opcode)
    {
        const auto& message = static_cast<const AuthenticationResponse&>(acMessage);
        using RT = AuthenticationResponse::ResponseType;
        if (message.Type != RT::kAccepted)
        {
            const char* why = message.Type == RT::kWrongVersion         ? "wrong version"
                              : message.Type == RT::kModsMismatch       ? "mods mismatch"
                              : message.Type == RT::kClientModsDisallowed ? "client mods disallowed"
                              : message.Type == RT::kWrongPassword      ? "wrong password"
                              : message.Type == RT::kServerFull         ? "server full"
                                                                        : "?";
            spdlog::error("Refused: {}. Server version '{}', ours '{}'", why, message.Version.c_str(), BUILD_COMMIT);
            m_phase = Phase::Stopped;
            Close();
            return;
        }

        m_playerId = message.PlayerId;
        m_serverVersion = message.Version.c_str();
        bool skyrimFound = false;
        for (const auto& mod : message.UserMods.ModList)
        {
            if (mod.Filename == "Skyrim.esm")
            {
                m_skyrimModId = mod.Id;
                skyrimFound = true;
            }
        }
        if (!skyrimFound)
            spdlog::warn("The server did not echo Skyrim.esm in the mod list; using mod id 0");
        m_worldSpace = Skyrim(kTamriel);

        spdlog::info("Accepted as player {} on server {} (Skyrim.esm is mod {}; PvP {}, death system {})", m_playerId, m_serverVersion, m_skyrimModId, message.Settings.PvpEnabled ? "on" : "off",
                     message.Settings.DeathSystemEnabled ? "on" : "off");
        Scout();
        return;
    }

    if (opcode == StringCacheUpdate::Opcode)
    {
        StringCache::Get().Deserialize(static_cast<const StringCacheUpdate&>(acMessage));
        return;
    }

    if (opcode == CharacterSpawnRequest::Opcode)
    {
        const auto& message = static_cast<const CharacterSpawnRequest&>(acMessage);
        if (!message.IsPlayer || message.PlayerId == m_playerId)
            return; // NPCs are the host's business; our own character never comes back to us

        KnownPlayer* pPlayer = FindPlayerCharacter(message.ServerId);
        if (!pPlayer)
        {
            m_players.emplace_back();
            pPlayer = &m_players.back();
        }
        pPlayer->ServerId = message.ServerId;
        pPlayer->PlayerId = message.PlayerId;
        pPlayer->Position = FromNet(message.Position);
        pPlayer->Rotation = glm::vec2(message.Rotation.x, message.Rotation.y);
        pPlayer->CellId = message.CellId;
        pPlayer->WorldSpaceId = m_worldSpace;
        pPlayer->AppearanceBuffer = message.AppearanceBuffer;
        pPlayer->ChangeFlags = message.ChangeFlags;
        pPlayer->FaceTints = message.FaceTints;
        pPlayer->InventoryContent = message.InventoryContent;
        const auto health = message.InitialActorValues.ActorValuesList.find(kHealth);
        pPlayer->Health = health != message.InitialActorValues.ActorValuesList.end() ? health->second : 0.f;
        for (const auto& [id, name] : m_names)
            if (id == message.PlayerId)
                pPlayer->Name = name;

        spdlog::info("Player character {:X} of player {} '{}' at ({:.0f}, {:.0f}, {:.0f}), health {:.0f}{}", message.ServerId, message.PlayerId, pPlayer->Name, pPlayer->Position.x, pPlayer->Position.y,
                     pPlayer->Position.z, pPlayer->Health, message.IsDead ? ", dead" : "");
        return;
    }

    if (opcode == AssignCharacterResponse::Opcode)
    {
        const auto& message = static_cast<const AssignCharacterResponse&>(acMessage);
        if (message.Cookie != m_cookie)
            return;

        m_serverId = message.ServerId;
        m_phase = Phase::InWorld;
        m_lastMovement = {};
        spdlog::info("In the world: our character is {:X} (owner {}), player id {}", m_serverId, message.Owner ? "yes" : "no", message.PlayerId);
        SendCellEntry();
        return;
    }

    if (opcode == ServerReferencesMoveRequest::Opcode)
    {
        const auto& message = static_cast<const ServerReferencesMoveRequest&>(acMessage);
        for (const auto& entry : message.Updates)
        {
            if (KnownPlayer* pPlayer = FindPlayerCharacter(entry.first))
            {
                pPlayer->Position = FromNet(entry.second.UpdatedMovement.Position);
                pPlayer->Rotation = glm::vec2(entry.second.UpdatedMovement.Rotation.x, entry.second.UpdatedMovement.Rotation.y);
            }
        }
        return;
    }

    if (opcode == NotifyRemoveCharacter::Opcode)
    {
        const auto& message = static_cast<const NotifyRemoveCharacter&>(acMessage);
        for (auto it = m_players.begin(); it != m_players.end();)
        {
            if (it->ServerId == message.ServerId)
            {
                spdlog::info("Server removed player character {:X} of player {} '{}' from our view", it->ServerId, it->PlayerId, it->Name);
                it = m_players.erase(it);
            }
            else
                ++it;
        }
        return;
    }

    if (opcode == NotifyOwnershipTransfer::Opcode)
    {
        const auto& message = static_cast<const NotifyOwnershipTransfer&>(acMessage);
        spdlog::info("Server handed us actor {:X}; handing it back (a bot never owns anything)", message.ServerId);
        RequestOwnershipTransfer request{};
        request.ServerId = message.ServerId;
        SendMsg(request);
        return;
    }

    // The server puts the first player in a party as its leader, and a leader's game claims every actor. If the bot
    // was first, it leaves so the party dissolves and the host founds the next one; as a plain member it stays.
    if (opcode == NotifyPartyJoined::Opcode || opcode == NotifyPartyInfo::Opcode)
    {
        const bool leader = opcode == NotifyPartyJoined::Opcode ? static_cast<const NotifyPartyJoined&>(acMessage).IsLeader : static_cast<const NotifyPartyInfo&>(acMessage).IsLeader;
        const uint32_t leaderId = opcode == NotifyPartyJoined::Opcode ? static_cast<const NotifyPartyJoined&>(acMessage).LeaderPlayerId : static_cast<const NotifyPartyInfo&>(acMessage).LeaderPlayerId;
        if (leader && !m_leftParty)
        {
            spdlog::info("The server made us party leader (we joined first); leaving the party so the host leads");
            m_leftParty = true;
            SendMsg(PartyLeaveRequest{});
        }
        else if (!leader)
            spdlog::info("In the party of player {} as a member", leaderId);
        return;
    }

    if (opcode == NotifyRelinquishControl::Opcode)
    {
        spdlog::debug("Relinquish control of {:X}", static_cast<const NotifyRelinquishControl&>(acMessage).ServerId);
        return;
    }

    if (opcode == NotifyPlayerJoined::Opcode)
    {
        const auto& message = static_cast<const NotifyPlayerJoined&>(acMessage);
        m_names.emplace_back(message.PlayerId, std::string(message.Username.c_str()));
        for (auto& player : m_players)
            if (player.PlayerId == message.PlayerId)
                player.Name = message.Username.c_str();
        spdlog::info("Player {} '{}' joined (level {})", message.PlayerId, message.Username.c_str(), message.Level);
        return;
    }

    if (opcode == NotifyPlayerLeft::Opcode)
    {
        const auto& message = static_cast<const NotifyPlayerLeft&>(acMessage);
        spdlog::info("Player {} '{}' left", message.PlayerId, message.Username.c_str());
        for (auto it = m_players.begin(); it != m_players.end();)
            it = it->PlayerId == message.PlayerId ? m_players.erase(it) : std::next(it);
        return;
    }

    if (opcode == NotifyRespawn::Opcode)
    {
        const auto& message = static_cast<const NotifyRespawn&>(acMessage);
        spdlog::info("Character {:X} respawned; asking for its fresh spawn like a client would", message.ActorId);
        RequestRespawn request{};
        request.ActorId = message.ActorId;
        SendMsg(request);
        return;
    }

    if (opcode == NotifyActorValueChanges::Opcode)
    {
        const auto& message = static_cast<const NotifyActorValueChanges&>(acMessage);
        if (KnownPlayer* pPlayer = FindPlayerCharacter(message.Id))
        {
            const auto health = message.Values.find(kHealth);
            if (health != message.Values.end() && std::abs(health->second - pPlayer->Health) >= 1.f)
            {
                spdlog::info("Player {} '{}' health {:.0f} -> {:.0f}", pPlayer->PlayerId, pPlayer->Name, pPlayer->Health, health->second);
                pPlayer->Health = health->second;
            }
        }
        return;
    }
}

bool Bot::RunScript() noexcept
{
    if (m_pc >= m_script.size())
    {
        if (m_phase != Phase::Stopped)
        {
            spdlog::info("Script finished; disconnecting");
            m_phase = Phase::Stopped;
            Close();
        }
        return false;
    }

    const Command& command = m_script[m_pc];

    // Only a few commands make sense without a body.
    const bool detachedOk = command.Name == "wait" || command.Name == "reconnect" || command.Name == "log" || command.Name == "stop";
    if (m_phase != Phase::InWorld && !detachedOk)
        return false;

    if (command.Name == "loop")
    {
        m_pc = 0;
        m_commandFresh = true;
        return true;
    }

    if (m_commandFresh)
        m_commandStart = Clock::now();

    const bool done = StepCommand(command, m_commandFresh);
    m_commandFresh = false;
    if (done)
    {
        ++m_pc;
        m_commandFresh = true;
    }
    return done;
}

bool Bot::StepCommand(const Command& acCommand, const bool aFirstTick) noexcept
{
    const auto now = Clock::now();
    const float dt = Seconds(now - m_lastTick) + 0.01f; // a tick is ~10 ms
    const auto& args = acCommand.Args;
    const auto& name = acCommand.Name;

    auto arg = [&](const size_t aIndex, const float aDefault)
    {
        float value = aDefault;
        if (aIndex < args.size() && !ParseFloat(args[aIndex], value))
            spdlog::warn("Line {}: '{}' is not a number; using {}", acCommand.Line, args[aIndex], aDefault);
        return value;
    };

    if (name == "log")
    {
        spdlog::info("[script] {}", Join(args));
        return true;
    }

    if (name == "wait")
        return Seconds(now - m_commandStart) >= arg(0, 1.f);

    if (name == "walk")
    {
        if (aFirstTick)
        {
            if (!args.empty() && args[0] == "rel")
            {
                m_walkTarget = m_position + glm::vec3(arg(1, 0.f), arg(2, 0.f), 0.f);
                m_walkSpeed = arg(3, 150.f);
            }
            else
            {
                m_walkTarget = glm::vec3(arg(0, m_position.x), arg(1, m_position.y), m_position.z);
                m_walkSpeed = arg(2, 150.f);
            }
            spdlog::info("Walking to ({:.0f}, {:.0f}) at {:.0f} units/s", m_walkTarget.x, m_walkTarget.y, m_walkSpeed);
        }
        return MoveTowards(m_walkTarget, m_walkSpeed, dt);
    }

    if (name == "follow")
    {
        const float distance = arg(0, 250.f);
        const float seconds = arg(1, 30.f);
        if (const KnownPlayer* pHost = Host())
        {
            glm::vec3 away = m_position - pHost->Position;
            away.z = 0.f;
            const float length = std::sqrt(away.x * away.x + away.y * away.y);
            away = length > 1.f ? away / length : glm::vec3(1.f, 0.f, 0.f);
            MoveTowards(pHost->Position + away * distance, 300.f, dt);
        }
        return Seconds(now - m_commandStart) >= seconds;
    }

    if (name == "equip" || name == "unequip")
    {
        uint32_t baseId = 0;
        if (args.empty() || !ParseHex(args[0], baseId))
        {
            spdlog::warn("Line {}: {} needs a hex form id", acCommand.Line, name);
            return true;
        }
        uint32_t slot = kSlotRight;
        bool spell = false;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (args[i] == "left")
                slot = kSlotLeft;
            else if (args[i] == "both")
                slot = kSlotBoth;
            else if (args[i] == "spell")
                spell = true;
        }
        SendEquip(baseId, slot, name == "unequip", spell);
        return true;
    }

    if (name == "health")
    {
        SendHealth(arg(0, m_maxHealth));
        return true;
    }

    if (name == "damage")
    {
        SendHealth(m_health - arg(0, 10.f));
        return true;
    }

    if (name == "die")
    {
        SendHealth(-4.f);
        return true;
    }

    if (name == "respawn")
    {
        // The client sends the respawn first and its restored health on its next value tick, so the server's stored
        // health is still the death value when the others rebuild the copy. Reproduced on purpose.
        SendMsg(PlayerRespawnRequest{});
        m_healthRestoreAt = now + 500ms;
        m_healthRestorePending = true;
        spdlog::info("Respawn sent; health back to {:.0f} in 500 ms", m_maxHealth);
        return true;
    }

    if (name == "disconnect")
    {
        spdlog::info("[script] disconnecting on purpose");
        m_phase = Phase::Detached;
        Close();
        return true;
    }

    if (name == "reconnect")
    {
        if (m_phase == Phase::Detached)
        {
            ++m_reconnects;
            spdlog::info("[script] reconnecting (number {})", m_reconnects);
            m_phase = Phase::Idle;
            m_connectAt = now;
        }
        return true;
    }

    if (name == "stop")
    {
        spdlog::info("[script] stop");
        m_phase = Phase::Stopped;
        Close();
        return true;
    }

    spdlog::warn("Line {}: unknown command '{}'", acCommand.Line, name);
    return true;
}

bool Bot::MoveTowards(const glm::vec3& acTarget, const float aSpeed, const float aDt) noexcept
{
    const auto gridBefore = GridCellCoords::CalculateGridCellCoords(m_position.x, m_position.y);

    glm::vec3 delta = acTarget - m_position;
    delta.z = 0.f;
    const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    bool arrived = false;

    if (distance <= 1.f)
        arrived = true;
    else
    {
        const float step = aSpeed * aDt;
        if (step >= distance)
        {
            m_position.x = acTarget.x;
            m_position.y = acTarget.y;
            arrived = true;
        }
        else
        {
            m_position += delta / distance * step;
        }
        m_yaw = std::atan2(delta.x, delta.y); // Skyrim's z rotation: 0 faces +Y, clockwise positive
        if (m_yaw < 0.f)
            m_yaw += 2.f * kPi;
    }

    // Ground height is unknown to a headless player; stay at the host's height.
    if (const KnownPlayer* pHost = Host())
        m_position.z = pHost->Position.z;

    const auto gridAfter = GridCellCoords::CalculateGridCellCoords(m_position.x, m_position.y);
    if (gridAfter != gridBefore)
        SendCellEntry();

    return arrived;
}

const KnownPlayer* Bot::Host() const noexcept
{
    // The host connected first, so the lowest player id is the best guess.
    const KnownPlayer* pBest = nullptr;
    for (const auto& player : m_players)
        if (!pBest || player.PlayerId < pBest->PlayerId)
            pBest = &player;
    return pBest;
}

KnownPlayer* Bot::FindPlayerCharacter(const uint32_t aServerId) noexcept
{
    for (auto& player : m_players)
        if (player.ServerId == aServerId)
            return &player;
    return nullptr;
}

std::optional<glm::vec2> Bot::GuessStart() const noexcept
{
    if (m_options.HostLog.empty())
        return std::nullopt;

    std::ifstream file(m_options.HostLog);
    if (!file)
    {
        spdlog::warn("Could not read the host log '{}'", m_options.HostLog);
        return std::nullopt;
    }

    // "standing in (5, -2) at (22516, -7587)" from Grid change lines, "player at (20509, -6091)" from InterpDiag.
    static const std::regex gridLine(R"(standing in \(-?\d+, -?\d+\) at \((-?\d+), (-?\d+)\))");
    static const std::regex playerLine(R"(player at \((-?\d+), (-?\d+)\))");

    std::optional<glm::vec2> last;
    std::string line;
    std::smatch match;
    while (std::getline(file, line))
    {
        if (std::regex_search(line, match, gridLine) || std::regex_search(line, match, playerLine))
            last = glm::vec2(std::stof(match[1].str()), std::stof(match[2].str()));
    }

    if (last)
        spdlog::info("Host last seen at ({:.0f}, {:.0f}) according to '{}'", last->x, last->y, m_options.HostLog);
    else
        spdlog::warn("No position line found in '{}'", m_options.HostLog);
    return last;
}
