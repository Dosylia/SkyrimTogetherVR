#include "Bot.h"

#include <Messages/AssignCharacterRequest.h>
#include <Messages/AuthenticationRequest.h>
#include <Messages/AuthenticationResponse.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/Message.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyDroppedItem.h>
#include <Messages/NotifyDroppedItemRemoved.h>
#include <Messages/NotifyDroppedItemMove.h>
#include <Messages/RequestDroppedItemMove.h>
#include <Messages/RequestDroppedItemAdd.h>
#include <Messages/RequestDroppedItemRemove.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/PartyLeaveRequest.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/PlayerRespawnRequest.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
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
// "no id", for a name a script used that does not resolve to anybody yet. Zero cannot serve: it is a real server
// id, and the bot that connects first to a fresh server is given exactly that one. Using zero as the sentinel made
// every self-directed action of that bot silently do nothing -- a hit that was never sent, health never recorded --
// while its script reported success for having asked.
constexpr uint32_t kNoId = 0xFFFFFFFFu;
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

    // Its own reference point: m_lastTick moves every iteration, so measuring against that compares ten
    // milliseconds to the deadline and never trips.
    const auto runStart = Clock::now();

    bool ranOutOfTime = false;
    while (m_phase != Phase::Stopped)
    {
        Update(); // network: OnConnected, OnConsume and OnDisconnected fire from here
        Tick();

        if (m_options.MaxRuntime > 0.f && Seconds(Clock::now() - runStart) > m_options.MaxRuntime)
        {
            ranOutOfTime = true;
            spdlog::error("[script] out of time after {:.0f}s in phase {}; the script did not finish", m_options.MaxRuntime, static_cast<int>(m_phase));
            m_failures.push_back(fmt::format("run did not finish within {:.0f}s", m_options.MaxRuntime));
            break;
        }

        std::this_thread::sleep_for(10ms);
    }
    (void)ranOutOfTime;

    // The exit code is the verdict, so a run can be checked by a script instead of read by a person.
    //
    // A run that never reached the world is a failure even with nothing to report: on 2026-09-25 a version
    // mismatch refused the bot at the door, no check ever ran, and the suite reported a clean pass four times in
    // a row. Silence is not success.
    if (m_checksPassed == 0 && m_failures.empty())
    {
        spdlog::error("[script] the script ran no checks; the bot never got far enough to test anything");
        return 2;
    }

    if (!m_failures.empty())
    {
        spdlog::error("[script] {} of {} checks failed", m_failures.size(), m_failures.size() + m_checksPassed);
        for (const auto& failure : m_failures)
            spdlog::error("  {}", failure);
        return 1;
    }

    if (m_checksPassed)
        spdlog::info("[script] all {} checks passed", m_checksPassed);

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
    m_ownershipEpoch = 0;
    m_hasCharacter = false;
    m_gridReported = false;
    // Everything known about the world came from a connection that no longer exists. Keeping it means a check after
    // a reconnect can be satisfied by a value the server never resent, which is the opposite of what these tests are
    // for: after reconnecting, anything the bot knows must have arrived again.
    m_actors.clear();
    m_players.clear();
    m_names.clear();
    m_drops.clear();
    m_dropPlaces.clear();
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
        else if (m_options.Standalone && Seconds(now - m_connectAt) >= m_options.HostTimeout)
        {
            UseEmptyBody();
            Assign();
        }
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
        // A real client tells the server every time it crosses into a new grid square, and the server decides who
        // is in range of whom from that. The bot said it once, on arriving, and then never again -- so a bot that
        // walked ten cells was still, as far as the server was concerned, standing where it started. Nothing that
        // depends on range could be tested at all, which is most of what happens when players separate.
        {
            const auto grid = GridCellCoords::CalculateGridCellCoords(m_position.x, m_position.y);
            if (!m_gridReported || grid.X != m_reportedGridX || grid.Y != m_reportedGridY)
            {
                if (m_gridReported)
                    spdlog::info("Grid change: ({}, {}) -> ({}, {})", m_reportedGridX, m_reportedGridY, grid.X, grid.Y);
                m_reportedGridX = grid.X;
                m_reportedGridY = grid.Y;
                m_gridReported = true;
                if (m_standaloneCell)
                    m_cell = GameId{};  // recomputed for the new square by SendCellEntry
                SendCellEntry();
            }
        }
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

void Bot::UseEmptyBody() noexcept
{
    if (m_hasBody)
        return;

    m_appearance = TiltedPhoques::String{};
    m_changeFlags = 0;
    m_faceTints = Tints{};
    m_inventory = Inventory{};
    m_cell = GameId{};
    m_position = glm::vec3(m_start ? m_start->x : 0.f, m_start ? m_start->y : 0.f, 0.f);
    m_yaw = 0.f;
    m_hasBody = true;

    spdlog::info("No host found in {:.0f}s; going in standalone with an empty body at ({:.0f}, {:.0f}). Nothing will render this bot; it is here to exercise the protocol.", m_options.HostTimeout,
                 m_position.x, m_position.y);
}

void Bot::Assign() noexcept
{
    const KnownPlayer* pHost = Host();
    if (!pHost && !m_hasBody)
    {
        m_phase = Phase::Scouting;
        return;
    }

    if (pHost && !m_hasBody)
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

    if (pHost)
        spdlog::info("Asked for a character at ({:.0f}, {:.0f}, {:.0f}), {:.0f} units from player {}", m_position.x, m_position.y, m_position.z, m_options.Spacing, pHost->PlayerId);
    else
        spdlog::info("Asked for a character at ({:.0f}, {:.0f}, {:.0f}), standalone", m_position.x, m_position.y, m_position.z);
    m_phase = Phase::Assigning;
    m_phaseStart = Clock::now();
}

void Bot::SendCellEntry() noexcept
{
    const auto grid = GridCellCoords::CalculateGridCellCoords(m_position.x, m_position.y);

    // A standalone bot has no real cell, and an empty one is indistinguishable from "no cell at all": the server's
    // CellIdComponent is truthy only when its Cell is set, and HandleExteriorCellEnter raises the cell-change
    // event -- the one that re-sends a character to everyone still in range of it -- only for a player that
    // already had a cell. So a bot with an empty cell could walk the length of Solstheim and the server would
    // never tell anybody. A synthetic id per grid square fixes that; nothing reads it except interior comparisons,
    // and a bot standing in a worldspace is never in an interior.
    if (m_cell == GameId{})
    {
        m_standaloneCell = true;
        m_cell = GameId{0, 0x100000u | ((static_cast<uint32_t>(grid.X) & 0xFFFu) << 12) | (static_cast<uint32_t>(grid.Y) & 0xFFFu)};
    }

    EnterExteriorCellRequest enter{};
    enter.WorldSpaceId = m_worldSpace;
    enter.CellId = m_cell;
    enter.CurrentCoords = grid;
    SendMsg(enter);

    // A real client sends these two on different triggers: the exterior-enter when the grid square it stands in
    // changes, the grid shift when the *centre* of the loaded block moves. Walking into the next cell along
    // sends the first alone, which is a case this bot could not produce while it always sent both.
    if (m_sendGridShift)
    {
        ShiftGridCellRequest shift{};
        shift.WorldSpaceId = m_worldSpace;
        shift.PlayerCell = m_cell;
        shift.CenterCoords = grid;
        SendMsg(shift);
    }

    spdlog::info("Cell entry sent: grid ({}, {}){}", grid.X, grid.Y, m_sendGridShift ? "" : " (exterior enter only, no grid shift)");
}

// Follow the host through a load door.
//
// A headless bot cannot walk through a door -- there is no door, only coordinates -- so when Emma went into
// Windhelm on 2026-09-26 the bot stayed in the exterior cell and the server quite correctly stopped sending it
// to her. That made the one bug worth testing, the copy that vanishes at a cell change, untestable without a
// second real player.
//
// It does not need a door. The server already broadcasts NotifyPlayerCellChanged to everyone else, so the bot
// can be told where the host went and simply say it is there too: announce the same cell and put itself at the
// host's coordinates. That is a teleport, which is exactly what a load door is from the network's point of view.
void Bot::FollowHostIntoCell(const GameId& acCell, const GameId& acWorldSpace) noexcept
{
    if (!m_hasCharacter)
        return;

    const KnownPlayer* pHost = Host();
    if (!pHost)
        return;

    m_cell = acCell;
    m_worldSpace = acWorldSpace;
    m_standaloneCell = false;

    // Beside the host rather than inside them, and at their height: an interior has no terrain to stand on and
    // the exterior z the bot was carrying would put it through the floor or in the sky.
    m_position = pHost->Position + glm::vec3(m_options.Spacing, 0.f, 0.f);
    // Whatever it was walking towards is in the cell it just left.
    m_walkTarget = m_position;

    if (acWorldSpace == GameId{})
    {
        EnterInteriorCellRequest enter{};
        enter.CellId = acCell;
        SendMsg(enter);
        spdlog::info("Host went into interior cell {:X}:{:X}; following to ({:.0f}, {:.0f}, {:.0f})", acCell.ModId, acCell.BaseId, m_position.x, m_position.y, m_position.z);
    }
    else
    {
        const auto grid = GridCellCoords::CalculateGridCellCoords(m_position.x, m_position.y);

        EnterExteriorCellRequest enter{};
        enter.WorldSpaceId = acWorldSpace;
        enter.CellId = acCell;
        enter.CurrentCoords = grid;
        SendMsg(enter);

        ShiftGridCellRequest shift{};
        shift.WorldSpaceId = acWorldSpace;
        shift.PlayerCell = acCell;
        shift.CenterCoords = grid;
        SendMsg(shift);

        spdlog::info("Host went out into worldspace {:X}:{:X} cell {:X}; following to grid ({}, {})", acWorldSpace.ModId, acWorldSpace.BaseId, acCell.BaseId, grid.X, grid.Y);
    }

    // The move has to go out too, or the server keeps the old position and the copy the host sees stays where
    // the bot used to be -- outdoors, while she is inside.
    SendMovement();
}

void Bot::SendMovement() noexcept
{
    // Whether there is a character, not whether its id is non-zero: entity 0 is a real id, the first character a fresh
    // server makes. Testing the id kept that bot from ever sending a movement, so the server never learnt which cell
    // its character stood in -- the dropmove pair of 2026-09-30 walked into a room with it and was never given it.
    if (!m_hasCharacter)
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
    if (!m_hasCharacter)
        return;

    // The server relays a health change to everyone except whoever sent it, so nothing would ever teach this bot
    // its own health. Recording it here is what makes "expect health me <= 100" mean anything.
    {
        KnownActor& self = Actor(m_serverId);
        self.Health = aHealth;
        self.HealthKnown = true;
        self.IsPlayer = true;
        self.LastChange = Clock::now();
    }

    RequestActorValueChanges request{};
    request.Id = m_serverId;
    request.OwnershipEpoch = m_ownershipEpoch;
    request.Values[kHealth] = aHealth;
    SendMsg(request);

    spdlog::info("Health {:.0f} -> {:.0f} sent", m_health, aHealth);
    m_health = aHealth;
}

void Bot::SendDeath(const bool aDead) noexcept
{
    if (!m_hasCharacter)
        return;

    Actor(m_serverId).Dead = aDead;

    RequestDeathStateChange request{};
    request.Id = m_serverId;
    request.OwnershipEpoch = m_ownershipEpoch;
    request.IsDead = aDead;
    SendMsg(request);

    spdlog::info("Death state {} sent", aDead ? "dead" : "alive");
}

void Bot::SendHit(const uint32_t aTargetId, const float aDelta) noexcept
{
    if (!m_hasCharacter || aTargetId == kNoId)
        return;

    // This is the PvP path, and it is not the same message as a health change of our own: no ownership is claimed
    // and none is checked, because the whole point is that somebody else's character is being hurt. Damage is a
    // negative delta -- Actor::DamageActor raises HealthChangeEvent(formId, -realDamage) -- and the server adds it.
    RequestHealthChangeBroadcast request{};
    request.Id = aTargetId;
    request.DeltaHealth = aDelta;
    SendMsg(request);

    spdlog::info("Hit {:X} for {:.1f}", aTargetId, aDelta);
}

void Bot::SendEquip(const uint32_t aBaseId, const uint32_t aSlot, const bool aUnequip, const bool aSpell) noexcept
{
    if (!m_hasCharacter) // entity 0 is a real id; see SendMovement
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
    // Same trap as the health changes: the server checks the epoch on this message too and drops it silently when
    // it does not match, so every equipment change this bot ever sent was discarded before anyone could see it.
    request.OwnershipEpoch = m_ownershipEpoch;
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
        if (m_options.WorldSpaceFormId.has_value())
        {
            // The low 24 bits are the form id inside its plugin; the top byte is this machine's load order index,
            // which means nothing to the server, so the plugin is looked up by name instead.
            const uint32_t baseId = *m_options.WorldSpaceFormId & 0x00FFFFFF;
            uint32_t modId = m_skyrimModId;
            bool pluginFound = false;
            for (const auto& mod : message.UserMods.ModList)
            {
                if (mod.Filename == m_options.WorldSpacePlugin.c_str())
                {
                    modId = mod.Id;
                    pluginFound = true;
                }
            }
            if (!pluginFound)
                spdlog::warn("The server does not have '{}' loaded; looking in Skyrim.esm instead", m_options.WorldSpacePlugin);
            m_worldSpace = GameId(modId, baseId);
            spdlog::info("Looking in worldspace {:X} of '{}' (server mod {})", baseId, m_options.WorldSpacePlugin, modId);
        }
        else
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
        if (message.IsPlayer && message.PlayerId == m_playerId)
            return; // our own character never comes back to us

        // An NPC: kept as a copy with the health and death state the server built it from, which is all a test of an
        // NPC needs -- the revive pair asks whether one brought back to life arrives alive and at its health. Nothing
        // else about it is followed; NPCs are otherwise the host's business.
        if (!message.IsPlayer)
        {
            const auto health = message.InitialActorValues.ActorValuesList.find(kHealth);
            KnownActor& actor = Actor(message.ServerId);
            actor.IsPlayer = false;
            actor.Health = health != message.InitialActorValues.ActorValuesList.end() ? health->second : 0.f;
            actor.HealthKnown = health != message.InitialActorValues.ActorValuesList.end();
            actor.Dead = message.IsDead;
            actor.LastChange = Clock::now();
            spdlog::info("NPC copy {:X} ({:X}), health {:.0f}{}", message.ServerId, message.FormId.BaseId, actor.Health, message.IsDead ? ", dead" : "");
            Record(Collect::Spawn, fmt::format("npc spawn {:X} health {:.1f}{}", message.ServerId, actor.Health, message.IsDead ? " dead" : ""));
            return;
        }

        KnownPlayer* pPlayer = FindPlayerCharacter(message.ServerId);
        if (!pPlayer)
        {
            m_players.emplace_back();
            pPlayer = &m_players.back();
        }
        ++pPlayer->Spawns;
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

        // A spawn carries the health and death state the server holds for that character, and it is the only place
        // a bot joining late ever learns them. Recording it here is what lets a test ask whether a character was
        // rebuilt correctly -- the respawn bug of 2026-09-25 was a spawn arriving at the health the player died on,
        // and no check could see it while spawns only ever touched the player list.
        {
            KnownActor& actor = Actor(message.ServerId);
            actor.IsPlayer = true;
            actor.Health = pPlayer->Health;
            actor.HealthKnown = health != message.InitialActorValues.ActorValuesList.end();
            actor.Dead = message.IsDead;
            actor.Name = pPlayer->Name;
            actor.LastChange = Clock::now();
        }

        spdlog::info("Player character {:X} of player {} '{}' at ({:.0f}, {:.0f}, {:.0f}), health {:.0f}{}", message.ServerId, message.PlayerId, pPlayer->Name, pPlayer->Position.x, pPlayer->Position.y,
                     pPlayer->Position.z, pPlayer->Health, message.IsDead ? ", dead" : "");
        Record(Collect::Spawn, fmt::format("spawn {:X} health {:.1f}{}", message.ServerId, pPlayer->Health, message.IsDead ? " dead" : ""));
        return;
    }

    if (opcode == NotifyPlayerCellChanged::Opcode)
    {
        const auto& message = static_cast<const NotifyPlayerCellChanged&>(acMessage);
        for (auto& player : m_players)
        {
            if (player.PlayerId != message.PlayerId)
                continue;
            player.CellId = message.CellId;
            player.WorldSpaceId = message.WorldSpaceId;
            break;
        }

        const KnownPlayer* pHost = Host();
        if (pHost && pHost->PlayerId == message.PlayerId && !m_options.Standalone)
            FollowHostIntoCell(message.CellId, message.WorldSpaceId);
        return;
    }

    if (opcode == AssignCharacterResponse::Opcode)
    {
        const auto& message = static_cast<const AssignCharacterResponse&>(acMessage);

        // An NPC this bot registered, rather than its own character. Recorded as an ordinary actor it owns, so
        // the ownership-churn pair has something that is not a player to hand back and forth: a player character
        // cannot be claimed at all (CharacterService::CanClaimOwnership refuses IsPlayer and IsMount).
        if (m_npcCookie && message.Cookie == m_npcCookie)
        {
            KnownActor& npc = Actor(message.ServerId);
            npc.IsPlayer = false;
            npc.OwnedByUs = message.Owner;
            npc.OwnershipEpoch = message.OwnershipEpoch;
            npc.LastChange = Clock::now();
            spdlog::info("NPC registered as actor {:X} (owner {}) at epoch {}", message.ServerId, message.Owner ? "yes" : "no", message.OwnershipEpoch);
            Record(Collect::Ownership, fmt::format("npc {:X} owner {} epoch {}", message.ServerId, message.Owner ? 1 : 0, message.OwnershipEpoch));
            return;
        }

        if (message.Cookie != m_cookie)
            return;

        m_serverId = message.ServerId;
        m_ownershipEpoch = message.OwnershipEpoch;
        m_hasCharacter = true;
        m_phase = Phase::InWorld;
        m_lastMovement = {};

        // Record our own character the moment we have one. Nothing else will: the server tells everyone about a
        // character except the player it belongs to, so without this the bot knows about every actor in range
        // apart from itself, and a check like "known me" reads as though it never spawned.
        {
            KnownActor& self = Actor(m_serverId);
            self.IsPlayer = true;
            self.OwnedByUs = message.Owner;
            self.Health = m_health;
            self.HealthKnown = true;
            self.Name = m_options.Name;
            self.LastChange = Clock::now();
        }
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

    if (opcode == NotifyActorValueChanges::Opcode)
    {
        // Health reaches other clients two ways: as a delta (NotifyHealthChangeBroadcast, what a hit sends) and
        // as a snapshot (this, what the owner's value tick sends). The bot only ever read the first, so a
        // two-sided test watched a player take damage and die and concluded nothing had crossed at all -- the
        // messages were arriving and being dropped on the floor. Nothing was wrong with the relay.
        const auto& message = static_cast<const NotifyActorValueChanges&>(acMessage);
        for (const auto& [key, value] : message.Values)
        {
            if (key != 24) // health
                continue;
            KnownActor* pActor = FindActor(message.Id);
            if (!pActor)
                continue; // a value for a character the server never gave us
            KnownActor& actor = *pActor;
            actor.Health = value;
            actor.HealthKnown = true;
            actor.Dead = value <= 0.f;
            actor.LastChange = Clock::now();
            Record(Collect::Health, fmt::format("health {:X} snapshot {:.1f}", message.Id, value));
        }
        return;
    }

    if (opcode == NotifyHealthChangeBroadcast::Opcode)
    {
        const auto& message = static_cast<const NotifyHealthChangeBroadcast&>(acMessage);
        KnownActor& actor = Actor(message.Id);
        actor.Health += message.DeltaHealth;
        actor.HealthKnown = true;
        actor.LastChange = Clock::now();
        Record(Collect::Health, fmt::format("health {:X} delta {:+.1f} -> {:.1f} (from player {})", message.Id, message.DeltaHealth, actor.Health, message.AttackerPlayerId));
        return;
    }

    if (opcode == NotifyDeathStateChange::Opcode)
    {
        const auto& message = static_cast<const NotifyDeathStateChange&>(acMessage);
        KnownActor* pActor = FindActor(message.Id);
        if (!pActor)
            return; // a death for somebody we were never given; the game's clients ignore these too
        KnownActor& actor = *pActor;
        actor.Dead = message.IsDead;
        actor.LastChange = Clock::now();
        Record(Collect::Death, fmt::format("death {:X} dead={}", message.Id, message.IsDead));
        return;
    }

    // Equipment travels as its own message and **in range only**, which is why a character that drew a sword
    // while this client was away comes back holding the wrong thing. The bot could send equipment changes and
    // never hear one, so nothing could be asserted about whether they arrive at all.
    if (opcode == NotifyEquipmentChanges::Opcode)
    {
        const auto& message = static_cast<const NotifyEquipmentChanges&>(acMessage);
        KnownActor* pActor = FindActor(message.ServerId);
        if (!pActor)
            return; // equipment for a character the server never gave us
        KnownActor& actor = *pActor;
        const uint32_t baseId = message.ItemId.BaseId;

        auto& equipped = actor.Equipped;
        const auto it = std::find(equipped.begin(), equipped.end(), baseId);
        if (message.Unequip)
        {
            if (it != equipped.end())
                equipped.erase(it);
        }
        else if (it == equipped.end())
            equipped.push_back(baseId);

        actor.LastChange = Clock::now();
        Record(Collect::Equipment, fmt::format("{} {:X} on {:X}", message.Unequip ? "unequipped" : "equipped", baseId, message.ServerId));
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

        // The character copy goes as well. Leaving it behind meant the bot could never notice a copy that was
        // *not* removed, which is the whole point of watching somebody join and leave over and over: a removal
        // that misses one leaves a body nobody owns standing in the world.
        for (auto actorIt = m_actors.begin(); actorIt != m_actors.end();)
            actorIt = actorIt->ServerId == message.ServerId ? m_actors.erase(actorIt) : actorIt + 1;

        Record(Collect::Spawn, fmt::format("removed {:X}", message.ServerId));
        return;
    }

    if (opcode == NotifyDroppedItem::Opcode)
    {
        const auto& message = static_cast<const NotifyDroppedItem&>(acMessage);
        const bool cNew = m_drops.emplace(message.Id, message.Item.BaseId).second;
        const glm::vec3 cPlace(message.Position);
        m_dropFirstPlaces.emplace(message.Id, cPlace);
        if (!cNew && glm::distance(m_dropPlaces[message.Id], cPlace) > 20.f)
            spdlog::info("Dropped item {} was sent again, lying {:.0f} units from where this bot last knew it", message.Id,
                         glm::distance(m_dropPlaces[message.Id], cPlace));
        m_dropPlaces[message.Id] = cPlace;
        if (cNew)
        {
            spdlog::info("Dropped item {} is {:X}:{:X} x{} in cell {:X} at ({:.0f}, {:.0f}, {:.0f})", message.Id, message.Item.BaseId.ModId, message.Item.BaseId.BaseId,
                         message.Item.Count, message.CellId.BaseId, message.Position.x, message.Position.y, message.Position.z);
            Record(Collect::Spawn, fmt::format("drop {} {:X}", message.Id, message.Item.BaseId.BaseId));
        }
        return;
    }

    if (opcode == NotifyDroppedItemMove::Opcode)
    {
        const auto& message = static_cast<const NotifyDroppedItemMove&>(acMessage);
        ++m_dropMoves;
        const glm::vec3 cPlace(message.Position);
        m_dropPlaces[message.Id] = cPlace;
        spdlog::info("Dropped item {} moved to ({:.0f}, {:.0f}, {:.0f}){}", message.Id, cPlace.x, cPlace.y, cPlace.z, message.AtRest ? ", at rest" : "");
        Record(Collect::Spawn, fmt::format("drop {} moved{}", message.Id, message.AtRest ? " rest" : ""));
        return;
    }

    if (opcode == NotifyDroppedItemRemoved::Opcode)
    {
        const auto& message = static_cast<const NotifyDroppedItemRemoved&>(acMessage);
        if (m_drops.erase(message.Id))
        {
            ++m_dropRemovals;
            spdlog::info("Dropped item {} was picked up by somebody", message.Id);
            Record(Collect::Spawn, fmt::format("drop {} removed", message.Id));
        }
        return;
    }

    if (opcode == NotifyOwnershipTransfer::Opcode)
    {
        const auto& message = static_cast<const NotifyOwnershipTransfer&>(acMessage);

        // This message is not addressed to the new owner; it is broadcast to everyone in range, and to the
        // previous owner directly. OwnerPlayerId is what says whose it now is.
        //
        // Reading it as "ours" regardless was worth a whole afternoon: with two bots keeping what they were
        // given, both of them recorded the same actor at the same epoch in the same millisecond, and the pair
        // test passed because each side only ever checked its own count. The server was doing the right thing
        // throughout.
        const bool cOursNow = message.OwnerPlayerId == m_playerId;

        if (m_acceptOwnership)
        {
            KnownActor& actor = Actor(message.ServerId);
            actor.OwnedByUs = cOursNow;
            actor.OwnershipEpoch = message.OwnershipEpoch;
            actor.LastChange = Clock::now();
            spdlog::info("Actor {:X} is now player {}'s at epoch {}{}", message.ServerId, message.OwnerPlayerId, message.OwnershipEpoch,
                         cOursNow ? " -- that is us, keeping it" : " -- not us");
            Record(Collect::Ownership, fmt::format("{} {:X} epoch {}", cOursNow ? "owned" : "owner-is", message.ServerId, message.OwnershipEpoch));
            return;
        }

        // Nothing to hand back when it was never handed to us.
        if (!cOursNow)
            return;

        spdlog::info("Server handed us actor {:X}; handing it back (a bot never owns anything)", message.ServerId);
        RequestOwnershipTransfer request{};
        request.ServerId = message.ServerId;
        // The epoch the server just gave us, echoed back. Without it the server rejects the hand-back --
        // OnOwnershipTransferRequest requires the epoch to match -- and this bot went on owning every actor it had
        // just announced it was refusing. Third message in this family to be caught the same way (2026-09-26).
        request.OwnershipEpoch = message.OwnershipEpoch;
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

    // "burn <who> <dps> <seconds>" hurts somebody else the way a concentration spell does. A sword hit is one change
    // of many points; flames take a fraction of a point a frame, which ActorValueService::OnHealthChange does not send
    // one by one -- anything under a point is added up per actor and sent every 250 ms by RunSmallHealthUpdates. So
    // this sends dps/4 every quarter second, and a spell's damage arriving is a stream of small changes adding up.
    if (name == "burn")
    {
        const float dps = arg(1, 8.f);
        const float seconds = arg(2, 3.f);
        const int batches = std::max(1, static_cast<int>(seconds * 4.f + 0.5f));
        if (aFirstTick)
        {
            m_burnTarget = kNoId;
            m_burnSent = 0;
            if (!args.empty() && args[0] == "other")
            {
                for (const auto& player : m_players)
                    if (player.ServerId != m_serverId)
                    {
                        m_burnTarget = player.ServerId;
                        break;
                    }
            }
            else if (!args.empty())
                m_burnTarget = static_cast<uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16));

            if (m_burnTarget == kNoId)
            {
                spdlog::warn("Line {}: nobody to burn", acCommand.Line);
                return true;
            }
            spdlog::info("Burning {:X} at {:.1f} a second for {:.1f} s: {} changes of {:.2f}", m_burnTarget, dps, seconds, batches, -dps / 4.f);
        }
        if (m_burnTarget == kNoId)
            return true;

        // Due batches only: a slow tick sends the ones it owes, as the game's timer would have.
        const int due = std::min(batches, static_cast<int>(Seconds(now - m_commandStart) * 4.f) + 1);
        for (; m_burnSent < due; ++m_burnSent)
        {
            RequestHealthChangeBroadcast request{};
            request.Id = m_burnTarget;
            request.DeltaHealth = -dps / 4.f;
            SendMsg(request);
        }
        return m_burnSent >= batches;
    }

    // "hit <who> <delta>" hurts somebody else. A negative delta is damage, which is the direction the game
    // actually sends and the direction the server got wrong until 2026-09-24.
    if (name == "hit")
    {
        uint32_t target = kNoId;
        if (!args.empty() && args[0] == "other")
        {
            for (const auto& player : m_players)
                if (player.ServerId != m_serverId)
                {
                    target = player.ServerId;
                    break;
                }
        }
        else if (!args.empty())
            target = static_cast<uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16));

        if (target == kNoId)
        {
            spdlog::warn("Line {}: nobody to hit", acCommand.Line);
            return true;
        }

        SendHit(target, arg(1, -30.f));
        return true;
    }

    if (name == "die")
    {
        SendHealth(-4.f);
        SendDeath(true);
        return true;
    }

    if (name == "respawn")
    {
        // The client sends the respawn first and its restored health on its next value tick, so the server's stored
        // health is still the death value when the others rebuild the copy. Reproduced on purpose.
        SendMsg(PlayerRespawnRequest{});
        SendDeath(false);

        // "respawn bare" stops there. The point is to leave the server holding whatever health it had at the moment
        // of death, which is what anyone building a fresh copy of this character will be handed. A bot that helpfully
        // sends its health back afterwards repairs the very thing the test is trying to catch.
        if (!args.empty() && args[0] == "bare")
        {
            spdlog::info("Respawn sent, no health follow-up");
            return true;
        }

        m_healthRestoreAt = now + 500ms;
        m_healthRestorePending = true;
        spdlog::info("Respawn sent; health back to {:.0f} in 500 ms", m_maxHealth);
        return true;
    }

    // Walk into an interior, without a door.
    //
    // The bug this exists for: a player whose own cell unloads deletes every remote copy standing in it, the
    // server is never told, and the copy is gone until something else happens to re-send it. Seen was invisible
    // for seventy seconds that way on 2026-09-26. Until now it could only be reproduced by two people and a load
    // door, which is not a test.
    //
    // A cell change is only a message. The bot says it is in an interior and the server treats it exactly as it
    // treats a player walking through a door, which is the whole of what the bug needs.
    if (name == "cell")
    {
        if (args.empty())
        {
            spdlog::error("[script] cell needs a hex cell id, or 'out' to go back to the worldspace");
            return true;
        }

        // Coming out has to put the worldspace back. The first run of the doorloss pair went inside, came out
        // announcing worldspace 0, and the server withheld the bot from its partner for ever -- "other
        // worldspace". That was this command losing it, not the mod.
        static GameId s_worldSpaceBeforeInterior{};

        if (args[0] == "out")
        {
            m_cell = GameId{};
            m_worldSpace = s_worldSpaceBeforeInterior;
            m_standaloneCell = false;
            SendCellEntry();
            SendMovement();
            spdlog::info("[script] back out into worldspace {:X}:{:X}", m_worldSpace.ModId, m_worldSpace.BaseId);
            return true;
        }

        const uint32_t baseId = static_cast<uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16)) & 0x00FFFFFF;
        s_worldSpaceBeforeInterior = m_worldSpace;
        m_cell = GameId{m_skyrimModId, baseId};
        m_worldSpace = GameId{};
        m_standaloneCell = false;

        EnterInteriorCellRequest enter{};
        enter.CellId = m_cell;
        SendMsg(enter);
        SendMovement();

        spdlog::info("[script] entered interior cell {:X}:{:X}", m_cell.ModId, m_cell.BaseId);
        return true;
    }

    // Announce cells the way a client walking between neighbouring outdoor cells does: the exterior-enter only.
    //
    // The bot has always sent the grid shift alongside it, so the server's handling of an exterior-enter on its
    // own was never exercised -- and that handler was the one of the three cell handlers that did not send the
    // arriving player what is around them. Anything it failed to do, the grid shift did a moment later, in the
    // test but not in the game.
    if (name == "gridshift")
    {
        if (args.empty() || (args[0] != "on" && args[0] != "off"))
        {
            spdlog::error("[script] gridshift needs 'on' or 'off'");
            return true;
        }

        m_sendGridShift = args[0] == "on";
        spdlog::info("[script] grid shift {}", m_sendGridShift ? "on: cell changes send both messages" : "off: cell changes send the exterior enter alone");
        return true;
    }

    // Drop an item where this bot stands. The server remembers it and gives it to everyone in range now, and to anyone
    // who enters this cell later -- which is the part a two-player session could never test on purpose.
    if (name == "dropitem")
    {
        if (args.empty())
        {
            spdlog::error("[script] dropitem needs a hex base form id, e.g. 'dropitem 12EB7' (an iron dagger)");
            return true;
        }

        RequestDroppedItemAdd request{};
        request.Item.BaseId = GameId(m_skyrimModId, static_cast<uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16)) & 0x00FFFFFF);
        request.Item.Count = -1; // the inventory change that made it, as the client sends it
        request.CellId = m_cell;
        request.WorldSpaceId = m_worldSpace;
        request.Position = m_position;
        SendMsg(request);

        spdlog::info("[script] dropped {:X} at ({:.0f}, {:.0f}, {:.0f}) in cell {:X}", request.Item.BaseId.BaseId, m_position.x, m_position.y, m_position.z, m_cell.BaseId);
        return true;
    }

    // Pick up an item the server has told this bot about.
    if (name == "pickup")
    {
        if (m_drops.empty())
        {
            spdlog::warn("[script] pickup: nothing lying here that the server has mentioned");
            return true;
        }

        RequestDroppedItemRemove request{};
        request.Id = m_drops.begin()->first;
        SendMsg(request);
        spdlog::info("[script] picked up dropped item {}", request.Id);
        m_dropPlaces.erase(request.Id);
        m_drops.erase(m_drops.begin());
        return true;
    }

    // Move an item the server has told this bot about, the way a hand carrying it does: 'held' while it travels,
    // 'rest' once it is put down. The offset is from where the bot last knew the item to be.
    if (name == "moveitem")
    {
        float dx = 0.f, dy = 0.f, dz = 0.f;
        if (args.size() < 4 || !ParseFloat(args[0], dx) || !ParseFloat(args[1], dy) || !ParseFloat(args[2], dz) || (args[3] != "held" && args[3] != "rest"))
        {
            spdlog::error("[script] moveitem needs an offset and held|rest, e.g. 'moveitem 0 150 0 held'");
            return true;
        }
        if (m_drops.empty())
        {
            spdlog::warn("[script] moveitem: nothing lying here that the server has mentioned");
            return true;
        }

        const uint32_t id = m_drops.begin()->first;
        glm::vec3& place = m_dropPlaces[id];
        place += glm::vec3(dx, dy, dz);

        RequestDroppedItemMove request{};
        request.Id = id;
        request.Position = place;
        request.AtRest = args[3] == "rest";
        SendMsg(request);
        spdlog::info("[script] moved dropped item {} to ({:.0f}, {:.0f}, {:.0f}){}", id, place.x, place.y, place.z, request.AtRest ? ", put down" : ", still held");
        return true;
    }

    // Keep an actor the server hands over, instead of refusing it.
    if (name == "ownership")
    {
        if (args.empty() || (args[0] != "accept" && args[0] != "refuse"))
        {
            spdlog::error("[script] ownership needs 'accept' or 'refuse'");
            return true;
        }

        m_acceptOwnership = args[0] == "accept";
        spdlog::info("[script] ownership {}", m_acceptOwnership ? "accept: actors handed to us are kept" : "refuse: actors handed to us go straight back");
        return true;
    }

    // Register an NPC owned by this bot.
    //
    // Two headless bots have nothing between them but their own two player characters, and those can never
    // change hands -- CanClaimOwnership refuses IsPlayer outright. So an ownership test has to bring its own
    // actor, and the server will make one for any reference id that is not 0:14: OnAssignCharacterRequest treats
    // everything else as an ordinary character and CreateCharacter hands it to whoever asked.
    if (name == "npc")
    {
        if (args.empty())
        {
            spdlog::error("[script] npc needs a hex form id, e.g. 'npc A2C94'");
            return true;
        }

        const uint32_t baseId = static_cast<uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16)) & 0x00FFFFFF;
        if (baseId == 0x14)
        {
            spdlog::error("[script] npc 14 is the player and can never change hands; pick another form");
            return true;
        }

        std::random_device device;
        m_npcCookie = device();

        AssignCharacterRequest request{};
        request.Cookie = m_npcCookie;
        request.ReferenceId = GameId(m_skyrimModId, baseId);
        request.FormId = GameId(m_skyrimModId, baseId);
        request.CellId = m_cell;
        request.WorldSpaceId = m_worldSpace;
        request.Position = ToNet(m_position);
        request.Rotation = ToNet(glm::vec2(0.f, m_yaw));
        request.CurrentActorData.InitialActorValues.ActorValuesList[kHealth] = 100.f;
        request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kHealth] = 100.f;
        request.CurrentActorData.IsDead = false;
        SendMsg(request);

        spdlog::info("[script] asked the server to register NPC {:X}:{:X}", m_skyrimModId, baseId);
        return true;
    }

    // "npclife dead|alive [health]": an NPC this bot owns dies, or comes back to life, as its owner's game reports it.
    // The client sends the death state on its quarter-second tick and the health on its one-second value tick
    // (ActorValueService::RunDeathStateUpdates / BroadcastActorValues), so a revive goes out as "alive" and then the
    // health it got back -- there is no respawn message for an NPC; that path is only the player's beast form.
    if (name == "npclife")
    {
        if (args.empty() || (args[0] != "dead" && args[0] != "alive"))
        {
            spdlog::error("[script] npclife needs dead or alive, e.g. 'npclife alive 100'");
            return true;
        }
        KnownActor* pNpc = nullptr;
        for (auto& actor : m_actors)
            if (actor.OwnedByUs && !actor.IsPlayer && actor.ServerId != m_serverId)
                pNpc = &actor;
        if (!pNpc)
        {
            spdlog::warn("[script] npclife: this bot owns no NPC");
            return true;
        }

        const bool dead = args[0] == "dead";
        const float health = arg(1, dead ? 0.f : 100.f);

        RequestDeathStateChange death{};
        death.Id = pNpc->ServerId;
        death.OwnershipEpoch = pNpc->OwnershipEpoch;
        death.IsDead = dead;

        RequestActorValueChanges values{};
        values.Id = pNpc->ServerId;
        values.OwnershipEpoch = pNpc->OwnershipEpoch;
        values.Values[kHealth] = health;

        // Dying, the health reaches zero first; coming back, the death state goes first.
        if (dead)
        {
            SendMsg(values);
            SendMsg(death);
        }
        else
        {
            SendMsg(death);
            SendMsg(values);
        }

        pNpc->Dead = dead;
        pNpc->Health = health;
        pNpc->HealthKnown = true;
        pNpc->LastChange = Clock::now();
        spdlog::info("[script] NPC {:X} is now {} at health {:.0f} (epoch {})", pNpc->ServerId, dead ? "dead" : "alive", health, pNpc->OwnershipEpoch);
        return true;
    }

    // Hand an owned actor back, which starts the server's search for the next owner.
    if (name == "release")
    {
        KnownActor* pActor = nullptr;
        if (!args.empty() && args[0] != "any")
        {
            const uint32_t wanted = static_cast<uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16));
            for (auto& actor : m_actors)
                if (actor.ServerId == wanted)
                    pActor = &actor;
        }
        else
        {
            // Our own character is never a candidate: relinquishing it would be refused, and it is not what a
            // churn test is about.
            for (auto& actor : m_actors)
                if (actor.OwnedByUs && !actor.IsPlayer && actor.ServerId != m_serverId)
                    pActor = &actor;
        }

        if (!pActor || !pActor->OwnedByUs)
        {
            spdlog::warn("[script] release: nothing owned to hand back");
            return true;
        }

        RequestOwnershipTransfer request{};
        request.ServerId = pActor->ServerId;
        // Echoed back exactly. OnOwnershipTransferRequest drops a release whose epoch does not match the
        // server's, and drops it silently.
        request.OwnershipEpoch = pActor->OwnershipEpoch;
        request.Reason = OwnershipReleaseReason::Relinquish;
        SendMsg(request);

        pActor->OwnedByUs = false;
        pActor->LastChange = Clock::now();
        spdlog::info("[script] handed actor {:X} back at epoch {}", pActor->ServerId, pActor->OwnershipEpoch);
        Record(Collect::Ownership, fmt::format("released {:X} epoch {}", pActor->ServerId, pActor->OwnershipEpoch));
        return true;
    }

    // Zero the per-character spawn counters, so a later `spawns` check counts only what arrived after this point.
    //
    // Needed because a bot is sent each character several times while it joins, so any absolute threshold is
    // already satisfied before the part of the script under test begins. The first cellwalk pair asserted
    // `spawns other >= 2` and passed against a server with the fix deliberately removed: the seven spawns it
    // counted had all arrived during the join, twenty seconds before the walk it was supposed to be measuring.
    if (name == "reset")
    {
        if (args.empty() || args[0] != "spawns")
        {
            spdlog::error("[script] reset needs 'spawns'");
            return true;
        }

        for (auto& player : m_players)
            player.Spawns = 0;

        spdlog::info("[script] spawn counters zeroed; later counts are of what arrives from here");
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

    if (name == "collect")
    {
        uint32_t mask = 0;
        for (const auto& kind : args)
        {
            if (kind == "all") mask = static_cast<uint32_t>(Collect::All);
            else if (kind == "none") mask = 0;
            else if (kind == "health") mask |= static_cast<uint32_t>(Collect::Health);
            else if (kind == "death") mask |= static_cast<uint32_t>(Collect::Death);
            else if (kind == "ownership") mask |= static_cast<uint32_t>(Collect::Ownership);
            else if (kind == "spawn") mask |= static_cast<uint32_t>(Collect::Spawn);
            else if (kind == "equipment") mask |= static_cast<uint32_t>(Collect::Equipment);
            else if (kind == "party") mask |= static_cast<uint32_t>(Collect::Party);
            else if (kind == "movement") mask |= static_cast<uint32_t>(Collect::Movement);
            else spdlog::warn("Line {}: unknown collect kind '{}'", acCommand.Line, kind);
        }
        m_collect = mask;
        spdlog::info("[script] collecting: {}", Join(args));
        return true;
    }

    if (name == "record")
    {
        const bool on = !args.empty() && args[0] != "off";
        if (on)
        {
            m_recording = true;
            m_recordName = args.empty() ? "unnamed" : Join(args);
            m_recordStart = now;
            m_events.clear();
            m_suppressed = 0;
            spdlog::info("[script] recording '{}'", m_recordName);
        }
        else
        {
            m_recording = false;
            spdlog::info("[script] recording stopped: {} events kept, {} suppressed", m_events.size(), m_suppressed);
        }
        return true;
    }

    if (name == "waitfor" || name == "expect")
    {
        // waitfor blocks until it holds or the timeout runs out; expect checks once, now.
        std::vector<std::string> condition(args.begin(), args.end());
        float timeout = 10.f;
        if (name == "waitfor" && condition.size() > 1)
        {
            float parsed = 0.f;
            if (ParseFloat(condition.back(), parsed))
            {
                timeout = parsed;
                condition.pop_back();
            }
        }

        std::string why;
        const std::optional<bool> held = Evaluate(condition, why);
        if (!held.has_value())
        {
            m_failures.push_back(fmt::format("line {}: cannot read condition '{}'", acCommand.Line, Join(condition)));
            spdlog::error("[script] line {}: cannot read condition '{}'", acCommand.Line, Join(condition));
            return true;
        }

        if (*held)
        {
            ++m_checksPassed;
            spdlog::info("[script] ok: {} ({})", Join(condition), why);
            Record(Collect::All, fmt::format("PASS {} ({})", Join(condition), why));
            return true;
        }

        if (name == "expect" || Seconds(now - m_commandStart) >= timeout)
        {
            const auto text = fmt::format("line {}: {} -- {}", acCommand.Line, Join(condition), why);
            m_failures.push_back(text);
            spdlog::error("[script] FAILED {}", text);
            Record(Collect::All, fmt::format("FAIL {}", text));
            return true;
        }

        return false; // keep waiting
    }

    if (name == "report")
    {
        WriteReport(args.empty() ? "bot-report.txt" : args[0]);
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

void Bot::Record(const Collect aKind, const std::string& acText) noexcept
{
    if (!m_recording)
        return;

    if ((m_collect & static_cast<uint32_t>(aKind)) == 0)
    {
        ++m_suppressed;
        return;
    }

    const double at = std::chrono::duration<double>(Clock::now() - m_recordStart).count();
    m_events.push_back(fmt::format("[{:8.3f}] {}", at, acText));
}

KnownActor* Bot::FindActor(const uint32_t aServerId) noexcept
{
    for (auto& actor : m_actors)
    {
        if (actor.ServerId == aServerId)
            return &actor;
    }
    return nullptr;
}

KnownActor& Bot::Actor(const uint32_t aServerId) noexcept
{
    for (auto& actor : m_actors)
    {
        if (actor.ServerId == aServerId)
            return actor;
    }

    m_actors.push_back(KnownActor{aServerId});
    return m_actors.back();
}

std::optional<bool> Bot::Evaluate(const std::vector<std::string>& acArgs, std::string& aOutWhy) const noexcept
{
    if (acArgs.empty())
        return std::nullopt;

    // "not <condition>": every condition can be waited on for going away as well as for arriving. Without it a
    // script can say "the sword appeared" but never "the sword was put away", which is half a test.
    if (acArgs[0] == "not")
    {
        if (acArgs.size() < 2)
            return std::nullopt;
        const std::vector<std::string> rest(acArgs.begin() + 1, acArgs.end());
        const std::optional<bool> held = Evaluate(rest, aOutWhy);
        if (!held.has_value())
            return std::nullopt;
        aOutWhy = fmt::format("not: {}", aOutWhy);
        return !*held;
    }

    const std::string& what = acArgs[0];

    auto findActor = [this](const uint32_t aServerId) -> const KnownActor*
    {
        for (const auto& actor : m_actors)
        {
            if (actor.ServerId == aServerId)
                return &actor;
        }
        return nullptr;
    };

    auto parseId = [&](const size_t aIndex, uint32_t& aOut)
    {
        if (aIndex >= acArgs.size())
            return false;
        // "me" is this bot's own character, which is what most checks are actually about.
        if (acArgs[aIndex] == "me")
        {
            aOut = m_hasCharacter ? m_serverId : kNoId;
            return true;
        }

        // "npc" is the first NPC this bot holds a copy of or owns. Like "other", a script cannot know its id.
        if (acArgs[aIndex] == "npc")
        {
            aOut = kNoId;
            for (const auto& actor : m_actors)
                if (!actor.IsPlayer && actor.ServerId != m_serverId)
                {
                    aOut = actor.ServerId;
                    break;
                }
            return true;
        }

        // "other" is the first other player this bot can see. A script cannot know the other side's server id in
        // advance, and without this no test can assert that anything actually crossed between two clients --
        // which is where the interesting bugs live.
        if (acArgs[aIndex] == "other")
        {
            for (const auto& player : m_players)
            {
                if (player.ServerId != m_serverId)
                {
                    aOut = player.ServerId;
                    return true;
                }
            }
            // Nobody else is here *yet*. That is a condition which is false right now, not a condition this
            // cannot read, and waitfor has to be able to tell those apart: a malformed condition should fail at
            // once, an unsatisfied one should be waited on.
            aOut = kNoId;
            static std::chrono::steady_clock::time_point s_lastWhoLog{};
            const auto now = Clock::now();
            if (now - s_lastWhoLog >= std::chrono::seconds(5))
            {
                s_lastWhoLog = now;
                std::string held;
                for (const auto& player : m_players)
                    held += fmt::format(" [{:X} p{}]", player.ServerId, player.PlayerId);
                spdlog::info("'other' resolves to nobody: our id {:X}, {} players held{}", m_serverId, m_players.size(), held.empty() ? std::string(" (none)") : held);
            }
            return true;
        }

        // A literal id. Anything that is not a number at all is a malformed condition and fails at once; a number
        // that happens to be zero is a real id and is not.
        const char* pStart = acArgs[aIndex].c_str();
        char* pEnd = nullptr;
        const unsigned long parsed = std::strtoul(pStart, &pEnd, 16);
        if (pEnd == pStart)
            return false;
        aOut = static_cast<uint32_t>(parsed);
        return true;
    };

    if (what == "players")
    {
        // players <op> <n>: how many other players this bot can see.
        if (acArgs.size() < 3)
            return std::nullopt;
        float wanted = 0.f;
        if (!ParseFloat(acArgs[2], wanted))
            return std::nullopt;
        const auto count = static_cast<float>(m_players.size());
        aOutWhy = fmt::format("{} players known", m_players.size());
        const std::string& op = acArgs[1];
        if (op == "==") return count == wanted;
        if (op == ">=") return count >= wanted;
        if (op == "<=") return count <= wanted;
        if (op == ">") return count > wanted;
        if (op == "<") return count < wanted;
        return std::nullopt;
    }

    if (what == "health")
    {
        // health <serverId hex> <op> <value>
        uint32_t id = 0;
        if (!parseId(1, id) || acArgs.size() < 4)
            return std::nullopt;
        float wanted = 0.f;
        if (!ParseFloat(acArgs[3], wanted))
            return std::nullopt;
        const KnownActor* pActor = id != kNoId ? findActor(id) : nullptr;
        if (!pActor || !pActor->HealthKnown)
        {
            aOutWhy = id != kNoId ? fmt::format("no health seen for {:X} yet", id) : std::string("nobody else is here yet");
            return false;
        }
        aOutWhy = fmt::format("{:X} health is {:.1f}", id, pActor->Health);
        const std::string& op = acArgs[2];
        if (op == "==") return pActor->Health == wanted;
        if (op == ">=") return pActor->Health >= wanted;
        if (op == "<=") return pActor->Health <= wanted;
        if (op == ">") return pActor->Health > wanted;
        if (op == "<") return pActor->Health < wanted;
        return std::nullopt;
    }

    if (what == "dead" || what == "alive")
    {
        uint32_t id = 0;
        if (!parseId(1, id))
            return std::nullopt;
        const KnownActor* pActor = id != kNoId ? findActor(id) : nullptr;
        if (!pActor)
        {
            aOutWhy = id != kNoId ? fmt::format("nothing known about {:X}", id) : std::string("nobody else is here yet");
            return false;
        }
        aOutWhy = fmt::format("{:X} dead={}", id, pActor->Dead);
        return what == "dead" ? pActor->Dead : !pActor->Dead;
    }

    if (what == "known")
    {
        uint32_t id = 0;
        if (!parseId(1, id))
            return std::nullopt;
        // "Known" means there is a character copy, not merely that the server mentioned somebody. Spawns now record
        // one, so this no longer needs to fall back to the player list -- and it must not: that fallback let "known
        // other" pass immediately after a reconnect, before the character had been sent, so the checks that followed
        // read an empty world and the script never waited for the copy it was about to test.
        const bool seen = id != kNoId && findActor(id) != nullptr;
        if (!seen && id != kNoId)
        {
            static std::chrono::steady_clock::time_point s_lastKnownLog{};
            const auto now = Clock::now();
            if (now - s_lastKnownLog >= std::chrono::seconds(3))
            {
                s_lastKnownLog = now;
                std::string held;
                for (const auto& actor : m_actors)
                    held += fmt::format(" {:X}", actor.ServerId);
                spdlog::info("'known {:X}' is false; copies held:{}", id, held.empty() ? std::string(" (none)") : held);
            }
        }
        aOutWhy = seen ? fmt::format("{:X} is known", id) : (id != kNoId ? fmt::format("{:X} never arrived", id) : std::string("nobody else is here yet"));
        return seen;
    }

    if (what == "equipped")
    {
        // equipped <who> <baseId hex>: is that actor holding that item, as far as this bot has been told.
        uint32_t id = 0;
        if (!parseId(1, id) || acArgs.size() < 3)
            return std::nullopt;

        const char* pStart = acArgs[2].c_str();
        char* pEnd = nullptr;
        const uint32_t wanted = static_cast<uint32_t>(std::strtoul(pStart, &pEnd, 16));
        if (pEnd == pStart)
            return std::nullopt;

        const KnownActor* pActor = id != kNoId ? findActor(id) : nullptr;
        if (!pActor)
        {
            aOutWhy = id != kNoId ? fmt::format("nothing known about {:X}", id) : std::string("nobody else is here yet");
            return false;
        }

        const bool holding = std::find(pActor->Equipped.begin(), pActor->Equipped.end(), wanted) != pActor->Equipped.end();
        std::string held;
        for (const uint32_t item : pActor->Equipped)
            held += fmt::format("{}{:X}", held.empty() ? "" : " ", item);
        aOutWhy = fmt::format("{:X} holds [{}]", id, held.empty() ? std::string("nothing") : held);
        return holding;
    }

    if (what == "samecell")
    {
        // samecell <who>: that player has announced the interior this bot stands in. "known" cannot say it: going
        // indoors does not take the outdoor copies away from a bot the way unloading the cell does in the game, so a
        // copy known outside still reads as known inside (dropmove pair, 2026-09-30).
        uint32_t id = 0;
        if (!parseId(1, id))
            return std::nullopt;
        const auto player = std::find_if(m_players.begin(), m_players.end(), [id](const KnownPlayer& acPlayer) { return acPlayer.ServerId == id; });
        const bool same = id != kNoId && player != m_players.end() && m_cell != GameId{} && player->CellId == m_cell;
        aOutWhy = player == m_players.end() ? std::string("nobody else is here yet")
                                            : fmt::format("{:X} announced cell {:X}, this bot is in {:X}", id, player->CellId.BaseId, m_cell.BaseId);
        return same;
    }

    if (what == "dropmoves" || what == "dropmoved")
    {
        // dropmoves <op> <n>: moves the server relayed to this bot.
        // dropmoved <op> <n>: how far the first known item now lies from where this bot first heard of it.
        if (acArgs.size() < 3)
            return std::nullopt;
        float wanted = 0.f;
        if (!ParseFloat(acArgs[2], wanted))
            return std::nullopt;
        float moved = -1.f;
        if (!m_drops.empty())
        {
            const uint32_t id = m_drops.begin()->first;
            if (m_dropPlaces.count(id) && m_dropFirstPlaces.count(id))
                moved = glm::distance(m_dropPlaces.at(id), m_dropFirstPlaces.at(id));
        }
        const float value = what == "dropmoves" ? static_cast<float>(m_dropMoves) : moved;
        aOutWhy = fmt::format("{} move(s) relayed; first item lies {} from where it was first heard of", m_dropMoves,
                              moved < 0.f ? std::string("(no item)") : fmt::format("{:.0f} units", moved));
        if (what == "dropmoved" && moved < 0.f)
            return false;
        const std::string& op = acArgs[1];
        if (op == "==") return value == wanted;
        if (op == ">=") return value >= wanted;
        if (op == "<=") return value <= wanted;
        if (op == ">") return value > wanted;
        if (op == "<") return value < wanted;
        return std::nullopt;
    }

    if (what == "drops" || what == "dropremovals")
    {
        // drops <op> <n>: items lying in the world as the server has told this bot.
        // dropremovals <op> <n>: how many of them somebody else picked up.
        if (acArgs.size() < 3)
            return std::nullopt;
        float wanted = 0.f;
        if (!ParseFloat(acArgs[2], wanted))
            return std::nullopt;
        const float value = what == "drops" ? static_cast<float>(m_drops.size()) : static_cast<float>(m_dropRemovals);
        aOutWhy = fmt::format("{} dropped item(s) known, {} picked up by others", m_drops.size(), m_dropRemovals);
        const std::string& op = acArgs[1];
        if (op == "==") return value == wanted;
        if (op == ">=") return value >= wanted;
        if (op == "<=") return value <= wanted;
        if (op == ">") return value > wanted;
        if (op == "<") return value < wanted;
        return std::nullopt;
    }

    if (what == "owned")
    {
        // owned <op> <n>: how many actors this bot owns that are not its own character.
        //
        // Exactly the number a churn test watches: it should be 1 on the side holding the actor and 0 on the
        // other, and every hand-over should move it. Two bots reading 1 at once means the server handed the same
        // actor to both; both reading 0 means a hand-over went nowhere and the actor is ownerless.
        if (acArgs.size() < 3)
            return std::nullopt;
        float wanted = 0.f;
        if (!ParseFloat(acArgs[2], wanted))
            return std::nullopt;

        uint32_t count = 0;
        std::string held;
        for (const auto& actor : m_actors)
        {
            if (!actor.OwnedByUs || actor.IsPlayer || actor.ServerId == m_serverId)
                continue;
            ++count;
            held += fmt::format("{}{:X}@{}", held.empty() ? "" : " ", actor.ServerId, actor.OwnershipEpoch);
        }

        aOutWhy = fmt::format("{} actor(s) owned [{}]", count, held.empty() ? std::string("none") : held);
        const std::string& op = acArgs[1];
        const auto value = static_cast<float>(count);
        if (op == "==") return value == wanted;
        if (op == ">=") return value >= wanted;
        if (op == "<=") return value <= wanted;
        if (op == ">") return value > wanted;
        if (op == "<") return value < wanted;
        return std::nullopt;
    }

    if (what == "spawns")
    {
        // spawns <who> <op> <n>: how many times the server has sent us that character.
        //
        // The server re-sends a character to put a copy right that is stale or missing, so this counts repairs.
        // It is the only way a test can see a repair that *should* have happened and did not: a copy that was
        // never sent again is indistinguishable from one that is simply still correct, right up until the moment
        // the player it belongs to turns out to be invisible.
        if (acArgs.size() < 4)
            return std::nullopt;

        uint32_t id = kNoId;
        if (!parseId(1, id))
            return std::nullopt;

        float wanted = 0.f;
        if (!ParseFloat(acArgs[3], wanted))
            return std::nullopt;

        const KnownPlayer* pPlayer = nullptr;
        for (const auto& player : m_players)
        {
            if (player.ServerId == id)
            {
                pPlayer = &player;
                break;
            }
        }

        const auto count = pPlayer ? static_cast<float>(pPlayer->Spawns) : 0.f;
        aOutWhy = pPlayer ? fmt::format("{:X} has been sent to us {} time(s)", id, pPlayer->Spawns)
                          : std::string("nobody else is here yet");

        const std::string& op = acArgs[2];
        if (op == "==") return count == wanted;
        if (op == ">=") return count >= wanted;
        if (op == "<=") return count <= wanted;
        if (op == ">") return count > wanted;
        if (op == "<") return count < wanted;
        return std::nullopt;
    }

    if (what == "actors")
    {
        // actors <op> <n>: how many character copies this bot is holding, its own included. A join-and-leave loop
        // should always come back to the same number; a number that climbs is a removal that missed one.
        if (acArgs.size() < 3)
            return std::nullopt;
        float wanted = 0.f;
        if (!ParseFloat(acArgs[2], wanted))
            return std::nullopt;
        const auto count = static_cast<float>(m_actors.size());
        aOutWhy = fmt::format("{} character copies held", m_actors.size());
        const std::string& op = acArgs[1];
        if (op == "==") return count == wanted;
        if (op == ">=") return count >= wanted;
        if (op == "<=") return count <= wanted;
        if (op == ">") return count > wanted;
        if (op == "<") return count < wanted;
        return std::nullopt;
    }

    if (what == "connected")
    {
        const bool inWorld = m_phase == Phase::InWorld;
        aOutWhy = inWorld ? "in world" : "not in world";
        return inWorld;
    }

    return std::nullopt;
}

void Bot::WriteReport(const std::string& acPath) const noexcept
{
    std::ofstream out(acPath, std::ios::trunc);
    if (!out)
    {
        spdlog::error("[script] could not write the report to {}", acPath);
        return;
    }

    out << "Skyrim Together VR bot report\n";
    out << "run: " << (m_recordName.empty() ? "unnamed" : m_recordName) << "\n";
    out << "server: " << m_options.Server << " (" << m_serverVersion << ")\n";
    out << "checks passed: " << m_checksPassed << ", failed: " << m_failures.size() << "\n";
    out << "verdict: " << (m_failures.empty() ? "PASS" : "FAIL") << "\n\n";

    if (!m_failures.empty())
    {
        out << "failures\n";
        for (const auto& failure : m_failures)
            out << "  " << failure << "\n";
        out << "\n";
    }

    out << "events kept: " << m_events.size() << ", suppressed by the collect filter: " << m_suppressed << "\n";
    for (const auto& event : m_events)
        out << event << "\n";

    out << "\nactors seen\n";
    for (const auto& actor : m_actors)
    {
        out << fmt::format("  {:X}{}{} health {}{} \n", actor.ServerId, actor.IsPlayer ? " (player)" : "",
                           actor.Name.empty() ? "" : " " + actor.Name,
                           actor.HealthKnown ? fmt::format("{:.1f}", actor.Health) : std::string("unknown"),
                           actor.Dead ? ", dead" : "");
    }

    spdlog::info("[script] report written to {} ({})", acPath, m_failures.empty() ? "PASS" : "FAIL");
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
