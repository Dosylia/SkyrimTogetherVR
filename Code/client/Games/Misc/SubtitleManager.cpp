#include "SubtitleManager.h"

#include <Events/SubtitleEvent.h>

#include <TESObjectREFR.h>
#include <Games/ActorExtension.h>

#include <Forms/TESTopicInfo.h>
#include <Misc/BSFixedString.h>

SubtitleManager* SubtitleManager::Get() noexcept
{
    POINTER_SKYRIMSE(SubtitleManager*, s_singleton, 400443, 514283);
    return *s_singleton.Get();
}

TP_THIS_FUNCTION(TShowSubtitle, void, SubtitleManager, TESObjectREFR* apSpeaker, const char* apSubtitleText, bool aIsInDialogue);
static TShowSubtitle* RealShowSubtitle = nullptr;

void SubtitleManager::ShowSubtitle(TESObjectREFR* apSpeaker, const char* apSubtitleText, TESTopicInfo* apTopicInfo, bool aUnk1) noexcept
{
#ifdef SKYRIMVR
    // ShowSubtitle has no VR address (see s_subtitleHooks).
    (void)apSpeaker; (void)apSubtitleText; (void)apTopicInfo; (void)aUnk1;
    return;
#else
    TiltedPhoques::ThisCall(RealShowSubtitle, this, apSpeaker, apSubtitleText, aUnk1);
#endif
}

void* SubtitleManager::HideSubtitle(TESObjectREFR* apSpeaker) noexcept
{
#ifdef SKYRIMVR
    // No VR address for 52627.
    (void)apSpeaker;
    return nullptr;
#endif
    TP_THIS_FUNCTION(THideSubtitle, void*, SubtitleManager, TESObjectREFR* apSpeaker);
    POINTER_SKYRIMSE(THideSubtitle, s_hideSubtitle, 52627, 52627);
    return TiltedPhoques::ThisCall(s_hideSubtitle, this, apSpeaker);
}

void TP_MAKE_THISCALL(HookShowSubtitle, SubtitleManager, TESObjectREFR* apSpeaker, const char* apSubtitleText, bool aIsInDialogue)
{
    // spdlog::debug("Subtitle for actor {:X} (bool {}):\n\t{}", apSpeaker ? apSpeaker->formID : 0, aIsInDialogue, apSubtitleText);

    Actor* pActor = Cast<Actor>(apSpeaker);
    if (apSubtitleText && pActor && pActor->GetExtension()->IsLocal() && !pActor->GetExtension()->IsPlayer())
        World::Get().GetRunner().Trigger(SubtitleEvent(apSpeaker->formID, apSubtitleText));

    TiltedPhoques::ThisCall(RealShowSubtitle, apThis, apSpeaker, apSubtitleText, aIsInDialogue);
}

static TiltedPhoques::Initializer s_subtitleHooks(
    []()
    {
#ifdef SKYRIMVR
        // Subtitle sync is off on VR: ShowSubtitle (52626) has no VR address. The guessed one hooked
        // an unrelated function and crashed. It should be near KillSubtitles (SE 51755, VR 0x8fa0a0).
        return;
#endif
        POINTER_SKYRIMSE(TShowSubtitle, s_showSubtitle, 52626, 52626);

        RealShowSubtitle = s_showSubtitle.Get();

        TP_HOOK(&RealShowSubtitle, HookShowSubtitle);
    });
