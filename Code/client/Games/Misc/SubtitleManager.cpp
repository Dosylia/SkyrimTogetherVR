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
    // See s_subtitleHooks below - the VR address is unknown.
    (void)apSpeaker; (void)apSubtitleText; (void)apTopicInfo; (void)aUnk1;
    return;
#else
    TiltedPhoques::ThisCall(RealShowSubtitle, this, apSpeaker, apSubtitleText, aUnk1);
#endif
}

void* SubtitleManager::HideSubtitle(TESObjectREFR* apSpeaker) noexcept
{
#ifdef SKYRIMVR
    // id 52627 resolved through the crosswalk to 0x93b240, a bare "ret 0" stub.
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
        // Disabled on VR: 52626 is the AE id, left untranslated. The crosswalk
        // sent it to 0x93b1e0, which is a small lookup helper (reads [rcx+8],
        // called from a list-search loop), not ShowSubtitle. Hooking it made
        // HookShowSubtitle treat an unrelated pointer as the speaker and crash
        // inside Cast<Actor> (the object's "vtable" was a heap address).
        // CommonLibVR-NG only maps the neighbouring KillSubtitles
        // (RELOCATION_ID(51755, 52628) -> VR 0x8fa0a0); the real VR
        // ShowSubtitle is presumably close to it but unverified. Subtitle sync
        // is off on VR until it is found.
        return;
#endif
        POINTER_SKYRIMSE(TShowSubtitle, s_showSubtitle, 52626, 52626);

        RealShowSubtitle = s_showSubtitle.Get();

        TP_HOOK(&RealShowSubtitle, HookShowSubtitle);
    });

