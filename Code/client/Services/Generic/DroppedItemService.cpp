#include <Services/DroppedItemService.h>

#include <World.h>
#include <Services/TransportService.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/DroppedItemEvents.h>

#include <Messages/RequestDroppedItemAdd.h>
#include <Messages/RequestDroppedItemRemove.h>
#include <Messages/NotifyDroppedItem.h>
#include <Messages/NotifyDroppedItemRemoved.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <PlayerCharacter.h>
#include <ExtraData/ExtraCount.h>

#include <Games/Overrides.h>

namespace
{
//! How close an item on the floor has to be to where the server says one lies to be taken for it. The server's
//! position is the moment of the drop; the one here has since fallen, rolled, or been saved and loaded.
constexpr float kSameItemRadius = 200.f;

//! Outdoors, how far from the player an item can be and still be in a loaded cell (the 5 by 5 grid is two cells
//! either side of the player's, and a cell is 4096 units).
constexpr float kLoadedRadius = 8192.f;

const Vector<FormType> kItemTypes{FormType::Weapon, FormType::Armor, FormType::Ammo, FormType::Book, FormType::Ingredient, FormType::Alchemy,
                                  FormType::Misc, FormType::Key, FormType::SoulGem, FormType::Scroll, FormType::Light};

bool IsItem(const TESObjectREFR* apRef) noexcept
{
    if (!apRef || !apRef->baseForm)
        return false;
    for (const FormType type : kItemTypes)
        if (apRef->baseForm->formType == type)
            return true;
    return false;
}

float DistanceTo(const TESObjectREFR* apRef, const glm::vec3& acPosition) noexcept
{
    return glm::distance(glm::vec3(apRef->position.x, apRef->position.y, apRef->position.z), acPosition);
}

bool Readable(const void* apAddress, const size_t aBytes) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!apAddress || !VirtualQuery(apAddress, &info, sizeof(info)))
        return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return reinterpret_cast<uintptr_t>(apAddress) + aBytes <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}

// The game's ExtraDroppedItemList, as CommonLibSSE has it: the BSExtraData header, then a BSSimpleList of reference
// handles -- the handle, padding, and the next node. Only ever read after RTTI has confirmed the type, and every node
// is checked readable, so a wrong guess about this layout reads nothing rather than crashing.
struct DroppedItemNode
{
    uint32_t Handle;
    uint32_t Pad;
    DroppedItemNode* Next;
};
struct DroppedItemListView
{
    void* VTable;
    void* NextExtra;
    DroppedItemNode Head;
};
} // namespace

DroppedItemService::DroppedItemService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&DroppedItemService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&DroppedItemService::OnDisconnected>(this);
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&DroppedItemService::OnCellChange>(this);
    m_droppedConnection = aDispatcher.sink<ItemDroppedEvent>().connect<&DroppedItemService::OnItemDropped>(this);
    m_pickedUpConnection = aDispatcher.sink<ItemPickedUpEvent>().connect<&DroppedItemService::OnItemPickedUp>(this);
    m_notifyConnection = aDispatcher.sink<NotifyDroppedItem>().connect<&DroppedItemService::OnNotifyDroppedItem>(this);
    m_removedConnection = aDispatcher.sink<NotifyDroppedItemRemoved>().connect<&DroppedItemService::OnNotifyDroppedItemRemoved>(this);
}

void DroppedItemService::OnConnected(const ConnectedEvent&) noexcept
{
    AnnounceOldDrops();
}

void DroppedItemService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // Ids belong to one server session's view; a reconnect gets them again with the next cell entry.
    m_refById.clear();
    m_idByRef.clear();
    m_pending.clear();
    m_announced.clear();
}

void DroppedItemService::OnCellChange(const CellChangeEvent&) noexcept
{
    if (m_transport.IsConnected())
        AnnounceOldDrops();
}

void DroppedItemService::Map(const uint32_t aId, const uint32_t aRefFormId) noexcept
{
    m_refById[aId] = aRefFormId;
    m_idByRef[aRefFormId] = aId;
}

bool DroppedItemService::SendAdd(TESObjectREFR* apRef, const Inventory::Entry& acItem, const bool aAnnouncement) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    RequestDroppedItemAdd request{};
    request.Item = acItem;
    request.Announcement = aAnnouncement;
    request.Position = glm::vec3(apRef->position.x, apRef->position.y, apRef->position.z);

    TESObjectCELL* pCell = apRef->GetParentCellEx();
    if (!pCell || !modSystem.GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::warn("DroppedItem: cannot name the cell item {:X} lies in, so it is not shared", apRef->formID);
        return false;
    }
    if (TESWorldSpace* pWorldSpace = apRef->GetWorldSpace())
        modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId);

    m_transport.Send(request);
    m_pending.push_back(Pending{apRef->formID, acItem.BaseId});
    return true;
}

void DroppedItemService::OnItemDropped(const ItemDroppedEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(acEvent.RefFormId));
    if (!pRef || acEvent.Item.BaseId == GameId{})
        return;

    if (SendAdd(pRef, acEvent.Item, false))
        spdlog::info("DroppedItem: dropped {:X}:{:X} x{} as {:X}, shared with the server", acEvent.Item.BaseId.ModId, acEvent.Item.BaseId.BaseId,
                     std::abs(acEvent.Item.Count), pRef->formID);
}

void DroppedItemService::OnItemPickedUp(const ItemPickedUpEvent& acEvent) noexcept
{
    const auto it = m_idByRef.find(acEvent.RefFormId);
    if (it == m_idByRef.end())
        return; // not an item the server was keeping

    RequestDroppedItemRemove request{};
    request.Id = it->second;
    m_transport.Send(request);

    spdlog::info("DroppedItem: picked up {} ({:X}); asking everyone to take it off the floor", it->second, acEvent.RefFormId);
    m_refById.erase(it->second);
    m_idByRef.erase(it);
}

void DroppedItemService::OnNotifyDroppedItem(const NotifyDroppedItem& acMessage) noexcept
{
    if (m_refById.count(acMessage.Id))
        return; // already here; the server sends again on every cell change

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    auto& modSystem = m_world.GetModSystem();
    const glm::vec3 cPosition(acMessage.Position);

    // Only an item in a loaded cell can be adopted or placed. One elsewhere is sent again when the player gets there.
    if (!acMessage.WorldSpaceId)
    {
        TESObjectCELL* pCell = pPlayer->GetParentCellEx();
        GameId cellId{};
        if (!pCell || !modSystem.GetServerModId(pCell->formID, cellId) || cellId != acMessage.CellId)
            return;
    }
    else
    {
        TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace();
        GameId worldSpaceId{};
        if (!pWorldSpace || !modSystem.GetServerModId(pWorldSpace->formID, worldSpaceId) || worldSpaceId != acMessage.WorldSpaceId ||
            DistanceTo(pPlayer, cPosition) > kLoadedRadius)
            return;
    }

    const uint32_t cBaseFormId = modSystem.GetGameId(acMessage.Item.BaseId);

    // 1. This player's own drop or announcement, coming back with its id. First in, first answered.
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    {
        if (it->BaseId != acMessage.Item.BaseId)
            continue;
        const uint32_t cRefFormId = it->RefFormId;
        m_pending.erase(it);
        Map(acMessage.Id, cRefFormId);
        spdlog::info("DroppedItem: {} is our own {:X}", acMessage.Id, cRefFormId);
        return;
    }

    // 2. Already lying here: kept by this save from an earlier session, or placed before a reconnect.
    TESObjectREFR* pFound = nullptr;
    float nearest = kSameItemRadius;
    const auto scan = [&](const TESObjectCELL* apCell)
    {
        if (!apCell)
            return;
        for (TESObjectREFR* pRef : apCell->GetRefsByFormTypes(kItemTypes))
        {
            if (!pRef || !pRef->baseForm || pRef->baseForm->formID != cBaseFormId || pRef->IsDisabled() || pRef->IsDeleted() || m_idByRef.count(pRef->formID))
                continue;
            const float distance = DistanceTo(pRef, cPosition);
            if (distance <= nearest)
            {
                nearest = distance;
                pFound = pRef;
            }
        }
    };
    scan(Cast<TESObjectCELL>(TESForm::GetById(modSystem.GetGameId(acMessage.CellId))));
    scan(pPlayer->GetParentCellEx());

    if (pFound)
    {
        Map(acMessage.Id, pFound->formID);
        spdlog::info("DroppedItem: {} was already lying here as {:X}, {:.0f} units from where it was dropped", acMessage.Id, pFound->formID, nearest);
        return;
    }

    // 3. Missing: place it. The game only drops an item it holds, so it is given to the player and dropped straight
    // back out at the item's position, both under the inventory override so neither step is sent as a real change --
    // and so the drop hook does not announce it as this player's own drop.
    TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(cBaseFormId));
    if (!pObject)
    {
        spdlog::warn("DroppedItem: {} is {:X}:{:X}, which this game does not have", acMessage.Id, acMessage.Item.BaseId.ModId, acMessage.Item.BaseId.BaseId);
        return;
    }

    Inventory::Entry entry = acMessage.Item;
    const int32_t cCount = std::max(1, std::abs(entry.Count));
    entry.Count = cCount;

    BSPointerHandle<TESObjectREFR> handle{};
    {
        ScopedInventoryOverride _;
        ExtraDataList* pExtra = TESObjectREFR::GetExtraDataFromItem(entry);
        NiPoint3 location(cPosition);
        NiPoint3 rotation{};
        pPlayer->AddObjectToContainer(pObject, pExtra, cCount, nullptr);
        handle = pPlayer->RemoveItem(pObject, cCount, ITEM_REMOVE_REASON::kDropping, pExtra, nullptr, &location, &rotation);
    }

    if (TESObjectREFR* pPlaced = TESObjectREFR::GetByHandle(handle.handle.iBits))
    {
        Map(acMessage.Id, pPlaced->formID);
        spdlog::info("DroppedItem: placed {} ({:X} x{}) as {:X}", acMessage.Id, pObject->formID, cCount, pPlaced->formID);
    }
    else
        spdlog::warn("DroppedItem: placing {} ({:X}) produced no reference", acMessage.Id, pObject->formID);
}

void DroppedItemService::OnNotifyDroppedItemRemoved(const NotifyDroppedItemRemoved& acMessage) noexcept
{
    const auto it = m_refById.find(acMessage.Id);
    if (it == m_refById.end())
        return; // not here, or picked up here

    const uint32_t cRefFormId = it->second;
    m_idByRef.erase(cRefFormId);
    m_refById.erase(it);

    // Disabled, then marked for deletion through the game's own Papyrus Delete: the game frees it in its own time.
    // Freeing a reference underneath something still holding it is the crash class of 2026-09-27 and -30.
    TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(cRefFormId));
    if (pRef && !pRef->IsDeleted())
    {
        pRef->Disable();
        pRef->Delete();
        spdlog::info("DroppedItem: {} was picked up elsewhere; removed {:X} here", acMessage.Id, cRefFormId);
    }
}

void DroppedItemService::AnnounceOldDrops() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    // Both types have to have resolved on this build: DynamicCast with a null type descriptor is not safe to call.
    if (!internal::RttiLocator<BSExtraData>::Get() || !internal::RttiLocator<ExtraDroppedItemList>::Get())
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            spdlog::warn("DroppedItem: the dropped-item list type did not resolve on this build; earlier drops are not announced");
        }
        return;
    }

    auto& modSystem = m_world.GetModSystem();
    uint32_t found = 0;
    uint32_t sent = 0;

    uint32_t guard = 0;
    for (BSExtraData* pExtra = pPlayer->extraData.data; pExtra && guard < 512; pExtra = pExtra->next, ++guard)
    {
        if (!Readable(pExtra, sizeof(BSExtraData)) || !Cast<ExtraDroppedItemList>(pExtra))
            continue;

        const auto* pList = reinterpret_cast<const DroppedItemListView*>(pExtra);
        const DroppedItemNode* pNode = &pList->Head;
        for (uint32_t n = 0; pNode && n < 1024 && Readable(pNode, sizeof(DroppedItemNode)); ++n, pNode = pNode->Next)
        {
            if (!pNode->Handle)
                continue;

            TESObjectREFR* pRef = TESObjectREFR::GetByHandle(pNode->Handle);
            if (!IsItem(pRef) || pRef->IsDisabled() || pRef->IsDeleted())
                continue;
            ++found;

            if (m_idByRef.count(pRef->formID) || m_announced.count(pRef->formID))
                continue;
            m_announced.insert(pRef->formID);

            Inventory::Entry item{};
            if (!modSystem.GetServerModId(pRef->baseForm->formID, item.BaseId))
                continue;
            item.Count = 1;
            if (auto* pCount = static_cast<ExtraCount*>(pRef->extraData.GetByType(ExtraDataType::Count)))
                item.Count = std::max<int32_t>(1, pCount->count);
            TESObjectREFR::GetItemFromExtraData(item, &pRef->extraData);

            if (SendAdd(pRef, item, true))
                ++sent;
        }
        break; // one list per actor
    }

    if (found || sent)
        spdlog::info("DroppedItem: the player's dropped-item list holds {} item(s); announced {} not yet shared this session", found, sent);
}
