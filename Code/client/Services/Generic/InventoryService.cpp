#include <Services/InventoryService.h>
#include <PerfScope.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>

#include <Events/UpdateEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/EquipmentChangeEvent.h>

#include <World.h>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Actor.h>
#include <Structs/ObjectData.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <EquipManager.h>
#include <Games/ActorExtension.h>
#include <Forms/TESNPC.h>
#include <DefaultObjectManager.h>
#include <PlayerCharacter.h>
#include <Forms/MagicItem.h>

namespace
{
// Defined with ApplyHandEquipment below: the record of what this side put in a remote player copy's hands.
void NoteHandItem(Actor* apActor, TESForm* apItem, bool aLeft, bool aEquipped) noexcept;
} // namespace

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&InventoryService::OnUpdate>(this);
    m_inventoryConnection = m_dispatcher.sink<InventoryChangeEvent>().connect<&InventoryService::OnInventoryChangeEvent>(this);
    m_equipmentConnection = m_dispatcher.sink<EquipmentChangeEvent>().connect<&InventoryService::OnEquipmentChangeEvent>(this);
    m_inventoryChangeConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&InventoryService::OnNotifyInventoryChanges>(this);
    m_equipmentChangeConnection = m_dispatcher.sink<NotifyEquipmentChanges>().connect<&InventoryService::OnNotifyEquipmentChanges>(this);
}

void InventoryService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    PerfScope perfScope("InventoryService::OnUpdate");

    RunWeaponStateUpdates();
    RunNakedNPCBugChecks();
    RunEquipmentSnapshotUpdates();
}

void InventoryService::OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.FormId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    uint32_t serverId = 0;
    if (acEvent.OwnershipEpoch != 0)
    {
        const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
        const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(*iter);
        const bool ownershipMatches = (pLocalComponent && pLocalComponent->Id == acEvent.ServerId && pLocalComponent->OwnershipEpoch == acEvent.OwnershipEpoch)
            || (pRemoteComponent && pRemoteComponent->Id == acEvent.ServerId && pRemoteComponent->OwnershipEpoch == acEvent.OwnershipEpoch);

        if (!ownershipMatches)
        {
            spdlog::debug("Discarded an inventory change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
            return;
        }

        serverId = acEvent.ServerId;
    }
    else
    {
        if (Cast<Actor>(TESForm::GetById(acEvent.FormId)))
            return;

        const std::optional<uint32_t> serverIdRes = Utils::GetServerId(*iter);
        if (!serverIdRes)
        {
            spdlog::warn(
                "Discarded inventory change for form {:X} because it has no server entity (item {:X}, count {})", acEvent.FormId, acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
            return;
        }
        serverId = *serverIdRes;
    }

    RequestInventoryChanges request;
    request.ServerId = serverId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;
    request.Item = acEvent.Item;
    request.Drop = acEvent.Drop;
    request.UpdateClients = acEvent.UpdateClients;

    m_transport.Send(request);

    spdlog::info("Sending item request, item: {:X}, count: {}, target object: {:X}, drop: {}", acEvent.Item.BaseId.BaseId, acEvent.Item.Count, acEvent.FormId, acEvent.Drop);
}

void InventoryService::OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.ActorId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
    if (acEvent.OwnershipEpoch == 0 || !pLocalComponent || pLocalComponent->Id != acEvent.ServerId || pLocalComponent->OwnershipEpoch != acEvent.OwnershipEpoch)
    {
        spdlog::debug("Discarded an equipment change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    RequestEquipmentChanges request;
    request.ServerId = acEvent.ServerId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;

    if (!modSystem.GetServerModId(acEvent.EquipSlotId, request.EquipSlotId))
        return;
    if (!modSystem.GetServerModId(acEvent.ItemId, request.ItemId))
        return;

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();

    m_transport.Send(request);

    spdlog::info("Sending equipment request, item: {:X}, count: {}, target object: {:X}", acEvent.ItemId, acEvent.Count, acEvent.ActorId);
}

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (acMessage.OwnershipEpoch != 0)
    {
        Actor* pActor = nullptr;

        auto remoteView = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
        const auto remoteIt = std::find_if(remoteView.begin(), remoteView.end(), [remoteView, &acMessage](const entt::entity aEntity)
        {
            const auto& remoteComponent = remoteView.get<RemoteComponent>(aEntity);
            return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
        });

        if (remoteIt != remoteView.end())
            pActor = Cast<Actor>(TESForm::GetById(remoteView.get<FormIdComponent>(*remoteIt).Id));
        else
        {
            auto localView = m_world.view<LocalComponent, FormIdComponent>();
            const auto localIt = std::find_if(localView.begin(), localView.end(), [localView, &acMessage](const entt::entity aEntity)
            {
                const auto& localComponent = localView.get<LocalComponent>(aEntity);
                return localComponent.Id == acMessage.ServerId && localComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
            });

            if (localIt != localView.end())
                pActor = Cast<Actor>(TESForm::GetById(localView.get<FormIdComponent>(*localIt).Id));
        }

        if (!pActor)
        {
            spdlog::debug("Discarded an inventory update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }

        ScopedInventoryOverride _;

        // Dropped items were reported invisible to the other player (2026-09-18). With the sender's "drop: true"
        // line this shows whether the drop arrived here at all.
        if (acMessage.Drop)
        {
            spdlog::info("Remote actor {:X} (server id {:X}) drops item {:X} x{}", pActor->formID, acMessage.ServerId, acMessage.Item.BaseId.BaseId, acMessage.Item.Count);
            pActor->DropOrPickUpObject(acMessage.Item, nullptr, nullptr);
        }
        else
            pActor->AddOrRemoveItem(acMessage.Item);

        return;
    }

    TESObjectREFR* pObject = Utils::GetByServerId<TESObjectREFR>(acMessage.ServerId);
    if (!pObject)
        return;

    ScopedInventoryOverride _;
    pObject->AddOrRemoveItem(acMessage.Item);
}

void InventoryService::OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
    const auto it = std::find_if(view.begin(), view.end(), [view, &acMessage](const entt::entity aEntity)
    {
        const auto& remoteComponent = view.get<RemoteComponent>(aEntity);
        return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
    });
    if (it == view.end())
    {
        spdlog::debug("Discarded an equipment update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pActor)
        return;

    // No item: an equipment snapshot (see RunEquipmentSnapshotUpdates).
    if (!acMessage.ItemId)
    {
        if (pActor->GetExtension()->IsRemote())
            ApplyHandEquipment(pActor, acMessage.CurrentInventory);
        return;
    }

    auto& modSystem = World::Get().GetModSystem();

    spdlog::info("Equipment sync: remote actor {:X} {} {:X}:{:X} (spell: {}, shout: {}, slot {:X}:{:X})", pActor->formID, acMessage.Unequip ? "unequips" : "equips", acMessage.ItemId.ModId, acMessage.ItemId.BaseId, acMessage.IsSpell, acMessage.IsShout, acMessage.EquipSlotId.ModId, acMessage.EquipSlotId.BaseId);

    uint32_t itemId = modSystem.GetGameId(acMessage.ItemId);
    TESForm* pItem = TESForm::GetById(itemId);

    if (!pItem)
    {
        spdlog::error("Could not find inventory item {:X}:{:X}", acMessage.ItemId.ModId, acMessage.ItemId.BaseId);
        return;
    }

    uint32_t equipSlotId = modSystem.GetGameId(acMessage.EquipSlotId);
    TESForm* pEquipSlot = TESForm::GetById(equipSlotId);

    uint32_t slotId = 0;
    if (pEquipSlot == DefaultObjectManager::Get().rightEquipSlot)
        slotId = 1;

    auto* pEquipManager = EquipManager::Get();

    if (acMessage.IsSpell)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipSpell(pActor, pItem, slotId);
        else
            pEquipManager->EquipSpell(pActor, pItem, slotId);

        return;
    }
    else if (acMessage.IsShout)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipShout(pActor, pItem);
        else
            pEquipManager->EquipShout(pActor, pItem);

        return;
    }

    // TODO: ExtraData necessary? probably
    if (acMessage.Unequip)
    {
        pEquipManager->UnEquip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
        NoteHandItem(pActor, pItem, pEquipSlot == DefaultObjectManager::Get().leftEquipSlot, false);
    }
    else
    {
        // Unequip all armor first, since the game won't auto unequip armor
        Inventory wornArmor{};
        if (pItem->formType == FormType::Armor)
        {
            wornArmor = pActor->GetWornArmor();
            for (const auto& armor : wornArmor.Entries)
            {
                uint32_t armorId = modSystem.GetGameId(armor.BaseId);
                TESForm* pArmor = TESForm::GetById(armorId);
                if (pArmor)
                    pEquipManager->UnEquip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false, nullptr);
            }
        }

        pEquipManager->Equip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false);
        NoteHandItem(pActor, pItem, pEquipSlot == DefaultObjectManager::Get().leftEquipSlot, true);

        for (const auto& armor : wornArmor.Entries)
        {
            uint32_t armorId = modSystem.GetGameId(armor.BaseId);
            TESForm* pArmor = TESForm::GetById(armorId);
            if (pArmor)
                pEquipManager->Equip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false);
        }
    }
}

void InventoryService::RunWeaponStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        auto& localComponent = view.get<LocalComponent>(entity);

        bool isWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        if (isWeaponDrawn != localComponent.IsWeaponDrawn)
        {
            localComponent.IsWeaponDrawn = isWeaponDrawn;

            DrawWeaponRequest request;
            request.Id = localComponent.Id;
            request.IsWeaponDrawn = isWeaponDrawn;

            m_transport.Send(request);
        }
    }
}

void InventoryService::RunNakedNPCBugChecks() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        if (pActor->GetExtension()->IsPlayer())
            continue;

        if (pActor->IsDead())
            continue;

        if (pActor->IsWearingBodyPiece())
            continue;

        if (!pActor->ShouldWearBodyPiece())
            continue;

        // Don't broadcast changes, it'll just make things messier.
        // If all clients have this problem, they'll all fix it individually.
        ScopedEquipOverride seo;
        ScopedInventoryOverride sio;

        pActor->ResetInventory(false);
    }
}

void InventoryService::RunEquipmentSnapshotUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });
    if (it == view.end())
        return;

    const uint32_t serverId = view.get<LocalComponent>(*it).Id;

    Inventory equipment;
    {
        // Measured: reads the whole inventory to keep the worn entries. The perf line reports it per 30 s.
        PerfCounterScope perfScope(PerfCounter::kEquipmentSnapshot);
        equipment = pPlayer->GetEquipment();
    }

    // Compare only what is worn where: charges and similar extra data change constantly in combat.
    const auto isSameEquipment = [](const Inventory& acLhs, const Inventory& acRhs)
    {
        if (acLhs.Entries.size() != acRhs.Entries.size() || !(acLhs.CurrentMagicEquipment == acRhs.CurrentMagicEquipment))
            return false;

        for (size_t i = 0; i < acLhs.Entries.size(); ++i)
        {
            const auto& lhs = acLhs.Entries[i];
            const auto& rhs = acRhs.Entries[i];
            if (lhs.BaseId != rhs.BaseId || lhs.ExtraWorn != rhs.ExtraWorn || lhs.ExtraWornLeft != rhs.ExtraWornLeft)
                return false;
        }

        return true;
    };

    if (serverId == m_lastSnapshotServerId && isSameEquipment(equipment, m_lastEquipmentSnapshot))
        return;

    RequestEquipmentChanges request;
    request.ServerId = serverId;
    request.CurrentInventory = equipment;

    m_transport.Send(request);

    spdlog::info("Equipment snapshot sent: {} worn entries, left spell {:X}, right spell {:X}", equipment.Entries.size(), equipment.CurrentMagicEquipment.LeftHandSpell.BaseId, equipment.CurrentMagicEquipment.RightHandSpell.BaseId);

    m_lastSnapshotServerId = serverId;
    m_lastEquipmentSnapshot = std::move(equipment);
}

namespace
{
struct HandItem
{
    TESForm* pForm;
    bool Left;
};

bool ContainsHandItem(const Vector<HandItem>& acItems, TESForm* apForm, bool aLeft) noexcept
{
    return std::any_of(acItems.begin(), acItems.end(), [apForm, aLeft](const HandItem& aItem) { return aItem.pForm == apForm && aItem.Left == aLeft; });
}

// What this side has put in each remote player copy's hands since it appeared, by form id. The container's worn
// flags cannot stand in for it: SetInventory equips the worn entries before the copy has its 3D, so the flags say
// "held" while the hands stay empty. Every spawn of 2026-09-20 logged "1 hand items wanted, 1 held now" and the
// weapon only showed once the owner re-equipped it ("we always spawn with empty hands").
TiltedPhoques::Map<uint32_t, Vector<HandItem>> s_handsApplied;

void NoteHandItem(Actor* apActor, TESForm* apItem, bool aLeft, bool aEquipped) noexcept
{
    constexpr uint8_t cLightFormType = 31;
    if (!apActor || !apItem || (apItem->formType != FormType::Weapon && static_cast<uint8_t>(apItem->formType) != cLightFormType))
        return;
    Vector<HandItem>& held = s_handsApplied[apActor->formID];
    held.erase(std::remove_if(held.begin(), held.end(), [apItem, aLeft](const HandItem& aItem) { return aItem.pForm == apItem && aItem.Left == aLeft; }), held.end());
    if (aEquipped)
        held.push_back({apItem, aLeft});
}
} // namespace

void InventoryService::ApplyHandEquipment(Actor* apActor, const Inventory& acEquipment, bool aArrival) noexcept
{
    constexpr uint8_t cLightFormType = 31;

    auto& modSystem = World::Get().GetModSystem();
    auto* pEquipManager = EquipManager::Get();
    auto& defaultObjects = DefaultObjectManager::Get();

    const auto collectHandItems = [&modSystem](const Inventory& acInventory)
    {
        Vector<HandItem> items;
        for (const auto& entry : acInventory.Entries)
        {
            if (!entry.IsWorn())
                continue;

            TESForm* pForm = TESForm::GetById(modSystem.GetGameId(entry.BaseId));
            if (!pForm || (pForm->formType != FormType::Weapon && static_cast<uint8_t>(pForm->formType) != cLightFormType))
                continue;

            if (entry.ExtraWorn)
                items.push_back({pForm, false});
            if (entry.ExtraWornLeft)
                items.push_back({pForm, true});
        }
        return items;
    };

    const auto containsItem = [](const Vector<HandItem>& acItems, const HandItem& acItem)
    { return std::any_of(acItems.begin(), acItems.end(), [&acItem](const HandItem& aItem) { return aItem.pForm == acItem.pForm && aItem.Left == acItem.Left; }); };

    const Vector<HandItem> desiredItems = collectHandItems(acEquipment);
    Vector<HandItem>& heldItems = s_handsApplied[apActor->formID];
    if (aArrival)
        heldItems.clear(); // a fresh copy: SetInventory set the worn flags, the hands are empty
    const Vector<HandItem> flaggedItems = collectHandItems(apActor->GetEquipment());
    spdlog::info("Equipment sync: remote actor {:X} hands {}: {} hand items wanted ({} worn entries in all), {} put there by us, {} flagged worn, left spell {}, right spell {}", apActor->formID,
                 aArrival ? "on arrival" : "on snapshot", desiredItems.size(), acEquipment.Entries.size(), heldItems.size(), flaggedItems.size(),
                 acEquipment.CurrentMagicEquipment.LeftHandSpell ? "yes" : "no", acEquipment.CurrentMagicEquipment.RightHandSpell ? "yes" : "no");

    // Off: what we put there and what the container flags as worn, when the owner no longer holds it.
    Vector<HandItem> toUnequip = heldItems;
    for (const auto& item : flaggedItems)
        if (!ContainsHandItem(toUnequip, item.pForm, item.Left))
            toUnequip.push_back(item);
    for (const auto& item : toUnequip)
    {
        if (containsItem(desiredItems, item))
            continue;
        spdlog::info("Equipment sync: remote actor {:X} unequips {:X} ({} hand)", apActor->formID, item.pForm->formID, item.Left ? "left" : "right");
        pEquipManager->UnEquip(apActor, item.pForm, nullptr, 1, item.Left ? defaultObjects.leftEquipSlot : defaultObjects.rightEquipSlot, false, true, false, false, nullptr);
        NoteHandItem(apActor, item.pForm, item.Left, false);
    }

    // On: what the owner holds that we have not put there ourselves. A worn flag alone does not count.
    for (const auto& item : desiredItems)
    {
        if (ContainsHandItem(heldItems, item.pForm, item.Left))
            continue;
        auto* pObject = Cast<TESBoundObject>(item.pForm);
        if (!pObject)
            continue;
        // The remote copy may not have the item yet (picked up after it was spawned).
        if (apActor->GetItemCountInInventory(item.pForm) <= 0)
        {
            ScopedInventoryOverride _;
            apActor->AddObjectToContainer(pObject, nullptr, 1, nullptr);
        }
        spdlog::info("Equipment sync: remote actor {:X} equips {:X} ({} hand)", apActor->formID, item.pForm->formID, item.Left ? "left" : "right");
        pEquipManager->Equip(apActor, item.pForm, nullptr, 1, item.Left ? defaultObjects.leftEquipSlot : defaultObjects.rightEquipSlot, false, true, false, false);
        NoteHandItem(apActor, item.pForm, item.Left, true);
    }

    const auto syncSpell = [&](const GameId& acDesiredSpell, uint32_t aHand)
    {
        TESForm* pDesired = acDesiredSpell ? TESForm::GetById(modSystem.GetGameId(acDesiredSpell)) : nullptr;
        TESForm* pCurrent = apActor->magicItems[aHand];
        if (pDesired == pCurrent)
            return;

        spdlog::info("Equipment sync: remote actor {:X} {} hand spell {:X} -> {:X}", apActor->formID, aHand == 0 ? "left" : "right", pCurrent ? pCurrent->formID : 0, pDesired ? pDesired->formID : 0);

        if (pDesired)
            pEquipManager->EquipSpell(apActor, pDesired, aHand);
        else
            pEquipManager->UnEquipSpell(apActor, pCurrent, aHand);
    };

    syncSpell(acEquipment.CurrentMagicEquipment.LeftHandSpell, 0);
    syncSpell(acEquipment.CurrentMagicEquipment.RightHandSpell, 1);
}
