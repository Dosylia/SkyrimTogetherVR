#include "Forms/TESObjectCELL.h"
#include <PerfScope.h>
#include "Forms/TESWorldSpace.h"
#include "Services/PapyrusService.h"
#include <Services/PartyService.h>

#include <Services/CharacterService.h>
#include <Services/QuestService.h>
#include <Services/TransportService.h>
#include <Services/InventoryService.h>
#include <Services/DiscoveryService.h>
#include <RecentDeletes.h>
#include <CrashHandler.h>
#include <unordered_set>
#include <CopyRemovalPolicy.h>

#include <Games/References.h>
#include <Games/Misc/SubtitleManager.h>

#include <Forms/TESNPC.h>
#include <Interface/UI.h>
#include <Forms/TESQuest.h>

#include <BranchInfo.h>
#include <Components.h>

#include <Systems/InterpolationSystem.h>
#include <Systems/AnimationSystem.h>
#include <Systems/CacheSystem.h>
#include <Systems/FaceGenSystem.h>
#include <Systems/LeveledNpcSystem.h>

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
#include <Messages/RequestInventoryChanges.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyRemoveCharacter.h>
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

// Actors this client's own game made (placed by console, a quest spawn, a random encounter) that now belong to
// another player and are no longer in this client's view: server character id -> the actor it is here.
//
// A temporary actor has no id two games agree on, so the server sends it as "a character with no reference of its
// own" and every client makes a copy for it -- including, when it came back, the client whose game had made it and
// still had it. That client then announced its own actor to the server as a new character as well. One creature
// became two for everybody, and a third body on the screen of the player who made it: three Seekers placed, six
// standing after one walk away and back (2026-10-02). It is also what the three crashes of that night have in
// common: `live-copy-abandon` crashed five times out of five within a second of travelling away from such a pair,
// while a copy alone (`live-seeker-copy-remote`, `-local`) and the game's own actors alone travel cleanly. Why the
// pair crashes is not known -- the game frees both on the way out and its physics then meets a skeleton whose owner
// is freed memory -- but the pair should not exist in the first place.
//
// So the client that made the actor remembers which server character it became, does not announce it a second
// time, and takes it back as that same actor when the server sends the character again. The record lasts until
// the server removes the character or the connection ends.
struct HandedAway
{
    uint32_t FormId{};
    uint32_t BaseFormId{}; // 0 when the actor was already gone from the game when it was let go
};
TiltedPhoques::Map<uint32_t, HandedAway> s_handedAway;

// Whether this actor is one that was handed away, and as which server character.
bool IsHandedAway(const Actor* apActor, uint32_t& aServerId) noexcept
{
    for (const auto& [serverId, entry] : s_handedAway)
    {
        if (entry.FormId != apActor->formID)
            continue;
        if (entry.BaseFormId != 0 && (!apActor->baseForm || apActor->baseForm->formID != entry.BaseFormId))
            continue;
        aServerId = serverId;
        return true;
    }
    return false;
}

// The temporary actors this client made itself -- copies of other players and of their creatures -- by form id.
//
// "Temporary and remote" used to be taken to mean exactly that, and it does not: an actor the game spawned on its
// own (a summon, a random encounter, anything placed by a script) is temporary too, and becomes remote the moment
// the server hands it to another player -- which it does during this player's load screens. When the cell then
// unloads, the game disposes of its own actor and says so, and CancelServerAssignment disabled it in the middle of
// that as if it were a copy of ours. 25 ms later the game's movement code ran on it and crashed
// (SkyrimVR.exe+0x714EB2, twice out of twice on 2026-10-01 with a Seeker placed by console, a bot in range and one
// trip through a load door; the crash report shows the actor already marked deleted by the game, which this client
// never does on that path). Only what is in this set is this client's to disable or delete.
//
// An id has to leave this set the moment its copy stops being ours, at every place that happens, because the game
// hands a deleted actor's form id to the next actor it makes. Three places deleted copies without saying so here
// (a removal ordered by the server, the clean-up on connect, the other players' copies on disconnect), and on
// 2026-10-02 `live-temp-reuse` got the crash back with them: the bot left, its copy FF0008E4 was deleted, the game
// gave FF0008E4 to a Seeker placed by console ten seconds later, and when the player walked away the client took
// that Seeker for its copy and disabled it -- SkyrimVR.exe+0x714EB2 again, 256 ms later, the Seeker in R15. A third
// Seeker in the same run, with an id never used for a copy, was left to the game and did nothing. When in doubt an
// id is taken out: a copy mistaken for the game's is left standing, the other mistake is the crash.
std::unordered_set<uint32_t> s_ownCopies;

Actor* RememberOwnCopy(Actor* apActor) noexcept
{
    if (apActor)
        s_ownCopies.insert(apActor->formID);
    return apActor;
}

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

// The base a copy of the owner's temporary actor is made from. The owner's resolved leveled pick is preferred over the
// lossy template base -- when the pick is the same character under another record. A named NPC that takes its stats
// from a levelled list has that list's pick as its "pick" too: Boethiah Cultist B0E87, stats from Bandit Marauder
// 39D25, was made here as the marauder, and Seen saw a bandit where Emma fought a cultist (2026-10-04 16:00). A base
// with a name of its own that differs from the pick's is the character; the pick is only where its stats come from.
TESNPC* BaseForCopy(World& aWorld, const CharacterSpawnRequest& acMessage) noexcept
{
    TESNPC* pBase = Cast<TESNPC>(TESForm::GetById(aWorld.GetModSystem().GetGameId(acMessage.BaseId)));
    if (acMessage.LeveledNpcPickId == GameId{})
        return pBase;

    TESNPC* pPick = Cast<TESNPC>(TESForm::GetById(aWorld.GetModSystem().GetGameId(acMessage.LeveledNpcPickId)));
    const char* pBaseName = pBase ? pBase->fullName.value.AsAscii() : nullptr;
    const char* pPickName = pPick ? pPick->fullName.value.AsAscii() : nullptr;
    if (pPick && pBase && pPick != pBase && pBaseName && *pBaseName && std::strcmp(pBaseName, pPickName ? pPickName : "") != 0)
    {
        spdlog::info("Copy for server id {:X} made from its own base {:X} ({}), not the levelled pick {:X} ({}) it takes its stats from", acMessage.ServerId, pBase->formID, pBaseName,
                     pPick->formID, pPickName ? pPickName : "?");
        return pBase;
    }
    return pPick ? pPick : pBase;
}

// Spawns the owner's creature and turns the local one into a ghost. Null when no copy could be made; the caller then
// adopts the local creature as before.
Actor* StandInForForeignCreature(Actor* apLocal, TESNPC* apOwnerBase) noexcept
{
    Actor* pCopy = RememberOwnCopy(Actor::Create(apOwnerBase));
    if (!pCopy)
    {
        spdlog::warn("Ghost: could not create a copy of {:X} for reference {:X}, adopting the local creature instead", apOwnerBase->formID, apLocal->formID);
        return nullptr;
    }

    const TESNPC* pLocalBase = Cast<TESNPC>(apLocal->baseForm);
    if (pLocalBase && pLocalBase->IsTemporary())
        pLocalBase = pLocalBase->GetTemplateBase();

    s_ghosts[apLocal->formID] = pCopy->formID;

    // Only a creature this game has loaded is disabled now. The server names references the owner sees, and this game
    // can hold one in memory without its 3D -- Troll 85FAD at the corner of the grid, never loaded here, the owner's
    // Bear 15700 units away. The game was disposing of it, and disabling it in the middle of that crashed in its
    // movement code 32 ms later (SkyrimVR.exe+0x714EB2, the troll in R15 with its form id gone, 2026-10-04 16:26:41).
    // One that is not loaded goes dark once it loads, like a stand-in for a reference that was not here at all
    // (ProcessNewEntity).
    const bool cLoaded = apLocal->GetNiNode() != nullptr && !apLocal->IsDeleted();
    if (cLoaded)
        apLocal->Disable();

    spdlog::info("Ghost: reference {:X} rolled as {} ({:X}) here but the owner has {} ({:X}); copy {:X} stands in for it and the local one is {}", apLocal->formID,
                 pLocalBase ? pLocalBase->fullName.value.AsAscii() : "?", pLocalBase ? pLocalBase->formID : 0, apOwnerBase->fullName.value.AsAscii(), apOwnerBase->formID,
                 pCopy->formID, cLoaded ? "disabled" : "not loaded here (disabled once it loads)");
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

// The teardown of a remote copy is back to exactly what it was before 2026-09-27, and this note is why.
//
// Three variations were tried in one afternoon and each replaced one bug with another:
//   - delete one frame later  -> still a use-after-free; Seen's dump had the actor in Rdi 1930 ms after.
//   - never delete            -> 652 copies piled up in Emma's world in five minutes and ate her framerate.
//   - disable, delete after 5 s -> her Seekers went invisible (nothing re-enables a disabled copy), and the
//                                  crash came back anyway with the actor still in Rcx **24 seconds** later.
//
// Twenty-four seconds means no delay is safe, and disabling breaks the copy's own recovery path, which relies
// on the actor being *gone* so a fresh one can be spawned. The original behaviour is at least the one the rest
// of the system is built around, and it is the baseline the user calls "perfect". What stays from the attempt
// is the evidence: RecentDeletes still records every deletion and the crash handler still checks it against
// every register, so the next crash says plainly whether this path is involved.
//
// What it does NOT say yet is who holds the pointer for twenty-four seconds. A combat target is the obvious
// suspect -- Emma shot the Seeker that then went wrong -- and that is the next thing to look at, on evidence
// rather than by rearranging this code again.

// Copies disabled and waiting for the game to release them. The rules are in CopyRemovalPolicy.h, kept
// separate and free of game types so the one thing that kept going wrong -- the reasoning -- is unit tested.
// The bound is the safety property: this list can never hold more than kMaxWaiting, whatever the game does.
struct WaitingCopy
{
    uint32_t FormId{};
    std::chrono::steady_clock::time_point QueuedAt{};
};

TiltedPhoques::Vector<WaitingCopy> s_waitingCopies;

// The first word at this address, or 0 if it cannot be read: an access violation is caught here instead of asking
// VirtualQuery first. CrashGuard tells the crash handler that a fault in here is expected.
uintptr_t ReadFirstWord(const void* apAddress) noexcept
{
    __try
    {
        return *static_cast<const uintptr_t*>(apAddress);
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return 0;
    }
}

//! Whether a pointer from a form lookup is still a real game object: readable, and its vtable inside the process
//! image, where every game vtable lives. Freed memory reused for anything else fails at once -- 2026-09-30's had
//! 0x3b33e809f967790a where the vtable belongs.
//!
//! The read is guarded rather than checked with VirtualQuery first. VirtualQuery was the whole cost of this check: in
//! a big process it works out how far the region around the address reaches, and the game's heap regions are large
//! and grow, so one call took about ten milliseconds. RunSpawnUpdates asks this once a frame for every copy still
//! waiting for its 3D, which made it the slowest part of the frame up to 43 ms on 2026-10-01 to 10-03: 14 ms with
//! one copy waiting, 24 with two, 36 with four. Placing the copies themselves measured 0.0 ms.
bool IsLiveGameObject(const TESForm* apForm) noexcept
{
    if (!apForm)
        return false;

    static const auto s_image = []
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + pDos->e_lfanew);
        return std::pair<uintptr_t, uintptr_t>{base, base + pNt->OptionalHeader.SizeOfImage};
    }();

    CrashGuard::Enter();
    const uintptr_t vtable = ReadFirstWord(apForm);
    CrashGuard::Leave();

    return vtable >= s_image.first && vtable < s_image.second;
}

uint32_t OutstandingHandles(const Actor* apActor) noexcept
{
    return static_cast<uint32_t>(apActor->handleRefObject.refCount) & 0x3FF;
}

void FreeCopy(Actor* apActor, const uint32_t aFormId, const char* acpWhy) noexcept
{
    const uint32_t refWord = static_cast<uint32_t>(apActor->handleRefObject.refCount);
    RecentDeletes::Record(apActor, aFormId, refWord & 0x3FF, refWord, 0);
    // Colon, not brackets, and a reason that names one outcome only. Logs written before 2026-09-28 used
    // "(the game let go of it)" for both the success and the give-up path, so the punctuation is what tells a
    // reader -- and session-report.py -- that a line can be trusted to mean what it says.
    spdlog::info("Temporary Remote Deleted {:X}: {}", aFormId, acpWhy);
    apActor->Delete();
    s_ownCopies.erase(aFormId); // the game may give this id to an actor of its own next
}

void RunWaitingCopies() noexcept
{
    if (s_waitingCopies.empty())
        return;

    static std::chrono::steady_clock::time_point s_next;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + 250ms;

    TiltedPhoques::Vector<WaitingCopy> stillWaiting;
    stillWaiting.reserve(s_waitingCopies.size());

    for (const WaitingCopy& waiting : s_waitingCopies)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(waiting.FormId));
        if (!pActor || pActor->IsDeleted() || !pActor->IsTemporary())
        {
            s_ownCopies.erase(waiting.FormId);
            continue; // gone already, which is the outcome we wanted
        }

        const auto waitedMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - waiting.QueuedAt).count());
        const bool overBound = stillWaiting.size() >= CopyRemovalPolicy::kMaxWaiting;
        const uint32_t handles = OutstandingHandles(pActor);
        const auto reason = CopyRemovalPolicy::ReleaseReason(handles, waitedMs, overBound);

        if (reason != CopyRemovalPolicy::Released::NotYet)
        {
            FreeCopy(pActor, waiting.FormId, CopyRemovalPolicy::Describe(reason));

            // Said separately and loudly, because this is the case the wait exists to prevent and the old
            // wording hid it: every release of 2026-09-27 was reported as the game letting go, and every one of
            // them was actually this -- 60 s elapsed with two handles still outstanding.
            if (CopyRemovalPolicy::FreedWhileHeld(reason))
                spdlog::warn("CopyFreedHeld: {:X} freed after {} ms with {} handle(s) still outstanding -- {}",
                             waiting.FormId, waitedMs, handles, CopyRemovalPolicy::Describe(reason));
            continue;
        }

        stillWaiting.push_back(waiting);
    }

    s_waitingCopies.swap(stillWaiting);
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
    // Invisibility (54) travels with every other value. A copy born invisible stays invisible until a reconnect
    // rebuilds it (2026-09-20 evening, "can be hit but not seen", many times). Never on a copy.
    if (auto inv = values.ActorValuesList.find(ActorValueInfo::kInvisibility); inv != values.ActorValuesList.end() && inv->second != 0.f)
    {
        spdlog::info("Remote player {:X} copy would have spawned invisible ({:.2f}); spawned visible instead", aFormId, inv->second);
        inv.value() = 0.f;
    }
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

    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&CharacterService::OnPartyJoinedEvent>(this);
}

// A copy just made for this character has this form id; no other character may still point at it. The game gives a
// deleted actor's id to the next actor it makes, and a character whose copy the game had already thrown away kept the
// old id: on 2026-10-03 at 09:32:53 Seen's game made Emma's copy as FF001231, lost it within 29 ms, and made a Bandit
// Outlaw under the same id. Emma's character went on pointing at FF001231, so every frame the bandit was moved to
// where Emma stood, and Emma was invisible to Seen -- a bandit in her place -- until he reconnected.
void CharacterService::ClaimCopyId(const entt::entity aEntity, const uint32_t aFormId) const noexcept
{
    auto view = m_world.view<RemoteComponent>();
    for (const auto other : view)
    {
        if (other == aEntity)
            continue;
        auto& remoteComponent = view.get<RemoteComponent>(other);
        if (remoteComponent.CachedRefId != aFormId)
            continue;

        spdlog::warn("Copy id {:X} now belongs to server character {:X}; server character {:X} still pointed at it (its own copy is gone) and "
                     "gets a new one", aFormId, m_world.get<RemoteComponent>(aEntity).Id, remoteComponent.Id);
        remoteComponent.CachedRefId = 0;
        if (const auto* pFormId = m_world.try_get<FormIdComponent>(other); pFormId && pFormId->Id == aFormId)
            m_world.remove<FormIdComponent>(other);
    }
}

void CharacterService::DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept
{
    m_world.remove<FaceGenComponent, InterpolationComponent, RemoteAnimationComponent, RemoteComponent, CacheComponent, WaitingFor3D, PlayerComponent>(aEntity);
}

void CharacterService::DeclineOwnership(const uint32_t aServerId, const uint32_t aOwnershipEpoch) const noexcept
{
    RequestOwnershipTransfer request{};
    request.ServerId = aServerId;
    request.OwnershipEpoch = aOwnershipEpoch;
    request.Reason = OwnershipReleaseReason::DeclineGrant;
    m_transport.Send(request);
}

void CharacterService::ReconcileActorData(
    const entt::entity aEntity, Actor* apActor, const uint32_t aOwnershipEpoch, const ActorData& acActorData, const bool aApplyInventory, const bool aIsLocalOwner) noexcept
{
    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(aEntity))
    {
        pWaitingFor3D->SpawnRequest.InitialActorValues = acActorData.InitialActorValues;
        pWaitingFor3D->SpawnRequest.InventoryContent = acActorData.InitialInventory;
        pWaitingFor3D->SpawnRequest.IsDead = acActorData.IsDead;
        pWaitingFor3D->SpawnRequest.IsWeaponDrawn = acActorData.IsWeaponDrawn;
        pWaitingFor3D->SpawnRequest.OwnershipEpoch = aOwnershipEpoch;
    }

    if (!apActor)
        return;

    apActor->SetActorValues(acActorData.InitialActorValues);

    if (aApplyInventory)
    {
        const Inventory currentInventory = apActor->GetActorInventory();
        if (currentInventory.Entries != acActorData.InitialInventory.Entries || currentInventory.CurrentMagicEquipment != acActorData.InitialInventory.CurrentMagicEquipment)
            apActor->SetActorInventory(acActorData.InitialInventory);
    }

    if (apActor->IsDead() != acActorData.IsDead)
        acActorData.IsDead ? apActor->Kill() : apActor->Respawn();

    if (aIsLocalOwner)
    {
        // A remote draw correction may still be queued when an ownership grant arrives.
        m_weaponDrawUpdates.erase(apActor->formID);

        if (apActor->actorState.IsWeaponDrawn() != acActorData.IsWeaponDrawn)
            apActor->SetWeaponDrawnEx(acActorData.IsWeaponDrawn);
    }
    else
        m_weaponDrawUpdates[apActor->formID] = {acActorData.IsWeaponDrawn};
}

// The server's copy of an actor's inventory brought in line with this game's: for each kind of item (same base, same
// extra data), the difference in count goes up as an ordinary inventory change, which the server applies to its copy
// and relays to the other players. For a follower whose gear another game overwrote on the server while it owned her.
void CharacterService::SendInventoryDifference(Actor* apActor, const uint32_t aServerId, const uint32_t aOwnershipEpoch, const Inventory& acServerInventory) const noexcept
{
    if (!apActor || !m_transport.IsConnected())
        return;

    const Inventory mine = apActor->GetActorInventory();
    const auto countIn = [](const Inventory& acInventory, const Inventory::Entry& acKind)
    {
        int32_t count = 0;
        for (const auto& entry : acInventory.Entries)
            if (entry.CanBeMerged(acKind))
                count += entry.Count;
        return count;
    };

    TiltedPhoques::Vector<Inventory::Entry> done;
    const auto alreadyDone = [&done](const Inventory::Entry& acKind)
    { return std::any_of(done.begin(), done.end(), [&acKind](const Inventory::Entry& acDone) { return acDone.CanBeMerged(acKind); }); };

    int32_t added = 0, removed = 0;
    for (const Inventory* pSide : {&mine, &acServerInventory})
    {
        for (const auto& kind : pSide->Entries)
        {
            if (alreadyDone(kind))
                continue;
            done.push_back(kind);
            const int32_t difference = countIn(mine, kind) - countIn(acServerInventory, kind);
            if (difference == 0)
                continue;

            RequestInventoryChanges request;
            request.ServerId = aServerId;
            request.OwnershipEpoch = aOwnershipEpoch;
            request.Item = kind;
            request.Item.Count = difference;
            request.Drop = false;
            request.UpdateClients = true;
            m_transport.Send(request);
            if (difference > 0)
                ++added;
            else
                ++removed;
        }
    }

    spdlog::info("Follower {:X}: the server's copy of her inventory put right ({} kinds of item added there, {} taken away)", apActor->formID, added, removed);
}

bool CharacterService::RequestOwnership(const uint32_t aFormId, const uint32_t aServerId, const entt::entity aEntity) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor)
    {
        spdlog::warn("Cannot request ownership of actor {:X} because its form {:X} is unavailable", aServerId, aFormId);
        return false;
    }

    ActorExtension* pExtension = pActor->GetExtension();
    if (pExtension->IsRemotePlayer())
    {
        spdlog::warn("Cannot request ownership of remote player actor {:X}", aServerId);
        return false;
    }

    if (pActor->IsPlayerSummon())
    {
        spdlog::warn("Cannot request ownership of remote player summon {:X}", aServerId);
        return false;
    }

    const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(aEntity);
    if (!pRemoteComponent || pRemoteComponent->Id != aServerId || pRemoteComponent->OwnershipEpoch == 0)
        return false;

    RequestOwnershipClaim request;
    request.ServerId = aServerId;
    request.ExpectedOwnershipEpoch = pRemoteComponent->OwnershipEpoch;

    if (!m_transport.Send(request))
        return false;

    return true;
}

void CharacterService::DeleteTempActor(const uint32_t aFormId) noexcept
{
    if (IsProcessExiting())
        return;

    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (pActor && ((pActor->formID & 0xFF000000) == 0xFF000000))
    {
        const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
        RecentDeletes::Record(pActor, aFormId, refWord & 0x3FF, refWord, RecentDeletes::kOnServersWord);
        pActor->Delete();
        s_ownCopies.erase(aFormId); // the game may give this id to an actor of its own next
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
    // Same id and, for a copy this client made, the same base: an actor the game made under a reused id is its own
    // (see ClaimCopyId).
    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [&acEvent, view, pActor](entt::entity entity)
        {
            auto& remoteComponent = view.get<RemoteComponent>(entity);
            if (remoteComponent.CachedRefId != acEvent.FormId)
                return false;
            if (remoteComponent.CachedBaseId != 0 && (!pActor || !pActor->baseForm || pActor->baseForm->formID != remoteComponent.CachedBaseId))
                return false;
            return true;
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
    if (auto* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId)))
    {
        // GetExtension() returns null for an actor this client did not allocate, and this runs while the game is
        // tearing actors down, which is the worst moment to assume otherwise. Same family as the crash of
        // 2026-09-23 in HookActorProcess.
        if (ActorExtension* pExtension = pActor->GetExtension())
            pExtension->Reconciliation = ActorExtension::ReconciliationStage::None;
    }

    m_pendingLeveledConforms.erase(acEvent.FormId);

    auto view = m_world.view<FormIdComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.FormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == formId; });

    if (entityIt == view.end())
    {
        // Not an error: the game removes plenty of actors this client never tracked, and a cell change retires
        // dozens at once. It was logged at error level and filled the console with red on every load door.
        spdlog::debug("Actor removed that was not tracked here: {:X}", acEvent.FormId);
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
        RunOrphanedRemoteDiag();
    }
    {
        PerfScope perfScope("CharacterService::RunWeaponTouch");
        RunWeaponTouch();
    }
    RunWaitingCopies();

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
    {
        PerfScope perfScope("CharacterService::ProcessLeveledConforms");
        ProcessLeveledConforms();
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
            {
                const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
                RecentDeletes::Record(pActor, formIdComponent.Id, refWord & 0x3FF, refWord, RecentDeletes::kOnConnect);
                pActor->Delete();
            }
            s_ownCopies.erase(formIdComponent.Id);

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

        // GetExtension() returns null by design for an actor this client did not allocate, and this loop walks
        // whatever the world still holds at disconnect. The same unguarded dereference was swept out of fourteen
        // hook sites on 2026-09-25 and this one was missed: it is on the disconnect path, which is exactly when
        // the world is in its least tidy state.
        ActorExtension* pExtension = pActor->GetExtension();
        if (!pExtension)
            continue;

        if (pExtension->IsRemotePlayer())
        {
            const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
            RecentDeletes::Record(pActor, formIdComponent.Id, refWord & 0x3FF, refWord, RecentDeletes::kOnDisconnect);
            pActor->Delete();
        }
        else
            pExtension->SetRemote(false);
    }

    // The players' copies are deleted and every other copy has just been handed to the game: none of them is this
    // client's to disable or delete any more, and their ids will be given out again. See s_ownCopies.
    s_ownCopies.clear();
    s_handedAway.clear();

    m_world.clear<WaitingForAssignmentComponent, LocalComponent, RemoteComponent>();

    ReleaseAllGhosts();
    for (const auto& [formId, pickFormId] : m_pendingLeveledConforms)
    {
        if (auto* pActor = Cast<Actor>(TESForm::GetById(formId)))
        {
            auto& stage = pActor->GetExtension()->Reconciliation;
            // Don't leave the actor disabled if we disconnect before re-enabling it.
            if (stage == ActorExtension::ReconciliationStage::WaitingForDisable && !pActor->IsDeleted())
                pActor->EnableImpl();

            stage = ActorExtension::ReconciliationStage::None;
        }
    }

    m_pendingLeveledConforms.clear();
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
    const bool isCancelled = view.get<WaitingForAssignmentComponent>(cEntity).Cancelled;

    m_world.remove<WaitingForAssignmentComponent>(cEntity);
#if (!IS_MASTER)
    m_world.remove<ReplayedActionsDebugComponent>(cEntity);
#endif

    if (isCancelled)
    {
        if (acMessage.Owner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);

        if (m_world.valid(cEntity))
            m_world.destroy(cEntity);

        return;
    }

    if (acMessage.OwnershipEpoch == 0)
    {
        spdlog::warn("Ignored assignment for actor {:X} because the server returned an invalid ownership epoch", acMessage.ServerId);
        return;
    }

    const auto formIdComponent = m_world.try_get<FormIdComponent>(cEntity);
    if (!formIdComponent)
    {
        if (acMessage.Owner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);

        if (m_world.valid(cEntity))
            m_world.destroy(cEntity);

        spdlog::warn("Discarded assignment for actor {:X} because the local entity no longer has a form", acMessage.ServerId);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent->Id));
    if (!pActor)
    {
        if (acMessage.Owner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);

        spdlog::warn("Discarded assignment for actor {:X} because form {:X} is unavailable", acMessage.ServerId, formIdComponent->Id);
        m_world.destroy(cEntity);
        return;
    }

    // TODO: how could this possibly trigger?
    // it's kinda interfering with my WaitingFor3D code
    if (acMessage.PlayerId != 0)
        m_world.emplace_or_replace<PlayerComponent>(cEntity, acMessage.PlayerId);

    ActorData actorData{};
    actorData.InitialActorValues = acMessage.AllActorValues;
    actorData.InitialInventory = acMessage.CurrentInventory;
    actorData.IsDead = acMessage.IsDead;
    actorData.IsWeaponDrawn = acMessage.IsWeaponDrawn;

    if (acMessage.Owner)
    {
        spdlog::debug("Received local actor, form id: {:X}", pActor->formID);

        pActor->GetExtension()->SetRemote(true);
        // This player's follower coming back: her gear here is the truth, and the server's copy is put right
        // instead (see the remote branch below for what goes wrong otherwise).
        const bool cMyFollower = pActor->IsPlayerTeammate() && !pActor->IsTemporary();
        ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, actorData, !cMyFollower, true);
        if (cMyFollower)
            SendInventoryDifference(pActor, acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.CurrentInventory);

        auto& localAnimationComponent = m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);

        if (auto* pEarlyAnimComponent = m_world.try_get<EarlyAnimationBufferComponent>(cEntity))
        {
            for (const auto& action : pEarlyAnimComponent->Actions)
            {
                localAnimationComponent.Append(action);
            }
        }
        m_world.remove<EarlyAnimationBufferComponent>(cEntity);

        auto& localComponent = m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId, acMessage.OwnershipEpoch);
        localComponent.IsDead = acMessage.IsDead;
        localComponent.IsWeaponDrawn = acMessage.IsWeaponDrawn;
        pActor->GetExtension()->SetRemote(false);
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
                m_world.emplace<RemoteComponent>(copyEntity, acMessage.ServerId, pCopy->formID, acMessage.OwnershipEpoch);
                pCopy->MoveTo(PlayerCharacter::Get()->parentCell, acMessage.Position);
                pCopy->SetActorValues(acMessage.AllActorValues);
                auto& copyInterpolation = InterpolationSystem::Setup(m_world, copyEntity);
                copyInterpolation.Position = acMessage.Position;
                AnimationSystem::Setup(m_world, copyEntity);
                m_world.emplace<WaitingFor3D>(copyEntity, spawn);
                return;
            }
        }

        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, formIdComponent->Id, acMessage.OwnershipEpoch);

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

        // This player's own follower, owned by another player. It happens on a loading screen: this game sends
        // nothing for ten seconds, the server gives her to whoever is still near where she was, and this game then
        // brings her through the door anyway. Twice (2026-10-03 15:09, 2026-10-04 09:33) the two games then pulled
        // her back and forth -- each "is owned over there" moved her to the other player, out of the loaded cells,
        // and her follower AI brought her straight back: 85 appearances in 12 s. And the other game, where she is
        // not anyone's follower, dressed her in the outfit she has in that player's own save and sent it; this game
        // applied it, so she came out wearing that and Emma's Orcish armour at once. Her gear here is the truth:
        // nothing of the server's or the other game's is applied to her, she is not moved away, and she is asked
        // for at once.
        const bool cMyFollower = pActor->IsPlayerTeammate() && !pActor->IsTemporary();

        ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, actorData, !cMyFollower, false);
        if (pActor->GetExtension()->IsRemotePlayer())
            InventoryService::ApplyHandEquipment(pActor, acMessage.CurrentInventory, true);

        if (cMyFollower)
        {
            static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_nextFollowerClaim;
            const auto now = std::chrono::steady_clock::now();
            auto& next = s_nextFollowerClaim[pActor->formID];
            if (now >= next)
            {
                next = now + std::chrono::seconds(1);
                spdlog::info("Follower {:X} (server id {:X}) came back as another player's; kept as she is here and asked for", pActor->formID, acMessage.ServerId);
                RequestOwnership(pActor->formID, acMessage.ServerId, cEntity);
            }
        }
        else
            MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);

        // The owner's leveled pick rides the assignment response for actors we discovered ourselves
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId);
    }
}

void CharacterService::OnCharacterSpawn(const CharacterSpawnRequest& acMessage) const noexcept
{
    if (acMessage.OwnershipEpoch == 0)
    {
        spdlog::warn("Ignored spawn for actor {:X} because the ownership epoch is invalid", acMessage.ServerId);
        return;
    }

    auto remoteView = m_world.view<RemoteComponent>();
    const auto remoteItor = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ServerId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteItor != std::end(remoteView))
    {
        const entt::entity cExisting = *remoteItor;
        const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cExisting);
        const bool hasActor = m_world.all_of<WaitingFor3D>(cExisting) || (pFormIdComponent && TESForm::GetById(pFormIdComponent->Id));

        if (hasActor)
        {
            // The actor is here, so it is not respawned -- but the message is not simply thrown away either.
            // A spawn is re-sent when something about the character changed on the server, and the ownership
            // epoch is the part that matters: upstream's ownership rework refuses a claim whose epoch does not
            // match the server's, so a stale epoch here means every later attempt to take this actor is rejected
            // for reasons nothing logs. Refreshing it costs nothing and cannot move or redraw anything.
            //
            // Position, cell and death state are deliberately left alone. They arrive continuously through the
            // movement and death paths, and forcing them from a spawn message would fight those.
            auto& remoteComponent = remoteView.get<RemoteComponent>(cExisting);
            if (remoteComponent.OwnershipEpoch != acMessage.OwnershipEpoch)
            {
                spdlog::info("Character {:X} is already spawned; its ownership epoch moved {} -> {}", acMessage.ServerId, remoteComponent.OwnershipEpoch, acMessage.OwnershipEpoch);
                remoteComponent.OwnershipEpoch = acMessage.OwnershipEpoch;
            }
            else
                spdlog::warn("Character with remote id {:X} is already spawned.", acMessage.ServerId);

            // ...but "they arrive continuously through the movement path" stops being true the moment the server
            // withholds them, and that is exactly when this re-send happens.
            //
            // The server range-filters updates by grid (uGridsToLoad 5, so two cells). Walk out of that and the
            // updates stop: on 2026-09-26 a bot five cells away produced "0 character updates sent, 321 withheld".
            // Meanwhile the real actor carries on being moved by its owner. Walk back and the server re-sends this
            // spawn to put the copy right -- and the copy, which never stopped existing, ignored it and stayed
            // where it was last seen. That is a body in the wrong place and a follower that looks frozen.
            //
            // So: apply the spawn's position only when nothing has been arriving for this character. If updates
            // are flowing the interpolation is fresh and is left alone, which is what the note above is protecting.
            if (auto* pInterpolation = m_world.try_get<InterpolationComponent>(cExisting))
            {
                constexpr uint64_t cStaleAfterMs = 2000;
                const uint64_t currentTick = m_transport.GetClock().GetCurrentTick();
                const uint64_t newestTick = pInterpolation->TimePoints.empty() ? 0 : pInterpolation->TimePoints.back().Tick;
                const bool stale = newestTick == 0 || (currentTick > newestTick && currentTick - newestTick > cStaleAfterMs);

                if (stale && pFormIdComponent)
                {
                    if (Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id)))
                    {
                        const glm::vec3 wanted{acMessage.Position.x, acMessage.Position.y, acMessage.Position.z};
                        spdlog::info("Character {:X} was {} ms without an update and the server has re-sent it; moving the copy to where the server says ({:.0f}, {:.0f}, {:.0f})", acMessage.ServerId,
                                     newestTick == 0 ? currentTick : currentTick - newestTick, wanted.x, wanted.y, wanted.z);

                        // Drop what is buffered: every point in it predates the silence and would drag the copy
                        // back through where it used to be.
                        pInterpolation->TimePoints.clear();
                        pInterpolation->Position = wanted;
                        pActor->ForcePosition(NiPoint3(wanted));

                        // Position is not the only thing that went stale. Health travels as
                        // NotifyActorValueChanges and equipment as its own message, and **both are sent in range
                        // only** -- so a character that healed, took a beating or drew a sword while this client
                        // was away comes back wrong in all of those too. That is "she seems frozen, not swapping
                        // weapons" (2026-09-25). The spawn carries the right values; under this staleness gate
                        // there is nothing fresher to fight with, which is what made applying them unsafe before.
                        pActor->SetActorValues(acMessage.IsPlayer ? StandingValues(acMessage.InitialActorValues, pActor->formID) : acMessage.InitialActorValues);
                        if (pActor->actorState.IsWeaponDrawn() != acMessage.IsWeaponDrawn)
                            pActor->SetWeaponDrawnEx(acMessage.IsWeaponDrawn);

                        // Death, which is the worst of them to miss. A death is broadcast to every player rather
                        // than only those in range, precisely so that nobody can miss it -- but the receiving
                        // handler only applies it when the copy's ownership epoch matches the message's, and
                        // `NotifyOwnershipTransfer` **is** range-filtered. So a player who was away while the
                        // actor changed hands has a stale epoch, drops the death on arrival, and is left with a
                        // corpse still walking around. The epoch is refreshed a few lines above, but the death
                        // that was already thrown away does not come back on its own; the spawn is the only place
                        // left that still knows.
                        if (pActor->IsDead() != acMessage.IsDead)
                        {
                            spdlog::info("Character {:X} came back {} and this copy had it the other way; correcting", acMessage.ServerId, acMessage.IsDead ? "dead" : "alive");
                            acMessage.IsDead ? pActor->Kill() : pActor->Respawn();
                        }

                        // Factions, for the same reason and with more at stake than they look: a character that
                        // turned hostile while this client was away is still friendly on this copy, and a copy in
                        // the wrong faction is a creature that squares up to somebody and never swings. That is
                        // the shape of the Burned Spriggan of 2026-09-25, which spent a fight trying to attack
                        // Lydia and never landing one. Faction changes are range-filtered like everything else
                        // here, so being away is exactly when they are missed.
                        if (auto* pCache = m_world.try_get<CacheComponent>(cExisting))
                        {
                            if (!(pCache->FactionsContent == acMessage.FactionsContent))
                            {
                                spdlog::info("Character {:X} came back in different factions from the server's; applying them", acMessage.ServerId);
                                pCache->FactionsContent = acMessage.FactionsContent;
                                pActor->SetFactions(pCache->FactionsContent);
                            }
                        }

                        // Inventory is deliberately **not** applied yet, only compared. SetActorInventory is what
                        // the naked-NPC check calls, and that is the path that produced 717 refused Unequip
                        // replays on a single guard; doing it on every return from out of range, unprompted, is
                        // how that comes back. One session's worth of this line says whether it is worth the risk.
                        if (!(pActor->GetActorInventory() == acMessage.InventoryContent))
                            spdlog::info("Character {:X} also came back with a different inventory from the server's; not applied, only noticed", acMessage.ServerId);
                    }
                }
            }

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

    // One of this game's own actors, coming back from the player it was given to. See s_handedAway.
    if (acMessage.FormId == GameId{})
    {
        if (const auto handed = s_handedAway.find(acMessage.ServerId); handed != s_handedAway.end())
        {
            const uint32_t cOwnId = handed->second.FormId;
            s_handedAway.erase(handed);

            Actor* pOwn = Cast<Actor>(TESForm::GetById(cOwnId));
            TESNPC* pSentBase = acMessage.BaseId != GameId{} ? Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.BaseId))) : nullptr;
            const bool cSameKind = pOwn && pSentBase && pOwn->baseForm == pSentBase;

            const auto view = m_world.view<FormIdComponent>();
            const auto itor = std::find_if(std::begin(view), std::end(view), [cOwnId, view](entt::entity aOther) { return view.get<FormIdComponent>(aOther).Id == cOwnId; });

            if (pOwn && !pOwn->IsDeleted() && cSameKind)
            {
                pActor = pOwn;
                entity = itor != std::end(view) ? *itor : m_world.create();
                spdlog::info("HandedAway: server character {:X} is this game's own actor {:X} come back; it is that actor again and no copy is made", acMessage.ServerId, cOwnId);
            }
            else
            {
                spdlog::info("HandedAway: server character {:X} came back, but the actor it was here ({:X}) is {}; a copy is made", acMessage.ServerId, cOwnId,
                             !pOwn ? "not loaded" : pOwn->IsDeleted() ? "deleted" : "another creature now");
                // Whatever carries that id now was held back from the server on the strength of the record.
                if (pOwn && itor != std::end(view))
                    ProcessNewEntity(*itor);
            }
        }
    }

    if (pActor)
    {
        // Taken back above.
    }
    // Custom forms
    else if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        entity = m_world.create();

        if (acMessage.BaseId != GameId{})
        {
            pNpc = BaseForCopy(m_world, acMessage);

            if (!pNpc)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod, base: {:X}:{:X}, form: {:X}:{:X}", acMessage.BaseId.BaseId, acMessage.BaseId.ModId, acMessage.FormId.BaseId, acMessage.FormId.ModId);
                return;
            }

            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            // Players and npcs with temporary ref ids and base ids (usually random events)
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, *entity, acMessage.FaceTints);
        }

        pActor = RememberOwnCopy(Actor::Create(pNpc));
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
            Actor* pCopy = pOwnerBase ? RememberOwnCopy(Actor::Create(pOwnerBase)) : nullptr;
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

    // Pending reconciliation re-enables the actor after applying the owner's pick.
    if (pActor->IsDisabled() && pActor->GetExtension()->Reconciliation != ActorExtension::ReconciliationStage::WaitingForDisable)
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

    // A body that is to be dead is killed once it has its 3D, by the WaitingFor3D step every spawn goes through
    // (below). Killed before, it is dead with nothing played, and that step then finds it dead already and does
    // nothing: a Flame Atronach that had burst on its owner's screen came back here as a fresh copy and stood for
    // good (2026-10-04 16:03, server id 5000B1).
    if (pActor->IsDead() != acMessage.IsDead && (!acMessage.IsDead || pActor->GetNiNode()))
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

    spdlog::info("Spawn Request Is summon {}", acMessage.IsPlayerSummon);

    if (acMessage.IsPlayerSummon)
    {
        // Prevents remote summons agroing other players.
        pActor->SetCommandingActor(PlayerCharacter::Get()->GetHandle());
    }

    // Static references arrive with their own locally rolled leveled pick; conform to the owner's.
    if (acMessage.FormId != GameId{})
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId);

    auto& newRemote = m_world.emplace_or_replace<RemoteComponent>(*entity, acMessage.ServerId, pActor->formID, acMessage.OwnershipEpoch);
    if (s_ownCopies.find(pActor->formID) != s_ownCopies.end() && pActor->baseForm)
        newRemote.CachedBaseId = pActor->baseForm->formID;
    ClaimCopyId(*entity, pActor->formID);

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

        // The busy path, and the one that had no bound on it. See AnimationSystem::TrimBacklog: a Dwarven Centurion
        // reached 5,123 queued actions here on 2026-09-26 and Neloth 17,411, which is minutes of replay behind the
        // present on an actor that plays one action a frame.
        if (const auto* pFormId = m_world.try_get<FormIdComponent>(*itor))
            AnimationSystem::TrimBacklog(animationComponent, pFormId->Id);
        else
            AnimationSystem::TrimBacklog(animationComponent, 0);
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
    // Every remote actor, not only those with a CacheComponent. Copies made from a server spawn never get one (it is
    // set up for this client's own actors), so this handler matched none of them and a faction change made while
    // both players were there never reached the other screen; only the spawn re-sent after a trip carried it
    // (live-factions, 2026-10-03: the bot's bear joined BanditFaction in front of the player and the copy never did).
    auto view = m_world.view<RemoteComponent, FormIdComponent>();

    for (const auto& [id, factions] : acEvent.Changes)
    {
        const auto itor = std::find_if(std::begin(view), std::end(view), [id = id, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

        if (itor == std::end(view))
            continue;

        auto* const pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*itor).Id));
        if (!pActor)
            continue;

        if (auto* pCache = m_world.try_get<CacheComponent>(*itor))
            pCache->FactionsContent = factions;

        pActor->SetFactions(factions);
        spdlog::info("Factions of remote actor {:X} ({:X}) updated: {} faction(s) from its owner", pActor->formID, id, factions.NpcFactions.size() + factions.ExtraFactions.size());
    }
}

void CharacterService::OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) noexcept
{
    if (acMessage.OwnershipEpoch == 0)
    {
        spdlog::warn("Ignored ownership update for actor {:X} because the epoch is invalid", acMessage.ServerId);
        return;
    }

    auto entity = Utils::FindEntityByServerId(acMessage.ServerId);
    if (entity && !m_world.any_of<LocalComponent, RemoteComponent>(*entity))
        entity.reset();

    uint32_t currentEpoch = 0;
    if (entity)
    {
        if (const auto* pLocalComponent = m_world.try_get<LocalComponent>(*entity))
            currentEpoch = pLocalComponent->OwnershipEpoch;
        else if (const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(*entity))
            currentEpoch = pRemoteComponent->OwnershipEpoch;
    }

    if (currentEpoch != 0 && acMessage.OwnershipEpoch <= currentEpoch)
    {
        spdlog::debug("Ignored stale ownership update for actor {:X} at epoch {}; current epoch is {}", acMessage.ServerId, acMessage.OwnershipEpoch, currentEpoch);
        return;
    }

    const bool isLocalOwner = acMessage.OwnerPlayerId == m_transport.GetLocalPlayerId();
    if (!entity)
    {
        // A transfer does not contain enough form data to recreate an unknown actor. Decline so the server can try another loaded client.
        if (isLocalOwner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);
        else
            spdlog::debug("Ignored ownership update for unknown actor {:X} at epoch {}", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    const entt::entity cEntity = *entity;
    const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cEntity);
    Actor* pActor = pFormIdComponent ? Cast<Actor>(TESForm::GetById(pFormIdComponent->Id)) : nullptr;

    // Preserve the accepted epoch's pick for actors that still need to be created.
    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(cEntity))
        pWaitingFor3D->SpawnRequest.LeveledNpcPickId = acMessage.LeveledNpcPickId;

    // A reference that rolled as another creature here stands behind a ghost copy of the owner's kind. Taking it
    // over would put our own roll in charge and flip the creature for everyone (a wolf on his side became a troll
    // on hers after a hand-off, 2026-09-20 11:50). Decline, and the server keeps looking.
    //
    // Matched by either id. The character is usually bound to the stand-in copy, not to the local reference the
    // ghost list is keyed by, and looking up only the key missed it: on 2026-10-03 Emma's game took over Seen's bandit
    // C00F8 through its stand-in copy FF001177 ("Gained ownership of actor 25"), ran a copy it had made itself, and
    // the bandit stood frozen and could not be killed.
    if (isLocalOwner && pFormIdComponent)
    {
        const uint32_t cBoundId = pFormIdComponent->Id;
        const auto ghost = std::find_if(s_ghosts.begin(), s_ghosts.end(), [cBoundId](const auto& acEntry) { return acEntry.first == cBoundId || acEntry.second == cBoundId; });
        if (ghost != s_ghosts.end())
        {
            const auto* pCopy = Cast<Actor>(TESForm::GetById(ghost->second));
            const Actor* pLocal = Cast<Actor>(TESForm::GetById(ghost->first));
            const auto* pLocalBase = pLocal ? Cast<TESNPC>(pLocal->baseForm) : nullptr;
            const auto* pCopyBase = pCopy ? Cast<TESNPC>(pCopy->baseForm) : nullptr;
            spdlog::info("Hand-off of {:X} (server id {:X}) declined: a copy of ours stands in for reference {:X} here ({} here, the owner's kind is {}); taking it "
                         "would run our copy as the creature", pFormIdComponent->Id, acMessage.ServerId, ghost->first, pLocalBase ? pLocalBase->fullName.value.AsAscii() : "not loaded",
                         pCopyBase ? pCopyBase->fullName.value.AsAscii() : "?");
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }
    }

    if (isLocalOwner)
    {
        if (!pFormIdComponent || !pActor || !pActor->GetNiNode())
        {
            uint32_t cachedRefId = pFormIdComponent ? pFormIdComponent->Id : 0;
            if (const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(cEntity))
                cachedRefId = pRemoteComponent->CachedRefId;

            if (pActor)
                pActor->GetExtension()->SetRemote(true);

            m_world.remove<LocalAnimationComponent, LocalComponent>(cEntity);
            if (m_world.all_of<RemoteComponent>(cEntity))
                m_world.get<RemoteComponent>(cEntity).OwnershipEpoch = acMessage.OwnershipEpoch;
            else if (cachedRefId != 0)
                m_world.emplace<RemoteComponent>(cEntity, acMessage.ServerId, cachedRefId, acMessage.OwnershipEpoch);

            spdlog::warn("Declined ownership of actor {:X} at epoch {} because the actor is not ready", acMessage.ServerId, acMessage.OwnershipEpoch);
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }

        // Reconcile while hooks still treat the actor as remote/non-authoritative.
        pActor->GetExtension()->SetRemote(true);
        m_world.remove<LocalAnimationComponent, LocalComponent>(cEntity);
        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, pFormIdComponent->Id, acMessage.OwnershipEpoch);

        // This player's own follower coming back: her gear here is the truth (see OnAssignCharacter).
        const bool cMyFollower = pActor->IsPlayerTeammate() && !pActor->IsTemporary();
        ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, acMessage.CurrentActorData, !cMyFollower, true);
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId);

        DeleteRemoteEntityComponents(cEntity);
        CacheSystem::Setup(m_world, cEntity, pActor);
        m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);
        auto& localComponent = m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId, acMessage.OwnershipEpoch);
        localComponent.IsDead = acMessage.CurrentActorData.IsDead;
        localComponent.IsWeaponDrawn = acMessage.CurrentActorData.IsWeaponDrawn;

        // LocalComponent is installed only after canonical reconciliation is complete.
        pActor->GetExtension()->SetRemote(false);
        spdlog::info("Gained ownership of actor {:X} at epoch {}", acMessage.ServerId, acMessage.OwnershipEpoch);
        if (cMyFollower)
            SendInventoryDifference(pActor, acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.CurrentActorData.InitialInventory);
        return;
    }

    if (pActor)
        pActor->GetExtension()->SetRemote(true);

    m_world.remove<LocalAnimationComponent, LocalComponent>(cEntity);

    if (pFormIdComponent)
    {
        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, pFormIdComponent->Id, acMessage.OwnershipEpoch);

        if (!m_world.all_of<InterpolationComponent>(cEntity))
            InterpolationSystem::Setup(m_world, cEntity);
        if (!m_world.all_of<RemoteAnimationComponent>(cEntity))
            AnimationSystem::Setup(m_world, cEntity);
    }
    else if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(cEntity))
    {
        pRemoteComponent->OwnershipEpoch = acMessage.OwnershipEpoch;
    }

    // This player's own follower given to somebody else -- the server does that while this game is on a loading
    // screen and silent (2026-10-04 09:33, 2026-10-03 15:09). Her gear here is kept, and she is asked for back at once
    // rather than after the twelve seconds of being pulled between the two games (see OnAssignCharacter).
    const bool cMyFollower = pActor && pActor->IsPlayerTeammate() && !pActor->IsTemporary();
    ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, acMessage.CurrentActorData, pActor && pActor->GetNiNode() && !cMyFollower, false);
    if (pActor)
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId);

    spdlog::info("Actor {:X} is now owned by player {:X} at epoch {}", acMessage.ServerId, acMessage.OwnerPlayerId, acMessage.OwnershipEpoch);
    // Asked for back only while she is here, loaded and near the player. One left behind (the player went on, she
    // has not caught up yet) cannot be run by this game, which would hand her straight back: in the rig the server
    // and this game passed her to and fro every four seconds (2026-10-04 10:51). When she does catch up, she arrives
    // as another player's and is asked for then (OnAssignCharacter).
    if (cMyFollower)
    {
        const auto* pPlayer = PlayerCharacter::Get();
        const bool cHere = pActor->GetNiNode() && pPlayer && glm::distance(glm::vec3(pActor->position), glm::vec3(pPlayer->position)) < 2048.f;
        spdlog::info("Follower {:X} (server id {:X}) was given to player {:X}; {}", pActor->formID, acMessage.ServerId, acMessage.OwnerPlayerId,
                     cHere ? "asked for back" : "not near this player, asked for once she is");
        if (cHere)
            RequestOwnership(pActor->formID, acMessage.ServerId, cEntity);
    }
}

void CharacterService::OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) const noexcept
{
    // Removals go to every player, in range or not, so a record kept for a character nobody has any more ends here.
    if (const auto handed = s_handedAway.find(acMessage.ServerId); handed != s_handedAway.end())
    {
        const uint32_t cOwnId = handed->second.FormId;
        s_handedAway.erase(handed);
        spdlog::info("HandedAway: server character {:X} is gone from the server; actor {:X} is an ordinary actor of this game again", acMessage.ServerId, cOwnId);

        // If it stands here unannounced, it is announced now.
        const auto formView = m_world.view<FormIdComponent>();
        const auto itor = std::find_if(std::begin(formView), std::end(formView), [cOwnId, formView](entt::entity aOther) { return formView.get<FormIdComponent>(aOther).Id == cOwnId; });
        if (itor != std::end(formView))
            ProcessNewEntity(*itor);
    }

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
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));
            if (pActor && pActor->IsTemporary())
                CharacterService::DeleteTempActor(pFormIdComponent->Id);
            else if (pActor)
                pActor->GetExtension()->SetRemote(false);
        }

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

    // find_if returns end() when the player's entity is not in the world yet, or has already been torn down, and
    // dereferencing that is undefined. A werewolf or vampire lord transformation at either of those moments would
    // have taken the game with it.
    if (it == view.end())
    {
        spdlog::warn("{}: the player has no entity right now; beast form change not sent", __FUNCTION__);
        return;
    }

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
    const auto* pRiderLocalComponent = m_world.try_get<LocalComponent>(cRiderEntity);
    if (!pRiderLocalComponent)
        return;

    const auto mountIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.MountID, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (mountIt == std::end(view))
    {
        spdlog::warn("Mount not found, form id: {:X}", acEvent.MountID);
        return;
    }

    const entt::entity cMountEntity = *mountIt;

    uint32_t mountServerId = 0;
    uint32_t mountOwnershipEpoch = 0;
    if (const auto* pMountLocalComponent = m_world.try_get<LocalComponent>(cMountEntity))
    {
        mountServerId = pMountLocalComponent->Id;
        mountOwnershipEpoch = pMountLocalComponent->OwnershipEpoch;
    }
    else if (const auto* pMountRemoteComponent = m_world.try_get<RemoteComponent>(cMountEntity))
    {
        mountServerId = pMountRemoteComponent->Id;
        mountOwnershipEpoch = pMountRemoteComponent->OwnershipEpoch;
    }
    else
        return;

    MountRequest request{};
    request.RiderId = pRiderLocalComponent->Id;
    request.RiderOwnershipEpoch = pRiderLocalComponent->OwnershipEpoch;
    request.MountId = mountServerId;
    request.MountOwnershipEpoch = mountOwnershipEpoch;

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
    if (!pRider)
        return;

    const auto mountIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.MountId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });
    if (mountIt == std::end(remoteView))
    {
        spdlog::warn("Cannot apply mount update because mount {:X} is unavailable", acMessage.MountId);
        return;
    }

    const auto& mountFormIdComponent = remoteView.get<FormIdComponent>(*mountIt);
    Actor* pMount = Cast<Actor>(TESForm::GetById(mountFormIdComponent.Id));
    if (!pMount)
        return;

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
    if (!pPackage)
    {
        // The id resolved to a form that is not a package: a mod mismatch between the two games, or a form this
        // side loaded as something else. Handing null to SetPackage is not worth finding out about the hard way.
        spdlog::warn("Form {:X} is not a package; leaving actor {:X} on the package it has", cPackageFormId, pActor->formID);
        return;
    }

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

            RequestOwnership(pActor->formID, pRemoteComponent->Id, aEntity);
        }
        else
            spdlog::info("New entity remotely managed, form id: {:X}, server id: {:X}", pActor->formID, pRemoteComponent->Id);

        return;
    }

    if (m_world.any_of<RemoteComponent, LocalComponent, WaitingForAssignmentComponent>(aEntity))
        return;

    // One of this game's own actors that another player was given while it was out of view. The server still has it
    // as that character and sends it when it is in range; announcing it here would make a second one of it for
    // everybody. See s_handedAway.
    if (uint32_t serverId = 0; IsHandedAway(pActor, serverId))
    {
        const auto remoteView = m_world.view<RemoteComponent>();
        const bool cCopyStands = std::any_of(std::begin(remoteView), std::end(remoteView), [remoteView, serverId](entt::entity aOther) { return remoteView.get<RemoteComponent>(aOther).Id == serverId; });
        if (cCopyStands)
            spdlog::warn("HandedAway: actor {:X} is back in view after the server sent character {:X}, so a copy of it stands here too; not handled, both stay", pActor->formID,
                         serverId);
        else
            spdlog::info("HandedAway: actor {:X} is back in view and is still server character {:X}; not announced as a new one, the server sends it when it is in range",
                         pActor->formID, serverId);
        return;
    }

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

    if (const TESNPC* pPick = pActor->GetLeveledPick())
    {
        const uint32_t pickFormId = pPick->formID;
        if (m_world.GetModSystem().GetServerModId(pickFormId, message.LeveledNpcPickId))
            spdlog::info("Captured leveled NPC pick {:X} for actor {:X} (base {:X})", pickFormId, pActor->formID, pNpc->formID);
        else
            spdlog::warn("Leveled NPC pick {:X} has no server id, identity sync skipped", pickFormId);
    }
    else if (pNpc->IsTemporary())
    {
        spdlog::info("No leveled pick recoverable for temp base {:X} (actor {:X}), identity sync unavailable", pNpc->formID, pActor->formID);
    }

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
    // TEMPORARY (2026-10-03): what a temporary actor was when its removal was noticed. Three crashes on 2026-10-02
    // came within a second of a trip that left copies of Seekers behind which this client ran itself, and for those
    // nothing is logged below: either the game had freed them already, or they were not in the list of copies.
    const bool cTemporaryId = (aFormId & 0xFF000000) == 0xFF000000;
    const bool cGameMade = s_ownCopies.find(aFormId) == s_ownCopies.end();
    if (cTemporaryId)
    {
        Actor* pSeen = Cast<Actor>(TESForm::GetById(aFormId));
        spdlog::info("CopyGone: {:X} removed as {}; the actor is {}; made by this client: {}", aFormId,
                     m_world.all_of<RemoteComponent>(aEntity) ? "remote" : m_world.all_of<LocalComponent>(aEntity) ? "local" : m_world.all_of<WaitingForAssignmentComponent>(aEntity) ? "waiting" : "untracked",
                     pSeen ? (pSeen->IsDeleted() ? "still there, marked deleted" : pSeen->GetNiNode() ? "still there, with 3D" : "still there, no 3D") : "gone from the game already",
                     s_ownCopies.find(aFormId) != s_ownCopies.end() ? "yes" : "no");
    }

    if (m_world.all_of<RemoteComponent>(aEntity))
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));

        if (pActor)
        {
            if (pActor->IsTemporary() && !IsProcessExiting() && s_ownCopies.find(aFormId) == s_ownCopies.end())
            {
                // The game's own temporary actor, which only became remote because the server gave it to somebody
                // else. The game is disposing of it right now and will finish; all that is ours here is the record
                // of it. See s_ownCopies.
                spdlog::info("Temporary remote {:X} was made by the game, not by this client; left to the game (form flags {:X})", aFormId, pActor->flags);
                const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
                RecentDeletes::Record(pActor, aFormId, refWord & 0x3FF, refWord, RecentDeletes::kLeftToGame);
                if (ActorExtension* pExtension = pActor->GetExtension())
                    pExtension->SetRemote(false);

                const uint32_t cServerId = m_world.get<RemoteComponent>(aEntity).Id;
                s_handedAway[cServerId] = HandedAway{aFormId, pActor->baseForm ? pActor->baseForm->formID : 0};
                spdlog::info("HandedAway: actor {:X} is server character {:X}, another player's now; remembered, so that it is the same actor when it comes back", aFormId,
                             cServerId);
            }
            else if (pActor->IsTemporary() && !IsProcessExiting())
            {
                // Deleted on a later frame, not here.
                //
                // Seen's dump of 2026-09-27 12:09 proved this by address rather than by timing: the object the
                // game made a virtual call on, Rcx = 0x9ceabe10, is exactly the actor FF001178 that this line
                // had just deleted. Its first vtable had already been overwritten with heap garbage. The game
                // was still holding the pointer and still using it.
                //
                // This runs from the game's own "that reference is gone" notification, which fires while the
                // game is partway through disposing of the actor -- during a cell unload, dozens at a time.
                // Freeing it underneath that is the race. The work still happens, one frame later, from the
                // service update, by which point the game has finished with it.
                // Who still had a claim on it, captured before the delete because afterwards it is gone. The
                // outstanding handle count is the one that matters: a BSPointerHandle is exactly how the game
                // keeps hold of an actor across time, and a pointer that survived 24 seconds (2026-09-27 16:05)
                // was being kept by something.
                uint32_t claims = 0;
                if (const PlayerCharacter* pPlayer = PlayerCharacter::Get())
                {
                    if (pPlayer->GetCombatTarget() == pActor)
                        claims |= RecentDeletes::kPlayerCombatTarget;
                }
                if (pActor->currentProcess)
                    claims |= RecentDeletes::kHasProcess;
                if (pActor->GetNiNode())
                    claims |= RecentDeletes::kHas3D;
                if (pActor->actorState.IsDeadOrDying())
                    claims |= RecentDeletes::kDeadOrDying;

                // Anyone else fighting it. Only the actors this mod already tracks, which is the set that
                // matters and keeps this to a short loop.
                {
                    auto combatView = m_world.view<FormIdComponent>();
                    for (auto other : combatView)
                    {
                        Actor* pOther = Cast<Actor>(TESForm::GetById(combatView.get<FormIdComponent>(other).Id));
                        if (pOther && pOther != pActor && pOther->GetCombatTarget() == pActor)
                        {
                            claims |= RecentDeletes::kOtherCombatTarget;
                            break;
                        }
                    }
                }

                const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
                const uint32_t handles = refWord & 0x3FF;
                RecentDeletes::Record(pActor, aFormId, handles, refWord, claims);

                if (CopyRemovalPolicy::OnRemoved(handles, s_waitingCopies.size()) == CopyRemovalPolicy::Action::WaitDisabled)
                {
                    // Logged after the decision, not before it. The old line said "Temporary Remote Deleted"
                    // ahead of the branch, so the log could not tell a copy that was safely freed from one that
                    // was forced out because the list was full from one that was merely held back -- three very
                    // different outcomes, all written the same way, in the log this whole investigation reads.
                    spdlog::info("Temporary Remote Held {:X}: {} handle(s) still on it, disabled and queued ({} of {} slots used)",
                                 aFormId, handles, s_waitingCopies.size() + 1, CopyRemovalPolicy::kMaxWaiting);

                    // Disabled, not freed: silent, no AI, not drawn, and nothing dangling. It costs a slot in a
                    // list that cannot grow past kMaxWaiting.
                    pActor->Disable();
                    s_waitingCopies.push_back(WaitingCopy{aFormId, std::chrono::steady_clock::now()});
                    ReleaseGhostOf(aFormId);
                    DeleteRemoteEntityComponents(aEntity);

                    // The server still counts this character as spawned here, exactly as on the branch below, and
                    // this branch forgot to say so. It is the one a copy takes when something still holds it, which
                    // a player copy nearly always does -- so this was Seen invisible three times on 2026-10-03
                    // (09:19, 09:30, 09:35): each time the server re-sent him as he came through the door, the
                    // re-send landed on his old copy still in the previous cell, and 0.1 to 3 s later that cell's
                    // teardown held the copy here and dropped the record. Nobody asked again until he reconnected.
                    if (m_transport.IsConnected())
                        DiscoveryService::RequestCellReannounce();
                    return;
                }

                // Nothing holds it, or the waiting list is full: free it here.
                //
                // Waiting for the handles to reach zero was tried on 2026-09-27 and put Emma straight back into
                // the pile-up that had already cost her one session: the copies are dead Seekers the game keeps
                // claims on, the claims do not clear while the cell is loaded, so nothing was ever freed and her
                // frame rate went again. Twice with the same failure is enough -- nothing that can accumulate
                // ships again without a mechanism that provably cannot accumulate.
                //
                // So this is the old behaviour, which costs Seen a crash and costs Emma nothing. The
                // DeleteClaim line above still records the handles, which is how the cause is known at all.
                if (handles == 0)
                {
                    spdlog::info("Temporary Remote Deleted {:X}: nothing held it", aFormId);
                }
                else
                {
                    // The bound turning a wait into an immediate delete. This is the old crash path, taken on
                    // purpose rather than by accident, and it needs to be visible as such -- a session where
                    // this is the common outcome is a session where the wait is not protecting anything and
                    // kMaxWaiting is what needs raising.
                    spdlog::warn("CopyFreedHeld: {:X} freed immediately with {} handle(s) still outstanding -- the waiting list was full ({} slots)",
                                 aFormId, handles, CopyRemovalPolicy::kMaxWaiting);
                }

                pActor->Delete();
                s_ownCopies.erase(aFormId);
                ReleaseGhostOf(aFormId);

                // This is a decision taken here, not one the server asked for: the reference went away because a
                // cell unloaded under it. The server still counts this character as spawned on this client and
                // will not send it again, so the copy stays gone -- the invisible player of 2026-09-26 11:20.
                // Announcing the cell again makes the server re-send everything in it.
                if (m_transport.IsConnected())
                    DiscoveryService::RequestCellReannounce();
            }
            else
            {
                pActor->GetExtension()->SetRemote(false);
            }
        }

        DeleteRemoteEntityComponents(aEntity);

        return;
    }

    // Not remote: if this was a copy of ours, it has since been given to this client to run, and the game is
    // letting go of it like any actor of its own. It is not deleted here, so its id must not stay in the list of
    // copies that are. See s_ownCopies.
    s_ownCopies.erase(aFormId);

    // Keep the cookie until the server response arrives so awarded ownership can be relinquished.
    if (m_world.all_of<WaitingForAssignmentComponent>(aEntity))
    {
        auto& waitingComponent = m_world.get<WaitingForAssignmentComponent>(aEntity);
        waitingComponent.Cancelled = true;
        return;
    }

    if (m_world.all_of<LocalComponent>(aEntity))
    {
        auto& localComponent = m_world.get<LocalComponent>(aEntity);

        RequestOwnershipTransfer request{};
        request.ServerId = localComponent.Id;
        request.OwnershipEpoch = localComponent.OwnershipEpoch;
        request.Reason = OwnershipReleaseReason::Relinquish;

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

                if (TESObjectCELL* pCell = pActor->GetParentCellEx())
                {
                    if (!modSystem.GetServerModId(pCell->formID, request.CellId))
                        spdlog::error("Cell id not found, despite having a cell, {:X}", pCell->formID);
                }

                request.Position = pActor->position;
            }
        }

        spdlog::info(
            "Transferring ownership of local actor, server id: {:X}, epoch: {}, worldspace: {:X}, cell: {:X}, position: "
            "({}, {}, {})",
            request.ServerId, request.OwnershipEpoch, request.WorldSpaceId.BaseId, request.CellId.BaseId, request.Position.x, request.Position.y, request.Position.z);

        m_transport.Send(request);

        // One of this game's own temporary actors, given up because its cell unloaded. If another player is there
        // the server gives it to them, and it is then theirs under this same id. See s_handedAway.
        if (cTemporaryId && cGameMade && !IsProcessExiting())
        {
            const Actor* pOwn = Cast<Actor>(TESForm::GetById(aFormId));
            s_handedAway[request.ServerId] = HandedAway{aFormId, pOwn && pOwn->baseForm ? pOwn->baseForm->formID : 0};
            spdlog::info("HandedAway: actor {:X} was server character {:X} and is given up; remembered, in case the server gives it to another player and sends it back", aFormId,
                         request.ServerId);
        }

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
            pNpc = BaseForCopy(m_world, acMessage);

            if (!pNpc)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod");
                return nullptr;
            }

            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, aEntity, acMessage.FaceTints);
        }

        pActor = RememberOwnCopy(Actor::Create(pNpc));
    }
    else
    {
        // A placed reference whose stand-in copy is gone. Stand-ins are made at once for a reference that is not loaded
        // here, which means at the owner's position, outside this game's loaded cells: they get no 3D, and the next
        // grid shift unloads the cell they were parented to and the game deletes them without a word. Emma's session of
        // 2026-10-03 lost five that way at 09:08:30.493, the very moment of a grid change, and those creatures were
        // invisible to her from then on (the local references disabled as ghosts, the copies gone). This only runs
        // once the character is inside the loaded grid, so the new stand-in is made where it can stay.
        const uint32_t cRefId = m_world.GetModSystem().GetGameId(acMessage.FormId);
        TESNPC* pOwnerBase = nullptr;
        if (acMessage.LeveledNpcPickId != GameId{})
            pOwnerBase = Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.LeveledNpcPickId)));
        if (!pOwnerBase && acMessage.BaseId != GameId{})
            pOwnerBase = Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.BaseId)));

        if (pOwnerBase && cRefId != 0)
        {
            pActor = RememberOwnCopy(Actor::Create(pOwnerBase));
            if (pActor)
            {
                s_ghosts[cRefId] = pActor->formID;
                if (Actor* pLocal = Cast<Actor>(TESForm::GetById(cRefId)); pLocal && !pLocal->IsDisabled() && !IsProcessExiting())
                    pLocal->Disable();
                spdlog::info("Stand-in: the copy for reference {:X} (server id {:X}) was gone; copy {:X} of the owner's {} ({:X}) stands in again, inside the loaded cells",
                             cRefId, m_world.get<RemoteComponent>(aEntity).Id, pActor->formID, pOwnerBase->fullName.value.AsAscii(), pOwnerBase->formID);
            }
        }
    }

    auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);

    if (!pActor)
    {
        // Once per character: RunSpawnUpdates asks again every five seconds.
        static std::unordered_set<uint32_t> s_said;
        if (s_said.size() > 1024)
            s_said.clear();
        if (s_said.insert(remoteComponent.Id).second)
            spdlog::error(__FUNCTION__ ": could not spawn actor for remote server id {:X} ({}); asking again every 5 s", remoteComponent.Id,
                          acMessage.FormId != GameId{} ? "a placed reference whose copy is gone" : "no base to build it from");
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
void CharacterService::ApplyLeveledNpcPick(Actor* apActor, const GameId& acPickId) const noexcept
{
    if (acPickId == GameId{})
        return;

    // Back on for VR since 2026-09-23: SetLeveledCreature has a real address now (AE 20231 -> VR 0x1402b8f10),
    // and CreateTemplateActorBase (AE 14375 -> VR 0x19c0c0) is corroborated by cmpayc/TiltedEvolutionVR, which
    // derived the same address independently and declares it `TESNPC* thiscall(TESNPC*, TESNPC*)` against
    // upstream's `TESActorBase* fastcall(TESActorBase*, TESActorBase*)` -- the same registers on x64, and TESNPC
    // derives from TESActorBase. The piece that crashed Seen on 2026-09-22, GarbageCollector::Add, is simply not
    // called here any more; see LeveledNpcSystem::ApplyPick.

    TESNPC* pBase = Cast<TESNPC>(apActor->baseForm);
    if (!pBase)
        return;

    if (!LeveledNpcSystem::GetOriginalBase(apActor))
        {
        spdlog::warn("Leveled pick {:x}:{:x} received for actor {:X} without an original leveled base, keeping local base", acPickId.ModId, acPickId.BaseId, apActor->formID);
            return;
        }

    const uint32_t cPickId = World::Get().GetModSystem().GetGameId(acPickId);
    if (cPickId == 0)
    {
        spdlog::warn("Leveled NPC pick {:X}:{:X} not resolvable, possibly missing mod, keeping local pick", acPickId.ModId, acPickId.BaseId);
        return;
    }

    TESNPC* pPick = Cast<TESNPC>(TESForm::GetById(cPickId));
    if (!pPick)
    {
        spdlog::warn("Leveled NPC pick {:X} is not an NPC, keeping local pick", cPickId);
        return;
    }

    const TESNPC* pLocalPick = apActor->GetLeveledPick();
    const uint32_t localPickId = pLocalPick ? pLocalPick->formID : 0;

    // Even a pick matching the current base must supersede pending work.
    if (pBase->IsTemporary() && localPickId == cPickId && m_pendingLeveledConforms.find(apActor->formID) == m_pendingLeveledConforms.end())
    {
        spdlog::info("Leveled actor {:X} already matches owner's pick {:X}", apActor->formID, cPickId);
        return;
    }

    spdlog::info("Queued leveled NPC reconciliation for actor {:X}, base: {:X}, local pick: {:X}, owner's pick: {:X}",
        apActor->formID, pBase->formID, localPickId, cPickId);

    // Defer reference changes to the service update: cell attach may still own the actor here.
    // Queueing to the runner from a drained task would re-lock its drain mutex.
    // Preserve the stage when a newer pick arrives during a disable or rebuild.
    m_pendingLeveledConforms[apActor->formID] = cPickId;
}

void CharacterService::ProcessLeveledConforms() noexcept
{
    using ReconciliationStage = ActorExtension::ReconciliationStage;

    if (m_pendingLeveledConforms.empty())
        return;

    // Never touch references while the loading screen is up - the cell attach
    // owns them and mutating mid-stream crashes the loader
    UI* pUI = UI::Get();
    if (pUI && pUI->GetMenuOpen(BSFixedString("Loading Menu")))
        return;

    for (auto it = m_pendingLeveledConforms.begin(); it != m_pendingLeveledConforms.end();)
    {
        const uint32_t cPickFormId = it->second;

        Actor* pActor = Cast<Actor>(TESForm::GetById(it->first));
        TESNPC* pPick = Cast<TESNPC>(TESForm::GetById(cPickFormId));
        if (!pActor || pActor->IsDeleted() || !pPick)
        {
            if (pActor)
                pActor->GetExtension()->Reconciliation = ReconciliationStage::None;

            it = m_pendingLeveledConforms.erase(it);
            continue;
        }

        auto& stage = pActor->GetExtension()->Reconciliation;
        if (stage == ReconciliationStage::WaitingFor3D)
        {
            const auto* pCell = pActor->GetParentCellEx();
            if (!pCell || !pCell->IsAttached())
            {
                spdlog::info("Abandoning leveled NPC reconciliation for actor {:X} because its cell is not attached, pick: {:X}, cell state: {}, disabled: {}",
                    it->first, cPickFormId, pCell ? static_cast<int>(pCell->cellState) : -1, pActor->IsDisabled());
                stage = ReconciliationStage::None;
                it = m_pendingLeveledConforms.erase(it);
                continue;
            }

            if (pActor->IsDisabled() || !pActor->GetNiNode())
            {
                ++it;
                continue;
            }

            if (pActor->baseForm && pActor->baseForm->IsTemporary() && pActor->GetLeveledPick() == pPick)
            {
                spdlog::info("Completed leveled NPC reconciliation for actor {:X}, base: {:X}, pick: {:X}", it->first, pActor->baseForm->formID, cPickFormId);
                stage = ReconciliationStage::None;
                it = m_pendingLeveledConforms.erase(it);
                continue;
            }

            // A newer pick arrived during the rebuild; start its disable now.
            stage = ReconciliationStage::None;
        }

        if (stage == ReconciliationStage::WaitingForDisable)
        {
            if (!pActor->IsDisabled() || pActor->GetNiNode())
            {
                spdlog::debug("Waiting for leveled actor {:X} to finish disabling before applying pick {:X}, disabled: {}, has 3D: {}",
                    it->first, cPickFormId, pActor->IsDisabled(), pActor->GetNiNode() != nullptr);
                ++it;
                continue;
            }

            if (!LeveledNpcSystem::ApplyPick(pActor, pPick))
            {
                spdlog::warn("Could not rebuild leveled actor {:X} from its original base and pick {:X}, keeping local base", it->first, cPickFormId);
            pActor->EnableImpl();
                stage = ReconciliationStage::None;
                it = m_pendingLeveledConforms.erase(it);
                continue;
            }

            // Recompute the graph descriptor after changing picks; stale variable indices can cause out-of-bounds writes.
            pActor->GetExtension()->GraphDescriptorHash = 0;

            // Enable can return before the rebuilt 3D is available to discovery.
            stage = ReconciliationStage::WaitingFor3D;
            pActor->EnableImpl();
            spdlog::info("Re-enabled conformed leveled actor {:X}, base: {:X}, pick: {:X}, waiting for 3D",
                it->first, pActor->baseForm->formID, cPickFormId);
            ++it;
            continue;
        }

        if (!pActor->loadedState && !LeveledNpcSystem::IsLeveledNpcBase(Cast<TESNPC>(pActor->baseForm)))
        {
            // Wait for distant actors to load 3D; newer picks replace pending work and disconnects clear it.
            // Unresolved shells bypass this wait because they need a pick before they can load a model.
            ++it;
            continue;
        }

        // DisableImpl() is asynchronous: it only queues a request to disable this actor.
        // Wait for the disabled flag and old 3D removal before changing the base.
        pActor->DisableImpl();
        stage = ReconciliationStage::WaitingForDisable;
        ++it;
    }
}

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

    // How long this client was silent, named at the moment it stops being silent.
    //
    // A timeout is the server deciding nobody is home, and it is the single most destructive thing that happens to
    // a session: Seen's client dropped five times on 2026-09-25 and handed back 45 actors in one burst, which is
    // where the launched bandit, the body in the wrong place, the frozen follower and the spriggan that could not
    // land a hit all came from. Every log so far shows the aftermath and never the gap itself. This is the gap: if
    // the pause counter is up it was a menu, if a load was running it was the load, and if it was neither then the
    // client stalled and the section timings above say on what.
    if (lastSendTimePoint.time_since_epoch().count() != 0)
    {
        const auto silence = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSendTimePoint).count();
        if (silence >= 1000)
        {
            UI* pUI = UI::Get();
            std::string menus;
            if (pUI)
            {
                for (IMenu* pMenu : pUI->menuStack)
                {
                    if (!pMenu)
                        continue;
                    if (BSFixedString* pName = pUI->LookupMenuNameByInstance(pMenu))
                        menus += fmt::format("{}{}", menus.empty() ? "" : ", ", pName->AsAscii());
                }
            }
            spdlog::warn("Silence: sent nothing for {} ms; menus [{}]. The server drops a client that stops talking, and a drop hands its actors away.", silence,
                         menus.empty() ? "none" : menus);
        }
    }

    lastSendTimePoint = now;

#ifdef SKYRIMVR
    // Nocked arrow, reported 2026-09-25: the arrow *leaves* fine because a shot travels as its own projectile
    // message, but the arrow sitting on the string is an animation attachment and the other side never gets one.
    // Before anything is built for it, this says whether VR archery moves the attack state at all -- the state the
    // game itself uses to decide an arrow is on the bow. If it never leaves 0 through a whole draw and release,
    // there is nothing to replicate and the arrow has to be put there by hand on the receiving side.
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
    {
        static uint32_t s_lastAttackState = 0xFFFFFFFFu;
        const uint32_t attackState = pPlayer->actorState.AttackState();
        if (attackState != s_lastAttackState)
        {
            s_lastAttackState = attackState;
            spdlog::info("VRArchery: local attack state {} (9 bow draw, 10 arrow attached, 11 drawn, 12 releasing, 13 released), weapon drawn {}", attackState,
                         pPlayer->actorState.IsWeaponDrawn());
        }
    }
#endif

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

// The other player's name and health above their head, on the game's own world-space meter (the bar and name an
// NPC gets, WSEnemyMeters). There is one such meter and the game wants it too, so the rules keep it out of a real
// fight: the game's own target wins while it is fresh, and the friend's bar comes up when you look at them, or
// whenever they are hurt however you are facing. The first try held off for 8 s after any game target, which in a
// dungeon meant it almost never got a turn ("the healthbars don't seem to be consistent", 2026-09-20 21:15).
void CharacterService::RunEnemyMeterUpdates() noexcept
{
#ifdef SKYRIMVR
    constexpr float cRange = 3000.f;      // about 40 m
    constexpr float cAcquireAngle = 18.f; // degrees off the centre of your gaze to bring the bar up
    constexpr float cHoldAngle = 32.f;    // wider once it is up, so a glance aside does not drop it
    constexpr float cHurtFraction = 0.6f; // below this, show them wherever you are looking
    constexpr float cBlindRange = 1500.f; // fallback when the headset node cannot be read

    static std::chrono::steady_clock::time_point s_next;
    static uint32_t s_shownHandle = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + 250ms; // the meter fades if it is not told again, so this is a refresh as much as a decision

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
    // The game pointed the meter at something of its own (a hit on an enemy). Leave it alone, but only briefly:
    // it re-points on every hit, so a long hold-off means the friend's bar never appears in a fight.
    if (now - UI::LastGameEnemyMeterTargetAt() < 2s)
    {
        s_shownHandle = 0;
        return;
    }
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    Actor* pBest = nullptr;
    float bestScore = 0.f, bestDistance = 0.f, bestAngle = -1.f;
    const char* pReason = "";

    auto view = m_world.view<PlayerComponent, FormIdComponent, RemoteComponent>();
    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor || !pActor->GetNiNode() || pActor->actorState.IsDeadOrDying())
            continue;

        const float dx = pActor->position.x - pPlayer->position.x;
        const float dy = pActor->position.y - pPlayer->position.y;
        const float dz = pActor->position.z - pPlayer->position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > cRange)
            continue;

        NiPoint3 chest = pActor->position;
        chest.z += 100.f; // the head and chest, not the feet: that is what you look at
        const float angle = VRBodySync::HeadsetAngleTo(chest);

        const uint32_t handle = pActor->GetHandle().handle.iBits;
        const bool held = handle != 0 && handle == s_shownHandle;
        const bool looking = angle < 0.f ? distance < cBlindRange : angle <= (held ? cHoldAngle : cAcquireAngle);

        const float maxHealth = pActor->GetActorPermanentValue(ActorValueInfo::kHealth);
        const float fraction = maxHealth > 0.f ? pActor->GetActorValue(ActorValueInfo::kHealth) / maxHealth : 1.f;
        const bool hurt = fraction < cHurtFraction;

        if (!looking && !hurt)
            continue;

        // Looking at someone beats a scratch across the room; the worse they are hurt, the more it counts.
        const float score = (looking ? 100.f - std::max(angle, 0.f) : 0.f) + (hurt ? (1.f - fraction) * 60.f : 0.f);
        if (score > bestScore)
        {
            bestScore = score;
            bestDistance = distance;
            bestAngle = angle;
            pBest = pActor;
            pReason = looking ? (hurt ? "looked at, and hurt" : "looked at") : "hurt";
        }
    }

    if (!pBest)
    {
        clear();
        return;
    }
    const uint32_t handle = pBest->GetHandle().handle.iBits;
    if (!handle)
        return;
    if (handle != s_shownHandle)
        spdlog::info("Enemy meter: showing remote player {:X} ({}), {:.0f} units away, {:.0f} degrees off centre, level {}", pBest->formID, pReason, bestDistance, bestAngle,
                     pBest->GetLevel());
    UI::SetEnemyMeterTarget(handle, pBest->GetLevel());
    s_shownHandle = handle;
#endif
}

#ifdef SKYRIMVR
// TEMPORARY (2026-09-20): a player's copy that appears after that player's client dropped and reconnected can be hit
// but not seen (19:09 Seen for Emma, 19:17 Emma for Seen; nothing in the spawn lines differs from a visible spawn).
// Every 5 s, what the game holds for each remote player's copy: distance and height difference to us, form flags
// (0x800 disabled, 0x20 deleted), 3D and its root, health and state, body scale. Compare a visible copy against an
// invisible one.
// TEMPORARY (2026-09-26): Lydia follows and will not attack, and it survives a disconnect -- the enemy that was
// also refusing to attack started the moment the connection dropped, and she did not. Something about her state
// is being left behind rather than driven.
//
// The suspect this tests: an actor still carrying our "remote" flag while the component that justified it is gone.
// HookActorProcess suppresses the AI of anything flagged remote, and the disconnect cleanup only walks actors that
// still have a RemoteComponent -- so an actor whose component was removed first (the server dropping it, or going
// out of range) keeps the flag with nothing left to clear it.
//
// Reasoning has been wrong about this twice, so it is counted instead. An orphan here is the bug; none at all
// means the flag is innocent and the cause is somewhere else entirely.
void CharacterService::RunOrphanedRemoteDiag() noexcept
{
#ifdef SKYRIMVR
    static std::chrono::steady_clock::time_point s_next;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + 10s;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    uint32_t orphans = 0;
    uint32_t firstId = 0;
    const char* pFirstName = "?";

    auto view = m_world.view<FormIdComponent>();
    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor)
            continue;

        const ActorExtension* pExtension = pActor->GetExtension();
        if (!pExtension || !pExtension->IsRemote())
            continue;

        // Flagged remote with nothing to say it should be.
        if (m_world.all_of<RemoteComponent>(entity))
            continue;

        ++orphans;
        if (!firstId)
        {
            firstId = pActor->formID;
            if (const TESNPC* pBase = Cast<TESNPC>(pActor->baseForm))
                pFirstName = pBase->fullName.value.AsAscii();
        }
    }

    if (orphans)
        spdlog::warn("OrphanDiag: {} actors are still flagged remote with no remote component; first is {:X} ({}). Their AI is being suppressed and nothing is left to turn it back on.", orphans,
                     firstId, pFirstName);
#endif
}

// Every frame, because a haptic pulse that arrives a tenth of a second after the blades meet is not a feel, it
// is a notification. The work is one form lookup per tracked actor and a distance test; everything further than
// arm's reach is dropped before any skeleton is touched.
void CharacterService::RunWeaponTouch() noexcept
{
#ifdef SKYRIMVR
    VRBodySync::BeginWeaponTouch();

    auto view = m_world.view<FormIdComponent>();
    for (auto entity : view)
        VRBodySync::ConsiderForWeaponTouch(Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id)));

    VRBodySync::EndWeaponTouch();
#endif
}

void CharacterService::RunRemotePlayerDiag() noexcept
{
    static std::chrono::steady_clock::time_point s_next;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    // Two seconds, not five: the corrective half of this pass is a pointer read and a float compare per remote
    // player, and the thing it corrects is a body nobody can see. The expensive measuring below keeps its own
    // thirty-second timer.
    s_next = now + 2s;
    if (!m_transport.IsConnected())
        return;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;
    static std::chrono::steady_clock::time_point s_nextBody;
    bool measured = false;
    auto view = m_world.view<PlayerComponent, FormIdComponent, RemoteComponent>();
    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor)
            continue;
        const float dx = pActor->position.x - pPlayer->position.x;
        const float dy = pActor->position.y - pPlayer->position.y;
        const float dz = pActor->position.z - pPlayer->position.z;
        // The sweep: whatever put invisibility on the copy (a synced value, a magic effect the copy itself ran), it is
        // taken off again within 5 s, and the log says so. Belt and braces to the refusals at spawn and on update.
        const float invisibility = pActor->GetActorValue(ActorValueInfo::kInvisibility);
        if (invisibility != 0.f)
        {
            spdlog::info("Remote player {:X} copy was invisible ({:.2f}); cleared", pActor->formID, invisibility);
            pActor->SetActorValue(ActorValueInfo::kInvisibility, 0.f);
        }

        // A fade fix lived here on 2026-09-26 and was wrong. The reasoning: the copy's node carries a fade at
        // +0x158 which read 0.000 on the two samples where Seen could not be seen, so it was forced back to 1.
        // What that reasoning skipped was the distance on the very same log line -- 7,703 units on one sample and
        // 10,048 on the other, against a fade far-distance of 1,000. The game fades out anything past that, and
        // fade 0 at ten thousand units is the renderer working correctly. Forcing it to 1 would draw a body at
        // full opacity from across the map.
        //
        // The real finding is in the distances themselves and is written up in VR_TODO: the copy is not faded,
        // it is somewhere else. See "the invisible body is a misplaced body".

        // The line below costs 20 to 30 ms, because DescribeBody walks the whole skeleton by name and measures it.
        // At five seconds that is a dropped frame or two every five seconds, for ever: Seen's session of
        // 2026-09-25 has 54 "Mod update took ..." warnings naming this function, on a client that was already
        // timing out. A diagnostic that destabilises the thing it is watching is worse than no diagnostic, so the
        // measuring runs every 30 s and the cheap corrective pass above -- which is what actually puts an
        // invisible copy right -- keeps its five.
        //
        // Thirty seconds was still a stall of 17 to 34 ms every thirty seconds for as long as another player is near
        // -- thirteen "Mod update took ..." warnings naming this function in seventeen minutes on 2026-10-01, two or
        // three dropped frames each at 90 Hz. Each copy is now measured once when it is first seen, which is the
        // sample that says how it arrived, and after that every five minutes.
        static std::unordered_set<uint32_t> s_measuredOnce;
        if (s_measuredOnce.size() > 256)
            s_measuredOnce.clear();
        const bool cFirstSight = s_measuredOnce.insert(pActor->formID).second;
        if (!cFirstSight && now < s_nextBody)
            continue;

        spdlog::info("CopyDiag: player copy {:X} '{}' {:.0f} units away, dz {:.0f}, form flags {:X}, health {:.0f}, dead {}, bleedout {}, state1 {:X}, invisibility {:.2f}, {}", pActor->formID,
                     pActor->baseForm ? Cast<TESNPC>(pActor->baseForm)->fullName.value.AsAscii() : "?", std::sqrt(dx * dx + dy * dy + dz * dz), dz, pActor->flags,
                     pActor->GetActorValue(24), pActor->IsDead(), pActor->actorState.IsBleedingOut(), pActor->actorState.flags1, invisibility, VRBodySync::DescribeBody(pActor));
        measured = true;
    }

    if (measured)
        s_nextBody = now + 5min;
}
#else
void CharacterService::RunRemotePlayerDiag() noexcept {}
void CharacterService::RunOrphanedRemoteDiag() noexcept {}
void CharacterService::RunWeaponTouch() noexcept {}
#endif

void CharacterService::RunRemoteUpdates() noexcept
{
    // The same sample drives the reference/controller and its visible skeleton. The previous 300 ms position
    // paired with a 100 ms pose made fast-moving players' visible bodies diverge from their actor references. The session log
    // showed at least 95 ms of buffered headroom at a 300 ms delay during ordinary play; 225 ms leaves room for
    // that jitter while reducing the displacement in combat.
#ifdef SKYRIMVR
    constexpr uint64_t cPlaybackDelay = 225;
#else
    constexpr uint64_t cPlaybackDelay = 300;
#endif
    const uint64_t nowTick = m_transport.GetClock().GetCurrentTick();
    const uint64_t tick = nowTick > cPlaybackDelay ? nowTick - cPlaybackDelay : 0;

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

        InterpolationSystem::Update(pActor, interpolationComponent, tick);

#ifdef SKYRIMVR
        // Cheap: it returns at once unless this is a dead body within arm's reach. A body somebody else owns that is
        // being moved here is asked for, so that this side sends it: only the owner sends a body, and on 2026-10-03 a
        // bandit Emma had killed was carried around by Seen (09:28) and did not move at all on her screen.
        if (VRBodySync::ObserveRemoteBodyMotion(pActor, interpolationComponent.Position) && pFormIdComponent)
        {
            const auto& remoteComponent = interpolatedEntities.get<RemoteComponent>(entity);
            spdlog::info("Body {:X} (server id {:X}) is being moved here; asking for it, so that this side sends it", pFormIdComponent->Id, remoteComponent.Id);
            RequestOwnership(pFormIdComponent->Id, remoteComponent.Id, entity);
        }
#endif
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

    Vector<entt::entity> readyEntities;
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
        {
            if (waitingFor3D.SpawnRequest.IsDead)
                spdlog::info("Character {:X} ({:X}) came dead; killed now that it has its 3D", waitingFor3D.SpawnRequest.ServerId, pActor->formID);
            waitingFor3D.SpawnRequest.IsDead ? pActor->Kill() : pActor->Respawn();
        }

        if (pActor->IsVampireLord())
            pActor->FixVampireLordModel();

        readyEntities.push_back(entity);

        spdlog::info("Applied 3D for actor, form id: {:X}", pActor->formID);
    }

    for (auto entity : readyEntities)
    {
        m_world.remove<WaitingFor3D>(entity);

        // Reprocess the remote actor now that an ownership grant can be accepted without immediately declining it.
        ProcessNewEntity(entity);
    }
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
                // The cached copy may be long gone, and the lookup may still answer with where it used to be. Emma's crash
                // of 2026-09-30 19:47:30 was this line: the id lookup handed back an object whose "vtable" was
                // 0x3b33e809f967790a and whose form type was 166 -- freed memory reused for something else -- and the
                // cast faulted reading it. The same day the game's own script engine crashed the same way, on a freed
                // temporary form an id lookup returned; both lookups go through EngineFixesVR's FormCaching. So the object
                // is checked to be a live game object before it is cast, and a dead one is treated as no copy at all.
                TESForm* pCached = TESForm::GetById(remoteComponent.CachedRefId);
                if (pCached && !IsLiveGameObject(pCached))
                {
                    spdlog::warn("Cached copy {:X} of remote character {:X} no longer resolves to a live object; making a fresh one",
                                 remoteComponent.CachedRefId, remoteComponent.Id);
                    remoteComponent.CachedRefId = 0;
                    pCached = nullptr;
                }

                // A copy whose base is not the one it was made with is not this character's copy any more: the game
                // threw ours away and gave its id to an actor of its own. Moving that actor around would make it walk
                // in this character's place (see ClaimCopyId).
                if (pCached && remoteComponent.CachedBaseId != 0)
                {
                    const auto* pCachedActor = Cast<Actor>(pCached);
                    if (!pCachedActor || !pCachedActor->baseForm || pCachedActor->baseForm->formID != remoteComponent.CachedBaseId)
                    {
                        spdlog::warn("Copy {:X} of server character {:X} is gone; its id now names another actor (base {:X}, the copy's was {:X}). A new copy is made",
                                     remoteComponent.CachedRefId, remoteComponent.Id, pCachedActor && pCachedActor->baseForm ? pCachedActor->baseForm->formID : 0,
                                     remoteComponent.CachedBaseId);
                        remoteComponent.CachedRefId = 0;
                        remoteComponent.CachedBaseId = 0;
                        pCached = nullptr;
                    }
                }

                auto* pActor = Cast<Actor>(pCached);
                if (!pActor)
                {
                    // Not every frame. A character that cannot be built here (a placed reference whose stand-in copy is
                    // gone) failed 60 times a second for as long as it stayed in range: 78,769 "could not spawn" lines
                    // in Emma's session of 2026-10-03 and 51,002 in Seen's, from five stand-ins that vanished five
                    // seconds after being made, and the mod's own cost went from 0.14 to 0.75 ms a frame meanwhile.
                    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextAttempt;
                    const auto cNow = std::chrono::steady_clock::now();
                    auto& nextAttempt = s_nextAttempt[remoteComponent.Id];
                    if (cNow < nextAttempt)
                        continue;
                    if (s_nextAttempt.size() > 1024)
                        s_nextAttempt.clear();

                    pActor = CreateCharacterForEntity(entity);
                    if (!pActor)
                    {
                        s_nextAttempt[remoteComponent.Id] = cNow + std::chrono::seconds(5);
                        continue;
                    }

                    remoteComponent.CachedRefId = pActor->formID;
                    remoteComponent.CachedBaseId = s_ownCopies.find(pActor->formID) != s_ownCopies.end() && pActor->baseForm ? pActor->baseForm->formID : 0;
                    ClaimCopyId(entity, pActor->formID);
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

        // Remote actors get 2 passes because Skyrim's weapon drawing is the most finnicky thing in existence.
        double maxTime = data.m_isFirstPass ? 0.5 : 2.0;
        if (data.m_timer <= maxTime)
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(cId));
        if (!pActor || !pActor->GetExtension()->IsRemote())
        {
            toRemove.push_back(cId);
            continue;
        }

        pActor->SetWeaponDrawnEx(data.m_drawWeapon);

        if (!data.m_isFirstPass)
            toRemove.push_back(cId);

        data.m_isFirstPass = false;
    }

    for (uint32_t id : toRemove)
        m_weaponDrawUpdates.erase(id);
}
