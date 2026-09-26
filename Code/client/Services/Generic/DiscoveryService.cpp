#include <TiltedOnlinePCH.h>
#include <PerfScope.h>

#include <Services/DiscoveryService.h>
#include <Games/TES.h>

#include <Games/References.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESNPC.h>
#include <Games/ActorExtension.h>

#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/PreUpdateEvent.h>
#include <Events/GridCellChangeEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/LocationChangeEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>

#include <World.h>

namespace
{
bool IsDefaultModlist(GameList<Mod>& aCurrentModlist) noexcept
{
    static const auto s_defaultModlist = std::to_array<TiltedPhoques::String>(
        {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", 
        "Dragonborn.esm", "_ResourcePack.esl", "SkyrimTogether.esp", "SkyrimTogetherQuestPatches.esp"}
    );

    if (aCurrentModlist.Size() != s_defaultModlist.size())
        return false;

    auto expectedMod = s_defaultModlist.begin();
    for (const auto* pCurrentMod : aCurrentModlist)
    {
        if (pCurrentMod->filename != *expectedMod)
            return false;

        ++expectedMod;
    }

    return true;
}

// A copy this client threw away on its own. See RequestCellReannounce.
std::chrono::steady_clock::time_point s_reannounceAt{};
} // namespace

// The invisible player, named at last (2026-09-26, 11:20).
//
// The server sends a character's spawn to the other players when **that character** changes cell. It has no handler
// for the reverse: a player whose own cell unloads and takes every remote copy in it down with it. So the order
// that morning was
//
//   11:20:17  Emma enters interior cell 34FD2; the server tells Seen to drop her copy and waits.
//   11:20:21  Seen enters 34FD2 too; the server sends his spawn to Emma, whose client answers
//             "Character with remote id 20 is already spawned" and only nudges the old copy's position.
//   11:20:22  Emma's old cell finishes unloading and deletes that very copy.
//
// From then on the server believes Emma has Seen and Emma has nobody: "could not find actor server id 20" three
// times over the next minute, and Seen invisible until the *next* load door at 11:21:25 happened to fix it.
//
// Announcing the cell again is enough: the server re-sends every character in it, and by then the teardown is
// finished so the spawn lands on an empty slot. Nothing else is disturbed -- the server's leave-cell cleanup bails
// as soon as it finds a player still in the cell, which is us.
void DiscoveryService::RequestCellReannounce() noexcept
{
    // A load door tears copies down over several frames; one announcement after the last of them is what is wanted,
    // so each request pushes the moment back rather than queuing another.
    s_reannounceAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
}

DiscoveryService::DiscoveryService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
{
    m_preUpdateConnection = m_dispatcher.sink<PreUpdateEvent>().connect<&DiscoveryService::OnUpdate>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&DiscoveryService::OnConnected>(this);

    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

void DiscoveryService::VisitCell(bool aForceTrigger) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    if (pPlayer->GetWorldSpace())
        VisitExteriorCell(aForceTrigger);
    else if (pPlayer->GetParentCellEx())
        VisitInteriorCell(aForceTrigger);

    // exactly how the game does it too
    if (m_pLocation != pPlayer->locationForm)
    {
        m_dispatcher.trigger(LocationChangeEvent());
        m_pLocation = pPlayer->locationForm;
    }
}

void DiscoveryService::VisitExteriorCell(bool aForceTrigger) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const auto pWorldSpace = pPlayer->GetWorldSpace();

    m_interiorCellId = 0;

    const TES* pTES = TES::Get();
    const uint32_t worldSpaceId = pWorldSpace->formID;
    const GridCellCoords gameCurrentGrid(pTES->currentGridX, pTES->currentGridY);
    const GridCellCoords gameCenterGrid(pTES->centerGridX, pTES->centerGridY);

    if (m_worldSpaceId != worldSpaceId || aForceTrigger)
    {
        DetectGridCellChange(pWorldSpace, true);
        // If the world space changes, then we want to send out a CellChangeEvent out too.
        aForceTrigger = true;
    }
    else if (gameCenterGrid != m_centerGrid)
    {
        DetectGridCellChange(pWorldSpace, false);
    }

    if (gameCurrentGrid != m_currentGrid || aForceTrigger)
    {
        CellChangeEvent cellChangeEvent{};

        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, cellChangeEvent.WorldSpaceId))
        {
            spdlog::error("Failed to find world space id for form id {:X}", pWorldSpace->formID);
            return;
        }

        TESObjectCELL* pCell = pPlayer->GetParentCellEx();
        if (!pCell)
            pCell = ModManager::Get()->GetCellFromCoordinates(gameCurrentGrid.X, gameCurrentGrid.Y, pWorldSpace, false);

        if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellChangeEvent.CellId))
        {
            spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
            return;
        }

        cellChangeEvent.CurrentCoords = gameCurrentGrid;

        m_dispatcher.trigger(cellChangeEvent);

        m_currentGrid = gameCurrentGrid;
    }
}

void DiscoveryService::VisitInteriorCell(bool aForceTrigger) noexcept
{
    ResetCachedCellData();

    const uint32_t cellId = PlayerCharacter::Get()->GetParentCellEx()->formID;
    if (m_interiorCellId != cellId || aForceTrigger)
    {
        CellChangeEvent cellChangeEvent{};

        if (!m_world.GetModSystem().GetServerModId(cellId, cellChangeEvent.CellId))
        {
            spdlog::error("Failed to find cell id {:X}", cellId);
            return;
        }

        m_dispatcher.trigger(cellChangeEvent);
        m_interiorCellId = cellId;
    }
}

void DiscoveryService::DetectGridCellChange(TESWorldSpace* aWorldSpace, bool aNewCellGrid) noexcept
{
    GridCellChangeEvent changeEvent{};

    const uint32_t worldSpaceId = aWorldSpace->formID;
    changeEvent.WorldSpaceId = worldSpaceId;

    const TES* pTES = TES::Get();
    const int32_t startGridX = pTES->centerGridX - GridCellCoords::m_gridsToLoad / 2;
    const int32_t startGridY = pTES->centerGridY - GridCellCoords::m_gridsToLoad / 2;

    for (int32_t i = 0; i < GridCellCoords::m_gridsToLoad; ++i)
    {
        for (int32_t j = 0; j < GridCellCoords::m_gridsToLoad; ++j)
        {
            // If it is a new cell grid, don't check for previously loaded cells.
            if (!aNewCellGrid)
            {
                if (GridCellCoords::IsCellInGridCell(m_centerGrid, {startGridX + i, startGridY + j}, false))
                    continue;
            }

            const TESObjectCELL* pCell = ModManager::Get()->GetCellFromCoordinates(startGridX + i, startGridY + j, aWorldSpace, 0);

            if (!pCell)
            {
                spdlog::warn("Cell not found at coordinates ({}, {}) in worldspace {:X}", startGridX + i, startGridY + j, aWorldSpace->formID);
                continue;
            }

            GameId cellId{};
            if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
            {
                spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
                continue;
            }

            changeEvent.Cells.push_back(cellId);
        }
    }

    TESObjectCELL* pCell = PlayerCharacter::Get()->GetParentCellEx();
    if (!pCell)
        pCell = ModManager::Get()->GetCellFromCoordinates(pTES->currentGridX, pTES->currentGridY, aWorldSpace, false);

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, changeEvent.PlayerCell))
    {
        spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
        return;
    }

    changeEvent.CenterCoords = m_centerGrid = {pTES->centerGridX, pTES->centerGridY};

    // The server ranges every remote actor against this centre. If it disagrees with the grid the player actually
    // stands in, actors next to the player are withheld (see RangeDiag on the server, InterpDiag on the client).
    {
        const auto& position = PlayerCharacter::Get()->position;
        const auto standing = GridCellCoords::CalculateGridCellCoords(position.x, position.y);
        spdlog::info("Grid change: reporting centre ({}, {}), game's current grid ({}, {}), standing in ({}, {}) at ({:.0f}, {:.0f}), {} cells sent", pTES->centerGridX, pTES->centerGridY,
                     pTES->currentGridX, pTES->currentGridY, standing.X, standing.Y, position.x, position.y, changeEvent.Cells.size());
    }

    m_dispatcher.trigger(changeEvent);

    m_worldSpaceId = worldSpaceId;
}

void DiscoveryService::VisitForms() noexcept
{
    // Actors briefly lose their 3D when crossing the edge of the loaded cells (a circling dragon does
    // it every few seconds). Removing them right away made the server destroy and recreate them each
    // time, so a still existing actor is only removed after a grace period.
    constexpr auto cRemovalGracePeriod = std::chrono::seconds(5);

    ProcessLists* const pProcessLists = ProcessLists::Get();
    if (!pProcessLists)
        return;

    const auto now = std::chrono::steady_clock::now();
    const uint64_t visit = ++m_visitCounter;

    static Vector<uint32_t> s_removedForms;
    s_removedForms.clear();

    const auto visitor = [this, visit](TESObjectREFR* apReference)
    {
        const auto formId = apReference->formID;

        auto it = m_forms.find(formId);
        if (it != m_forms.end() && it->second.pReference != apReference)
        {
            // Same form id but another object (recycled temporary id, or the form was reloaded).
            m_dispatcher.trigger(ActorRemovedEvent(formId));
            m_forms.erase(it);
            it = m_forms.end();
        }

        if (it == m_forms.end())
        {
            m_forms[formId] = KnownForm{apReference, visit, {}};

            m_dispatcher.enqueue(ActorAddedEvent(formId));
        }
        else
        {
            it->second.LastSeenVisit = visit;
            it->second.MissingSince = {};
        }
    };

    for (uint32_t i = 0; i < pProcessLists->highActorHandleArray.length; ++i)
    {
        TESObjectREFR* const pRefr = TESObjectREFR::GetByHandle(pProcessLists->highActorHandleArray[i]);
        if (pRefr)
        {
            if (pRefr->GetNiNode())
            {
                visitor(pRefr);
            }
        }
    }

    // Not in actor holder
    visitor(PlayerCharacter::Get());

    // We dispatch removal events first to prevent needless reallocations
    for (auto& [formId, known] : m_forms)
    {
        if (known.LastSeenVisit == visit)
            continue;

        if (known.MissingSince == std::chrono::steady_clock::time_point{})
            known.MissingSince = now;

        const bool isSameObject = TESForm::GetById(formId) == static_cast<TESForm*>(known.pReference);

        // Copies of actors owned by another player are removed right away once their 3D is gone: the server may
        // already be asking to spawn them again (after a load door, for example), and that request is dropped for good
        // while the old copy still exists. This used to skip the grace period for every remote actor, including one
        // that still had its 3D and had merely dropped out of the high process list for a frame. Removing it cancelled
        // its server assignment, the next visit asked for a new one, and it fell out again: a remote follower could
        // thrash between removed and re-assigned about eleven times a second, which is what "not synced at all" looked
        // like. Still holding 3D means it is only flickering, so let it sit out the grace period like anything else.
        auto* pActor = isSameObject ? Cast<Actor>(known.pReference) : nullptr;
        const bool isRemote = pActor && pActor->GetExtension()->IsRemote();
        const bool isUnloaded = !pActor || !pActor->GetNiNode();

        if (!isSameObject || (isRemote && isUnloaded) || now - known.MissingSince >= cRemovalGracePeriod)
            s_removedForms.push_back(formId);
    }

    for (uint32_t formId : s_removedForms)
    {
        // A conform can remove both the 3D and the high-process handle. Keep the
        // existing discovery entry so rebuilding it does not cancel its assignment.
        // TODO: GetById performance in loop?
        if (auto* pActor = Cast<Actor>(TESForm::GetById(formId)); pActor && !pActor->IsDeleted())
        {
            using ReconciliationStage = ActorExtension::ReconciliationStage;
            const auto cStage = pActor->GetExtension()->Reconciliation;
            const auto* pCell = pActor->GetParentCellEx();
            // Finish the disable/enable pair even if the cell starts unloading.
            // Once enabled, an unloaded cell is a real removal.
            if (cStage == ReconciliationStage::WaitingForDisable || (cStage == ReconciliationStage::WaitingFor3D && pCell && pCell->IsAttached()))
            {
                continue;
            }
        }

        m_dispatcher.trigger(ActorRemovedEvent(formId));
        m_forms.erase(formId);
        m_dispatcher.trigger(ActorRemovedEvent(formId));
    }

    // Dispatch all adds
    m_dispatcher.update<ActorAddedEvent>();
}

void DiscoveryService::OnUpdate(const PreUpdateEvent& acUpdateEvent) noexcept
{
    PerfScope perfScope("DiscoveryService::OnUpdate");

    TP_UNUSED(acUpdateEvent);

    if (s_reannounceAt != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= s_reannounceAt)
    {
        s_reannounceAt = {};
        spdlog::info("Announcing this cell again: a remote copy was torn down here by a cell unloading, and the server does not know");
        VisitCell(true);
        VisitForms();
        return;
    }

    VisitCell();
    VisitForms();
}

void DiscoveryService::OnConnected(const ConnectedEvent& acEvent) noexcept
{
    // uGridsToLoad should always be 5, as this is what the server enforces
    auto* pSetting = INISettingCollection::Get()->GetSetting("uGridsToLoad:General");
    if (pSetting && pSetting->data != 5)
    {
        ConnectionErrorEvent errorEvent{};
        errorEvent.ErrorDetail = "{\"error\": \"bad_uGridsToLoad\"}";

        m_world.GetRunner().Trigger(errorEvent);
    }

    VisitCell(true);
}

BSTEventResult DiscoveryService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    spdlog::info("Finished loading, triggering visit cell");

#ifndef SKYRIMVR
    // A VR install always has more plugins (SKSE VR, the address library, ESL support), so this warning would fire
    // on every load, and it arrived as a "connection failed" notification.
    if (!IsDefaultModlist(ModManager::Get()->mods))
    {
        ConnectionErrorEvent errorEvent{};
        errorEvent.ErrorDetail = "{\"error\": \"non_default_install\"}";

        m_world.GetRunner().Trigger(errorEvent);
    }
#endif

    VisitCell(true);

    return BSTEventResult::kOk;
}

void DiscoveryService::ResetCachedCellData() noexcept
{
    m_worldSpaceId = 0;
    m_centerGrid.Reset();
    m_currentGrid.Reset();
}
