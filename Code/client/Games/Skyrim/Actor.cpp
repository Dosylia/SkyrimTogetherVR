#include <Games/References.h>
#include <Games/Skyrim/EquipManager.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <Misc/GameVM.h>
#include <DefaultObjectManager.h>
#include <Forms/TESNPC.h>
#include <Forms/TESFaction.h>
#include <Components/TESActorBaseData.h>
#include <ExtraData/ExtraFactionChanges.h>
#include <ExtraData/ExtraLeveledCreature.h>
#include <Games/Memory.h>
#include <Combat/CombatController.h>
#include <CrashHandler.h>

#include <Events/HealthChangeEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/MountEvent.h>
#include <Events/DialogueEvent.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Events/HitEvent.h>
#include <Events/RemoveSpellEvent.h>

#include <Games/TES.h>
#include <World.h>
#include <Services/PapyrusService.h>

#include <Forms/ActorValueInfo.h>
#include <Forms/TESRace.h>

#include <Effects/ValueModifierEffect.h>

#include <Games/Skyrim/Misc/InventoryEntry.h>
#include <Games/Skyrim/ExtraData/ExtraCount.h>
#include <Games/Misc/ActorKnowledge.h>

#include <ExtraData/ExtraDataList.h>
#include <ExtraData/ExtraCharge.h>
#include <ExtraData/ExtraCount.h>
#include <ExtraData/ExtraEnchantment.h>
#include <ExtraData/ExtraHealth.h>
#include <ExtraData/ExtraPoison.h>
#include <ExtraData/ExtraSoul.h>
#include <ExtraData/ExtraTextDisplayData.h>
#include <Forms/EnchantmentItem.h>
#include <Forms/AlchemyItem.h>
#include <Forms/TESObjectCELL.h>

#include <Structs/Skyrim/AnimationGraphDescriptor_BHR_Master.h>

#include <Games/Overrides.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Havok/hkbStateMachine.h>
#include <Havok/hkbBehaviorGraph.h>
#include <Havok/bhkCharacterController.h>
#include <Forms/TESLevItem.h>
#include <Forms/BGSOutfit.h>
#include <Forms/TESObjectARMO.h>

#include <ModCompat/BehaviorVar.h>
#include <PerfScope.h>

namespace
{
void QueueActorInventoryChange(Actor* apActor, InventoryChangeEvent aEvent, TESObjectREFR* apTransferReference = nullptr)
{
    auto ownershipToken = Utils::GetLocalOwnershipToken(apActor->formID);
    if (!ownershipToken && apTransferReference == PlayerCharacter::Get())
        ownershipToken = Utils::GetRemoteOwnershipToken(apActor->formID);

    if (!ownershipToken)
        return;

    aEvent.ServerId = ownershipToken->ServerId;
    aEvent.OwnershipEpoch = ownershipToken->OwnershipEpoch;
    World::Get().GetRunner().Trigger(std::move(aEvent));
}
}

#ifdef SAVE_STUFF

#include <Games/Skyrim/SaveLoad.h>
#include "Actor.h"

void Actor::Save_Reversed(const uint32_t aChangeFlags, Buffer::Writer& aWriter)
{
    BGSSaveFormBuffer buffer;

    Save(&buffer);

    AIProcess* pProcess = currentProcess;
    const int32_t handlerId = pProcess != nullptr ? pProcess->handlerId : -1;

    aWriter.WriteBytes((uint8_t*)&handlerId, 4); // TODO: is this needed ?
    aWriter.WriteBytes((uint8_t*)&flags1, 4);

    //     if (!handlerId
    //         && (uint8_t)AIProcess::GetBoolInSubStructure(pProcess))
    //     {
    //         Actor::SaveSkinFar(this);
    //     }

    TESObjectREFR::Save_Reversed(aChangeFlags, aWriter);

    if (pProcess)
        ; // Skyrim saves the process manager state, but we don't give a shit so skip !

    aWriter.WriteBytes((uint8_t*)&unk194, 4);
    aWriter.WriteBytes((uint8_t*)&headTrackingUpdateDelay, 4);
    aWriter.WriteBytes((uint8_t*)&unk9C, 4);
    // We skip 0x180 as it's not something we care about, some timer related data

    aWriter.WriteBytes((uint8_t*)&unk98, 4);
    // skip A8 - related to timers
    // skip AC - related to timers as well
    aWriter.WriteBytes((uint8_t*)&unkB0, 4);
    // skip E4 - never seen this used
    // skip E8 - same as E4
    aWriter.WriteBytes((uint8_t*)&unk84, 4);
    aWriter.WriteBytes((uint8_t*)&unkA4, 4);
    // skip baseForm->weight
    // skip 12C

    // Save actor state sub_6F0FB0
}

#endif

TP_THIS_FUNCTION(TRemoveSpell, bool, Actor, MagicItem*);
TP_THIS_FUNCTION(TCharacterConstructor, Actor*, Actor);
TP_THIS_FUNCTION(TCharacterConstructor2, Actor*, Actor, uint8_t aUnk);
TP_THIS_FUNCTION(TCharacterDestructor, Actor*, Actor);
TP_THIS_FUNCTION(TAddInventoryItem, void, Actor, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner);
TP_THIS_FUNCTION(TPickUpObject, void*, Actor, TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2);
TP_THIS_FUNCTION(TDropObject, void*, Actor, void* apResult, TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation);
TP_THIS_FUNCTION(TSetPosition, char, Actor, NiPoint3& acPosition);
TP_THIS_FUNCTION(TActorProcess, char, Actor, float aValue);

using TGetLocation = TESForm*(TESForm*);
static TGetLocation* FUNC_GetActorLocation;

TCharacterConstructor* RealCharacterConstructor;
TCharacterConstructor2* RealCharacterConstructor2;
TCharacterDestructor* RealCharacterDestructor;

static TRemoveSpell* RealRemoveSpell = nullptr;
static TAddInventoryItem* RealAddInventoryItem = nullptr;
static TPickUpObject* RealPickUpObject = nullptr;
static TDropObject* RealDropObject = nullptr;
static TSetPosition* RealSetPosition = nullptr;
static TActorProcess* RealActorProcess = nullptr;

float Actor::GetSpeed() noexcept
{
    static BSFixedString speedSampledStr("SpeedSampled");
    float speed = 0.f;
    animationGraphHolder.GetVariableFloat(&speedSampledStr, &speed);

    return speed;
}

void Actor::SetSpeed(float aSpeed) noexcept
{
    static BSFixedString speedSampledStr("SpeedSampled");
    animationGraphHolder.SetVariableFloat(&speedSampledStr, aSpeed);
}

TESNPC* Actor::GetLeveledPick() const noexcept
{
    const auto* pExtra = static_cast<ExtraLeveledCreature*>(extraData.GetByType(ExtraDataType::LeveledCreature));
    TESActorBase* pTemplate = pExtra ? pExtra->templateBase : nullptr;
    if (!pTemplate || pTemplate->formType != FormType::Npc || pTemplate->IsTemporary())
        return nullptr;

    return static_cast<TESNPC*>(pTemplate);
}

uint16_t Actor::GetLevel() const noexcept
{
    TP_THIS_FUNCTION(TGetLevel, uint16_t, const Actor);
    POINTER_SKYRIMSE(TGetLevel, s_getLevel, 37334, 36344);
    return TiltedPhoques::ThisCall(s_getLevel, this);
}

void Actor::ForcePosition(const NiPoint3& acPosition) noexcept
{
    ScopedReferencesOverride recursionGuard;

    bool updateController = true;
    if (GetExtension()->IsRemote() && currentProcess)
    {
        if (auto* pController = currentProcess->GetCharController())
        {
            // A newly created controller may be positioned before ActorProcess.
            // Both SetPositionImpl and the following velocity reset need a step
            updateController = pController->UpdateStepTiming();
        }
    }

    // With no usable step yet, update the reference/3D now; interpolation will
    // catch the controller up once physics timing becomes available
    SetPosition(acPosition, updateController);
}

void Actor::QueueUpdate() noexcept
{
    auto* pSetting = INISettingCollection::Get()->GetSetting("bUseFaceGenPreprocessedHeads:General");
    const auto originalValue = pSetting->data;
    pSetting->data = 0;

    TP_THIS_FUNCTION(TQueueUpdate, void, Actor, bool);
    POINTER_SKYRIMSE(TQueueUpdate, QueueUpdate, 40255, 39181);

    TiltedPhoques::ThisCall(QueueUpdate, this, true);

    pSetting->data = originalValue;
}

GamePtr<Actor> Actor::Create(TESNPC* apBaseForm) noexcept
{
    auto pActor = New();
    // Prevent saving
    pActor->SetSkipSaveFlag(true);
    pActor->GetExtension()->SetRemote(true);

    const auto pPlayer = static_cast<Actor*>(GetById(0x14));
    auto pCell = pPlayer->parentCell;
    const auto pWorldSpace = pPlayer->GetWorldSpace();

    pActor->SetLevelMod(4);
    pActor->MarkChanged(0x40000000);
    pActor->SetParentCell(pCell);
    pActor->SetObjectReference(apBaseForm);

    auto position = pPlayer->position;
    auto rotation = pPlayer->rotation;

    if (pCell && !(pCell->cellFlags & 1))
        pCell = nullptr;

    ModManager::Get()->Spawn(position, rotation, pCell, pWorldSpace, pActor);

    pActor->ForcePosition(position);

    pActor->GetMagicCaster(MagicSystem::CastingSource::LEFT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::RIGHT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::OTHER);

    pActor->flags &= 0xFFDFFFFF;

    return pActor;
}

GamePtr<Actor> Actor::Spawn(uint32_t aBaseFormId) noexcept
{
    TESNPC* pNpc = Cast<TESNPC>(TESForm::GetById(aBaseFormId));
    return Actor::Create(pNpc);
}

void Actor::SetLevelMod(uint32_t aLevel) noexcept
{
    TP_THIS_FUNCTION(TActorSetLevelMod, void, ExtraDataList, uint32_t);
    POINTER_SKYRIMSE(TActorSetLevelMod, realSetLevelMod, 11806, 11806);

    const auto pExtraDataList = &extraData;

    TiltedPhoques::ThisCall(realSetLevelMod, pExtraDataList, aLevel);
}

ActorExtension* Actor::GetExtension() noexcept
{
    if (AsExActor())
    {
        return static_cast<ActorExtension*>(AsExActor());
    }

    if (AsExPlayerCharacter())
    {
        return static_cast<ActorExtension*>(AsExPlayerCharacter());
    }

    return nullptr;
}

ExActor* Actor::AsExActor() noexcept
{
    if (formType == Type && this != PlayerCharacter::Get())
        return static_cast<ExActor*>(this);

    return nullptr;
}

ExPlayerCharacter* Actor::AsExPlayerCharacter() noexcept
{
    if (this == PlayerCharacter::Get())
        return static_cast<ExPlayerCharacter*>(this);

    return nullptr;
}

extern thread_local bool g_forceAnimation;

void Actor::SetWeaponDrawnEx(bool aDraw) noexcept
{
    spdlog::debug("Setting weapon drawn: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());

    if (actorState.IsWeaponDrawn() == aDraw)
    {
        actorState.SetWeaponDrawn(!aDraw);

        spdlog::debug("Setting weapon drawn after update: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());
    }

    g_forceAnimation = true;
    SetWeaponDrawn(aDraw);
    g_forceAnimation = false;
}

static thread_local bool s_execInitPackage = false;

void Actor::SetPackage(TESPackage* apPackage) noexcept
{
    s_execInitPackage = true;
    PutCreatedPackage(apPackage);
    s_execInitPackage = false;
}

void Actor::SetPlayerRespawnMode(bool aSet) noexcept
{
    SetEssentialEx(aSet);
    // Makes the player go in an unrecoverable bleedout state
    SetNoBleedoutRecovery(aSet);

    if (formID != 0x14)
    {
        auto pPlayerFaction = Cast<TESFaction>(TESForm::GetById(0xDB1));
        SetFactionRank(pPlayerFaction, 1);
    }
}

void Actor::SetEssentialEx(bool aSet) noexcept
{
    SetEssential(aSet);
    TESNPC* pBase = Cast<TESNPC>(baseForm);
    if (pBase)
        pBase->actorData.SetEssential(aSet);
}

void Actor::SetNoBleedoutRecovery(bool aSet) noexcept
{
    TP_THIS_FUNCTION(TSetNoBleedoutRecovery, void, Actor, bool);
    POINTER_SKYRIMSE(TSetNoBleedoutRecovery, s_setNoBleedoutRecovery, 38533, 38533);
    TiltedPhoques::ThisCall(s_setNoBleedoutRecovery, this, aSet);
}

void Actor::DispelAllSpells(bool aNow) noexcept
{
    magicTarget.DispelAllSpells(aNow);
}

bool Actor::IsInCombat() const noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, IsInCombat);
    return s_pIsInCombat(this);
}

Actor* Actor::GetCombatTarget() const noexcept
{
    PAPYRUS_FUNCTION(Actor*, Actor, GetCombatTarget);
    return s_pGetCombatTarget(this);
}

// TODO: this is a really hacky solution.
// The internal targeting system should be disabled instead.
void Actor::StartCombatEx(Actor* apTarget) noexcept
{
    if (GetCombatTarget() != apTarget)
    {
        StopCombat();
        StartCombat(apTarget);
    }
}

void Actor::SetCombatTargetEx(Actor* apTarget) noexcept
{
    if (pCombatController)
        pCombatController->SetTarget(apTarget);
}

void Actor::StartCombat(Actor* apTarget) noexcept
{
    PAPYRUS_FUNCTION(void, Actor, StartCombat, Actor*);
    s_pStartCombat(this, apTarget);
}

void Actor::StopCombat() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, StopCombat);
    s_pStopCombat(this);
}

bool Actor::RemoveSpell(MagicItem* apSpell) noexcept
{
    if (!apSpell)
    {
        spdlog::error(__FUNCTION__ ": apSpell is null");
        return false;
    }
    // spdlog::info(__FUNCTION__ ": removing: {} from actor: {}", apSpell->formID, formID);
    return TiltedPhoques::ThisCall(RealRemoveSpell, this, apSpell);
}

bool Actor::HasPerk(uint32_t aPerkFormId) const noexcept
{
    return GetPerkRank(aPerkFormId) != 0;
}

uint8_t Actor::GetPerkRank(uint32_t aPerkFormId) const noexcept
{
    BGSPerk* pPerk = Cast<BGSPerk>(TESForm::GetById(aPerkFormId));
    if (!pPerk)
        return 0;

    TP_THIS_FUNCTION(TGetPerkRank, uint8_t, const Actor, BGSPerk*);
    POINTER_SKYRIMSE(TGetPerkRank, getPerkRank, 37698, 36690);

    return TiltedPhoques::ThisCall(getPerkRank, this, pPerk);
}

bool TP_MAKE_THISCALL(HookRemoveSpell, Actor, MagicItem* apSpell)
{
    bool result = TiltedPhoques::ThisCall(RealRemoveSpell, apThis, apSpell);
    if (apThis->GetExtension()->IsLocalPlayer() && result)
    {
        //spdlog::info(__FUNCTION__ ": spell: {}, ID: {} from local player", apSpell->GetName() , apSpell->formID);
       RemoveSpellEvent removalEvent;

        removalEvent.TargetId = apThis->formID;
        removalEvent.SpellId = apSpell->formID;
        World::Get().GetRunner().Trigger(removalEvent);
    }

    return result;
}

Actor* TP_MAKE_THISCALL(HookCharacterConstructor, Actor)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    TiltedPhoques::ThisCall(RealCharacterConstructor, apThis);

    return apThis;
}

Actor* TP_MAKE_THISCALL(HookCharacterConstructor2, Actor, uint8_t aUnk)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    TiltedPhoques::ThisCall(RealCharacterConstructor2, apThis, aUnk);

    return apThis;
}

Actor* TP_MAKE_THISCALL(HookCharacterDestructor, Actor)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    auto pExtension = apThis->GetExtension();

    if (pExtension)
    {
        pExtension->~ActorExtension();
    }

    TiltedPhoques::ThisCall(RealCharacterDestructor, apThis);

    return apThis;
}

GamePtr<Actor> Actor::New() noexcept
{
    auto* const pActor = Memory::Allocate<Actor>();

    TiltedPhoques::ThisCall(RealCharacterConstructor, pActor);

    return {pActor};
}

void Actor::InterruptCast(bool abRefund) noexcept
{
    TP_THIS_FUNCTION(TInterruptCast, void, Actor, bool abRefund);

    POINTER_SKYRIMSE(TInterruptCast, s_interruptCast, 38757, 37808);

    TiltedPhoques::ThisCall(s_interruptCast, this, abRefund);
}

TESForm* Actor::GetEquippedWeapon(uint32_t aSlotId) const noexcept
{
    if (currentProcess && currentProcess->middleProcess)
    {
        auto pMiddleProcess = currentProcess->middleProcess;

        if (aSlotId == 0 && pMiddleProcess->leftEquippedObject)
            return pMiddleProcess->leftEquippedObject->pObject;

        else if (aSlotId == 1 && pMiddleProcess->rightEquippedObject)
            return pMiddleProcess->rightEquippedObject->pObject;
    }

    return nullptr;
}

TESForm* Actor::GetEquippedAmmo() const noexcept
{
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->ammoEquippedObject)
    {
        // TODO: rtti cast to check if is ammo object? or actually, just call AIProcess::GetCurrentAmmo()
        return currentProcess->middleProcess->ammoEquippedObject->pObject;
    }

    return nullptr;
}

bool Actor::IsWearingBodyPiece() const noexcept
{
#ifdef SKYRIMVR
    // GetArmorInSlot has no VR address. The same answer from the container entries: an armour flagged as a body
    // piece with a worn extra on one of its stacks. Cheap enough for the once-a-second naked-NPC check, which this
    // turns back on (it was off on VR, so bandits stayed naked; 2026-09-19).
    const ExtraContainerChanges::Data* pChanges = GetContainerChanges();
    if (!pChanges || !pChanges->entries)
        return true; // nothing to look at, and re-equipping blindly is worse than leaving it

    for (ExtraContainerChanges::Entry* pEntry : *pChanges->entries)
    {
        if (!pEntry || !pEntry->form || !pEntry->dataList)
            continue;

        const TESObjectARMO* pArmor = Cast<TESObjectARMO>(pEntry->form);
        if (!pArmor || !pArmor->IsBodyPiece())
            continue;

        for (ExtraDataList* pList : *pEntry->dataList)
        {
            if (pList && (pList->Contains(ExtraDataType::Worn) || pList->Contains(ExtraDataType::WornLeft)))
                return true;
        }
    }

    return false;
#endif
    return GetContainerChanges()->GetArmor(32) != nullptr;
}

bool Actor::ShouldWearBodyPiece() const noexcept
{
    TESNPC* pBase = Cast<TESNPC>(baseForm);
    if (!pBase)
        return false;

    BGSOutfit* pDefaultOutfit = pBase->defaultOutfit;
    if (!pDefaultOutfit)
        return false;

    for (auto* pItem : pDefaultOutfit->outfitItems)
    {
        TESObjectARMO* pArmor = nullptr;

        if (pItem->formType == FormType::Armor)
            pArmor = Cast<TESObjectARMO>(pItem);
        else if (pItem->formType == FormType::LeveledItem)
        {
            TESLevItem* pLevItem = Cast<TESLevItem>(pItem);
            if (!pLevItem || !pLevItem->pLeveledListA || !pLevItem->pLeveledListA->pForm)
                continue;

            pArmor = Cast<TESObjectARMO>(pLevItem->pLeveledListA->pForm);
        }
        else
            continue;

        if (!pArmor)
            continue;

        if (pArmor->IsBodyPiece()) 
            return true;
    }

    return false;
}

// Get owner of a summon or raised corpse
Actor* Actor::GetCommandingActor() const noexcept
{
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->commandingActor)
    {
        auto handle = currentProcess->middleProcess->commandingActor.handle;
        auto* pOwner = Cast<Actor>(TESObjectREFR::GetByHandle(handle.iBits));
        return pOwner;
    }

    return nullptr;
}

// Get owner of a summon or raised corpse
void Actor::SetCommandingActor(BSPointerHandle<TESObjectREFR> aCommandingActor) noexcept
{
    if (currentProcess && currentProcess->middleProcess)
    {
        currentProcess->middleProcess->commandingActor = aCommandingActor;
        flags2 |= ActorFlags::IS_COMMANDED_ACTOR;
    }
}

bool Actor::IsPlayerSummon() const noexcept
{
    const Actor* pCommandingActor = GetCommandingActor();
    return pCommandingActor && pCommandingActor->formID == 0x14;
}

TESForm* Actor::GetCurrentLocation()
{
    // we use the safe function which also
    // checks the form type
    return FUNC_GetActorLocation(this);
}

Factions Actor::GetFactions() const noexcept
{
    Factions result;

    auto& modSystem = World::Get().GetModSystem();

    auto* pNpc = Cast<TESNPC>(baseForm);
    if (pNpc)
    {
        auto& factions = pNpc->actorData.factions;

        for (auto i = 0u; i < factions.length; ++i)
        {
            Faction faction;

            modSystem.GetServerModId(factions[i].faction->formID, faction.Id);
            faction.Rank = factions[i].rank;

            result.NpcFactions.push_back(faction);
        }
    }

    auto* pChanges = Cast<ExtraFactionChanges>(extraData.GetByType(ExtraDataType::Faction));
    if (pChanges)
    {
        for (auto i = 0u; i < pChanges->entries.length; ++i)
        {
            Faction faction;

            modSystem.GetServerModId(pChanges->entries[i].faction->formID, faction.Id);
            faction.Rank = pChanges->entries[i].rank;

            result.ExtraFactions.push_back(faction);
        }
    }

    return result;
}

ActorValues Actor::GetEssentialActorValues() const noexcept
{
    ActorValues actorValues;

    int essentialValues[] = {ActorValueInfo::kHealth, ActorValueInfo::kStamina, ActorValueInfo::kMagicka};
    for (auto i : essentialValues)
    {
        float value = actorValueOwner.GetValue(i);
        actorValues.ActorValuesList.insert({i, value});
        float maxValue = actorValueOwner.GetPermanentValue(i);
        actorValues.ActorMaxValuesList.insert({i, maxValue});
    }

    return actorValues;
}

float Actor::GetActorValue(uint32_t aId) const noexcept
{
    return actorValueOwner.GetValue(aId);
}

float Actor::GetActorPermanentValue(uint32_t aId) const noexcept
{
    return actorValueOwner.GetPermanentValue(aId);
}

void Actor::SetActorValue(uint32_t aId, float aValue) noexcept
{
    actorValueOwner.SetValue(aId, aValue);
}

void Actor::ForceActorValue(ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue) noexcept
{
    float initialValue = aMode == ActorValueOwner::ForceMode::PERMANENT 
                         ? GetActorPermanentValue(aId)
                         : GetActorValue(aId);

    if (aValue == initialValue)
        return;

    actorValueOwner.ForceCurrent(aMode, aId, aValue - initialValue);
}

Inventory Actor::GetActorInventory() const noexcept
{
    Inventory inventory = GetInventory();

    inventory.CurrentMagicEquipment = GetMagicEquipment();

    return inventory;
}

MagicEquipment Actor::GetMagicEquipment() const noexcept
{
    MagicEquipment equipment;

    auto& modSystem = World::Get().GetModSystem();

    uint32_t mainId = magicItems[0] ? magicItems[0]->formID : 0;
    modSystem.GetServerModId(mainId, equipment.LeftHandSpell);

    uint32_t secondaryId = magicItems[1] ? magicItems[1]->formID : 0;
    modSystem.GetServerModId(secondaryId, equipment.RightHandSpell);

    uint32_t shoutId = equippedShout ? equippedShout->formID : 0;
    modSystem.GetServerModId(shoutId, equipment.Shout);

    return equipment;
}

Inventory Actor::GetEquipment() const noexcept
{
    Inventory inventory = GetInventory();
    inventory.RemoveByFilter([](const auto& entry) { return !entry.IsWorn(); });
    inventory.CurrentMagicEquipment = GetMagicEquipment();
    return inventory;
}

int32_t Actor::GetGoldAmount() const noexcept
{
    TP_THIS_FUNCTION(TGetGoldAmount, int32_t, const Actor);
    POINTER_SKYRIMSE(TGetGoldAmount, s_getGoldAmount, 37527, 36527);
    if (!s_getGoldAmount.Get())
        return 0;
    return TiltedPhoques::ThisCall(s_getGoldAmount, this);
}

void Actor::SetActorInventory(const Inventory& acInventory) noexcept
{
    PerfCounterScope perfScope(PerfCounter::kInventoryApply);
    spdlog::debug("Setting inventory for actor {:X}", formID);

    // The UnEquipAll() that used to be here is redundant,
    // as RemoveAllItems() unequips every item if needed.
    // Placing this UnEquipAll() here seems to trigger a Skyrim bug/race.

    Inventory currentInventory = GetActorInventory();

    if (!this->GetExtension()->IsPlayer() && currentInventory.ContainsQuestItems())
        SetInventoryRetainingQuestItems(currentInventory, acInventory);
    else
        SetInventory(acInventory);

    SetMagicEquipment(acInventory.CurrentMagicEquipment);
}

void Actor::SetMagicEquipment(const MagicEquipment& acEquipment) noexcept
{
    auto* pEquipManager = EquipManager::Get();
    auto& modSystem = World::Get().GetModSystem();

    if (acEquipment.LeftHandSpell)
    {
        uint32_t mainHandWeaponId = modSystem.GetGameId(acEquipment.LeftHandSpell);
        spdlog::debug("Setting left hand spell: {:X}", mainHandWeaponId);
        pEquipManager->EquipSpell(this, TESForm::GetById(mainHandWeaponId), 0);
    }

    if (acEquipment.RightHandSpell)
    {
        uint32_t secondaryHandWeaponId = modSystem.GetGameId(acEquipment.RightHandSpell);
        spdlog::debug("Setting right hand spell: {:X}", secondaryHandWeaponId);
        pEquipManager->EquipSpell(this, TESForm::GetById(secondaryHandWeaponId), 1);
    }

    if (acEquipment.Shout)
    {
        uint32_t shoutId = modSystem.GetGameId(acEquipment.Shout);
        spdlog::debug("Setting shout: {:X}", shoutId);
        pEquipManager->EquipShout(this, TESForm::GetById(shoutId));
    }
}

void Actor::SetActorValues(const ActorValues& acActorValues) noexcept
{
    for (auto& value : acActorValues.ActorMaxValuesList)
        ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, value.first, value.second);

    for (auto& value : acActorValues.ActorValuesList)
        ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, value.first, value.second);
}

void Actor::SetFactions(const Factions& acFactions) noexcept
{
    RemoveFromAllFactions();

    auto& modSystem = World::Get().GetModSystem();

    for (auto& entry : acFactions.NpcFactions)
    {
        auto pForm = GetById(modSystem.GetGameId(entry.Id));
        auto pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }

    for (auto& entry : acFactions.ExtraFactions)
    {
        auto pForm = GetById(modSystem.GetGameId(entry.Id));
        auto pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }
}

void Actor::SetFactionRank(const TESFaction* apFaction, int8_t aRank) noexcept
{
    TP_THIS_FUNCTION(TSetFactionRankInternal, void, Actor, const TESFaction*, int8_t);

    POINTER_SKYRIMSE(TSetFactionRankInternal, s_setFactionRankInternal, 37677, 37677);

    TiltedPhoques::ThisCall(s_setFactionRankInternal, this, apFaction, aRank);
}

void Actor::SetPlayerTeammate(bool aSet) noexcept
{
    TP_THIS_FUNCTION(TSetPlayerTeammate, void, Actor, bool aSet, bool abCanDoFavor);
    POINTER_SKYRIMSE(TSetPlayerTeammate, setPlayerTeammate, 37717, 37717);
    return TiltedPhoques::ThisCall(setPlayerTeammate, this, aSet, true);
}

void Actor::UnEquipAll() noexcept
{
    EquipManager::Get()->UnequipAll(this);

    // Taken from skyrim's code shouts can be two form types apparently
    if (equippedShout && ((int)equippedShout->formType - 41) <= 1)
    {
        EquipManager::Get()->UnEquipShout(this, equippedShout);
        equippedShout = nullptr;
    }
}

void Actor::RemoveFromAllFactions() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, RemoveFromAllFactions);

    s_pRemoveFromAllFactions(this);
}

TP_THIS_FUNCTION(TInitiateMountPackage, bool, Actor, Actor* apMount);
static TInitiateMountPackage* RealInitiateMountPackage = nullptr;

bool Actor::InitiateMountPackage(Actor* apMount) noexcept
{
    if (!RealInitiateMountPackage)
        return false;
    return TiltedPhoques::ThisCall(RealInitiateMountPackage, this, apMount);
}

void Actor::GenerateMagicCasters() noexcept
{
    using CS = MagicSystem::CastingSource;

    for (int i = 0; i < 4; i++)
    {
        if (casters[i] == nullptr)
            casters[i] = Cast<ActorMagicCaster>(GetMagicCaster(static_cast<CS>(i)));
    }
}

bool Actor::IsDead() const noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, IsDead);

    return s_pIsDead(this);
}

bool Actor::IsDragon() const noexcept
{
    const ActorExtension* pExtension = const_cast<Actor*>(this)->GetExtension();
    if (BehaviorVar::IsDragon(pExtension->GraphDescriptorHash))
        return true;

    // A modded dragon behavior is only recognized after the actor was already sent to the server,
    // which then gave it the normal (too small) range. The race keyword ActorTypeDragon is known
    // right away. TESRace: BGSKeywordForm at +0x70 (keywords +0x78, count +0x80).
    constexpr uint8_t cRaceFormType = 14;
    constexpr uint8_t cKeywordFormType = 4;
    constexpr uint32_t cActorTypeDragon = 0x35D59;

    const TESForm* pRace = reinterpret_cast<const TESForm*>(race);
    if (!pRace || static_cast<uint8_t>(pRace->formType) != cRaceFormType)
        return false;

    const auto* ppKeywords = *reinterpret_cast<TESForm** const*>(reinterpret_cast<const uint8_t*>(pRace) + 0x78);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pRace) + 0x80);
    if (!ppKeywords || count > 256)
        return false;

    for (uint32_t i = 0; i < count; ++i)
    {
        const TESForm* pKeyword = ppKeywords[i];
        if (pKeyword && static_cast<uint8_t>(pKeyword->formType) == cKeywordFormType && pKeyword->formID == cActorTypeDragon)
            return true;
    }

    return false;
}

void Actor::Kill() noexcept
{
    // Never kill players
    ActorExtension* pExtension = GetExtension();
    if (pExtension->IsPlayer())
        return;

    // Remote actors only play animations sent by their owner (HookPerformAction). The death animation and
    // ragdoll have to go through here, or the body dies standing up.
    const bool forceAnimation = pExtension->IsRemote();
    if (forceAnimation)
        g_forceAnimation = true;

#ifdef SKYRIMVR
    // Call KillImpl (SE 36872) by address rather than through the virtual, whose VR slot is unverified.
    TP_THIS_FUNCTION(TKillImpl, void, Actor, Actor* apAttacker, float aDamage, bool aSendEvent, bool aRagdollInstant);
    POINTER_SKYRIMSE(TKillImpl, s_killImpl, 0, 36872);
    TiltedPhoques::ThisCall(s_killImpl, this, nullptr, 100.f, true, true);

    if (forceAnimation)
        g_forceAnimation = false;
    return;
#endif
    // TODO: these args are kind of bogus of course
    KillImpl(nullptr, 100.f, true, true);

    if (forceAnimation)
        g_forceAnimation = false;

    // Papyrus kill will not go through if it is queued by a kill move
    /*
    PAPYRUS_FUNCTION(void, Actor, Kill, void*);
    s_pKill(this, NULL);
    */
}

void Actor::Reset() noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, Reset, int, TESObjectREFR*);

    s_pReset(this, 0, nullptr);
}

bool Actor::PlayIdle(TESIdleForm* apIdle) noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, PlayIdle, TESIdleForm*);
    return s_pPlayIdle(this, apIdle);
}

void Actor::Respawn() noexcept
{
#ifdef SKYRIMVR
    // Resurrect(resetInventory, attach3D) = SE 36331, by address like KillImpl above.
    TP_THIS_FUNCTION(TResurrect, void, Actor, bool aResetInventory, bool aAttach3D);
    POINTER_SKYRIMSE(TResurrect, s_resurrect, 0, 36331);
    TiltedPhoques::ThisCall(s_resurrect, this, false, true);
    Reset();
    return;
#endif
    Resurrect(false);
    Reset();
}

bool Actor::IsVampireLord() const noexcept
{
    return race && race->formID == 0x200283A;
}

extern thread_local bool g_forceAnimation;

void Actor::FixVampireLordModel() noexcept
{
    TESBoundObject* pLordArmor = Cast<TESBoundObject>(TESForm::GetById(0x2011a84));
    if (!pLordArmor)
        return;

    {
        ScopedInventoryOverride _;
        AddObjectToContainer(pLordArmor, nullptr, 1, nullptr);
    }

    EquipManager::Get()->Equip(this, pLordArmor, nullptr, 1, nullptr, false, true, false, false);

    g_forceAnimation = true;

    BSFixedString str("isLevitating");
    uint32_t isLevitating = GetAnimationVariableInt(&str);
    spdlog::critical("isLevitating {}", isLevitating);

    // By default, a loaded vampire lord is not levitating.
    if (isLevitating)
    {
        BSFixedString levitation("LevitationToggle");
        SendAnimationEvent(&levitation);
    }

    // TODO: weapon draw code does not seem to take care of this
    //BSFixedString weapEquip("WeapEquip");
    //SendAnimationEvent(&weapEquip);

    g_forceAnimation = false;
}

char TP_MAKE_THISCALL(HookSetPosition, Actor, NiPoint3& aPosition)
{
    const auto pExtension = apThis ? apThis->GetExtension() : nullptr;
    const auto bIsRemote = pExtension && pExtension->IsRemote();

    // A remote corpse moves with its own ragdoll (see HookActorProcess).
    if (bIsRemote && !apThis->actorState.IsDeadOrDying() && !ScopedReferencesOverride::IsOverriden())
        return 1;

    // Don't interfere with non actor references, or the player, or if we are calling our self
    if (apThis->formType != Actor::Type || apThis == PlayerCharacter::Get() || ScopedReferencesOverride::IsOverriden())
        return TiltedPhoques::ThisCall(RealSetPosition, apThis, aPosition);

    ScopedReferencesOverride recursionGuard;

    // It just works TM
    apThis->SetPosition(aPosition, false);

    return 1;
}

TP_THIS_FUNCTION(TForceState, void, Actor, const NiPoint3&, float, float, TESObjectCELL*, TESWorldSpace*, bool);
static TForceState* RealForceState = nullptr;

void TP_MAKE_THISCALL(HookForceState, Actor, const NiPoint3& acPosition, float aX, float aZ, TESObjectCELL* apCell, TESWorldSpace* apWorldSpace, bool aUnkBool)
{
    /*const auto pNpc = Cast<TESNPC>(apThis->baseForm);
    if (pNpc)
    {
        spdlog::info("For TESNPC: {}, spawn at {} {} {}", pNpc->fullName.value, apPosition->m_x, apPosition->m_y,
                     apPosition->m_z);
    }*/

    // if (apThis != PlayerCharacter::Get())
    //     return;

    return TiltedPhoques::ThisCall(RealForceState, apThis, acPosition, aX, aZ, apCell, apWorldSpace, aUnkBool);
}

TP_THIS_FUNCTION(TSpawnActorInWorld, bool, Actor);
static TSpawnActorInWorld* RealSpawnActorInWorld = nullptr;

// TODO: this isn't SpawnActorInWorld, this is TESObjectREFR::UpdateReference3D()
bool TP_MAKE_THISCALL(HookSpawnActorInWorld, Actor)
{
    const auto* pNpc = Cast<TESNPC>(apThis->baseForm);
    if (pNpc)
    {
        spdlog::info("Spawn Actor: {:X}, and NPC {}", apThis->formID, pNpc->fullName.value);
    }

    return TiltedPhoques::ThisCall(RealSpawnActorInWorld, apThis);
}

TP_THIS_FUNCTION(TDamageActor, bool, Actor, float aDamage, Actor* apHitter, bool aKillMove);
static TDamageActor* RealDamageActor = nullptr;

// TODO: this is flawed, since it does not account for invulnerable actors
bool TP_MAKE_THISCALL(HookDamageActor, Actor, float aDamage, Actor* apHitter, bool aKillMove)
{
    if (apHitter)
        World::Get().GetRunner().Trigger(HitEvent(apHitter->formID, apThis->formID));

    float realDamage = GameplayFormulas::CalculateRealDamage(apThis, aDamage, aKillMove);

    float currentHealth = apThis->GetActorValue(ActorValueInfo::kHealth);
    bool wouldKill = (currentHealth - realDamage) <= 0.f;

    const auto* pExHittee = apThis->GetExtension();
    if (pExHittee->IsLocalPlayer())
    {
        if (!World::Get().GetServerSettings().PvpEnabled)
        {
            if (apHitter && apHitter->GetExtension()->IsRemotePlayer())
                return false;
        }

        World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
        return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
    }
    else if (pExHittee->IsRemotePlayer())
    {
        // A VR player never plays an attack animation, so the other player's game never sees the swing land and
        // could not apply it; the only game that knows this sword hit that body is this one. With PvP on, the hit is
        // sent as a health change, which the other side applies to its own player through the usual broadcast
        // (health is floored at zero there, players are never killed). Sword fights between players, 2026-09-18.
        // Only this player's own hits (sword, arrow, spell): the other player's game already handles what its own
        // copies of NPCs do to it, and forwarding an NPC's hits from here doubled them (a spider's bites arrived
        // 90 times a second on top of the victim's own spider, 2026-09-19).
        if (World::Get().GetServerSettings().PvpEnabled && realDamage > 0.f && apHitter && apHitter->GetExtension()->IsLocalPlayer())
        {
            if (realDamage >= 1.f)
                spdlog::info("PvP: hit remote player {:X} for {:.0f}", apThis->formID, realDamage);
            World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
        }

        // Never "killed": that answer made the game put the copy down here on the strength of its stale health,
        // while the player it stands for was fine. The owner's game decides, and a respawn replaces the copy.
        if (wouldKill)
            spdlog::info("Remote player {:X} copy would have been downed by a local hit; left standing", apThis->formID);
        return false;
    }

    if (apHitter)
    {
        const auto* pExHitter = apHitter->GetExtension();
        if (pExHitter->IsLocalPlayer())
        {
            World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
            return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
        }
        if (pExHitter->IsRemotePlayer())
        {
            return wouldKill;
        }
    }

    if (pExHittee->IsLocal())
    {
        World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
        return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
    }
    else
    {
        return wouldKill;
    }
}

TP_THIS_FUNCTION(TApplyActorEffect, void, ActiveEffect, Actor* apTarget, float aEffectValue, unsigned int unk1);
static TApplyActorEffect* RealApplyActorEffect = nullptr;

void TP_MAKE_THISCALL(HookApplyActorEffect, ActiveEffect, Actor* apTarget, float aEffectValue, unsigned int unk1)
{
    const auto* pValueModEffect = Cast<ValueModifierEffect>(apThis);

    if (pValueModEffect)
    {
        if (pValueModEffect->actorValueIndex == ActorValueInfo::kHealth && aEffectValue > 0.0f)
        {
            if (apTarget && apTarget->GetExtension())
            {
                const auto pExTarget = apTarget->GetExtension();
                if (pExTarget->IsLocal())
                {
                    World::Get().GetRunner().Trigger(HealthChangeEvent(apTarget->formID, aEffectValue));
                    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);
                }
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);
}

TP_THIS_FUNCTION(TRegenAttributes, void*, Actor, int aId, float regenValue);
static TRegenAttributes* RealRegenAttributes = nullptr;

void* TP_MAKE_THISCALL(HookRegenAttributes, Actor, int aId, float aRegenValue)
{
    if (aId != ActorValueInfo::kHealth)
    {
        return TiltedPhoques::ThisCall(RealRegenAttributes, apThis, aId, aRegenValue);
    }

    const auto* pExTarget = apThis->GetExtension();
    if (pExTarget->IsRemote())
    {
        return 0;
    }

    World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, aRegenValue));
    return TiltedPhoques::ThisCall(RealRegenAttributes, apThis, aId, aRegenValue);
}

void TP_MAKE_THISCALL(HookAddInventoryItem, Actor, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner)
{
    if (!ScopedInventoryOverride::IsOverriden())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apItem->formID, item.BaseId);
        item.Count = aCount;

        if (apExtraData)
            apThis->GetItemFromExtraData(item, apExtraData);

        QueueActorInventoryChange(apThis, InventoryChangeEvent(apThis->formID, std::move(item)), apOldOwner);
    }

    TiltedPhoques::ThisCall(RealAddInventoryItem, apThis, apItem, apExtraData, aCount, apOldOwner);
}

void* TP_MAKE_THISCALL(HookPickUpObject, Actor, TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2)
{
    if (!ScopedInventoryOverride::IsOverriden())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apObject->baseForm->formID, item.BaseId);
        item.Count = aCount;

        if (apObject->GetExtraDataList())
            apThis->GetItemFromExtraData(item, apObject->GetExtraDataList());

        // This is here so that objects that are picked up on both clients, aka non temps, are synced through activation sync.
        // The inventory change event should always be sent to the server, otherwise the server inventory won't be updated.
        bool shouldUpdateClients = apObject->IsTemporary() && !ScopedActivateOverride::IsOverriden();

        QueueActorInventoryChange(apThis, InventoryChangeEvent(apThis->formID, std::move(item), false, shouldUpdateClients));
    }

    return TiltedPhoques::ThisCall(RealPickUpObject, apThis, apObject, aCount, aUnk1, aUnk2);
}

void Actor::PickUpObject(TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2) noexcept
{
    TiltedPhoques::ThisCall(RealPickUpObject, this, apObject, aCount, aUnk1, aUnk2);
}

void* TP_MAKE_THISCALL(HookDropObject, Actor, void* apResult, TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation)
{
    auto& modSystem = World::Get().GetModSystem();

    Inventory::Entry item{};
    modSystem.GetServerModId(apObject->formID, item.BaseId);
    item.Count = -aCount;

    if (apExtraData)
        apThis->GetItemFromExtraData(item, apExtraData);

    QueueActorInventoryChange(apThis, InventoryChangeEvent(apThis->formID, std::move(item), true));

    ScopedInventoryOverride _;

    return TiltedPhoques::ThisCall(RealDropObject, apThis, apResult, apObject, apExtraData, aCount, apLocation, apRotation);
}

void Actor::DropOrPickUpObject(const Inventory::Entry& arEntry, NiPoint3* apLocation, NiPoint3* apRotation) noexcept
{
    ExtraDataList* pExtraData = GetExtraDataFromItem(arEntry);

    ModSystem& modSystem = World::Get().GetModSystem();

    uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
    TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(objectId));
    if (!pObject)
    {
        spdlog::warn("Object to drop not found, {:X}:{:X}.", arEntry.BaseId.ModId, arEntry.BaseId.BaseId);
        return;
    }

    if (arEntry.Count < 0)
        DropObject(pObject, pExtraData, -arEntry.Count, apLocation, apRotation);
    // TODO: pick up
}

void Actor::DropObject(TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation) noexcept
{
    spdlog::debug("Dropping object, form id: {:X}, count: {}, actor: {:X}", apObject->formID, aCount, formID);
    BSPointerHandle<TESObjectREFR> result{};
    TiltedPhoques::ThisCall(RealDropObject, this, &result, apObject, apExtraData, aCount, apLocation, apRotation);
}

TP_THIS_FUNCTION(TUpdateDetectionState, void, ActorKnowledge, void*);
static TUpdateDetectionState* RealUpdateDetectionState = nullptr;

void TP_MAKE_THISCALL(HookUpdateDetectionState, ActorKnowledge, void* apState)
{
    auto pOwner = TESObjectREFR::GetByHandle(apThis->hOwner);
    auto pTarget = TESObjectREFR::GetByHandle(apThis->hTarget);

    if (pOwner && pTarget)
    {
        auto pOwnerActor = Cast<Actor>(pOwner);
        auto pTargetActor = Cast<Actor>(pTarget);
        if (pOwnerActor && pTargetActor)
        {
            if (pOwnerActor->GetExtension()->IsRemotePlayer() && pTargetActor->GetExtension()->IsLocalPlayer())
            {
                spdlog::debug("Cancelling detection from remote player to local player, owner: {:X}, target: {:X}", pOwner->formID, pTarget->formID);
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealUpdateDetectionState, apThis, apState);
}

struct DialogueItem;

// TODO: This is an AIProcess function
TP_THIS_FUNCTION(TProcessResponse, uint64_t, void, DialogueItem* apVoice, Actor* apTalkingActor, Actor* apTalkedToActor);
static TProcessResponse* RealProcessResponse = nullptr;

uint64_t TP_MAKE_THISCALL(HookProcessResponse, void, DialogueItem* apVoice, Actor* apTalkingActor, Actor* apTalkedToActor)
{
    if (apTalkingActor)
    {
        if (apTalkingActor->GetExtension()->IsRemotePlayer())
            return 0;
    }
    return TiltedPhoques::ThisCall(RealProcessResponse, apThis, apVoice, apTalkingActor, apTalkedToActor);
}

bool TP_MAKE_THISCALL(HookInitiateMountPackage, Actor, Actor* apMount)
{
    // Same as HookActorProcess: an actor with no extension is not one of ours.
    const ActorExtension* pExtension = apThis->GetExtension();
    if (apMount && pExtension && pExtension->IsLocal())
        World::Get().GetRunner().Trigger(MountEvent(apThis->formID, apMount->formID));

    return TiltedPhoques::ThisCall(RealInitiateMountPackage, apThis, apMount);
}

TP_THIS_FUNCTION(TUnequipObject, void, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3);
static TUnequipObject* RealUnequipObject = nullptr;

void TP_MAKE_THISCALL(HookUnequipObject, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3)
{
    TiltedPhoques::ThisCall(RealUnequipObject, apThis, apUnk1, apObject, aUnk2, apUnk3);
}

TP_THIS_FUNCTION(TSpeakSoundFunction, bool, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14);
static TSpeakSoundFunction* RealSpeakSoundFunction = nullptr;

bool TP_MAKE_THISCALL(HookSpeakSoundFunction, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14)
{
    spdlog::debug("a3: {:X}, a4: {}, a5: {}, a6: {}, a7: {}, a8: {:X}, a9: {:X}, a10: {}, a11: {:X}, a12: {}, a13: {}, a14: {}", (uint64_t)a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);

    // The player having the conversation may not own this NPC. Ambient
    // speech still comes only from the actor's simulation owner.
    if (apThis->GetExtension()->IsLocal() || MenuTopicManager::IsPlayerDialogueSpeaker(apThis))
        World::Get().GetRunner().Trigger(DialogueEvent(apThis->formID, apName));

    return TiltedPhoques::ThisCall(RealSpeakSoundFunction, apThis, apName, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

void Actor::SpeakSound(const char* pFile)
{
    uint32_t handle[3]{};
    handle[0] = -1;
    TiltedPhoques::ThisCall(RealSpeakSoundFunction, this, pFile, handle, 0, 0x32, 0, 0, 0, 0, 0, 0, 0, 1, 1);
}

char TP_MAKE_THISCALL(HookActorProcess, Actor, float aDeltaTime)
{
    // Remote actors are driven by their owner, so their AI doesn't run here. A remote corpse still updates, or its
    // ragdoll never falls. Upstream also keeps the character controller's timestep valid while the AI is skipped: a
    // controller created during the skip is left with a zero step, and the actor then glides after a collision
    // (#901). That is the sliding we have been chasing.
    //
    // GetExtension() returns null by design for anything this client did not allocate as an ExActor, and this
    // hook is handed every actor the engine processes. Seen crashed here on 2026-09-23 with a null extension
    // while guards were reacting to a crime (HookActorProcess+0x1e calling ActorExtension::IsRemote with rcx 0,
    // on a Redoran Guard). Nothing about the merge caused it; the same unguarded dereference is in the pre-merge
    // code, it just needs an actor without an extension to meet it. An actor we do not extend is not remote, so
    // it takes the normal path.
    const ActorExtension* pExtension = apThis->GetExtension();
    if (pExtension && pExtension->IsRemote() && !apThis->actorState.IsDeadOrDying())
    {
        if (apThis->currentProcess)
        {
            if (auto* pController = apThis->currentProcess->GetCharController())
                pController->UpdateStepTiming(aDeltaTime);
        }
        return 0;
    }

    return TiltedPhoques::ThisCall(RealActorProcess, apThis, aDeltaTime);
}

TP_THIS_FUNCTION(TAddDeathItems, void, Actor);
static TAddDeathItems* RealAddDeathItems = nullptr;

void TP_MAKE_THISCALL(HookAddDeathItems, Actor)
{
    // Same as HookActorProcess: the engine hands this every dying actor, including ones with no extension.
    const ActorExtension* pExtension = apThis->GetExtension();
    if (pExtension && pExtension->IsRemote())
        return;

    TiltedPhoques::ThisCall(RealAddDeathItems, apThis);
}

TP_THIS_FUNCTION(TIsFleeing, bool, Actor);
static TIsFleeing* RealIsFleeing = nullptr;

bool TP_MAKE_THISCALL(HookIsFleeing, Actor)
{
    // TODO: Player or RemotePlayer? Can players be in fleeing mode in skyrim?
    // TODO: investigate why the flee flag is set at all on remote players sometimes.
    if (apThis->GetExtension()->IsPlayer())
        return false;
    
    return TiltedPhoques::ThisCall(RealIsFleeing, apThis);
}

// The crime alarm (Actor::StealAlarm, SE 36427) walks every actor that might have witnessed the offence and reads
// each one's flags at Actor+0xE8. Twice it read them off nothing: 2026-09-20 17:59, the friend's follower Frea
// turned hostile and killed his session, and 2026-09-21 19:55, a Skaal villager was struck four minutes into a
// session and killed hers. Same function, same +0x12D9, same null, both while connected, both with actors that had
// just been handed to the other player or deleted here as temporaries. None of our code is on the stack: it is the
// game's own walk over a list holding an empty slot, and the exe cannot be read to find which list.
//
// Until it can be, the alarm is allowed to fail rather than end the session. The hit lands either way; what is lost
// is the crime being reported for that one blow, and the log says when it happened and to whom. The sibling VR port
// guards two havok crash sites the same way.
//
// MSVC will not allow __try in a function that needs C++ unwinding, so the call sits alone in here.
TP_THIS_FUNCTION(TStealAlarm, void, Actor, TESObjectREFR*, TESForm*, int32_t, int32_t, TESForm*, bool);
static TStealAlarm* RealStealAlarm = nullptr;

static bool CallStealAlarm(Actor* apThis, TESObjectREFR* apRef, TESForm* apObject, int32_t aNum, int32_t aTotal, TESForm* apOwner, bool aAllowWarning) noexcept
{
    __try
    {
        TiltedPhoques::ThisCall(RealStealAlarm, apThis, apRef, apObject, aNum, aTotal, apOwner, aAllowWarning);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}

void TP_MAKE_THISCALL(HookStealAlarm, Actor, TESObjectREFR* apRef, TESForm* apObject, int32_t aNum, int32_t aTotal, TESForm* apOwner, bool aAllowWarning)
{
    // TEMPORARY (2026-09-21): the guard below did not catch the fault that killed four sessions, and a fault inside
    // relocated hook code would not reach it whether this runs or not. These few lines say which it is.
    static std::atomic<uint32_t> s_entries{0};
    if (const uint32_t cEntry = ++s_entries; cEntry <= 3)
        spdlog::info("Crime alarm ran ({}): offender {:X}, witness {:X}, object {:X}", cEntry, apThis ? apThis->formID : 0, apRef ? apRef->formID : 0, apObject ? apObject->formID : 0);

    CrashGuard::Enter();
    const bool cSurvived = CallStealAlarm(apThis, apRef, apObject, aNum, aTotal, apOwner, aAllowWarning);
    CrashGuard::Leave();

    if (!cSurvived)
        spdlog::warn("Crime alarm walked a null actor and was skipped: offender {:X}, witness {:X}, object {:X}, count {} of {}. The blow still landed, the crime was not reported.",
                     apThis ? apThis->formID : 0, apRef ? apRef->formID : 0, apObject ? apObject->formID : 0, aNum, aTotal);
}

static TiltedPhoques::Initializer s_actorHooks(
    []()
    {
        POINTER_SKYRIMSE(TActorProcess, s_actorProcess, 37356, 37356);
        POINTER_SKYRIMSE(TSetPosition, s_setPosition, 19790, 19790);
        POINTER_SKYRIMSE(TRemoveSpell, s_removeSpell, 38717, 37772);
        POINTER_SKYRIMSE(TCharacterConstructor, s_characterCtor, 40245, 40245);
        POINTER_SKYRIMSE(TCharacterConstructor2, s_characterCtor2, 40246, 40246);
        POINTER_SKYRIMSE(TCharacterDestructor, s_characterDtor, 37175, 37175);
        POINTER_SKYRIMSE(TGetLocation, s_GetActorLocation, 19812, 19385);
        POINTER_SKYRIMSE(TForceState, s_ForceState, 37313, 37313);
        POINTER_SKYRIMSE(TSpawnActorInWorld, s_SpawnActorInWorld, 19742, 19742);
        POINTER_SKYRIMSE(TDamageActor, s_damageActor, 37335, 36345);
        POINTER_SKYRIMSE(TApplyActorEffect, s_applyActorEffect, 35086, 35086);
        // SE 36452 was TESObjectREFR::GetSubmergeLevel, not a regen function: VR 0x1405e9b60 takes
        // (this, float, TESObjectCELL*) and was being hooked with (this, int, float). SE 37513
        // (Actor::RestoreActorValue, VR 0x1406296b0) is the one this hook wants, and it is what the
        // SE-era code in 5da6679e called. Checked in the VR disassembly, not just the id tables.
        POINTER_SKYRIMSE(TRegenAttributes, s_regenAttributes, 37448, 37513);
        POINTER_SKYRIMSE(TAddInventoryItem, s_addInventoryItem, 37525, 36525);
        POINTER_SKYRIMSE(TPickUpObject, s_pickUpObject, 37521, 37521);
        POINTER_SKYRIMSE(TDropObject, s_dropObject, 40454, 40454);
        POINTER_SKYRIMSE(TUpdateDetectionState, s_updateDetectionState, 42704, 42704);
        POINTER_SKYRIMSE(TProcessResponse, s_processResponse, 39643, 39643);
        POINTER_SKYRIMSE(TInitiateMountPackage, s_initiateMountPackage, 37905, 37905);
        POINTER_SKYRIMSE(TUnequipObject, s_unequipObject, 37975, 37975);
        POINTER_SKYRIMSE(TSpeakSoundFunction, s_speakSoundFunction, 37542, 37542);
        POINTER_SKYRIMSE(TAddDeathItems, addDeathItems, 37198, 36218);
        POINTER_SKYRIMSE(TIsFleeing, isFleeing, 37577, 37577);
#ifdef SKYRIMVR
        POINTER_SKYRIMSE(TStealAlarm, s_stealAlarm, 36427, 36427);
        RealStealAlarm = s_stealAlarm.Get();
        if (RealStealAlarm)
            TP_HOOK(&RealStealAlarm, HookStealAlarm);
        else
            spdlog::warn("Crime alarm guard off: SE 36427 did not resolve");
#endif

        RealActorProcess = s_actorProcess.Get();
        RealSetPosition = s_setPosition.Get();
        RealRemoveSpell = s_removeSpell.Get();
        FUNC_GetActorLocation = s_GetActorLocation.Get();
        RealCharacterConstructor = s_characterCtor.Get();
        RealCharacterConstructor2 = s_characterCtor2.Get();
        RealForceState = s_ForceState.Get();
        RealSpawnActorInWorld = s_SpawnActorInWorld.Get();
        RealDamageActor = s_damageActor.Get();
        RealApplyActorEffect = s_applyActorEffect.Get();
        RealRegenAttributes = s_regenAttributes.Get();
        RealAddInventoryItem = s_addInventoryItem.Get();
        RealPickUpObject = s_pickUpObject.Get();
        RealDropObject = s_dropObject.Get();
        RealUpdateDetectionState = s_updateDetectionState.Get();
        RealProcessResponse = s_processResponse.Get();
        RealInitiateMountPackage = s_initiateMountPackage.Get();
        RealUnequipObject = s_unequipObject.Get();
        RealSpeakSoundFunction = s_speakSoundFunction.Get();
        RealAddDeathItems = addDeathItems.Get();
        RealIsFleeing = isFleeing.Get();

        TP_HOOK(&RealActorProcess, HookActorProcess);
        TP_HOOK(&RealSetPosition, HookSetPosition);
        TP_HOOK(&RealRemoveSpell, HookRemoveSpell);
        TP_HOOK(&RealCharacterConstructor, HookCharacterConstructor);
        TP_HOOK(&RealCharacterConstructor2, HookCharacterConstructor2);
        TP_HOOK(&RealForceState, HookForceState);
        TP_HOOK(&RealSpawnActorInWorld, HookSpawnActorInWorld);
        TP_HOOK(&RealDamageActor, HookDamageActor);
        TP_HOOK(&RealApplyActorEffect, HookApplyActorEffect);
        TP_HOOK(&RealRegenAttributes, HookRegenAttributes);
        TP_HOOK(&RealAddInventoryItem, HookAddInventoryItem);
        TP_HOOK(&RealPickUpObject, HookPickUpObject);
        TP_HOOK(&RealDropObject, HookDropObject);
        TP_HOOK(&RealUpdateDetectionState, HookUpdateDetectionState);
        TP_HOOK(&RealProcessResponse, HookProcessResponse);
        TP_HOOK(&RealInitiateMountPackage, HookInitiateMountPackage);
        TP_HOOK(&RealUnequipObject, HookUnequipObject);
        TP_HOOK(&RealSpeakSoundFunction, HookSpeakSoundFunction);
        TP_HOOK(&RealAddDeathItems, HookAddDeathItems);
        TP_HOOK(&RealIsFleeing, HookIsFleeing);
    });
