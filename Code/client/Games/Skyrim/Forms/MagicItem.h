#pragma once

#include <Forms/TESBoundObject.h>
#include <Forms/EffectSetting.h>
#include <Forms/BGSKeyword.h>
#include <Magic/EffectItem.h>
#include <Components/BGSKeywordForm.h>
#include <Components/TESFullName.h>

struct MagicItem : TESBoundObject
{
    bool IsWardSpell() const noexcept;
    bool IsInvisibilitySpell() const noexcept;
    bool IsHealingSpell() const noexcept;
    bool IsBuffSpell() const noexcept;
    //! Carries the vanilla MagicDamageHealth keyword, on the spell or on one of its effects. Used to decide
    //! whether a hit on another player is worth sending as damage; anything this cannot recognise is simply not
    //! sent, which is what happened to every spell before.
    bool IsDamageHealthSpell() const noexcept;
    bool IsBoundWeaponSpell() noexcept;
    bool HasSummonEffect() const noexcept;

    EffectItem* GetEffect(const uint32_t aEffectId) noexcept;

    TESFullName fullName;
    BGSKeywordForm keyword;
    GameArray<EffectItem*> listOfEffects;
    int32_t iHostileCount;
    EffectSetting* pAVEffectSetting;
    uint32_t uiPreloadCount;
    void* pPreloadItem;
};

static_assert(sizeof(MagicItem) == 0x90);
