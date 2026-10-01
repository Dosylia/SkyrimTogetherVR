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
#include <Messages/RequestDroppedItemMove.h>
#include <Messages/NotifyDroppedItemMove.h>
#include <Events/UpdateEvent.h>
#include <NetImmerse/NiTransform.h>

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

//! Whether the reference behind an id is still the item that was mapped. A mapping made before the base was known
//! (0) is trusted; anything else must match.
template <class T> bool IsStillOurs(const TESObjectREFR* apRef, const T& acMotion) noexcept
{
    return acMotion.BaseFormId == 0 || (apRef->baseForm && apRef->baseForm->formID == acMotion.BaseFormId);
}

//! Movement worth sending: a couple of units, or a few degrees of turn. Below that it is physics jitter.
constexpr float kMovedDistance = 2.f;
constexpr float kMovedAngle = 0.05f;

//! Where an item really is. A held or falling item's 3D moves under physics, and the node's world transform follows it
//! every frame, where the reference's own position may lag. NiAVObject::world is at 0x7C, as VRBodySync reads it.
glm::vec3 WorldPositionOf(TESObjectREFR* apRef) noexcept
{
    if (NiNode* pNode = apRef->GetNiNode())
    {
        const auto& world = *reinterpret_cast<const NiTransform*>(reinterpret_cast<const uint8_t*>(pNode) + 0x7C);
        return {world.translate.x, world.translate.y, world.translate.z};
    }
    return {apRef->position.x, apRef->position.y, apRef->position.z};
}

glm::vec3 RotationOf(const TESObjectREFR* apRef) noexcept
{
    return {apRef->rotation.x, apRef->rotation.y, apRef->rotation.z};
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
    m_moveConnection = aDispatcher.sink<NotifyDroppedItemMove>().connect<&DroppedItemService::OnNotifyDroppedItemMove>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&DroppedItemService::OnUpdate>(this);
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
    m_reportedOldDrops = false;
    m_motion.clear();
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

    // Just dropped or just placed, it is still falling: each side lets its own item settle, and only movement after
    // that -- somebody picking it up in a hand, kicking it -- is sent.
    Motion& motion = m_motion[aId];
    motion = Motion{};
    if (TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(aRefFormId)))
    {
        motion.LastPosition = WorldPositionOf(pRef);
        motion.LastRotation = RotationOf(pRef);
        motion.BaseFormId = pRef->baseForm ? pRef->baseForm->formID : 0;
        motion.Loaded = pRef->GetNiNode() != nullptr;
    }
    motion.QuietUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
}

bool DroppedItemService::SendAdd(TESObjectREFR* apRef, const Inventory::Entry& acItem, const bool aAnnouncement) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    RequestDroppedItemAdd request{};
    request.Item = acItem;
    request.Announcement = aAnnouncement;
    request.Position = glm::vec3(apRef->position.x, apRef->position.y, apRef->position.z);
    request.Rotation = RotationOf(apRef);

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
    m_motion.erase(it->second);
    m_refById.erase(it->second);
    m_idByRef.erase(it);
}

void DroppedItemService::OnNotifyDroppedItem(const NotifyDroppedItem& acMessage) noexcept
{
    if (const auto mapped = m_refById.find(acMessage.Id); mapped != m_refById.end())
    {
        // Already here; the server sends again on every cell change. Moves are only relayed to players in range, so
        // one made while this player was elsewhere arrives here, as the place the item was left.
        TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(mapped->second));
        Motion& motion = m_motion[acMessage.Id];
        const auto now = std::chrono::steady_clock::now();
        const glm::vec3 cLeftAt(acMessage.Position);
        if (pRef && IsStillOurs(pRef, motion) && !pRef->IsDeleted() && !motion.Held && !motion.Moving && now - motion.LastLocalMove > std::chrono::seconds(2) &&
            DistanceTo(pRef, cLeftAt) > 20.f)
        {
            spdlog::info("DroppedItem: {} ({:X}) was moved while we were away; putting it where it was left, {:.0f} units off", acMessage.Id, mapped->second,
                         DistanceTo(pRef, cLeftAt));
            pRef->SetRotation(acMessage.Rotation.x, acMessage.Rotation.y, acMessage.Rotation.z);
            if (TESObjectCELL* pCell = pRef->GetParentCellEx())
                pRef->MoveTo(pCell, NiPoint3(cLeftAt));
            motion.LastPosition = cLeftAt;
            motion.LastRotation = acMessage.Rotation;
            motion.QuietUntil = now + std::chrono::seconds(2);
        }
        return;
    }

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
        NiPoint3 rotation(acMessage.Rotation);
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
    m_motion.erase(acMessage.Id);

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
    bool listFound = false;
    for (BSExtraData* pExtra = pPlayer->extraData.data; pExtra && guard < 512; pExtra = pExtra->next, ++guard)
    {
        if (!Readable(pExtra, sizeof(BSExtraData)) || !Cast<ExtraDroppedItemList>(pExtra))
            continue;
        listFound = true;

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

    // Said once per connection whatever the outcome. The first session with this code logged nothing at all, and
    // "no list on the player", "an empty list" and "never ran" all looked the same: silence.
    if (found || sent || !m_reportedOldDrops)
    {
        m_reportedOldDrops = true;
        if (!listFound)
            spdlog::info("DroppedItem: no dropped-item list on the player among {} extra data entries, so there are no earlier drops to announce", guard);
        else
            spdlog::info("DroppedItem: the player's dropped-item list holds {} item(s); announced {} not yet shared this session", found, sent);
    }
}

void DroppedItemService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_refById.empty() || !m_transport.IsConnected())
        return;

    // Ten times a second is enough to follow a hand, and the list is a handful of items.
    static std::chrono::steady_clock::time_point s_next{};
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + std::chrono::milliseconds(100);

    for (const auto& [id, refFormId] : m_refById)
    {
        Motion& motion = m_motion[id];
        TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(refFormId));
        if (!pRef || !IsStillOurs(pRef, motion) || pRef->IsDeleted() || pRef->IsDisabled() || !pRef->GetNiNode())
        {
            motion.Loaded = false;
            continue;
        }

        const glm::vec3 position = WorldPositionOf(pRef);
        const glm::vec3 rotation = RotationOf(pRef);

        // Its cell has just loaded: everything in it settles for a moment, on both sides at once. Treated like a
        // fresh drop, so neither side sends the other its own settling.
        if (!motion.Loaded)
        {
            motion.Loaded = true;
            motion.QuietUntil = std::max(motion.QuietUntil, now + std::chrono::seconds(2));
        }

        // Held on the other side's word and nothing heard for two seconds: put down over there, or the carrier went
        // quiet (a lost packet, a disconnect). Either way, back to physics here.
        if (motion.Held && now - motion.LastRemote > std::chrono::seconds(2))
        {
            pRef->SetMotionType(TESObjectREFR::kMotionDynamic);
            motion.Held = false;
            motion.QuietUntil = now + std::chrono::seconds(2);
            if (motion.RemoteAtRest)
                spdlog::info("DroppedItem: {} was put down over there; handed back to physics here", id);
            else
                spdlog::info("DroppedItem: {} stopped being moved over there without coming to rest; released it here", id);
        }

        if (motion.Held || now < motion.QuietUntil)
        {
            motion.LastPosition = position;
            motion.LastRotation = rotation;
            continue;
        }

        const glm::vec3 turned = glm::abs(rotation - motion.LastRotation);
        const bool cMoved = glm::distance(position, motion.LastPosition) > kMovedDistance || std::max({turned.x, turned.y, turned.z}) > kMovedAngle;

        const auto send = [&](const bool aAtRest)
        {
            RequestDroppedItemMove request{};
            request.Id = id;
            request.Position = position;
            request.Rotation = rotation;
            request.AtRest = aAtRest;
            m_transport.Send(request);
        };

        if (cMoved)
        {
            if (!motion.Moving)
                spdlog::info("DroppedItem: {} ({:X}) is being moved here; sending where it is", id, refFormId);
            motion.LastPosition = position;
            motion.LastRotation = rotation;
            motion.LastLocalMove = now;
            motion.Moving = true;
            send(false);
        }
        else if (motion.Moving && now - motion.LastLocalMove >= std::chrono::milliseconds(600))
        {
            // Still for long enough to call it put down. The server keeps this place for anyone who comes later.
            motion.Moving = false;
            send(true);
            spdlog::info("DroppedItem: {} came to rest at ({:.0f}, {:.0f}, {:.0f})", id, position.x, position.y, position.z);
        }
    }
}

void DroppedItemService::OnNotifyDroppedItemMove(const NotifyDroppedItemMove& acMessage) noexcept
{
    const auto it = m_refById.find(acMessage.Id);
    if (it == m_refById.end())
        return; // not placed here yet; the server hands over its resting place with the item

    TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(it->second));
    Motion& motion = m_motion[acMessage.Id];
    if (!pRef || !IsStillOurs(pRef, motion) || pRef->IsDeleted())
        return;

    const auto now = std::chrono::steady_clock::now();

    // Held still while the other side moves it: otherwise this side's physics pulls it down between updates and it
    // stutters between their hand and the floor. Also for a lone "put down": it is placed exactly, then let go.
    if (!motion.Held)
    {
        pRef->SetMotionType(TESObjectREFR::kMotionKeyframed);
        motion.Held = true;
        spdlog::info("DroppedItem: {} ({:X}) is being moved over there; holding it here and following", acMessage.Id, it->second);
    }

    const glm::vec3 position(acMessage.Position);
    const NiNode* pNodeBefore = pRef->GetNiNode();
    pRef->SetRotation(acMessage.Rotation.x, acMessage.Rotation.y, acMessage.Rotation.z);
    if (TESObjectCELL* pCell = pRef->GetParentCellEx())
        pRef->MoveTo(pCell, NiPoint3(position));

    // Not known without a headset: whether a move rebuilds the item's 3D. If it does, the new physics body is dynamic
    // again and would fall between updates, so it is held again -- and the log says so once, which answers it.
    if (const NiNode* pNodeAfter = pRef->GetNiNode(); pNodeBefore && pNodeAfter && pNodeAfter != pNodeBefore)
    {
        pRef->SetMotionType(TESObjectREFR::kMotionKeyframed);
        static bool s_reported = false;
        if (!s_reported)
        {
            s_reported = true;
            spdlog::info("DroppedItem: moving {} rebuilt its 3D; holding it again after each move", acMessage.Id);
        }
    }

    motion.LastRemote = now;
    motion.LastPosition = position;
    motion.LastRotation = acMessage.Rotation;
    motion.Moving = false;
    // Let go two seconds after the last word (OnUpdate), not now: "at rest" is also what a hand kept still sends, and
    // a further move within those two seconds just carries on.
    motion.RemoteAtRest = acMessage.AtRest;
}
