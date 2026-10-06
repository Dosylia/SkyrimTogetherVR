#include <TiltedOnlinePCH.h>
#include <PerfScope.h>

#include <Services/ActorValueService.h>
#include <World.h>
#include <Forms/ActorValueInfo.h>
#include <Games/References.h>
#include <Components.h>
#include <EpochMiss.h>

#include <Events/UpdateEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/HealthChangeEvent.h>
#include <Events/ClashEvent.h>
#include <PlayerCharacter.h>

#include <Messages/NotifyActorValueChanges.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/RequestDeathStateChange.h>

#include <misc/ActorValueOwner.h>

ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_world.on_construct<LocalComponent>().connect<&ActorValueService::OnLocalComponentAdded>(this);
    m_dispatcher.sink<DisconnectedEvent>().connect<&ActorValueService::OnDisconnected>(this);
    m_dispatcher.sink<ActorRemovedEvent>().connect<&ActorValueService::OnActorRemoved>(this);
    m_dispatcher.sink<UpdateEvent>().connect<&ActorValueService::OnUpdate>(this);
    m_dispatcher.sink<NotifyActorValueChanges>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_dispatcher.sink<NotifyActorMaxValueChanges>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_dispatcher.sink<HealthChangeEvent>().connect<&ActorValueService::OnHealthChange>(this);
    m_dispatcher.sink<NotifyHealthChangeBroadcast>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_dispatcher.sink<ClashEvent>().connect<&ActorValueService::OnClash>(this);
    m_dispatcher.sink<NotifyDeathStateChange>().connect<&ActorValueService::OnDeathStateChange>(this);
}

// The least health a remote player's copy is ever given. See StandingValues in CharacterService: a copy at 1 went
// down on the first hit and was never drawn again (2026-09-20).
constexpr float kRemotePlayerHealthFloor = 25.f;

void ActorValueService::CreateActorValuesComponent(const entt::entity aEntity, Actor* apActor) noexcept
{
    auto& actorValuesComponent = m_world.emplace_or_replace<ActorValuesComponent>(aEntity);

    for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
    {
        float value = apActor->GetActorValue(i);
        actorValuesComponent.CurrentActorValues.ActorValuesList.insert({i, value});
        float maxValue = apActor->GetActorPermanentValue(i);
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList.insert({i, maxValue});
    }
}

void ActorValueService::OnLocalComponentAdded(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    const auto& formIdComponent = aRegistry.get<FormIdComponent>(aEntity);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (pActor != NULL)
    {
        auto& localComponent = aRegistry.get<LocalComponent>(aEntity);
        localComponent.IsDead = pActor->IsDead();
        localComponent.IsWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        CreateActorValuesComponent(aEntity, pActor);
    }
}

void ActorValueService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    // TODO: this crashes sometimes, no clue why
    m_world.clear<ActorValuesComponent>();
    m_heldHits.clear();
    m_clashes.clear();
}

void ActorValueService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();
    const uint32_t formId = acEvent.FormId;

    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [view, formId](auto entity)
        {
            const auto& formIdComponent = view.get<FormIdComponent>(entity);

            return formIdComponent.Id == formId;
        });

    if (it != std::end(view))
        m_world.remove<ActorValuesComponent>(*it);
}

void ActorValueService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfScope perfScope("ActorValueService::OnUpdate");

    RunSmallHealthUpdates();
    RunHeldHits();
    RunDeathStateUpdates();
    RunActorValuesUpdates();
}

void ActorValueService::BroadcastActorValues() noexcept
{
    if (!m_transport.IsConnected())
        return;

    // Only changes go on the wire, which is right until the other side misses one -- and there is a window where
    // it always does. On 2026-09-26 at 10:48 Seen died and respawned; his health went -275 -> 315 while Emma's
    // copy of him did not yet exist, so that change was delivered to nobody. The copy was then created, took a
    // stale death delta down to -275, was floored to 25 by the "a copy never goes down" rule, and sat at 25 --
    // an eighth of a bar -- for ninety seconds, until Seen's health next happened to change. Emma watched it
    // creep back up on its own and read it as him healing.
    //
    // So the three values a health bar is made of are re-sent every few seconds whether or not they moved. Three
    // floats every three seconds per player is nothing, and it makes every copy self-correcting: a missed change,
    // a floored value, a copy that regenerated on its own, all repaired within one period instead of never.
    static std::chrono::steady_clock::time_point s_nextRefresh{};
    const auto refreshNow = std::chrono::steady_clock::now();
    const bool cRefresh = refreshNow >= s_nextRefresh;
    if (cRefresh)
        s_nextRefresh = refreshNow + 3s;

    auto view = m_world.view<FormIdComponent, LocalComponent, ActorValuesComponent>();

    for (auto entity : view)
    {
        auto& formIdComponent = view.get<FormIdComponent>(entity);
        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);

        if (!pActor)
            continue;

        auto& localComponent = view.get<LocalComponent>(entity);
        auto& actorValuesComponent = view.get<ActorValuesComponent>(entity);

        RequestActorValueChanges requestValueChanges;
        requestValueChanges.Id = localComponent.Id;
        requestValueChanges.OwnershipEpoch = localComponent.OwnershipEpoch;
        RequestActorMaxValueChanges requestMaxValueChanges;
        requestMaxValueChanges.Id = localComponent.Id;
        requestMaxValueChanges.OwnershipEpoch = localComponent.OwnershipEpoch;

        bool isPlayer = pActor->GetExtension() && pActor->GetExtension()->IsPlayer();

        for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
        {
            if (isPlayer && i == ActorValueInfo::kDragonSouls)
                continue;
            
            float newValue = pActor->GetActorValue(i);
            float oldValue = actorValuesComponent.CurrentActorValues.ActorValuesList[i];
            if (newValue != oldValue)
            {
                requestValueChanges.Values.insert({i, newValue});
                actorValuesComponent.CurrentActorValues.ActorValuesList[i] = newValue;
            }

            float newMaxValue = pActor->GetActorPermanentValue(i);
            float oldMaxValue = actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i];
            if (newMaxValue != oldMaxValue)
            {
                requestMaxValueChanges.Values.insert({i, newMaxValue});
                actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i] = newMaxValue;
            }
        }

        // Only for a player: an NPC's copy is repaired by the correction in OnNotifyActorValueChanges when its
        // owner's snapshot next arrives, and there are far more of them than there are players.
        if (cRefresh && isPlayer)
        {
            for (const uint32_t key : {uint32_t(ActorValueInfo::kHealth), uint32_t(ActorValueInfo::kMagicka), uint32_t(ActorValueInfo::kStamina)})
            {
                requestValueChanges.Values[key] = pActor->GetActorValue(key);
                requestMaxValueChanges.Values[key] = pActor->GetActorPermanentValue(key);
            }
        }

        if (requestValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestValueChanges);
        }

        if (requestMaxValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestMaxValueChanges);
        }
    }
}

void ActorValueService::OnHealthChange(const HealthChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto hitteeIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.HitteeId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (hitteeIt == std::end(view))
    {
        spdlog::warn("Health change event form id component not found, form id: {:X}", acEvent.HitteeId);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*hitteeIt);
    if (!serverIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    uint32_t serverId = serverIdRes.value();

    if (acEvent.DeltaHealth > -1.0f && acEvent.DeltaHealth < 1.0f)
    {
        if (m_smallHealthChanges.find(serverId) == m_smallHealthChanges.end())
            m_smallHealthChanges[serverId] = acEvent.DeltaHealth;
        else
            m_smallHealthChanges[serverId] += acEvent.DeltaHealth;
        return;
    }

    RequestHealthChangeBroadcast requestHealthChange;
    requestHealthChange.Id = serverId;
    requestHealthChange.DeltaHealth = acEvent.DeltaHealth;
    requestHealthChange.Tick = m_transport.GetClock().GetCurrentTick();

    m_transport.Send(requestHealthChange);

    spdlog::debug("Sent out delta health through collection: {:X}:{:f}", serverId, acEvent.DeltaHealth);
}

void ActorValueService::RunSmallHealthUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    if (!m_smallHealthChanges.empty())
    {
        for (auto& value : m_smallHealthChanges)
        {
            RequestHealthChangeBroadcast requestHealthChange;
            requestHealthChange.Id = value.first;
            requestHealthChange.DeltaHealth = value.second;
            requestHealthChange.Tick = m_transport.GetClock().GetCurrentTick();

            m_transport.Send(requestHealthChange);

            spdlog::debug("Sent out delta health through timer, {:X}:{:f}", value.first, value.second);
        }

        m_smallHealthChanges.clear();
    }
}

void ActorValueService::RunDeathStateUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto localView = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : localView)
    {
        const auto& formIdComponent = localView.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        auto& localComponent = localView.get<LocalComponent>(entity);

        bool isDead = pActor->IsDead();
        if (isDead != localComponent.IsDead)
        {
            localComponent.IsDead = isDead;
            spdlog::info("Death sync: local actor {:X} is now {}, telling the server", pActor->formID, isDead ? "dead" : "alive");

            RequestDeathStateChange requestChange;
            requestChange.Id = localComponent.Id;
            requestChange.OwnershipEpoch = localComponent.OwnershipEpoch;
            requestChange.IsDead = isDead;

            m_transport.Send(requestChange);
        }
    }
}

void ActorValueService::RunActorValuesUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    BroadcastActorValues();
}

namespace
{
// The defender's rule (Physics queue P4; agreed 2026-10-06: the defender's screen decides whether a hit was blocked).
// This game shows the other player's VR pose 225 ms after it happened (CharacterService::RunRemoteUpdates), so a hit
// that happened at tick t in the attacker's game is seen here at t + 225. It is held until a little after that, and
// dropped if this player's hand, or what it held, met that player's weapon around that moment.
constexpr uint64_t kHitSeenAfter = 225;
// The blades meet before the hit would have landed (the blade still had to reach the body), and a contact is only
// taken at the next frames. Measured in the first fight between two headsets (2026-10-06): where the defender's game
// saw a clash near a hit that landed, it came 120-290 ms before the hit's own tick (6 hits out of 6 within 300 ms;
// two more 420-460 ms before), so a clash from 300 ms before the hit happened up to 100 ms after it was seen here.
constexpr uint64_t kBlockBefore = kHitSeenAfter + 300;
constexpr uint64_t kBlockAfter = 100;
} // namespace

void ActorValueService::OnClash(const ClashEvent& acEvent) noexcept
{
    m_clashes.push_back({acEvent.FormId, acEvent.Tick});
}

void ActorValueService::RunHeldHits() noexcept
{
    if (m_heldHits.empty() && m_clashes.empty())
        return;

    const uint64_t cNow = m_transport.GetClock().GetCurrentTick();
    // Kept a while: a hit can arrive late.
    m_clashes.erase(std::remove_if(m_clashes.begin(), m_clashes.end(), [cNow](const auto& acClash) { return acClash.second + 30000 < cNow; }), m_clashes.end());

    for (auto it = m_heldHits.begin(); it != m_heldHits.end();)
    {
        const uint64_t cSeen = it->Tick + kHitSeenAfter;
        // A tick from the future (a clock not yet in step) is not waited for.
        if (cNow < cSeen + kBlockAfter && it->Tick < cNow + 5000)
        {
            ++it;
            continue;
        }

        uint32_t copy = 0;
        auto view = m_world.view<FormIdComponent, PlayerComponent>();
        for (const auto entity : view)
            if (view.get<PlayerComponent>(entity).Id == it->AttackerPlayerId)
            {
                copy = view.get<FormIdComponent>(entity).Id;
                break;
            }
        const auto clash = std::find_if(m_clashes.begin(), m_clashes.end(), [copy, cSeen](const auto& acClash)
                                        { return copy && acClash.first == copy && acClash.second + kBlockBefore >= cSeen && acClash.second <= cSeen + kBlockAfter; });
        if (clash != m_clashes.end())
        {
            const int64_t cOffset = static_cast<int64_t>(clash->second) - static_cast<int64_t>(cSeen);
            spdlog::info("Defender's rule: a hit of {:.0f} from player {} is blocked: our weapon met theirs {} ms {} it was seen here", -it->DeltaHealth, it->AttackerPlayerId,
                         std::abs(cOffset), cOffset <= 0 ? "before" : "after");
        }
        else
        {
            spdlog::info("Defender's rule: a hit of {:.0f} from player {} lands, {} ms after it happened; nothing of ours met their weapon", -it->DeltaHealth, it->AttackerPlayerId,
                         static_cast<int64_t>(cNow) - static_cast<int64_t>(it->Tick));
            ApplyHealthChange(*it);
        }
        it = m_heldHits.erase(it);
    }
}

void ActorValueService::OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast& acMessage) noexcept
{
#ifdef SKYRIMVR
    // Another player's hit on this player waits for the defender's rule (RunHeldHits).
    if (acMessage.DeltaHealth < 0.f && acMessage.AttackerPlayerId)
        if (Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id); pActor && pActor == PlayerCharacter::Get())
        {
            NotifyHealthChangeBroadcast held = acMessage;
            if (!held.Tick)
                held.Tick = m_transport.GetClock().GetCurrentTick();
            m_heldHits.push_back(held);
            return;
        }
#endif
    ApplyHealthChange(acMessage);
}

void ActorValueService::ApplyHealthChange(const NotifyHealthChangeBroadcast& acMessage) const noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.Id);
        return;
    }

    float newHealth = pActor->GetActorValue(ActorValueInfo::kHealth) + acMessage.DeltaHealth;

    // A player's copy is essential: at zero it goes down. Only its owner's game decides when that player is down (and
    // a respawn replaces the copy anyway), so the copy keeps a margin: 1 was not enough, one more hit took it down
    // (2026-09-20, two copies hittable but not seen).
    if (pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer() && newHealth < kRemotePlayerHealthFloor)
    {
        spdlog::info("Remote player {:X} copy kept at {:.0f} health instead of {:.0f}; its owner decides", pActor->formID, kRemotePlayerHealthFloor, newHealth);
        newHealth = kRemotePlayerHealthFloor;
    }

    pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, newHealth);

    const float health = pActor->GetActorValue(ActorValueInfo::kHealth);
    if (!pActor->IsDead() && health <= 0.f)
    {
        ActorExtension* pExtension = pActor->GetExtension();
        // Players should never be killed. Nor should an actor that is bleeding out: that is the game's downed
        // state, it is recoverable, and an essential NPC lives there rather than dying.
        if (pExtension && !pExtension->IsPlayer() && !pActor->actorState.IsBleedingOut())
        {
            // An essential NPC refuses to die, so IsDead() stays false and the next health broadcast tries again.
            // On 2026-09-24 that was 236 attempts on Commander Caius in two minutes, plus 38 on another actor,
            // each one re-entering the death transition. After a few refusals this actor is left alone: whatever
            // it is, this client cannot kill it, and hammering it only disturbs its ragdoll and its recovery.
            static TiltedPhoques::Map<uint32_t, uint32_t> s_refused;
            auto& refusals = s_refused[pActor->formID];
            if (refusals < 3)
            {
                pActor->Kill();
                if (pActor->IsDead())
                {
                    s_refused.erase(pActor->formID);
                    spdlog::info("Death sync: health broadcast killed actor {:X}", pActor->formID);
                }
                else if (++refusals == 3)
                    spdlog::warn("Death sync: actor {:X} refuses to die (essential, or protected); leaving it to its owner", pActor->formID);
            }
        }
    }

    StartCombatWithAttacker(pActor, acMessage.AttackerPlayerId, acMessage.DeltaHealth);

    // TODO(cosideci): find fix for player health sync so this can be used again
    /*
    if (pActor->GetExtension()->IsRemotePlayer())
        World::Get().GetOverlayService().SetPlayerHealthPercentage(pActor->formID);
    */
}

void ActorValueService::StartCombatWithAttacker(Actor* apActor, uint32_t aAttackerPlayerId, float aDeltaHealth) const noexcept
{
    // An NPC's combat belongs to whoever owns it, and until now a hit from the other player arrived as a bare health
    // delta with no attacker. So a guard that a crime made hostile to one player ignored the other player entirely,
    // however hard they hit it (2026-09-23: "npc i triggered by being a criminal only wanted to fight me, despite
    // both of us fighting them"). The server now names the attacker and this puts them in the NPC's combat.
    if (!apActor || !aAttackerPlayerId)
        return;

    // Damage only. The client adds the delta, so a hit is negative; a heal or a buff must never start a fight.
    if (aDeltaHealth >= 0.f)
        return;

    // Only the owner runs this actor's AI, and only it can usefully start combat. Players are never made to fight.
    ActorExtension* pExtension = apActor->GetExtension();
    if (!pExtension || !pExtension->IsLocal() || pExtension->IsPlayer())
        return;

    if (apActor->IsDead() || apActor->actorState.IsBleedingOut())
        return;

    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view, aAttackerPlayerId](auto aEntity)
                                 { return view.get<PlayerComponent>(aEntity).Id == aAttackerPlayerId; });
    if (it == view.end())
        return;

    Actor* pAttacker = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pAttacker || pAttacker == apActor)
        return;

    // A flurry of arrows would otherwise restart combat every frame.
    static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_startedAt;
    const auto now = std::chrono::steady_clock::now();
    for (auto entry = s_startedAt.begin(); entry != s_startedAt.end();)
        entry = now - entry->second > std::chrono::seconds(30) ? s_startedAt.erase(entry) : std::next(entry);

    auto& startedAt = s_startedAt[apActor->formID];
    if (startedAt.time_since_epoch().count() && now - startedAt < std::chrono::seconds(3))
        return;
    startedAt = now;

    apActor->StartCombatEx(pAttacker);
    spdlog::info("Combat: actor {:X} now also fights player {} (copy {:X}), which hit it for {:.0f}", apActor->formID, aAttackerPlayerId, pAttacker->formID, -aDeltaHealth);
}

void ActorValueService::OnActorValueChanges(const NotifyActorValueChanges& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    // Separated from the epoch test so a stale epoch can be told apart from a character we simply do not have.
    const auto itor = std::find_if(std::begin(view), std::end(view), [&acMessage, view](entt::entity entity)
    { return view.get<RemoteComponent>(entity).Id == acMessage.Id; });

    if (itor != std::end(view))
    {
        const auto& remoteFound = view.get<RemoteComponent>(*itor);
        if (acMessage.OwnershipEpoch == 0 || remoteFound.OwnershipEpoch != acMessage.OwnershipEpoch)
        {
            ReportEpochMiss("an actor value change", acMessage.Id, acMessage.OwnershipEpoch, remoteFound.OwnershipEpoch);
            return;
        }
    }

    if (itor == std::end(view))
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*itor);
    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    const bool isRemotePlayer = pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer();

    for (auto [key, value] : acMessage.Values)
    {
        // Syncing dragon souls triggers "Dragon soul collected" event
        if (key == ActorValueInfo::kDragonSouls)
            continue;

        // Every actor value is copied to a player's copy, and invisibility is one of them (54). A copy with it above
        // zero is exactly the bug of 2026-09-20 21:12, 21:20 and 21:27: standing there, hittable, animating, full
        // health, 3D intact, not drawn, cured only by a reconnect that rebuilds the copy from a fresh snapshot.
        // The other player must always see the body; whatever hides the owner on his own screen stays there.
        if (isRemotePlayer && key == ActorValueInfo::kInvisibility)
        {
            if (value != 0.f)
                spdlog::info("Remote player {:X} copy refused invisibility {:.2f} from its owner; a copy is always drawn", pActor->formID, value);
            continue;
        }

        // Health used to be skipped for everyone. A remote player's copy then only ever lost health (the damage
        // deltas arrive, the healing never did), and the copy is essential with no bleedout recovery, so once that
        // stale health hit zero the copy lay down in the grass for good: "his body is gone, I can still see his spell
        // light", cured only by his reconnect (2026-09-18 21:56, 09-19 09:14 and 10:26). The owner's own health is
        // the truth for a player copy; NPC copies keep the delta path, which also carries their deaths.
        if (key == ActorValueInfo::kHealth)
        {
            if (pActor->IsDead())
                continue;

            if (!isRemotePlayer)
            {
                // An NPC's health here is only ever the sum of the damage deltas that happened to arrive. Nothing
                // repairs it: a delta lost to a starved stream, a hit applied on one side only, an owner handover
                // mid-fight, and this side's number drifts away from the owner's for as long as the actor lives.
                // The owner's snapshot is the truth, so it is used -- but only as a correction, not as a constant
                // override, because the delta path is what carries a death and the two would otherwise fight
                // every frame.
                //
                // Deliberately blunt: a small difference is normal mid-combat and is left alone, and only a gap
                // big enough to matter is closed. Every correction is logged, so a session says whether this is
                // firing sanely or papering over a stream that is dropping deltas.
                const float current = pActor->GetActorValue(ActorValueInfo::kHealth);
                const float gap = std::abs(current - value);
                constexpr float cWorthCorrecting = 25.f;
                if (gap < cWorthCorrecting)
                    continue;

                static std::chrono::steady_clock::time_point s_nextSaid{};
                const auto now = std::chrono::steady_clock::now();
                if (now >= s_nextSaid)
                {
                    s_nextSaid = now + 5s;
                    spdlog::info("NPC {:X} health corrected from {:.0f} to its owner's {:.0f}", pActor->formID, current, value);
                }

                pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, value);
                continue;
            }

            const float current = pActor->GetActorValue(ActorValueInfo::kHealth);
            if (std::abs(current - value) >= 10.f)
                spdlog::info("Remote player {:X} copy health corrected from {:.0f} to the owner's {:.0f}", pActor->formID, current, value);

            // The owner's own death arrives here as a health at or below zero (-4 at 19:52:54, -1 and -16 at 19:57:24
            // on 2026-09-19). Applied as is, it put the copy into its unrecoverable bleedout, and the owner's respawn
            // then handed the fresh copy the same stored value (see StandingValues in CharacterService). A copy never
            // goes down; the owner's game decides, and his respawn replaces the copy.
            if (value < kRemotePlayerHealthFloor)
            {
                spdlog::info("Remote player {:X} copy kept at {:.0f} health instead of the owner's {:.0f}; a copy never goes down", pActor->formID, kRemotePlayerHealthFloor, value);
                value = kRemotePlayerHealthFloor;
            }
        }

        spdlog::debug("Actor value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        if (key == ActorValueInfo::kStamina || key == ActorValueInfo::kMagicka || key == ActorValueInfo::kHealth)
        {
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, key, value);
            continue;
        }
        pActor->SetActorValue(key, value);
    }
}

void ActorValueService::OnActorMaxValueChanges(const NotifyActorMaxValueChanges& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    // Separated from the epoch test so a stale epoch can be told apart from a character we simply do not have.
    const auto it = std::find_if(std::begin(view), std::end(view), [&acMessage, view](entt::entity entity)
    { return view.get<RemoteComponent>(entity).Id == acMessage.Id; });

    if (it != std::end(view))
    {
        const auto& remoteFound = view.get<RemoteComponent>(*it);
        if (acMessage.OwnershipEpoch == 0 || remoteFound.OwnershipEpoch != acMessage.OwnershipEpoch)
        {
            ReportEpochMiss("a health broadcast", acMessage.Id, acMessage.OwnershipEpoch, remoteFound.OwnershipEpoch);
            return;
        }
    }

    if (it == std::end(view))
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*it);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    for (const auto& [key, value] : acMessage.Values)
    {
        if (key == ActorValueInfo::kDragonSouls)
            continue;

        spdlog::debug("Actor max value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        pActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, key, value);
    }
}

void ActorValueService::OnDeathStateChange(const NotifyDeathStateChange& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    // Separated from the epoch test so a stale epoch can be told apart from a character we simply do not have.
    const auto it = std::find_if(std::begin(view), std::end(view), [&acMessage, view](entt::entity entity)
    { return view.get<RemoteComponent>(entity).Id == acMessage.Id; });

    if (it != std::end(view))
    {
        const auto& remoteFound = view.get<RemoteComponent>(*it);
        if (acMessage.OwnershipEpoch == 0 || remoteFound.OwnershipEpoch != acMessage.OwnershipEpoch)
        {
            ReportEpochMiss("a death state change", acMessage.Id, acMessage.OwnershipEpoch, remoteFound.OwnershipEpoch);
            return;
        }
    }

    // Every way out of here is named. Seen's session of 2026-09-30: Emma's client sent the death of a Mist Watch bandit
    // (server id 50000D) at 19:41:36, the server forwarded it (it reports every death it drops, and reported none),
    // and Seen's side logged nothing at all -- no death applied, no epoch miss. Three exits below were silent, and
    // "we never had that character", "it had no actor" and "it was already dead here" are three different bugs.
    // Rate limited: a death arrives once per actor, so this is a handful of lines per session.
    const auto say = [&acMessage](const char* acpWhy)
    {
        static std::chrono::steady_clock::time_point s_next{};
        const auto now = std::chrono::steady_clock::now();
        if (now < s_next)
            return;
        s_next = now + std::chrono::seconds(1);
        spdlog::info("Death sync: {} for server id {:X} arrived and was not applied: {}", acMessage.IsDead ? "a death" : "a revival", acMessage.Id, acpWhy);
    };

    if (it == std::end(view))
    {
        say("no copy of that character here");
        return;
    }

    auto& formIdComponent = view.get<FormIdComponent>(*it);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
    {
        say("its copy has no actor in the game");
        return;
    }

    ActorExtension* pExtension = pActor->GetExtension();
    // Players should never be killed
    if (pExtension->IsPlayer())
        return;

    if (pActor->IsDead() != acMessage.IsDead)
    {
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();
        spdlog::info("Death sync: remote actor {:X} set {} by its owner (now dead: {})", pActor->formID, acMessage.IsDead ? "dead" : "alive", pActor->IsDead());
    }
    else
    {
        // Not a failure -- the copy got there on its own, from a hit landed on this side -- but it means the body fell
        // here, under this side's ragdoll, and will lie somewhere other than the owner's. Worth knowing when a body is
        // reported "not synced after it died".
        spdlog::info("Death sync: remote actor {:X} was already {} here when its owner's word arrived", pActor->formID, acMessage.IsDead ? "dead" : "alive");
    }
}
