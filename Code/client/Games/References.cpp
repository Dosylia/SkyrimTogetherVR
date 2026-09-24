#include <TiltedOnlinePCH.h>

#include <Games/References.h>

#include <RTTI.h>

#include <Forms/BGSHeadPart.h>
#include <Forms/TESNPC.h>
#include <Forms/TESPackage.h>
#include <Forms/TESWeather.h>
#include <SaveLoad.h>
#include <ExtraData/ExtraDataList.h>

#include <Misc/GameVM.h>

#include <Games/TES.h>
#include <Games/Overrides.h>
#include <Games/Misc/Lock.h>
#include <AI/AIProcess.h>
#include <Magic/MagicCaster.h>
#include <Sky/Sky.h>

#include <Events/InitPackageEvent.h>

#include <TiltedCore/Serialization.hpp>

#include <Services/PapyrusService.h>
#include <Services/DebugService.h>
#include <World.h>

#include <AI/AITimer.h>
#include <Combat/CombatTargetSelector.h>

namespace Settings
{
int32_t* GetDifficulty() noexcept
{
    POINTER_SKYRIMSE(int32_t, s_difficulty, 381472, 381472);
    return s_difficulty.Get();
}

float* GetGreetDistance() noexcept
{
    POINTER_SKYRIMSE(float, s_greetDistance, 370892, 370892);
    return s_greetDistance.Get();
}

} // namespace Settings

namespace GameplayFormulas
{

float CalculateRealDamage(Actor* apHittee, float aDamage, bool aKillMove) noexcept
{
    using TGetDifficultyMultiplier = float(int32_t, int32_t, bool);
    POINTER_SKYRIMSE(TGetDifficultyMultiplier, s_getDifficultyMultiplier, 26503, 25920);

    bool isPlayer = apHittee == PlayerCharacter::Get();

    float multiplier = s_getDifficultyMultiplier(PlayerCharacter::Get()->difficulty, ActorValueInfo::kHealth, isPlayer);

    float realDamage = aDamage;

    // TODO(cosideci): this seems problematic? It may not register the kill for others?
    // Disabled for now, cause this check seems to have totally broken everything, let's see what happens.
    // if (!aKillMove || multiplier < 1.0)
    realDamage = aDamage * multiplier;

    return realDamage;
}

} // namespace GameplayFormulas

void FadeOutGame(bool aFadingOut, bool aBlackFade, float aFadeDuration, bool aRemainVisible, float aSecondsToFade) noexcept
{
#ifdef SKYRIMVR
    // VR takes two more arguments: a flag and a ref-counted "fade done" callback (its own callers pass false and a
    // pointer). Leaving them out made the game store stack garbage as the callback and call it when the fade
    // finished, which crashed right after respawning.
    //
    // It also reads the first two arguments as dwords, and the FIRST ONE WITH THE OPPOSITE POLARITY (TiltedEvolutionVR
    // checked the VR prologue and the fade object it fills against SE's). Passing the bools through made every call
    // do the opposite: the death fade did nothing and the respawn fade-in faded to black, which is the "bright light,
    // then black screen" after a respawn on 2026-09-19.
    using TFadeOutGame = void(int32_t, int32_t, float, bool, float, bool, void*);
    POINTER_SKYRIMSE(TFadeOutGame, fadeOutGame, 52847, 51909);
    fadeOutGame.Get()(aFadingOut ? 0 : 1, aBlackFade ? 1 : 0, aFadeDuration, aRemainVisible, aSecondsToFade, false, nullptr);
#else
    using TFadeOutGame = void(bool, bool, float, bool, float);
    POINTER_SKYRIMSE(TFadeOutGame, fadeOutGame, 52847, 51909);
    fadeOutGame.Get()(aFadingOut, aBlackFade, aFadeDuration, aRemainVisible, aSecondsToFade);
#endif
}

// Disable AI sync for now, experiment didn't work, code might be useful later on though.
#define AI_SYNC 0

TP_THIS_FUNCTION(TCheckForNewPackage, bool, void, Actor* apActor, uint64_t aUnk1);
static TCheckForNewPackage* RealCheckForNewPackage = nullptr;

bool TP_MAKE_THISCALL(HookCheckForNewPackage, void, Actor* apActor, uint64_t aUnk1)
{
#if AI_SYNC
    const ActorExtension* pPackageExtension = apActor ? apActor->GetExtension() : nullptr;
    if (pPackageExtension && pPackageExtension->IsRemote())
        return false;

#endif
    return TiltedPhoques::ThisCall(RealCheckForNewPackage, apThis, apActor, aUnk1);
}

TP_THIS_FUNCTION(TInitFromPackage, void, void, TESPackage* apPackage, TESObjectREFR* apTarget, Actor* arActor);
static TInitFromPackage* RealInitFromPackage = nullptr;

void TP_MAKE_THISCALL(HookInitFromPackage, void, TESPackage* apPackage, TESObjectREFR* apTarget, Actor* arActor)
{
#if AI_SYNC
    // This guard is here for when the client sets the package based on a remote message
    if (s_execInitPackage)
        return TiltedPhoques::ThisCall(RealInitFromPackage, apThis, apPackage, apTarget, arActor);

    const ActorExtension* pInitExtension = arActor ? arActor->GetExtension() : nullptr;
    if (pInitExtension && pInitExtension->IsRemote())
        return;

    if (arActor && apPackage)
        World::Get().GetRunner().Trigger(InitPackageEvent(arActor->formID, apPackage->formID));

#endif
    return TiltedPhoques::ThisCall(RealInitFromPackage, apThis, apPackage, apTarget, arActor);
}

TP_THIS_FUNCTION(TSetCurrentPickREFR, void, Console, BSPointerHandle<TESObjectREFR>* apRefr);
static TSetCurrentPickREFR* RealSetCurrentPickREFR = nullptr;

void TP_MAKE_THISCALL(HookSetCurrentPickREFR, Console, BSPointerHandle<TESObjectREFR>* apRefr)
{
    uint32_t formId = 0;

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(apRefr->handle.iBits);
    if (pObject)
        formId = pObject->formID;

    World::Get().GetDebugService().SetDebugId(formId);

    return TiltedPhoques::ThisCall(RealSetCurrentPickREFR, apThis, apRefr);
}

static TiltedPhoques::Initializer s_referencesHooks(
    []()
    {
        POINTER_SKYRIMSE(TCheckForNewPackage, s_checkForNewPackage, 39114, 39114);
        POINTER_SKYRIMSE(TInitFromPackage, s_initFromPackage, 38959, 38959);
        POINTER_SKYRIMSE(TSetCurrentPickREFR, s_setCurrentPickREFR, 51093, 50164);

        RealCheckForNewPackage = s_checkForNewPackage.Get();
        RealInitFromPackage = s_initFromPackage.Get();
        RealSetCurrentPickREFR = s_setCurrentPickREFR.Get();

        TP_HOOK(&RealCheckForNewPackage, HookCheckForNewPackage);
        TP_HOOK(&RealInitFromPackage, HookInitFromPackage);
        TP_HOOK(&RealSetCurrentPickREFR, HookSetCurrentPickREFR);
    });
