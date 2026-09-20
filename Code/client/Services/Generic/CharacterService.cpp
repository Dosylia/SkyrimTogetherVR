#include "Forms/TESObjectCELL.h"
#include <PerfScope.h>
#include "Forms/TESWorldSpace.h"
#include "Services/PapyrusService.h"
#include <Services/PartyService.h>

#include <Services/CharacterService.h>
#include <Services/QuestService.h>
#include <Services/TransportService.h>
#include <Services/InventoryService.h>

#include <Games/References.h>
#include <Games/Misc/SubtitleManager.h>

#include <Forms/TESNPC.h>
#include <Forms/TESQuest.h>

#include <BranchInfo.h>
#include <Components.h>

#include <Systems/InterpolationSystem.h>
#include <Systems/AnimationSystem.h>
#include <Systems/CacheSystem.h>
#include <Systems/FaceGenSystem.h>

#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/MountEvent.h>
#include <Events/InitPackageEvent.h>
#include <Events/BeastFormChangeEvent.h>
#include <Events/AddExperienceEvent.h>
#include <Events/DialogueEvent.h>
#include <Events/SubtitleEvent.h>
#include <Events/MoveActorEvent.h>
#include <Events/PartyJoinedEvent.h>

#include <Structs/ActionEvent.h>
#include <Messages/CancelAssignmentRequest.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifySpawnData.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/MountRequest.h>
#include <Messages/NotifyMount.h>
#include <Messages/NewPackageRequest.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/RequestRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/SyncExperienceRequest.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/DialogueRequest.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/SubtitleRequest.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/NotifyRelinquishControl.h>

#include <World.h>
#include <Games/Memory.h>
#include <Forms/ActorValueInfo.h>
#include <Games/TES.h>
#ifdef SKYRIMVR
#include <Games/Skyrim/VRBodySync.h>
#include <Games/Skyrim/Interface/UI.h>

#endif

namespace
{
// Levelled references the other player rolled as another kind of creature (a rabbit here, a snow fox there). Adopting
// the wrong creature froze it or made it slide; refusing it gave each player a private copy. So the owner's creature
// is spawned here as a copy (a temporary actor from the owner's base, the path summons already use) and the local
// reference becomes a "ghost": disabled while the copy stands in for it, never assigned to the server. Key: the
// ghost's form id; value: the copy's. The ghost is enabled again the moment its copy is deleted, on disconnect, or when
// this client goes away. Disable is saved with the reference, so a crash while a ghost is disabled leaves that one
// spawn point disabled in the save made during the session; a small, known cost (2026-09-19).
TiltedPhoques::Map<uint32_t, uint32_t> s_ghosts;

struct BaseMatch
{
    bool Differs = false;       // the owner's base is not this side's base
    bool Foreign = false;       // and it is another kind of creature
    TESNPC* pOwnerBase = nullptr;
};

// Same name first: a levelled list of humanoids mixes races freely (Nord and Breton bandits are both "Bandit"),
// and all of those share the humanoid skeleton and graph. Comparing races alone marked six Bandit/Bandit pairs
// as foreign on 2026-09-18 and left them without animations. Only when the names differ does the race decide
// (Elk vs Deer, Bear vs Frostbite Spider are foreign; Dragon vs Blood Dragon is not). Races are compared through
// Actor::race (read on VR by PlayerService) and TESNPC::raceForm (set on VR for every remote player's base by
// TESNPC::Initialize); if the two disagree for the local actor itself, that offset is not trusted.
BaseMatch CompareBase(World& aWorld, Actor* apActor, const GameId& acRemoteBaseId, const bool aLog) noexcept
{
    BaseMatch match{};
    if (acRemoteBaseId == GameId{})
        return match;

    const uint32_t cRemoteBaseId = aWorld.GetModSystem().GetGameId(acRemoteBaseId);

    const TESNPC* pLocalBase = Cast<TESNPC>(apActor->baseForm);
    if (pLocalBase && pLocalBase->IsTemporary())
        pLocalBase = pLocalBase->GetTemplateBase();

    if (!cRemoteBaseId || !pLocalBase || pLocalBase->formID == cRemoteBaseId)
        return match;

    TESNPC* pRemoteBase = Cast<TESNPC>(TESForm::GetById(cRemoteBaseId));
    if (!pRemoteBase)
        return match;

    match.Differs = true;
    match.pOwnerBase = pRemoteBase;

    const char* pLocalName = pLocalBase->fullName.value.AsAscii();
    const char* pRemoteName = pRemoteBase->fullName.value.AsAscii();

    const bool sameName = std::strcmp(pLocalName ? pLocalName : "", pRemoteName ? pRemoteName : "") == 0;

    const char* pHow;
    if (sameName)
    {
        match.Foreign = false;
        pHow = "name";
    }
    else if (apActor->race && pLocalBase->raceForm.race == apActor->race && pRemoteBase->raceForm.race)
    {
        match.Foreign = pRemoteBase->raceForm.race != apActor->race;
        pHow = "race";
    }
    else
    {
        match.Foreign = true;
        pHow = "name, race unreadable";
    }

    if (aLog)
        spdlog::info("Base form differs on reference {:X}: owner has {:X} ({}), this side {:X} ({}); {} (decided by {})", apActor->formID, cRemoteBaseId,
                     pRemoteName ? pRemoteName : "?", pLocalBase->formID, pLocalName ? pLocalName : "?",
                     match.Foreign ? "another kind of creature" : "same kind, synced normally", pHow);

    return match;
}

// The fallback when no copy could be made: adopt the local creature but never feed it the owner's animation data,
// which belongs to a graph it does not have (it slides instead of freezing).
void MarkForeignGraph(World& aWorld, const entt::entity aEntity, Actor* apActor, const GameId& acRemoteBaseId) noexcept
{
    if (!CompareBase(aWorld, apActor, acRemoteBaseId, true).Foreign)
        return;

    if (auto* pAnimation = aWorld.try_get<RemoteAnimationComponent>(aEntity))
        pAnimation->ForeignGraph = true;
    if (auto* pInterpolation = aWorld.try_get<InterpolationComponent>(aEntity))
        pInterpolation->ForeignGraph = true;
}

// Spawns the owner's creature and turns the local one into a ghost. Null when no copy could be made; the caller then
// adopts the local creature as before.
Actor* StandInForForeignCreature(Actor* apLocal, TESNPC* apOwnerBase) noexcept
{
    Actor* pCopy = Actor::Create(apOwnerBase);
    if (!pCopy)
    {
        spdlog::warn("Ghost: could not create a copy of {:X} for reference {:X}, adopting the local creature instead", apOwnerBase->formID, apLocal->formID);
        return nullptr;
    }

    const TESNPC* pLocalBase = Cast<TESNPC>(apLocal->baseForm);
    if (pLocalBase && pLocalBase->IsTemporary())
        pLocalBase = pLocalBase->GetTemplateBase();

    s_ghosts[apLocal->formID] = pCopy->formID;
    apLocal->Disable();

    spdlog::info("Ghost: reference {:X} rolled as {} ({:X}) here but the owner has {} ({:X}); copy {:X} stands in for it and the local one is disabled", apLocal->formID,
                 pLocalBase ? pLocalBase->fullName.value.AsAscii() : "?", pLocalBase ? pLocalBase->formID : 0, apOwnerBase->fullName.value.AsAscii(), apOwnerBase->formID,
                 pCopy->formID);
    return pCopy;
}

void EnableGhost(const uint32_t aGhostFormId) noexcept
{
    if (IsProcessExiting())
        return;

    Actor* pGhost = Cast<Actor>(TESForm::GetById(aGhostFormId));
    if (pGhost && pGhost->IsDisabled())
        pGhost->EnableImpl();
    spdlog::info("Ghost: reference {:X} is a normal local creature again{}", aGhostFormId, pGhost ? "" : " (not loaded)");
}

// Called for every temporary actor this client deletes: if it was a stand-in, its ghost comes back.
void ReleaseGhostOf(const uint32_t aCopyFormId) noexcept
{
    for (auto it = s_ghosts.begin(); it != s_ghosts.end(); ++it)
    {
        if (it->second != aCopyFormId)
            continue;
        const uint32_t cGhost = it->first;
        s_ghosts.erase(it);
        EnableGhost(cGhost);
        return;
    }
}

void ReleaseAllGhosts() noexcept
{
    for (const auto& [ghost, copy] : s_ghosts)
        EnableGhost(ghost);
    s_ghosts.clear();
}
//! The values a player's fresh copy starts with, health never below a quarter of its maximum.
//! The server hands a respawning player's copy the values it last stored, and those are the ones from his death
//! (-4, -16 on 2026-09-19 19:52 and 19:57). The copy is essential, so a copy born at -4 goes down on the spot. Starting
//! it at 1 (the first fix) was not enough: both copies that could be hit but not seen on 2026-09-20 (19:07:52 and
//! 19:10:55) started at 1 from a stored -30 and -11, and one stray hit or the landing after the spawn takes 1 health
//! to zero before the owner's real health arrives a second later. A copy never goes down; the owner's game decides.
ActorValues StandingValues(const ActorValues& acValues, const uint32_t aFormId) noexcept
{
    ActorValues values = acValues;
    auto it = values.ActorValuesList.find(ActorValueInfo::kHealth);
    if (it == values.ActorValuesList.end())
        return values;
    float floor = 25.f;
    if (const auto max = values.ActorMaxValuesList.find(ActorValueInfo::kHealth); max != values.ActorMaxValuesList.end() && max->second > 0.f)
        floor = std::max(floor, max->second * 0.25f);
    if (it->second < floor)
    {
        spdlog::info("Remote player {:X} copy would have spawned with the owner's health {:.0f} (a copy that starts down never gets up); started at {:.0f} instead", aFormId, it->second, floor);
        it.value() = floor;
    }
    return values;
}
} // namespace

CharacterService::CharacterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_referenceAddedConnection = m_dispatcher.sink<ActorAddedEvent>().connect<&CharacterService::OnActorAdded>(this);
    m_referenceRemovedConnection = m_dispatcher.sink<ActorRemovedEvent>().connect<&CharacterService::OnActorRemoved>(this);

    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&CharacterService::OnUpdate>(this);
    m_actionConnection = m_dispatcher.sink<ActionEvent>().connect<&CharacterService::OnActionEvent>(this);

    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&CharacterService::OnConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&CharacterService::OnDisconnected>(this);

    m_assignCharacterConnection = m_dispatcher.sink<AssignCharacterResponse>().connect<&CharacterService::OnAssignCharacter>(this);
    m_characterSpawnConnection = m_dispatcher.sink<CharacterSpawnRequest>().connect<&CharacterService::OnCharacterSpawn>(this);
    m_referenceMovementSnapshotConnection = m_dispatcher.sink<ServerReferencesMoveRequest>().connect<&CharacterService::OnReferencesMoveRequest>(this);
    m_factionsConnection = m_dispatcher.sink<NotifyFactionsChanges>().connect<&CharacterService::OnFactionsChanges>(this);
    m_ownershipTransferConnection = m_dispatcher.sink<NotifyOwnershipTransfer>().connect<&CharacterService::OnOwnershipTransfer>(this);
    m_removeCharacterConnection = m_dispatcher.sink<NotifyRemoveCharacter>().connect<&CharacterService::OnRemoveCharacter>(this);
    m_remoteSpawnDataReceivedConnection = m_dispatcher.sink<NotifySpawnData>().connect<&CharacterService::OnRemoteSpawnDataReceived>(this);

    m_mountConnection = m_dispatcher.sink<MountEvent>().connect<&CharacterService::OnMountEvent>(this);
    m_notifyMountConnection = m_dispatcher.sink<NotifyMount>().connect<&CharacterService::OnNotifyMount>(this);

    m_initPackageConnection = m_dispatcher.sink<InitPackageEvent>().connect<&CharacterService::OnInitPackageEvent>(this);
    m_newPackageConnection = m_dispatcher.sink<NotifyNewPackage>().connect<&CharacterService::OnNotifyNewPackage>(this);

    m_notifyRespawnConnection = m_dispatcher.sink<NotifyRespawn>().connect<&CharacterService::OnNotifyRespawn>(this);
    m_beastFormChangeConnection = m_dispatcher.sink<BeastFormChangeEvent>().connect<&CharacterService::OnBeastFormChange>(this);

    m_addExperienceEventConnection = m_dispatcher.sink<AddExperienceEvent>().connect<&CharacterService::OnAddExperienceEvent>(this);
    m_syncExperienceConnection = m_dispatcher.sink<NotifySyncExperience>().connect<&CharacterService::OnNotifySyncExperience>(this);

    m_dialogueEventConnection = m_dispatcher.sink<DialogueEvent>().connect<&CharacterService::OnDialogueEvent>(this);
    m_dialogueSyncConnection = m_dispatcher.sink<NotifyDialogue>().connect<&CharacterService::OnNotifyDialogue>(this);

    m_subtitleEventConnection = m_dispatcher.sink<SubtitleEvent>().connect<&CharacterService::OnSubtitleEvent>(this);
    m_subtitleSyncConnection = m_dispatcher.sink<NotifySubtitle>().connect<&CharacterService::OnNotifySubtitle>(this);

    m_actorTeleportConnection = m_dispatcher.sink<NotifyActorTeleport>().connect<&CharacterService::OnNotifyActorTeleport>(this);

    m_relinquishConnection = m_dispatcher.sink<NotifyRelinquishControl>().connect<&CharacterService::OnNotifyRelinquishControl>(this);

    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&CharacterService::OnPartyJoinedEvent>(this);
}

void CharacterService::DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept
{
    m_world.remove<FaceGenComponent, InterpolationComponent, RemoteAnimationComponent, RemoteComponent, CacheComponent, WaitingFor3D, PlayerComponent>(aEntity);
}

bool CharacterService::TakeOwnership(const uint32_t acFormId, const uint32_t acServerId, const entt::entity acEntity) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(acFormId));
    if (!pActor)
    {
        spdlog::error("Cannot find actor to take control over, form id: {:X}, server id: {:X}", acFormId, acServerId);
        return false;
    }

    ActorExtension* pExtension = pActor->GetExtension();
    if (pExtension->IsRemotePlayer())
    {
        spdlog::error("Cannot take control over remote player actor, form id: {:X}, server id: {:X}", acFormId, acServerId);
        return false;
    }

    if (pActor->IsPlayerSummon())
    {
        spdlog::error("Cannot take control over remote player summon, form id: {:X}, server id: {:X}", acFormId, acServerId);
        return false;
    }

    pExtension->SetRemote(false);

    // TODO(cosideci): this should be done differently.
    // Send an ownership claim request, and have the server broadcast the result.
    // Only then should components be added or removed.
    m_world.emplace_or_replace<LocalComponent>(acEntity, acServerId);
    m_world.emplace_or_replace<LocalAnimationComponent>(acEntity);
    DeleteRemoteEntityComponents(acEntity);

    RequestOwnershipClaim request;
    request.ServerId = acServerId;
    request.NewActorData = BuildActorData(pActor);

    m_transport.Send(request);

    return true;
}

void CharacterService::DeleteTempActor(const uint32_t aFormId) noexcept
{
    if (IsProcessExiting())
        return;

    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (pActor && ((pActor->formID & 0xFF000000) == 0xFF000000))
    {
        pActor->Delete();
        spdlog::info("\tDeleted actor {:X}", aFormId);
        ReleaseGhostOf(aFormId);
    }
}

void CharacterService::OnActorAdded(const ActorAddedEvent& acEvent) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));

    if (acEvent.FormId == 0x14)
    {
        pActor->GetExtension()->SetPlayer(true);
    }

    entt::entity entity;

    const auto view = m_world.view<RemoteComponent>();
    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [&acEvent, view](entt::entity entity)
        {
            auto& remoteComponent = view.get<RemoteComponent>(entity);
            return remoteComponent.CachedRefId == acEvent.FormId;
        });

    if (it != std::end(view))
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));
        pActor->GetExtension()->SetRemote(true);

        entity = *it;
    }
    else
        entity = m_world.create();

    m_world.emplace_or_replace<FormIdComponent>(entity, acEvent.FormId);
    m_world.emplace_or_replace<EarlyAnimationBufferComponent>(entity);

    ProcessNewEntity(entity);
}

void CharacterService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    auto view = m_world.view<FormIdComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.FormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == formId; });

    if (entityIt == view.end())
    {
        spdlog::error("Actor to remove not found in form ids map {:X}", acEvent.FormId);
        return;
    }

    const auto cId = *entityIt;

    auto& formIdComponent = view.get<FormIdComponent>(cId);
    CancelServerAssignment(*entityIt, formIdComponent.Id);

    m_world.remove<EarlyAnimationBufferComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

#ifdef SKYRIMVR
    VRBodySync::ClearRemotePose(acEvent.FormId);
#endif

    spdlog::info("Actor removed, form id: {:X}", acEvent.FormId);
}

void CharacterService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    {
        PerfScope perfScope("CharacterService::RunSpawnUpdates");
        RunSpawnUpdates();
    }
    {
        PerfScope perfScope("CharacterService::RunLocalUpdates");
        RunLocalUpdates();
    }
    {
        PerfScope perfScope("CharacterService::RunEnemyMeterUpdates");
        RunEnemyMeterUpdates();
    }
    {
        PerfScope perfScope("CharacterService::RunRemotePlayerDiag");
        RunRemotePlayerDiag();
    }
    {
        PerfScope perfScope("CharacterService::RunFactionsUpdates");
        RunFactionsUpdates();
    }
    {
        PerfScope perfScope("CharacterService::RunRemoteUpdates");
        RunRemoteUpdates();
    }
    {
        PerfScope perfScope("CharacterService::RunExperienceUpdates");
        RunExperienceUpdates();
    }
    {
        PerfScope perfScope("CharacterService::ApplyCachedWeaponDraws");
        ApplyCachedWeaponDraws(acUpdateEvent);
    }
}

void CharacterService::OnConnected(const ConnectedEvent& acConnectedEvent) const noexcept
{
    // Go through all the forms that were previously detected
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    Vector<entt::entity> entities(view.begin(), view.end());

    for (auto entity : entities)
    {
        auto& formIdComponent = m_world.get<FormIdComponent>(entity);
        // Delete all temporary actors on connect
        if (formIdComponent.Id > 0xFF000000)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (pActor)
                pActor->Delete();

            continue;
        }

        ProcessNewEntity(entity);
    }
}

void CharacterService::OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) const noexcept
{
    // The disconnect that comes with quitting arrives while the game is tearing itself down. Deleting remote players
    // and flipping actors back to local then fires equip events into plugins whose singletons are already gone:
    // every quit crash on 2026-09-18/19 was Enchantment Art Extender reading the destroyed UI singleton from its
    // equip handler (SkyrimVR+0x1F83200, +0x160 = numPausesGame), and the ENB effect-shader light plugin freeing
    // shader art the same way. The process is ending; the actors do not need putting back.
    if (IsProcessExiting())
    {
        spdlog::info("Disconnected while the game is exiting; actors are left alone");
        return;
    }

    auto remoteView = m_world.view<FormIdComponent, RemoteComponent>();
    for (auto entity : remoteView)
    {
        auto& formIdComponent = remoteView.get<FormIdComponent>(entity);

        auto pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        if (pActor->GetExtension()->IsRemotePlayer())
            pActor->Delete();
        else
            pActor->GetExtension()->SetRemote(false);
    }

    m_world.clear<WaitingForAssignmentComponent, LocalComponent, RemoteComponent>();

    ReleaseAllGhosts();
}

void CharacterService::OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept
{
    spdlog::debug("Received for cookie {:X}, server id {:X}", acMessage.Cookie, acMessage.ServerId);

    auto view = m_world.view<WaitingForAssignmentComponent>();
    const auto itor = std::find_if(std::begin(view), std::end(view), [view, cookie = acMessage.Cookie](auto entity) { return view.get<WaitingForAssignmentComponent>(entity).Cookie == cookie; });

    if (itor == std::end(view))
    {
        spdlog::warn("Never found requested cookie: {}", acMessage.Cookie);
        return;
    }

    const auto cEntity = *itor;

    m_world.remove<WaitingForAssignmentComponent>(cEntity);
#if (!IS_MASTER)
    m_world.remove<ReplayedActionsDebugComponent>(cEntity);
#endif

    const auto formIdComponent = m_world.try_get<FormIdComponent>(cEntity);
    if (!formIdComponent)
    {
        spdlog::error(__FUNCTION__ ": form id component doesn't exist, cookie: {:X}", acMessage.Cookie);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent->Id));
    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": actor not found, form id: {:X}", formIdComponent->Id);
        m_world.destroy(cEntity);
        return;
    }

    // TODO: how could this possibly trigger?
    // it's kinda interfering with my WaitingFor3D code
    if (acMessage.PlayerId != 0)
        m_world.emplace_or_replace<PlayerComponent>(cEntity, acMessage.PlayerId);

    if (acMessage.Owner)
    {
        spdlog::debug("Received local actor, form id: {:X}", pActor->formID);

        m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId);
        auto& localAnimationComponent = m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);

        pActor->GetExtension()->SetRemote(false);

        if (auto* pEarlyAnimComponent = m_world.try_get<EarlyAnimationBufferComponent>(cEntity))
        {
            for (const auto& action : pEarlyAnimComponent->Actions)
            {
                localAnimationComponent.Append(action);
            }
        }
        m_world.remove<EarlyAnimationBufferComponent>(cEntity);
    }
    else
    {
        spdlog::info("Received remote actor, form id: {:X}, isweapondrawn: {}", pActor->formID, acMessage.IsWeaponDrawn);

        // Another kind of creature than the owner's (this side asked first, the response carries the owner's base):
        // a copy stands in, set up through the deferred path a temporary actor takes, and the local reference keeps
        // its bare entity as a ghost.
        if (const BaseMatch match = CompareBase(m_world, pActor, acMessage.BaseId, false); match.Foreign && match.pOwnerBase)
        {
            if (Actor* pCopy = StandInForForeignCreature(pActor, match.pOwnerBase))
            {
                CharacterSpawnRequest spawn{};
                spawn.ServerId = acMessage.ServerId;
                spawn.BaseId = acMessage.BaseId;
                spawn.Position = acMessage.Position;
                spawn.CellId = acMessage.CellId;
                spawn.InitialActorValues = acMessage.AllActorValues;
                spawn.InventoryContent = acMessage.CurrentInventory;
                spawn.ActionsToReplay = acMessage.ActionsToReplay;
                spawn.PlayerId = acMessage.PlayerId;
                spawn.IsDead = acMessage.IsDead;
                spawn.IsWeaponDrawn = acMessage.IsWeaponDrawn;

                const entt::entity copyEntity = m_world.create();
                m_world.emplace<RemoteComponent>(copyEntity, acMessage.ServerId, pCopy->formID);
                pCopy->MoveTo(PlayerCharacter::Get()->parentCell, acMessage.Position);
                pCopy->SetActorValues(acMessage.AllActorValues);
                auto& copyInterpolation = InterpolationSystem::Setup(m_world, copyEntity);
                copyInterpolation.Position = acMessage.Position;
                AnimationSystem::Setup(m_world, copyEntity);
                m_world.emplace<WaitingFor3D>(copyEntity, spawn);
                return;
            }
        }

        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, formIdComponent->Id);

        pActor->GetExtension()->SetRemote(true);

        m_world.remove<EarlyAnimationBufferComponent>(cEntity);
        InterpolationSystem::Setup(m_world, cEntity);
        AnimationSystem::Setup(m_world, cEntity);
        AnimationSystem::AddActionsForReplay(m_world.get<RemoteAnimationComponent>(cEntity), acMessage.ActionsToReplay);
        // This path adopted a levelled reference without ever seeing the owner's base; the response carries it now.
        MarkForeignGraph(m_world, cEntity, pActor, acMessage.BaseId);

#if (!IS_MASTER)
        m_world.emplace_or_replace<ReplayedActionsDebugComponent>(cEntity, acMessage.ActionsToReplay);
#endif

        pActor->SetActorValues(acMessage.AllActorValues);
        pActor->SetActorInventory(acMessage.CurrentInventory);
        if (pActor->GetExtension()->IsRemotePlayer())
            InventoryService::ApplyHandEquipment(pActor, acMessage.CurrentInventory, true);

        if (pActor->IsDead() != acMessage.IsDead)
            acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

        m_weaponDrawUpdates[pActor->formID] = {acMessage.IsWeaponDrawn};

        MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);
    }
}

void CharacterService::OnCharacterSpawn(const CharacterSpawnRequest& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent>();
    const auto remoteItor = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ServerId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteItor != std::end(remoteView))
    {
        const entt::entity cExisting = *remoteItor;
        const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cExisting);
        const bool hasActor = m_world.all_of<WaitingFor3D>(cExisting) || (pFormIdComponent && TESForm::GetById(pFormIdComponent->Id));

        if (hasActor)
        {
            spdlog::warn("Character with remote id {:X} is already spawned.", acMessage.ServerId);
            return;
        }

        // The server still counts this character as spawned here, but the local copy is gone. Spawn it again, or
        // it stays invisible for the rest of the session.
        spdlog::info("Character with remote id {:X} has no actor anymore, spawning it again", acMessage.ServerId);
        DeleteRemoteEntityComponents(cExisting);
        if (m_world.orphan(cExisting))
            m_world.destroy(cExisting);
    }

    Actor* pActor = nullptr;

    std::optional<entt::entity> entity;

    // Custom forms
    if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        entity = m_world.create();

        if (acMessage.BaseId != GameId{})
        {
            const auto cNpcId = World::Get().GetModSystem().GetGameId(acMessage.BaseId);
            if (cNpcId == 0)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod, base: {:X}:{:X}, form: {:X}:{:X}", acMessage.BaseId.BaseId, acMessage.BaseId.ModId, acMessage.FormId.BaseId, acMessage.FormId.ModId);
                return;
            }

            pNpc = Cast<TESNPC>(TESForm::GetById(cNpcId));
            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            // Players and npcs with temporary ref ids and base ids (usually random events)
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, *entity, acMessage.FaceTints);
        }

        pActor = Actor::Create(pNpc);
    }
    else
    {
        const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);

        auto waitingView = m_world.view<FormIdComponent, WaitingForAssignmentComponent>();
        const auto waitingItor = std::find_if(std::begin(waitingView), std::end(waitingView), [waitingView, cActorId](auto entity) { return waitingView.get<FormIdComponent>(entity).Id == cActorId; });

        if (waitingItor != std::end(waitingView))
        {
            spdlog::info("Character with form id {:X} already has a spawn request in progress.", cActorId);
            return;
        }

        auto* const pForm = TESForm::GetById(cActorId);
        pActor = Cast<Actor>(pForm);

        if (!pActor)
        {
            // The reference is not loaded on this side (its cell outside our grid, or a placed reference we lack), yet
            // the owner drives it and it can walk up and hit us unseen: Novice Conjurer 10DE94 failed here five times
            // between 14:49 and 14:51 on 2026-09-20 while "something invisible" attacked. A copy of the owner's base
            // stands in, registered as a ghost so the local reference is disabled if it turns up later.
            TESNPC* pOwnerBase = acMessage.BaseId != GameId{} ? Cast<TESNPC>(TESForm::GetById(World::Get().GetModSystem().GetGameId(acMessage.BaseId))) : nullptr;
            Actor* pCopy = pOwnerBase ? Actor::Create(pOwnerBase) : nullptr;
            if (!pCopy)
            {
                spdlog::error("Failed to retrieve Actor {:X}, it will not be spawned, possibly missing mod", cActorId);
                spdlog::error("\tForm : {:X}", pForm ? pForm->formID : 0);
                return;
            }
            s_ghosts[cActorId] = pCopy->formID;
            spdlog::info("Stand-in: reference {:X} is not loaded here; copy {:X} of the owner's {} ({:X}) stands in for it", cActorId, pCopy->formID, pOwnerBase->fullName.value.AsAscii(),
                         pOwnerBase->formID);
            pActor = pCopy;
            entity = m_world.create();
        }

        // Another kind of creature than the owner's: a copy of the owner's stands in, the local one becomes a ghost.
        // The local reference keeps its own entity (a bare FormIdComponent that ProcessNewEntity skips).
        if (const BaseMatch match = CompareBase(m_world, pActor, acMessage.BaseId, false); match.Foreign && match.pOwnerBase)
        {
            if (Actor* pCopy = StandInForForeignCreature(pActor, match.pOwnerBase))
            {
                pActor = pCopy;
                entity = m_world.create();
            }
        }

        if (!entity)
        {
            const auto view = m_world.view<FormIdComponent>();
            const auto itor = std::find_if(std::begin(view), std::end(view), [cActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == cActorId; });

            if (itor != std::end(view))
                entity = *itor;
            else
                entity = m_world.create();
        }
    }

    if (!pActor)
    {
        spdlog::error("Actor object {:X} could not be created.", acMessage.ServerId);
        return;
    }

    // The base name is here so two players' logs can be compared by form id: leveled creatures can resolve to a
    // different animal on each client (a fox on one screen, a rabbit on the other).
    const auto* pSpawnedBase = Cast<TESNPC>(pActor->baseForm);
    spdlog::info("CharacterSpawnRequest, server id: {:X}, form id: {:X}, local base {:X} ({})", acMessage.ServerId, pActor->formID, pActor->baseForm ? pActor->baseForm->formID : 0,
                 pSpawnedBase ? pSpawnedBase->fullName.value.AsAscii() : "?");

    if (pActor->IsDisabled())
    {
        spdlog::warn("Disabled actor is being re-enabled: {:X}", pActor->formID);
        pActor->EnableImpl();
    }

    pActor->GetExtension()->SetRemote(true);

    pActor->rotation.x = acMessage.Rotation.x;
    pActor->rotation.z = acMessage.Rotation.y;
    pActor->MoveTo(PlayerCharacter::Get()->parentCell, acMessage.Position);
    pActor->SetActorValues(acMessage.IsPlayer ? StandingValues(acMessage.InitialActorValues, pActor->formID) : acMessage.InitialActorValues);

    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    if (acMessage.IsPlayer)
    {
        pActor->SetIgnoreFriendlyHit(true);
        pActor->SetPlayerRespawnMode();
        // The copy stays essential (it cannot die here), but it may get up again: when its owner's real health arrives
        // after a knock-down, the game lets it recover instead of leaving it down and undrawn for the session.
        pActor->SetNoBleedoutRecovery(false);
        m_world.emplace_or_replace<PlayerComponent>(*entity, acMessage.PlayerId);
    }

    if (pActor->IsDead() != acMessage.IsDead)
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

    spdlog::info("Spawn Request Is summon {}", acMessage.IsPlayerSummon);

    if (acMessage.IsPlayerSummon)
    {
        // Prevents remote summons agroing other players.
        pActor->SetCommandingActor(PlayerCharacter::Get()->GetHandle());
    }

    auto& remoteComponent = m_world.emplace_or_replace<RemoteComponent>(*entity, acMessage.ServerId, pActor->formID);

    auto& interpolationComponent = InterpolationSystem::Setup(m_world, *entity);
    interpolationComponent.Position = acMessage.Position;

    AnimationSystem::Setup(m_world, *entity);
    MarkForeignGraph(m_world, *entity, pActor, acMessage.BaseId);

    m_world.emplace_or_replace<WaitingFor3D>(*entity, acMessage);

    auto& remoteAnimationComponent = m_world.get<RemoteAnimationComponent>(*entity);

    AnimationSystem::AddActionsForReplay(remoteAnimationComponent, acMessage.ActionsToReplay);

#if (!IS_MASTER)
    m_world.emplace_or_replace<ReplayedActionsDebugComponent>(*entity, acMessage.ActionsToReplay);
#endif
}

void CharacterService::OnRemoteSpawnDataReceived(const NotifySpawnData& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);

    const auto itor = std::find_if(
        std::begin(view), std::end(view),
        [view, id = acMessage.Id](auto entity)
        {
            if (auto serverId = Utils::GetServerId(entity))
            {
                if (*serverId == id)
                    return true;
            }
            return false;
        });

    if (itor == std::end(view))
        return;

    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(*itor))
    {
        pWaitingFor3D->SpawnRequest.InitialActorValues = acMessage.NewActorData.InitialActorValues;
        pWaitingFor3D->SpawnRequest.InventoryContent = acMessage.NewActorData.InitialInventory;
        pWaitingFor3D->SpawnRequest.IsDead = acMessage.NewActorData.IsDead;
        pWaitingFor3D->SpawnRequest.IsWeaponDrawn = acMessage.NewActorData.IsWeaponDrawn;
    }

    auto& formIdComponent = view.get<FormIdComponent>(*itor);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    pActor->SetActorValues(acMessage.NewActorData.InitialActorValues);
    pActor->SetActorInventory(acMessage.NewActorData.InitialInventory);
    m_weaponDrawUpdates[pActor->formID] = {acMessage.NewActorData.IsWeaponDrawn};

    if (pActor->IsDead() != acMessage.NewActorData.IsDead)
        acMessage.NewActorData.IsDead ? pActor->Kill() : pActor->Respawn();

    spdlog::info("Applied remote spawn data, actor form id: {:X}", pActor->formID);
}

void CharacterService::OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept
{
    auto view = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent>();

    for (const auto& [serverId, update] : acMessage.Updates)
    {
        auto itor = std::find_if(std::begin(view), std::end(view), [serverId = serverId, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == serverId; });

        if (itor == std::end(view))
            continue;

        auto& interpolationComponent = view.get<InterpolationComponent>(*itor);
        auto& animationComponent = view.get<RemoteAnimationComponent>(*itor);
        const auto& movement = update.UpdatedMovement;

        InterpolationComponent::TimePoint point;
        point.Tick = acMessage.Tick;
        point.Position = movement.Position;
        point.Rotation = {movement.Rotation.x, 0.f, movement.Rotation.y};
        point.Variables = movement.Variables;
        point.Direction = movement.Direction;
        point.VRPoseData = update.UpdatedVRPose;

        InterpolationSystem::AddPoint(interpolationComponent, point);

        for (const auto& action : update.ActionEvents)
        {
            animationComponent.TimePoints.push_back(action);
        }
    }
}

void CharacterService::OnActionEvent(const ActionEvent& acActionEvent) const noexcept
{
    auto view = m_world.view<LocalAnimationComponent, FormIdComponent>();
    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acActionEvent.ActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (itor != std::end(view))
    {
        auto& localComponent = view.get<LocalAnimationComponent>(*itor);

        localComponent.Append(acActionEvent);
    }
    else if (m_transport.IsOnline())
    {
        // A `LocalAnimationComponent` is not attached yet, but the actor already exists and is running animations

        auto view = m_world.view<FormIdComponent, EarlyAnimationBufferComponent>();
        const auto itor = std::find_if(std::begin(view), std::end(view), [id = acActionEvent.ActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

        if (itor != std::end(view))
        {
            view.get<EarlyAnimationBufferComponent>(*itor).Actions.push_back(acActionEvent);
        }
    }
}

void CharacterService::OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent, CacheComponent>();

    for (const auto& [id, factions] : acEvent.Changes)
    {
        const auto itor = std::find_if(std::begin(view), std::end(view), [id = id, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

        if (itor != std::end(view))
        {
            auto& formIdComponent = view.get<FormIdComponent>(*itor);

            auto* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (!pActor)
                return;

            auto& cacheComponent = view.get<CacheComponent>(*itor);
            cacheComponent.FactionsContent = factions;

            pActor->SetFactions(cacheComponent.FactionsContent);
        }
    }
}

void CharacterService::OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) const noexcept
{
    // TODO(cosideci): handle case if no one has it, therefore no RemoteComponent
    auto view = m_world.view<RemoteComponent, FormIdComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [&acMessage, &view](auto entity) { return view.get<RemoteComponent>(entity).Id == acMessage.ServerId; });

    if (itor != std::end(view))
    {
        auto& formIdComponent = view.get<FormIdComponent>(*itor);

        // A reference that rolled as another creature here stands behind a ghost copy of the owner's kind. Taking it
        // over would put our own roll in charge and flip the creature for everyone (a wolf on his side became a troll
        // on hers after a hand-off, 2026-09-20 11:50). Refused: the server marks us invalid for it and keeps looking.
        if (const auto ghost = s_ghosts.find(formIdComponent.Id); ghost != s_ghosts.end())
        {
            const auto* pLocal = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            const auto* pCopy = Cast<Actor>(TESForm::GetById(ghost->second));
            const auto* pLocalBase = pLocal ? Cast<TESNPC>(pLocal->baseForm) : nullptr;
            const auto* pCopyBase = pCopy ? Cast<TESNPC>(pCopy->baseForm) : nullptr;
            spdlog::info("Hand-off of {:X} (server id {:X}) declined: it rolled as {} here, the owner's kind is {}; taking it would flip it", formIdComponent.Id, acMessage.ServerId,
                         pLocalBase ? pLocalBase->fullName.value.AsAscii() : "?", pCopyBase ? pCopyBase->fullName.value.AsAscii() : "?");
            RequestOwnershipTransfer request{};
            request.ServerId = acMessage.ServerId;
            m_transport.Send(request);
            return;
        }

        if (TakeOwnership(formIdComponent.Id, acMessage.ServerId, *itor))
        {
            spdlog::info("Ownership claimed {:X}", acMessage.ServerId);
            return;
        }
    }

    spdlog::warn("Actor for ownership transfer not found {:X}", acMessage.ServerId);

    RequestOwnershipTransfer request{};
    request.ServerId = acMessage.ServerId;

    m_transport.Send(request);
}

void CharacterService::OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) const noexcept
{
    auto view = m_world.view<RemoteComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acMessage.ServerId, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

    if (itor != std::end(view))
    {
        // TEMPORARY (2026-09-20): a player copy the server takes away, with where we stand (see WorldDiag on the server).
        if (m_world.all_of<PlayerComponent>(*itor))
        {
            auto* pPlayer = PlayerCharacter::Get();
            const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*itor);
            spdlog::info("WorldDiag: server removed player copy {:X} (actor {:X}) while we stand in worldspace {:X} cell {:X}", acMessage.ServerId, pFormIdComponent ? pFormIdComponent->Id : 0,
                         pPlayer && pPlayer->GetWorldSpace() ? pPlayer->GetWorldSpace()->formID : 0, pPlayer && pPlayer->parentCell ? pPlayer->parentCell->formID : 0);
        }

        if (auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*itor))
            CharacterService::DeleteTempActor(pFormIdComponent->Id);

        DeleteRemoteEntityComponents(*itor);
    }
}

void CharacterService::OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(), [view, id = acMessage.ActorId](auto aEntity) { return view.get<RemoteComponent>(aEntity).Id == id; });

    if (entityIt == view.end())
    {
        spdlog::error("Actor to respawn not found in: {:X}", acMessage.ActorId);
        return;
    }

    const auto cId = *entityIt;

    auto& formIdComponent = view.get<FormIdComponent>(cId);
    CancelServerAssignment(*entityIt, formIdComponent.Id);

    m_world.remove<EarlyAnimationBufferComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

    RequestRespawn request;
    request.ActorId = acMessage.ActorId;

    m_transport.Send(request);
}

void CharacterService::OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept
{
    auto view = m_world.view<FormIdComponent>();

    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*it);
    if (!serverIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    uint32_t serverId = serverIdRes.value();

    RequestRespawn request;
    request.ActorId = serverId;

    Actor* pActor = Utils::GetByServerId<Actor>(serverId);
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor for server id {:X}", serverId);
        return;
    }

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor baseform for server id {:X}", serverId);
        return;
    }

    pNpc->Serialize(&request.AppearanceBuffer);
    request.ChangeFlags = pNpc->GetChangeFlags();

    m_transport.Send(request);
}

void CharacterService::OnMountEvent(const MountEvent& acEvent) const noexcept
{
    auto view = m_world.view<FormIdComponent>();

    const auto riderIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.RiderID, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (riderIt == std::end(view))
    {
        spdlog::warn("Rider not found, form id: {:X}", acEvent.RiderID);
        return;
    }

    const entt::entity cRiderEntity = *riderIt;

    std::optional<uint32_t> riderServerIdRes = Utils::GetServerId(cRiderEntity);
    if (!riderServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    const auto mountIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.MountID, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (mountIt == std::end(view))
    {
        spdlog::warn("Mount not found, form id: {:X}", acEvent.MountID);
        return;
    }

    const entt::entity cMountEntity = *mountIt;

    std::optional<uint32_t> mountServerIdRes = Utils::GetServerId(cMountEntity);
    if (!mountServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    if (m_world.try_get<RemoteComponent>(cMountEntity))
        TakeOwnership(acEvent.MountID, *mountServerIdRes, cMountEntity);

    MountRequest request;
    request.MountId = mountServerIdRes.value();
    request.RiderId = riderServerIdRes.value();

    m_transport.Send(request);
}

void CharacterService::OnNotifyMount(const NotifyMount& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();

    const auto riderIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.RiderId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (riderIt == std::end(remoteView))
    {
        spdlog::warn("Rider with remote id {:X} not found.", acMessage.RiderId);
        return;
    }

    auto& riderFormIdComponent = remoteView.get<FormIdComponent>(*riderIt);
    TESForm* pRiderForm = TESForm::GetById(riderFormIdComponent.Id);
    Actor* pRider = Cast<Actor>(pRiderForm);

    Actor* pMount = nullptr;

    auto formView = m_world.view<FormIdComponent>();
    Vector<entt::entity> entities(formView.begin(), formView.end());

    // TODO(cosideci): remove this, cause of NotifyRelinquishControl?
    for (auto entity : entities)
    {
        std::optional<uint32_t> serverIdRes = Utils::GetServerId(entity);
        if (!serverIdRes.has_value())
        {
            spdlog::error("{}: failed to find server id", __FUNCTION__);
            continue;
        }

        uint32_t serverId = serverIdRes.value();

        if (serverId == acMessage.MountId)
        {
            auto& mountFormIdComponent = m_world.get<FormIdComponent>(entity);

            if (m_world.all_of<LocalComponent>(entity))
            {
                m_world.remove<LocalAnimationComponent, LocalComponent>(entity);
                m_world.emplace_or_replace<RemoteComponent>(entity, acMessage.MountId, mountFormIdComponent.Id);
            }

            TESForm* pMountForm = TESForm::GetById(mountFormIdComponent.Id);
            pMount = Cast<Actor>(pMountForm);
            pMount->GetExtension()->SetRemote(true);

            InterpolationSystem::Setup(m_world, entity);
            AnimationSystem::Setup(m_world, entity);

            break;
        }
    }

    pRider->InitiateMountPackage(pMount);
}

void CharacterService::OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto actorIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.ActorId, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (actorIt == std::end(view))
        return;

    const entt::entity cActorEntity = *actorIt;

    std::optional<uint32_t> actorServerIdRes = Utils::GetServerId(cActorEntity);
    if (!actorServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    NewPackageRequest request;
    request.ActorId = actorServerIdRes.value();
    if (!m_world.GetModSystem().GetServerModId(acEvent.PackageId, request.PackageId.ModId, request.PackageId.BaseId))
        return;

    m_transport.Send(request);
}

void CharacterService::OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ActorId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteIt == std::end(remoteView))
    {
        spdlog::warn("Actor for package with remote id {:X} not found.", acMessage.ActorId);
        return;
    }

    auto formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);

    const TESForm* pForm = TESForm::GetById(formIdComponent.Id);
    Actor* pActor = Cast<Actor>(pForm);

    const uint32_t cPackageFormId = World::Get().GetModSystem().GetGameId(acMessage.PackageId);
    const TESForm* pPackageForm = TESForm::GetById(cPackageFormId);
    if (!pPackageForm)
    {
        spdlog::warn("Actor package not found, base id: {:X}, mod id: {:X}", acMessage.PackageId.BaseId, acMessage.PackageId.ModId);
        return;
    }

    TESPackage* pPackage = Cast<TESPackage>(pPackageForm);

    pActor->SetPackage(pPackage);
}

void CharacterService::OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept
{
    m_cachedExperience += acEvent.Experience;
}

void CharacterService::OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    if (PlayerCharacter::LastUsedCombatSkill == -1)
        return;

    pPlayer->AddSkillExperience(PlayerCharacter::LastUsedCombatSkill, acMessage.Experience);
}

void CharacterService::OnDialogueEvent(const DialogueEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.ActorID](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (entityIt == view.end())
        return;

    auto serverIdRes = Utils::GetServerId(*entityIt);
    if (!serverIdRes)
    {
        spdlog::error("{}: server id not found for form id {:X}", __FUNCTION__, acEvent.ActorID);
        return;
    }

    DialogueRequest request{};
    request.ServerId = serverIdRes.value();
    request.SoundFilename = acEvent.VoiceFile;

    m_transport.Send(request);
}

void CharacterService::OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept
{
    // A member can initiate dialogue with an NPC owned by this client.
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor)
        return;

    pActor->StopCurrentDialogue(true);
    pActor->SpeakSound(acMessage.SoundFilename.c_str());
}

void CharacterService::OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.SpeakerID](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (entityIt == view.end())
        return;

    auto serverIdRes = Utils::GetServerId(*entityIt);
    if (!serverIdRes)
    {
        spdlog::error("{}: server id not found for form id {:X}", __FUNCTION__, acEvent.SpeakerID);
        return;
    }

    SubtitleRequest request{};
    request.ServerId = serverIdRes.value();
    request.Text = acEvent.Text;
    request.TopicFormId = acEvent.TopicFormID;

    m_transport.Send(request);
}

void CharacterService::OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor)
        return;

    // This is only for fallout 4
    TESTopicInfo* pInfo = nullptr;
    pInfo = Cast<TESTopicInfo>(TESForm::GetById(acMessage.TopicFormId));

    SubtitleManager::Get()->ShowSubtitle(pActor, acMessage.Text.c_str(), pInfo);
}

void CharacterService::OnNotifyRelinquishControl(const NotifyRelinquishControl& acMessage) noexcept
{
    auto formView = m_world.view<FormIdComponent>();
    Vector<entt::entity> entities(formView.begin(), formView.end());

    // TODO(cosideci): this entity iteration shouldn't technically be necessary, just look for the local component
    for (auto entity : entities)
    {
        std::optional<uint32_t> serverIdRes = Utils::GetServerId(entity);
        if (!serverIdRes.has_value())
        {
            spdlog::error(__FUNCTION__ ": failed to find server id for entity");
            continue;
        }

        uint32_t serverId = serverIdRes.value();

        if (serverId == acMessage.ServerId)
        {
            auto& formIdComponent = m_world.get<FormIdComponent>(entity);

            if (m_world.all_of<LocalComponent>(entity))
            {
                m_world.remove<LocalAnimationComponent, LocalComponent>(entity);
                m_world.emplace_or_replace<RemoteComponent>(entity, acMessage.ServerId, formIdComponent.Id);
            }

            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (!pActor)
            {
                // Probably left the room and/or temporary.
                spdlog::info(__FUNCTION__ ": no local Actor for serverId {:X} to relinquish", serverId);
                continue;
            }

            pActor->GetExtension()->SetRemote(true);

            InterpolationSystem::Setup(m_world, entity);
            AnimationSystem::Setup(m_world, entity);

            spdlog::info(__FUNCTION__ ": relinquished control of actor {:X} with server id {:X}", pActor->formID, acMessage.ServerId);

            return;
        }
    }

    spdlog::error("Did not find actor to relinquish control of, server id {:X}", acMessage.ServerId);
}

void CharacterService::OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);
    Actor* pActor = Cast<Actor>(TESForm::GetById(cActorId));
    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": failed to retrieve actor to teleport.");
        return;
    }

    MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);

    spdlog::info("Successfully teleported actor, form id: {:X}, world space: {:X}, cell: {:X}, position: ({}, {}, {})", pActor->formID, acMessage.WorldSpaceId.BaseId, acMessage.CellId.BaseId, acMessage.Position.x, acMessage.Position.y, acMessage.Position.z);
}

void CharacterService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    // Takes ownership of all actors
    if (acEvent.IsLeader)
    {
        auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        Vector<entt::entity> entities(view.begin(), view.end());

        for (auto entity : entities)
            ProcessNewEntity(entity);
    }
}

void CharacterService::MoveActor(const Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) const noexcept
{
    TESObjectCELL* pCell = nullptr;
    if (!acWorldSpaceId)
    {
        const uint32_t cCellId = m_world.GetModSystem().GetGameId(acCellId);
        pCell = Cast<TESObjectCELL>(TESForm::GetById(cCellId));
    }
    // In case of lazy-loading of exterior cells
    else
    {
        const uint32_t cWorldSpaceId = m_world.GetModSystem().GetGameId(acWorldSpaceId);
        TESWorldSpace* const pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(cWorldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acPosition);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }
    }

    if (!pCell)
    {
        spdlog::error(__FUNCTION__ ": failed to fetch cell to teleport, actor: {:X}, worldspace: {:X}, cell: {:X}, position: {}, {}, {}", apActor->formID, acWorldSpaceId.BaseId, acCellId.BaseId, acPosition.x, acPosition.y, acPosition.z);
        return;
    }

    apActor->MoveTo(pCell, acPosition);
}

void CharacterService::ProcessNewEntity(entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": actor for new entity not found, form id: {:X}", formIdComponent.Id);
        return;
    }

    // A ghost: a copy of the owner's creature is bound to the server in its place (see s_ghosts). One that was not
    // loaded when the copy was made turns up here enabled once its cell loads; it goes dark like the others.
    if (s_ghosts.find(formIdComponent.Id) != s_ghosts.end())
    {
        if (pActor && !pActor->IsDisabled() && !IsProcessExiting())
        {
            pActor->Disable();
            spdlog::info("Ghost: reference {:X} loaded after its stand-in; disabled", formIdComponent.Id);
        }
        return;
    }

    if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(aEntity); pRemoteComponent)
    {
        // TODO(cosideci): don't just take all actors (i.e. from other parties),
        // maybe check it server side, add a variable to the request.
        if (m_world.GetPartyService().IsLeader() && !pActor->IsTemporary() && !pActor->IsMount())
        {
            spdlog::info("Sending ownership claim for actor {:X} with server id {:X}", pActor->formID, pRemoteComponent->Id);

            TakeOwnership(pActor->formID, pRemoteComponent->Id, aEntity);
        }
        else
            spdlog::info("New entity remotely managed, form id: {:X}, server id: {:X}", pActor->formID, pRemoteComponent->Id);

        return;
    }

    if (m_world.any_of<RemoteComponent, LocalComponent, WaitingForAssignmentComponent>(aEntity))
        return;

    CacheSystem::Setup(World::Get(), aEntity, pActor);

    RequestServerAssignment(aEntity);
}

void CharacterService::RequestServerAssignment(const entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    static uint32_t sCookieSeed = 0;

    const auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
        return;

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
        return;

    AssignCharacterRequest message{};

    message.Cookie = sCookieSeed;

    if (!m_world.GetModSystem().GetServerModId(formIdComponent.Id, message.ReferenceId))
    {
        spdlog::error("Server reference id not found for form id {:X}", formIdComponent.Id);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pActor->parentCell->formID, message.CellId))
    {
        spdlog::error("Server cell id not found for cell id {:X}", pActor->parentCell->formID);
        return;
    }

    if (const auto pWorldSpace = pActor->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, message.WorldSpaceId))
            return;
    }

    message.Position = pActor->position;
    message.Rotation.x = pActor->rotation.x;
    message.Rotation.y = pActor->rotation.z;

    // Serialize the base form
    const auto isPlayer = (formIdComponent.Id == 0x14);
    const auto isTemporary = pActor->formID >= 0xFF000000;

    if (isPlayer)
    {
        pNpc->MarkChanged(0x2000800);
    }

    const auto changeFlags = pNpc->GetChangeFlags();

    if (isPlayer || changeFlags != 0)
    {
        message.ChangeFlags = changeFlags;
        pNpc->Serialize(&message.AppearanceBuffer);
    }

    if (isPlayer)
    {
        auto& entries = message.FaceTints.Entries;

        const auto& tints = PlayerCharacter::Get()->GetTints();

        entries.resize(tints.length);

        for (auto i = 0u; i < tints.length; ++i)
        {
            entries[i].Alpha = tints[i]->alpha;
            entries[i].Color = tints[i]->color;
            entries[i].Type = tints[i]->type;

            if (tints[i]->texture)
                entries[i].Name = tints[i]->texture->name.AsAscii();
        }
    }

    if (isPlayer)
    {
        auto& questLog = message.QuestContent.Entries;
        auto& modSystem = m_world.GetModSystem();

        for (const auto& objective : PlayerCharacter::Get()->objectives)
        {
            auto* pQuest = objective.instance->quest;
            if (!pQuest)
                continue;

            if (!QuestService::IsNonSyncableQuest(pQuest))
            {
                GameId id{};

                if (modSystem.GetServerModId(pQuest->formID, id))
                {
                    auto& entry = questLog.emplace_back();
                    entry.Stage = pQuest->currentStage;
                    entry.Id = id;
                }
            }
        }

        // remove duplicates
        const auto ip = std::unique(questLog.begin(), questLog.end());
        questLog.resize(std::distance(questLog.begin(), ip));
    }

    message.CurrentActorData = BuildActorData(pActor);

    message.FactionsContent = pActor->GetFactions();
    message.IsDragon = pActor->IsDragon();
    message.IsMount = pActor->IsMount();
    message.IsPlayerSummon = pActor->GetCommandingActor() && pActor->GetCommandingActor()->formID == 0x14;

    if (pNpc->IsTemporary())
        pNpc = pNpc->GetTemplateBase();

    // A temporary actor has to be rebuilt from its base form on the other side, so that form has always travelled.
    // A persistent world reference used to send only its reference id, and the other side adopted whatever actor sat
    // at that id locally. When the reference's base is a levelled list the two machines roll it separately, so the
    // same id was a rabbit here and a fox there, and the fox was then driven with the rabbit's animation data and
    // stood frozen. Send the base form for those too, so the other side can tell its copy apart. The server only
    // rejects a base form on the player entity, which is excluded here.
    if (pNpc && !isPlayer)
    {
        if (!m_world.GetModSystem().GetServerModId(pNpc->formID, message.FormId) && isTemporary)
        {
            spdlog::error("Server NPC form id not found for form id {:X}", pNpc->formID);
            return;
        }
    }

    // Serialize actions
    auto* const pExtension = pActor->GetExtension();

    message.LatestAction = pExtension->LatestAnimation;
    pActor->SaveAnimationVariables(message.LatestAction.Variables);

    spdlog::debug("Request id: {:X}, cookie: {:X}, entity: {:X}", formIdComponent.Id, sCookieSeed, to_integral(aEntity));

    if (m_transport.Send(message))
    {
        m_world.emplace<WaitingForAssignmentComponent>(aEntity, sCookieSeed);

        sCookieSeed++;
    }
}

void CharacterService::CancelServerAssignment(const entt::entity aEntity, const uint32_t aFormId) const noexcept
{
    if (m_world.all_of<RemoteComponent>(aEntity))
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));

        if (pActor)
        {
            if (pActor->IsTemporary() && !IsProcessExiting())
            {
                spdlog::info("Temporary Remote Deleted {:X}", aFormId);
                pActor->Delete();
                ReleaseGhostOf(aFormId);
            }
            else
            {
                pActor->GetExtension()->SetRemote(false);
            }
        }

        DeleteRemoteEntityComponents(aEntity);

        return;
    }

    // In the event we were waiting for assignment, drop it
    if (m_world.all_of<WaitingForAssignmentComponent>(aEntity))
    {
        auto& waitingComponent = m_world.get<WaitingForAssignmentComponent>(aEntity);

        CancelAssignmentRequest message;
        message.Cookie = waitingComponent.Cookie;

        m_transport.Send(message);

        m_world.remove<WaitingForAssignmentComponent>(aEntity);
    }

    if (m_world.all_of<LocalComponent>(aEntity))
    {
        auto& localComponent = m_world.get<LocalComponent>(aEntity);

        RequestOwnershipTransfer request{};
        request.ServerId = localComponent.Id;

        if (Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId)))
        {
            if (!pActor->IsTemporary())
            {
                auto& modSystem = m_world.GetModSystem();

                if (TESWorldSpace* pWorldSpace = pActor->GetWorldSpace())
                {
                    if (!modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId))
                        spdlog::error("World space id not found, despite having a world space, {:X}", pWorldSpace->formID);
                }

                if (TESObjectCELL* pCell = pActor->GetParentCell())
                {
                    if (!modSystem.GetServerModId(pCell->formID, request.CellId))
                        spdlog::error("Cell id not found, despite having a cell, {:X}", pCell->formID);
                }

                request.Position = pActor->position;
            }
        }

        spdlog::info(
            "Transferring ownership of local actor, server id: {:X}, worldspace: {:X}, cell: {:X}, position: "
            "({}, {}, {})",
            request.ServerId, request.WorldSpaceId.BaseId, request.CellId.BaseId, request.Position.x, request.Position.y, request.Position.z);

        m_transport.Send(request);

        m_world.remove<LocalAnimationComponent, LocalComponent>(aEntity);
    }
}

Actor* CharacterService::CreateCharacterForEntity(entt::entity aEntity) const noexcept
{
    PerfCounterScope perfScope(PerfCounter::kActorSpawn);
    auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(aEntity);
    auto* pInterpolationComponent = m_world.try_get<InterpolationComponent>(aEntity);

    if (!pWaitingFor3D || !pInterpolationComponent)
    {
        spdlog::error(__FUNCTION__ ": could not find WaitingFor3D or InterpolationComponent");
        return nullptr;
    }

    auto& acMessage = pWaitingFor3D->SpawnRequest;

    Actor* pActor = nullptr;

    // Custom forms
    if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        if (acMessage.BaseId != GameId{})
        {
            const uint32_t cNpcId = World::Get().GetModSystem().GetGameId(acMessage.BaseId);
            if (cNpcId == 0)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod");
                return nullptr;
            }

            pNpc = Cast<TESNPC>(TESForm::GetById(cNpcId));
            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, aEntity, acMessage.FaceTints);
        }

        pActor = Actor::Create(pNpc);
    }

    auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);

    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": could not spawn actor for remote server id {:X}.", remoteComponent.Id);
        return nullptr;
    }

    pActor->GetExtension()->SetRemote(true);
    pActor->rotation.x = acMessage.Rotation.x;
    pActor->rotation.z = acMessage.Rotation.y;
    pActor->MoveTo(PlayerCharacter::Get()->parentCell, pInterpolationComponent->Position);
    pActor->SetActorValues(acMessage.IsPlayer ? StandingValues(acMessage.InitialActorValues, pActor->formID) : acMessage.InitialActorValues);

    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    if (acMessage.IsPlayer)
    {
        pActor->SetIgnoreFriendlyHit(true);
        pActor->SetPlayerRespawnMode();
        // The copy stays essential (it cannot die here), but it may get up again: when its owner's real health arrives
        // after a knock-down, the game lets it recover instead of leaving it down and undrawn for the session.
        pActor->SetNoBleedoutRecovery(false);
        m_world.emplace_or_replace<PlayerComponent>(aEntity, acMessage.PlayerId);
    }

    if (pActor->IsDead() != acMessage.IsDead)
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

    spdlog::info("Spawned character for entity, server id: {:X}", remoteComponent.Id);

    return pActor;
}

ActorData CharacterService::BuildActorData(Actor* apActor) const noexcept
{
    ActorData actorData{};
    actorData.InitialActorValues = apActor->GetEssentialActorValues();
    actorData.InitialInventory = apActor->GetActorInventory();
    actorData.IsDead = apActor->IsDead();
    actorData.IsWeaponDrawn = apActor->actorState.IsWeaponFullyDrawn();

    return actorData;
}

// TEMPORARY (2026-09-20): when this side last told the server where its actors are. The world probe prints its age,
// to see whether a player in a menu or a loading screen goes silent for the 3 s after which the server hands their
// actors away.
static std::chrono::steady_clock::time_point s_lastMoveSentAt;
void CharacterService::RunLocalUpdates() const noexcept
{
    // The local player is sent at ~30 Hz for smooth VR head and hand movement, other actors at 10 Hz.
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    static std::chrono::steady_clock::time_point lastFullSendTimePoint;
    constexpr auto cDelayBetweenPlayerSnapshots = 33ms;
    constexpr auto cDelayBetweenSnapshots = 100ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenPlayerSnapshots)
        return;

    lastSendTimePoint = now;

    const bool fullSnapshot = now - lastFullSendTimePoint >= cDelayBetweenSnapshots;
    if (fullSnapshot)
        lastFullSendTimePoint = now;

    ClientReferencesMoveRequest message;
    message.Tick = m_transport.GetClock().GetCurrentTick();

    auto animatedLocalView = m_world.view<LocalComponent, LocalAnimationComponent, FormIdComponent>();

    for (auto entity : animatedLocalView)
    {
        auto& localComponent = animatedLocalView.get<LocalComponent>(entity);
        auto& animationComponent = animatedLocalView.get<LocalAnimationComponent>(entity);
        auto& formIdComponent = animatedLocalView.get<FormIdComponent>(entity);

        if (!fullSnapshot && formIdComponent.Id != 0x14)
            continue;

        AnimationSystem::Serialize(m_world, message, localComponent, animationComponent, formIdComponent);
    }

    if (fullSnapshot || !message.Updates.empty())
    {
        m_transport.Send(message);
        s_lastMoveSentAt = std::chrono::steady_clock::now();
    }
}

std::chrono::steady_clock::time_point CharacterService::LastMoveSentAt() noexcept
{
    return s_lastMoveSentAt;
}

// The other player's name and health above their head, using the game's own enemy meter (decided 2026-09-20: the
// world-space bar NPCs get, nothing of ours). The nearest living remote player within 1500 units is pointed at every
// half second, which is how the game itself keeps the meter up during a fight; the game's own target (a real enemy
// hit in the last 8 s) always wins, and the meter is cleared once when no player is near any more.
void CharacterService::RunEnemyMeterUpdates() noexcept
{
#ifdef SKYRIMVR
    static std::chrono::steady_clock::time_point s_next;
    static uint32_t s_shownHandle = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + 500ms;

    const auto clear = [&]()
    {
        if (s_shownHandle)
        {
            UI::SetEnemyMeterTarget(0, 0);
            s_shownHandle = 0;
        }
    };

    if (!m_transport.IsConnected())
    {
        clear();
        return;
    }
    if (now - UI::LastGameEnemyMeterTargetAt() < 8s)
    {
        s_shownHandle = 0; // the game owns the meter for now
        return;
    }
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    Actor* pNearest = nullptr;
    float nearestSq = 1500.f * 1500.f;
    auto view = m_world.view<PlayerComponent, FormIdComponent, RemoteComponent>();
    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor || !pActor->GetNiNode() || pActor->actorState.IsDeadOrDying())
            continue;
        const float dx = pActor->position.x - pPlayer->position.x;
        const float dy = pActor->position.y - pPlayer->position.y;
        const float dz = pActor->position.z - pPlayer->position.z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < nearestSq)
        {
            nearestSq = distSq;
            pNearest = pActor;
        }
    }
    if (!pNearest)
    {
        clear();
        return;
    }
    const uint32_t handle = pNearest->GetHandle().handle.iBits;
    if (!handle)
        return;
    if (handle != s_shownHandle)
        spdlog::info("Enemy meter: showing remote player {:X} (level {}) at {:.0f} units", pNearest->formID, pNearest->GetLevel(), std::sqrt(nearestSq));
    UI::SetEnemyMeterTarget(handle, pNearest->GetLevel());
    s_shownHandle = handle;
#endif
}

// TEMPORARY (2026-09-20): a player's copy that appears after that player's client dropped and reconnected can be hit
// but not seen (19:09 Seen for Emma, 19:17 Emma for Seen; nothing in the spawn lines differs from a visible spawn).
// Every 5 s, what the game holds for each remote player's copy: distance and height difference to us, form flags
// (0x800 disabled, 0x20 deleted), 3D and its root, health and state, body scale. Compare a visible copy against an
// invisible one.
void CharacterService::RunRemotePlayerDiag() noexcept
{
    static std::chrono::steady_clock::time_point s_next;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + 5s;
    if (!m_transport.IsConnected())
        return;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;
    auto view = m_world.view<PlayerComponent, FormIdComponent, RemoteComponent>();
    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor)
            continue;
        const float dx = pActor->position.x - pPlayer->position.x;
        const float dy = pActor->position.y - pPlayer->position.y;
        const float dz = pActor->position.z - pPlayer->position.z;
        spdlog::info("CopyDiag: player copy {:X} '{}' {:.0f} units away, dz {:.0f}, form flags {:X}, health {:.0f}, dead {}, bleedout {}, state1 {:X}, {}", pActor->formID,
                     pActor->baseForm ? Cast<TESNPC>(pActor->baseForm)->fullName.value.AsAscii() : "?", std::sqrt(dx * dx + dy * dy + dz * dz), dz, pActor->flags,
                     pActor->GetActorValue(24), pActor->IsDead(), pActor->actorState.IsBleedingOut(), pActor->actorState.flags1, VRBodySync::DescribeBody(pActor));
    }
}

void CharacterService::RunRemoteUpdates() noexcept
{
    // Delay by 300ms to let the interpolation system accumulate interpolation points
    const auto tick = m_transport.GetClock().GetCurrentTick() - 300;
    // VR poses use a shorter delay: hands lagging behind are much more noticeable than movement.
    const auto poseTick = m_transport.GetClock().GetCurrentTick() - 100;

    // Interpolation has to keep running even if the actor is not in view, otherwise we will never know if we need to spawn it
    auto interpolatedEntities = m_world.view<RemoteComponent, InterpolationComponent>();

    for (auto entity : interpolatedEntities)
    {
        auto* pFormIdComponent = m_world.try_get<FormIdComponent>(entity);
        auto& interpolationComponent = interpolatedEntities.get<InterpolationComponent>(entity);

        Actor* pActor = nullptr;
        if (pFormIdComponent)
        {
            auto* pForm = TESForm::GetById(pFormIdComponent->Id);
            pActor = Cast<Actor>(pForm);
        }

        InterpolationSystem::Update(pActor, interpolationComponent, tick, poseTick);
    }

    auto animatedView = m_world.view<RemoteComponent, RemoteAnimationComponent, FormIdComponent>();

    for (auto entity : animatedView)
    {
        auto& animationComponent = animatedView.get<RemoteAnimationComponent>(entity);
        auto& formIdComponent = animatedView.get<FormIdComponent>(entity);

        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        AnimationSystem::Update(m_world, pActor, animationComponent, tick);
    }

    auto facegenView = m_world.view<FormIdComponent, FaceGenComponent>();

    for (auto entity : facegenView)
    {
        auto& formIdComponent = facegenView.get<FormIdComponent>(entity);
        auto& faceGenComponent = facegenView.get<FaceGenComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        FaceGenSystem::Update(m_world, pActor, faceGenComponent);
    }

    auto waitingView = m_world.view<FormIdComponent, WaitingFor3D>();

    Vector<entt::entity> toRemove;
    for (auto entity : waitingView)
    {
        auto& formIdComponent = waitingView.get<FormIdComponent>(entity);
        auto& waitingFor3D = waitingView.get<WaitingFor3D>(entity);

        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor || !pActor->GetNiNode())
            continue;

        // By now, the actor has materialized in the world and is ready for further setup

        pActor->SetActorInventory(waitingFor3D.SpawnRequest.InventoryContent);
        // The inventory apply above doesn't put weapons and torches in the hands of a remote player's copy on VR, so
        // they only showed up after the owner switched. Apply the hands the same way a later equipment change does.
        // Players only: an NPC's server inventory can predate its AI drawing a weapon.
        if (pActor->GetExtension()->IsRemotePlayer())
            InventoryService::ApplyHandEquipment(pActor, waitingFor3D.SpawnRequest.InventoryContent, true);
        pActor->SetFactions(waitingFor3D.SpawnRequest.FactionsContent);

        if (!waitingFor3D.SpawnRequest.ActionsToReplay.Actions.empty())
        {
            pActor->LoadAnimationVariables(waitingFor3D.SpawnRequest.ActionsToReplay.Actions[0].Variables);
        }

        m_weaponDrawUpdates[pActor->formID] = {waitingFor3D.SpawnRequest.IsWeaponDrawn};

        if (pActor->IsDead() != waitingFor3D.SpawnRequest.IsDead)
            waitingFor3D.SpawnRequest.IsDead ? pActor->Kill() : pActor->Respawn();

        if (pActor->IsVampireLord())
            pActor->FixVampireLordModel();

        toRemove.push_back(entity);

        spdlog::info("Applied 3D for actor, form id: {:X}", pActor->formID);
    }

    for (auto entity : toRemove)
        m_world.remove<WaitingFor3D>(entity);
}

void CharacterService::RunFactionsUpdates() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    RequestFactionsChanges message;

    auto factionedActors = m_world.view<LocalComponent, CacheComponent, FormIdComponent>();
    for (auto entity : factionedActors)
    {
        auto& formIdComponent = factionedActors.get<FormIdComponent>(entity);
        auto& localComponent = factionedActors.get<LocalComponent>(entity);
        auto& cacheComponent = factionedActors.get<CacheComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        const auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        // Check if cached factions and current factions are identical
        auto factions = pActor->GetFactions();

        if (cacheComponent.FactionsContent == factions)
            continue;

        cacheComponent.FactionsContent = factions;

        // If not send the current factions and replace the cached factions
        message.Changes[localComponent.Id] = factions;
    }

    if (!message.Changes.empty())
        m_transport.Send(message);
}

void CharacterService::RunSpawnUpdates() const noexcept
{
    auto invisibleView = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent, WaitingFor3D>(entt::exclude<FormIdComponent>);
    Vector<entt::entity> entities(invisibleView.begin(), invisibleView.end());

    for (const auto entity : entities)
    {
        auto& remoteComponent = m_world.get<RemoteComponent>(entity);
        auto& interpolationComponent = m_world.get<InterpolationComponent>(entity);

        if (const auto pWorldSpace = PlayerCharacter::Get()->GetWorldSpace())
        {
            float characterX = interpolationComponent.Position.x;
            float characterY = interpolationComponent.Position.y;
            const auto characterCoords = GridCellCoords::CalculateGridCellCoords(characterX, characterY);
            const TES* pTES = TES::Get();
            const auto playerCoords = GridCellCoords(pTES->centerGridX, pTES->centerGridY);

            // A dragon the other player fights sits well outside the 5x5 grid; the server keeps it at the wide range and
            // says so in the spawn request, so it is created here at that range too instead of never.
            const bool isDragon = m_world.get<WaitingFor3D>(entity).SpawnRequest.IsDragon;
            if (GridCellCoords::IsCellInGridCell(characterCoords, playerCoords, isDragon))
            {
                auto* pActor = Cast<Actor>(TESForm::GetById(remoteComponent.CachedRefId));
                if (!pActor)
                {
                    pActor = CreateCharacterForEntity(entity);
                    if (!pActor)
                        continue;

                    remoteComponent.CachedRefId = pActor->formID;
                }

                pActor->MoveTo(PlayerCharacter::Get()->parentCell, interpolationComponent.Position);
            }
        }
    }
}

void CharacterService::RunExperienceUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    if (m_cachedExperience == 0.f)
        return;

    if (!World::Get().GetPartyService().IsInParty())
        return;

    SyncExperienceRequest message;
    message.Experience = m_cachedExperience;

    m_cachedExperience = 0.f;

    m_transport.Send(message);

    spdlog::debug("Sending over experience {}", message.Experience);
}

void CharacterService::ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept
{
    std::vector<uint32_t> toRemove{};

    for (auto& [cId, _] : m_weaponDrawUpdates)
    {
        auto& data = m_weaponDrawUpdates[cId];

        data.m_timer += acUpdateEvent.Delta;

        // We do 2 passes because Skyrim's weapon drawing is the most finnicky thing in existence
        double maxTime = data.m_isFirstPass ? 0.5 : 2.0;
        if (data.m_timer <= maxTime)
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(cId));
        if (!pActor)
            continue;

        pActor->SetWeaponDrawnEx(data.m_drawWeapon);

        if (!data.m_isFirstPass)
            toRemove.push_back(cId);

        data.m_isFirstPass = false;
    }

    for (uint32_t id : toRemove)
        m_weaponDrawUpdates.erase(id);
}
