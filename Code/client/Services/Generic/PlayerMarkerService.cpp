#include <Services/PlayerMarkerService.h>

#include <PerfScope.h>
#include <World.h>
#include <Components.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerWhereabouts.h>

#include <Services/PapyrusService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>

#include <Structs/GridCellCoords.h>

#include <Actor.h>
#include <ExtraData/ExtraData.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESQuest.h>
#include <Forms/TESWorldSpace.h>
#include <Components/TESFullName.h>
#include <Games/Overrides.h>
#include <Games/TES.h>

namespace
{
// SkyrimTogether.esp's records, at the object ids Tools/VR/player-markers.py gives them.
constexpr uint32_t kQuestId = 0x005000;
constexpr uint32_t kHoldingCellId = 0x005001;
constexpr uint32_t kPartyMarkerBaseFirst = 0x005010;
constexpr uint32_t kPartyMarkerFirst = 0x005020;
constexpr uint32_t kMapMarkerFirst = 0x005040;

// A marker follows a player only once they have moved this far (game units, about 70 to the metre), so a player
// standing about does not move it every two seconds.
constexpr float kFollowDistance = 256.f;

// The world map's icon for other players. Map marker types are the game's own (11: a landmark's standing stone).
constexpr uint16_t kMapMarkerType = 11;

// The map marker's own data on its reference (ExtraMapMarker, 0x2C): its name, whether it shows on the map, its icon.
struct MapMarkerData
{
    void* pFullNameVTable;
    BSFixedString Name;
    uint8_t Flags; // 1 visible on the map, 2 can travel to
    uint8_t Pad;
    uint16_t Type;
};

struct MapMarkerExtra : BSExtraData
{
    MapMarkerData* pData;
};

constexpr auto kExtraMapMarker = static_cast<ExtraDataType>(0x2C);
constexpr uint8_t kMapMarkerVisible = 1;

Actor* FindPlayerActor(World& aWorld, uint32_t aPlayerId) noexcept
{
    auto view = aWorld.view<FormIdComponent, PlayerComponent>();
    for (const auto cEntity : view)
    {
        if (view.get<PlayerComponent>(cEntity).Id == aPlayerId)
            return Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id));
    }
    return nullptr;
}

// The cell a player's position is in: an interior by its id; outdoors the cell under the position, which the
// game makes if it has not loaded it (as "teleport to player" does).
TESObjectCELL* ResolveCell(World& aWorld, const GameId& acWorldSpaceId, const GameId& acCellId, const glm::vec3& acPosition) noexcept
{
    auto& modSystem = aWorld.GetModSystem();
    if (TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(modSystem.GetGameId(acCellId))))
        return pCell;

    if (!acWorldSpaceId)
        return nullptr;
    TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(modSystem.GetGameId(acWorldSpaceId)));
    if (!pWorldSpace)
        return nullptr;
    const GridCellCoords cGrid = GridCellCoords::CalculateGridCellCoords(acPosition.x, acPosition.y);
    return pWorldSpace->LoadCell(cGrid.X, cGrid.Y);
}

NiPoint3 ToNiPoint(const glm::vec3& acPosition) noexcept
{
    NiPoint3 point;
    point.x = acPosition.x;
    point.y = acPosition.y;
    point.z = acPosition.z;
    return point;
}

// "Seen, near Whiterun"; the name alone when their game has not said where they are.
String MarkerText(const String& acName, const String& acPlace) noexcept
{
    return acPlace.empty() ? acName : acName + ", near " + acPlace;
}
} // namespace

PlayerMarkerService::PlayerMarkerService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&PlayerMarkerService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&PlayerMarkerService::OnDisconnected>(this);
    m_joinedConnection = aDispatcher.sink<NotifyPlayerJoined>().connect<&PlayerMarkerService::OnPlayerJoined>(this);
    m_leftConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&PlayerMarkerService::OnPlayerLeft>(this);
    m_whereaboutsConnection = aDispatcher.sink<NotifyPlayerWhereabouts>().connect<&PlayerMarkerService::OnWhereabouts>(this);
}

bool PlayerMarkerService::FindForms() noexcept
{
    if (m_formsLooked)
        return m_formsFound;
    m_formsLooked = true;

    Mod* pPlugin = ModManager::Get()->GetByName("SkyrimTogether.esp");
    if (!pPlugin)
    {
        spdlog::warn("Player markers: SkyrimTogether.esp is not loaded; no players on the map");
        return false;
    }
    m_modBase = static_cast<uint32_t>(pPlugin->standardId) << 24;

    m_pQuest = Cast<TESQuest>(TESForm::GetById(m_modBase | kQuestId));
    m_pHoldingCell = Cast<TESObjectCELL>(TESForm::GetById(m_modBase | kHoldingCellId));
    bool allFound = m_pQuest && m_pHoldingCell;
    for (size_t i = 0; i < kPartyMarkers; ++i)
    {
        m_partySlots[i].pMarker = Cast<TESObjectREFR>(TESForm::GetById(m_modBase | (kPartyMarkerFirst + static_cast<uint32_t>(i))));
        allFound = allFound && m_partySlots[i].pMarker;
    }
    for (size_t i = 0; i < kMapMarkers; ++i)
    {
        m_mapSlots[i].pMarker = Cast<TESObjectREFR>(TESForm::GetById(m_modBase | (kMapMarkerFirst + static_cast<uint32_t>(i))));
        allFound = allFound && m_mapSlots[i].pMarker;
    }
    if (!allFound)
    {
        spdlog::warn("Player markers: SkyrimTogether.esp has no player marker records (quest {}, holding cell {}); an older plugin? No players on the map",
                     m_pQuest ? "found" : "missing", m_pHoldingCell ? "found" : "missing");
        return false;
    }

    auto& papyrus = m_world.ctx().at<PapyrusService>();
    if (!papyrus.Get("Quest", "SetObjectiveDisplayed") || !papyrus.Get("Quest", "IsObjectiveDisplayed") || !papyrus.Get("Quest", "Stop"))
    {
        spdlog::warn("Player markers: the game's Quest.SetObjectiveDisplayed, IsObjectiveDisplayed or Stop was not found; no players on the map");
        return false;
    }

    spdlog::info("Player markers: forms found in SkyrimTogether.esp (quest {:X})", m_pQuest->formID);
    m_formsFound = true;
    return true;
}

void PlayerMarkerService::OnConnected(const ConnectedEvent&) noexcept
{
    m_connected = true;
}

void PlayerMarkerService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_connected = false;
    m_names.clear();
    if (m_formsFound)
    {
        FreeAll();
        StopQuest();
    }
}

void PlayerMarkerService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    m_names[acMessage.PlayerId] = acMessage.Username;
}

void PlayerMarkerService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    m_names.erase(acMessage.PlayerId);
    if (!m_formsFound)
        return;

    for (size_t i = 0; i < kPartyMarkers; ++i)
        if (m_partySlots[i].PlayerId == acMessage.PlayerId)
            Free(m_partySlots[i], i, true);
    for (size_t i = 0; i < kMapMarkers; ++i)
        if (m_mapSlots[i].PlayerId == acMessage.PlayerId)
            Free(m_mapSlots[i], i, false);
}

void PlayerMarkerService::OnWhereabouts(const NotifyPlayerWhereabouts& acMessage) noexcept
{
    if (!m_connected || !FindForms())
        return;

    PerfScope perfScope("PlayerMarkerService::OnWhereabouts");

    StartQuest();

    const uint32_t cLocalId = m_world.GetTransport().GetLocalPlayerId();
    const auto& partyService = m_world.GetPartyService();
    const auto& partyMembers = partyService.GetPartyMembers();
    const auto isParty = [&](uint32_t aPlayerId) {
        return partyService.IsInParty() && std::find(partyMembers.begin(), partyMembers.end(), aPlayerId) != partyMembers.end();
    };
    const auto isListedAs = [&](uint32_t aPlayerId, bool aParty) {
        for (const auto& entry : acMessage.Players)
            if (entry.PlayerId == aPlayerId)
                return isParty(aPlayerId) == aParty;
        return false;
    };

    // A slot whose player has gone, or has joined or left the party, is freed first, so its marker can be reused.
    for (size_t i = 0; i < kPartyMarkers; ++i)
        if (m_partySlots[i].PlayerId && !isListedAs(m_partySlots[i].PlayerId, true))
            Free(m_partySlots[i], i, true);
    for (size_t i = 0; i < kMapMarkers; ++i)
        if (m_mapSlots[i].PlayerId && !isListedAs(m_mapSlots[i].PlayerId, false))
            Free(m_mapSlots[i], i, false);

    for (const auto& entry : acMessage.Players)
    {
        if (entry.PlayerId == cLocalId)
            continue;

        const bool cParty = isParty(entry.PlayerId);
        Slot* pSlots = cParty ? m_partySlots.data() : m_mapSlots.data();
        const size_t cCount = cParty ? kPartyMarkers : kMapMarkers;

        size_t index = cCount;
        for (size_t i = 0; i < cCount && index == cCount; ++i)
            if (pSlots[i].PlayerId == entry.PlayerId)
                index = i;
        for (size_t i = 0; i < cCount && index == cCount; ++i)
            if (pSlots[i].PlayerId == 0)
                index = i;
        if (index == cCount)
            continue; // more players than markers: the rest go unmarked

        Slot& slot = pSlots[index];
        slot.PlayerId = entry.PlayerId;

        const auto cName = m_names.find(entry.PlayerId);
        const String cText = MarkerText(cName != m_names.end() ? cName->second : String("A traveller"), entry.Place);
        if (cText != slot.Text)
        {
            slot.Text = cText;
            if (cParty)
                SetPartyMarkerName(index, cText);
            else
                SetMapMarkerName(slot.pMarker, cText);
        }

        // Their body is in our game: no marker, the player can see them. Everyone else's is followed.
        if (FindPlayerActor(m_world, entry.PlayerId))
            Park(slot, index);
        else
            Follow(slot, index, entry.WorldSpaceId, entry.CellId, glm::vec3(entry.Position.x, entry.Position.y, entry.Position.z));

        // The quest takes a moment to start, and an objective shown before it runs is not shown at all (rig,
        // 2026-10-10): only once it runs, and again whenever the game says it is not displayed.
        if (cParty && m_pQuest->getState() == TESQuest::State::Running && (!slot.Shown || !IsObjectiveDisplayed(index)))
        {
            SetObjectiveShown(slot, index, true);
            spdlog::info("Player markers: objective {} for player {} shown: {}", index + 1, entry.PlayerId, IsObjectiveDisplayed(index) ? "yes" : "no");
        }
    }
}

void PlayerMarkerService::StartQuest() noexcept
{
    if (m_questStarted)
        return;

    ScopedQuestOverride _;
    bool succeeded = false;
    m_pQuest->EnsureQuestStarted(succeeded, false);
    // Tracked from the start, so its markers show; the player can stop tracking it in the journal.
    m_pQuest->SetActive(true);
    m_questStarted = true;
    spdlog::info("Player markers: quest Fellow travellers started ({})", succeeded ? "ok" : "the game said no");
}

void PlayerMarkerService::StopQuest() noexcept
{
    if (!m_questStarted)
        return;

    ScopedQuestOverride _;
    using Quest = TESQuest;
    PAPYRUS_FUNCTION(void, Quest, Stop);
    s_pStop(m_pQuest);
    m_questStarted = false;
    spdlog::info("Player markers: quest Fellow travellers stopped");
}

void PlayerMarkerService::Follow(Slot& aSlot, size_t aIndex, const GameId& acWorldSpaceId, const GameId& acCellId, const glm::vec3& acPosition) noexcept
{
    if (!aSlot.Parked && aSlot.CellId == acCellId && glm::distance(aSlot.Position, acPosition) < kFollowDistance)
        return;

    TESObjectCELL* pCell = ResolveCell(m_world, acWorldSpaceId, acCellId, acPosition);
    if (!pCell)
        return;

    aSlot.pMarker->MoveTo(pCell, ToNiPoint(acPosition));
    if (aSlot.Parked)
        spdlog::info("Player markers: {:X} follows player {} ({}) to ({:.0f}, {:.0f}, {:.0f}) in cell {:X}", aSlot.pMarker->formID, aSlot.PlayerId, aSlot.Text.c_str(), acPosition.x,
                     acPosition.y, acPosition.z, pCell->formID);
    aSlot.Parked = false;
    aSlot.CellId = acCellId;
    aSlot.Position = acPosition;
}

void PlayerMarkerService::Park(Slot& aSlot, size_t aIndex) noexcept
{
    if (aSlot.Parked)
        return;

    aSlot.pMarker->MoveTo(m_pHoldingCell, ToNiPoint(glm::vec3(static_cast<float>(aIndex) * 64.f, 0.f, 0.f)));
    aSlot.Parked = true;
    spdlog::info("Player markers: {:X} put away (player {})", aSlot.pMarker->formID, aSlot.PlayerId);
}

void PlayerMarkerService::Free(Slot& aSlot, size_t aIndex, bool aParty) noexcept
{
    if (aParty && aSlot.Shown)
        SetObjectiveShown(aSlot, aIndex, false);
    Park(aSlot, aIndex);
    aSlot.PlayerId = 0;
    aSlot.Text.clear();
    aSlot.CellId = GameId{};
}

void PlayerMarkerService::FreeAll() noexcept
{
    for (size_t i = 0; i < kPartyMarkers; ++i)
        if (m_partySlots[i].PlayerId)
            Free(m_partySlots[i], i, true);
    for (size_t i = 0; i < kMapMarkers; ++i)
        if (m_mapSlots[i].PlayerId)
            Free(m_mapSlots[i], i, false);
}

void PlayerMarkerService::SetObjectiveShown(Slot& aSlot, size_t aIndex, bool aShown) noexcept
{
    ScopedQuestOverride _;
    using Quest = TESQuest;
    PAPYRUS_FUNCTION(void, Quest, SetObjectiveDisplayed, int, bool, bool);
    // Objective N is the quest's objective index N, aimed at party marker N (1 to 7).
    s_pSetObjectiveDisplayed(m_pQuest, static_cast<int>(aIndex) + 1, aShown, false);
    aSlot.Shown = aShown;
}

bool PlayerMarkerService::IsObjectiveDisplayed(size_t aIndex) const noexcept
{
    using Quest = TESQuest;
    PAPYRUS_FUNCTION(bool, Quest, IsObjectiveDisplayed, int);
    return s_pIsObjectiveDisplayed(m_pQuest, static_cast<int>(aIndex) + 1);
}

void PlayerMarkerService::SetPartyMarkerName(size_t aIndex, const String& acText) noexcept
{
    // The objective's text is its marker's name (<Alias=MemberN>), which is the name of the marker's own activator.
    TESFullName* pName = Cast<TESFullName>(TESForm::GetById(m_modBase | (kPartyMarkerBaseFirst + static_cast<uint32_t>(aIndex))));
    if (pName)
        pName->value.Set(acText.c_str());
}

void PlayerMarkerService::SetMapMarkerName(TESObjectREFR* apMarker, const String& acText) noexcept
{
    auto* pExtra = static_cast<MapMarkerExtra*>(apMarker->extraData.GetByType(kExtraMapMarker));
    if (!pExtra || !pExtra->pData)
    {
        spdlog::warn("Player markers: map marker {:X} has no map marker data", apMarker->formID);
        return;
    }
    pExtra->pData->Name.Set(acText.c_str());
    pExtra->pData->Flags = kMapMarkerVisible; // on the map, never a fast travel destination
    pExtra->pData->Type = kMapMarkerType;
}
