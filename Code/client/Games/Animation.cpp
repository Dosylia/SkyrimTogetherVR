#include <TiltedOnlinePCH.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <Forms/TESIdleForm.h>

#include <Structs/ActionEvent.h>

#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>

#include <Misc/BSFixedString.h>

#include <World.h>

#include <mutex>

TP_THIS_FUNCTION(TPerformAction, uint8_t, ActorMediator, TESActionData* apAction);
static TPerformAction* RealPerformAction;

// TODO: make scoped override
thread_local bool g_forceAnimation = false;

namespace
{
// TEMPORARY (2026-10-03): which actions flood the stream, and whether they even work on the actor that sends them.
// A Netch standing still sent IdleSpecialStart 282 times in 12 s on 2026-10-02 (1521 actions in another 12 s), and
// an actor once queued 717 Unequip replays on the other side (2026-09-24). Until 2026-10-03 every action this hook
// saw was sent, including those the game refused here (res 0). Said once per actor and action every ten seconds,
// when one actor performs the same action more than ten times in a second.
void NoteActionRate(uint32_t aActorId, uint32_t aActionId, const char* acpEventName, bool aWorked) noexcept
{
    struct Rate
    {
        std::chrono::steady_clock::time_point WindowStart{};
        uint32_t Count = 0;
        uint32_t Failed = 0;
        std::chrono::steady_clock::time_point NextReport{};
    };
    static std::mutex s_lock;
    static TiltedPhoques::Map<uint64_t, Rate> s_rates;

    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock _{s_lock};
    if (s_rates.size() > 512)
        s_rates.clear();
    Rate& rate = s_rates[(static_cast<uint64_t>(aActorId) << 32) | aActionId];
    if (now - rate.WindowStart >= std::chrono::seconds(1))
    {
        if (rate.Count > 10 && now >= rate.NextReport)
        {
            rate.NextReport = now + std::chrono::seconds(10);
            spdlog::info("ActionFlood: actor {:X} performed action {:X} ({}) {} times in a second, {} of them refused by the game here and not sent",
                         aActorId, aActionId, acpEventName ? acpEventName : "?", rate.Count, rate.Failed);
        }
        rate.WindowStart = now;
        rate.Count = 0;
        rate.Failed = 0;
    }
    ++rate.Count;
    if (!aWorked)
        ++rate.Failed;
}
// Whether this action is the one just sent for this actor, again, within a quarter of a second. A Netch standing still
// performs IdleSpecialStart about forty times a second and the game accepts every second one: the bot received 489 of
// them in 12 s (2026-10-03, after refused actions had stopped being sent), each carrying the creature's animation
// variables. The other players gain nothing from the forty-first restart of the same idle.
bool IsRepeatOfLastSent(const ActionEvent& acAction) noexcept
{
    struct Last
    {
        uint32_t ActionId = 0;
        uint32_t IdleId = 0;
        uint32_t TargetId = 0;
        std::chrono::steady_clock::time_point SentAt{};
    };
    static std::mutex s_lock;
    static TiltedPhoques::Map<uint32_t, Last> s_last;

    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock _{s_lock};
    if (s_last.size() > 1024)
        s_last.clear();
    Last& last = s_last[acAction.ActorId];
    if (last.ActionId == acAction.ActionId && last.IdleId == acAction.IdleId && last.TargetId == acAction.TargetId &&
        now - last.SentAt < std::chrono::milliseconds(250))
        return true;
    last = Last{acAction.ActionId, acAction.IdleId, acAction.TargetId, now};
    return false;
}
} // namespace

uint8_t TP_MAKE_THISCALL(HookPerformAction, ActorMediator, TESActionData* apAction)
{
    auto pActor = apAction->actor;
    const auto pExtension = pActor->GetExtension();

    if (!pExtension->IsRemote() || g_forceAnimation)
    {
        ActionEvent action;
        action.State1 = pActor->actorState.flags1;
        action.State2 = pActor->actorState.flags2;
        action.Type = apAction->unkInput | (apAction->someFlag ? 0x4 : 0);
        action.Tick = World::Get().GetTick();
        action.ActorId = pActor->formID;
        action.ActionId = apAction->action->formID;
        action.TargetId = apAction->target ? apAction->target->formID : 0;

        pActor->SaveAnimationVariables(action.Variables);

        const auto res = TiltedPhoques::ThisCall(RealPerformAction, apThis, apAction);

        // spdlog::debug("Action event name: {}, target name: {}", apAction->eventName.AsAscii(), apAction->targetEventName.AsAscii());

        // This is a weird case where it gets spammed and doesn't do much, not sure if it still needs to be sent over the network
        if (apAction->someFlag == 1 || g_forceAnimation)
            return res;

        action.EventName = apAction->eventName.AsAscii();
        action.TargetEventName = apAction->targetEventName.AsAscii();
        action.IdleId = apAction->idleForm ? apAction->idleForm->formID : 0;

        // Save for later
        if (res)
        {
            pExtension->LatestAnimation = action;
        }

        NoteActionRate(action.ActorId, action.ActionId, apAction->eventName.AsAscii(), res != 0);

        // An action the game refused here did not happen here, so it is not sent: the other players would replay
        // something the owner's actor never did, and a refused action tends to be retried every frame. On
        // 2026-10-03 one creature standing at Mistwatch tried action 132AF 53 to 62 times a second and was refused
        // every time, and a Seeker standing still tried IdleSpecialStart 121 times a second, half of them refused;
        // all of it went out with the creature's movement updates.
        if (!res)
            return res;

        if (IsRepeatOfLastSent(action))
            return res;

        World::Get().GetRunner().Trigger(action);

        return res;
    }

    return 0;
}

ActorMediator* ActorMediator::Get() noexcept
{
    POINTER_SKYRIMSE(ActorMediator*, s_actorMediator, 403567, 403567);

    return *(s_actorMediator.Get());
}

bool ActorMediator::PerformAction(TESActionData* apAction) noexcept
{
    if (apAction->actor->formID == 0x13482)
    {
        /*static Set<uint32_t> s_ids;

        spdlog::error("New frame");
        for(auto i = 0; i < action.Variables.size(); ++i)
        {
            auto& oldVars = pExtension->LatestVariables.Variables;
            auto& newVars = action.Variables;
            if(oldVars[i] != newVars[i] && s_ids.count(i) == 0)
            {
                //s_ids.insert(i);
                spdlog::info("Var {} changed from {} to {}", i, oldVars[i], newVars[i]);
            }
        }*/
        // spdlog::info("Play animation name: {} with idle {:X} and target {:X} and unk {:X}", apAction->action->keyword.AsAscii(), (apAction->idleForm ? apAction->idleForm->formID : 0), (apAction->target ? apAction->target->formID : 0), apAction->unkInput);
    }

    const auto res = TiltedPhoques::ThisCall(RealPerformAction, this, apAction);
    // const auto res = RePerformAction(apAction, aValue);

    if (res && apAction->actor->formID == 0x13482)
    {
        //    spdlog::info("Passed !");
    }

    return res != 0;
}

bool ActorMediator::ForceAction(TESActionData* apAction) noexcept
{
    TP_THIS_FUNCTION(TAnimationStep, uint8_t, ActorMediator, TESActionData*);
    using TApplyAnimationVariables = void*(void*, TESActionData*);

    POINTER_SKYRIMSE(TApplyAnimationVariables, ApplyAnimationVariables, 39004, 39004);
    POINTER_SKYRIMSE(TAnimationStep, PerformComplexAction, 38953, 38953);
    POINTER_SKYRIMSE(void*, qword_142F271B8, 403566, 403566);

    uint8_t result = 0;

    auto pActor = static_cast<Actor*>(apAction->actor);
    if (pActor)
    {
        result = TiltedPhoques::ThisCall(PerformComplexAction, this, apAction);

        ApplyAnimationVariables(*qword_142F271B8.Get(), apAction);
    }

    return result;
}

ActionInput::ActionInput(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
{
    // skip vtable as we never use this directly
    actor = apActor;
    target = apTarget;
    action = apAction;
    unkInput = aParam1;
}

void ActionInput::Release()
{
    actor.Release();
    target.Release();
}

ActionOutput::ActionOutput()
    : eventName("")
    , targetEventName("")
{
    // skip vtable as we never use this directly

    result = 0;
    targetIdleForm = nullptr;
    idleForm = nullptr;
    unk1 = 0;
}

void ActionOutput::Release()
{
    eventName.Release();
    targetEventName.Release();
}

BGSActionData::BGSActionData(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
    : ActionInput(aParam1, apActor, apAction, apTarget)
{
    // skip vtable as we never use this directly
    someFlag = 0;
}

TESActionData::TESActionData(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
    : BGSActionData(aParam1, apActor, apAction, apTarget)
{
    POINTER_SKYRIMSE(void*, s_vtbl, 188603, 232777);

    someFlag = false;

    *reinterpret_cast<void**>(this) = s_vtbl.Get();
}

TESActionData::~TESActionData()
{
    ActionOutput::Release();
    ActionInput::Release();
}

static TiltedPhoques::Initializer s_animationHook(
    []()
    {
        POINTER_SKYRIMSE(TPerformAction, performAction, 38949, 38949);

        RealPerformAction = performAction.Get();

        TP_HOOK(&RealPerformAction, HookPerformAction);
    });
