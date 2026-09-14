#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/VRBodySync.h>

#include <Actor.h>
#include <PlayerCharacter.h>
#include <NetImmerse/NiNode.h>
#include <NetImmerse/NiTransform.h>

#include <glm/gtc/quaternion.hpp>

#include <PerfScope.h>

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

// NetImmerse objects are read through raw offsets: the VR NiAVObject is 0x138 bytes, while the client's structs only
// model the SE size (0x110).
namespace
{
constexpr uint32_t kNameOffset = 0x10;   // NiObjectNET::name
constexpr uint32_t kParentOffset = 0x30; // NiAVObject::parent
constexpr uint32_t kLocalOffset = 0x48;  // NiAVObject::local
constexpr uint32_t kWorldOffset = 0x7C;  // NiAVObject::world
#ifdef SKYRIMVR
constexpr uint32_t kChildrenOffset = 0x138; // NiNode::children
constexpr uint32_t kNiAVObjectSize = 0x138;
#else
constexpr uint32_t kChildrenOffset = 0x110;
constexpr uint32_t kNiAVObjectSize = 0x110;
#endif
constexpr uint32_t kVTableGetRttiSlot = 2; // NiObject::GetRTTI
constexpr uint32_t kVTableAsNodeSlot = 3;  // NiObject::AsNode

// BSFlattenedBoneTree, measured on SkyrimVR 1.4.15 by TiltedEvolutionVR. The skeleton keeps a flat array of every bone,
// and bones that were flattened away (fingers, facial bones) only exist there.
constexpr uint32_t kTreeBoneArray = 0x158;
constexpr uint32_t kBoneEntrySize = 0x80;
constexpr uint32_t kBoneEntryLocal = 0x00;
constexpr uint32_t kBoneEntryWorld = 0x34;
constexpr uint32_t kBoneEntryIndices = 0x68; // four int16, one of them the parent index
constexpr uint32_t kBoneEntryNode = 0x70;    // the bone's node, null when it was flattened away
constexpr uint32_t kMaxBones = 1024;

// PlayerCharacter's headset node (same measurement), checked by name before use.
constexpr uint32_t kHmdNodeOffset = 0x570;

constexpr std::array<const char*, VRPose::kBoneCount> kBoneNames{
    "NPC Spine1 [Spn1]",   "NPC Spine2 [Spn2]",   "NPC Neck [Neck]",       "NPC Head [Head]",
    "NPC L Clavicle [LClv]", "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]",
    "NPC R Clavicle [RClv]", "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]",
};

// Parent bone index for each entry of kBoneNames (-1: searched under "NPC Root [Root]").
constexpr std::array<int8_t, VRPose::kBoneCount> kBoneParents{-1, 0, 1, 2, 1, 4, 5, 6, 1, 8, 9, 10};

using BoneNodes = std::array<void*, VRPose::kBoneCount>;

template <class T> T& At(void* apObject, uint32_t aOffset) noexcept
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(apObject) + aOffset);
}

const char* GetName(void* apObject) noexcept
{
    return At<const char*>(apObject, kNameOffset);
}

void* AsNode(void* apObject) noexcept
{
    using TAsNode = void*(__fastcall*)(void*);
    auto** ppVTable = *static_cast<void***>(apObject);
    return static_cast<TAsNode>(ppVTable[kVTableAsNodeSlot])(apObject);
}

// Only used while resolving a skeleton, never per frame: VirtualQuery is a system call.
bool IsReadable(const void* apPointer, size_t aSize) noexcept
{
    if (!apPointer)
        return false;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apPointer, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return false;

    constexpr DWORD cReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & cReadable))
        return false;

    return reinterpret_cast<uintptr_t>(apPointer) + aSize <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}

// Breadth-first search for the shallowest node with this name. Physics armour (SMP/3BA) carries its own copies of
// skeleton nodes like "NPC L Hand"; picking one of those stretched the remote arms.
void* FindShallowest(void* apStart, const char* acpName) noexcept
{
    if (!apStart)
        return nullptr;

    TiltedPhoques::Vector<void*> queue;
    queue.push_back(apStart);
    for (size_t head = 0; head < queue.size() && head < 8192; ++head)
    {
        void* pObject = queue[head];
        if (head > 0)
        {
            if (const char* pName = GetName(pObject); pName && _stricmp(pName, acpName) == 0)
                return pObject;
        }

        void* pNode = AsNode(pObject);
        if (!pNode)
            continue;

        void** pChildren = At<void**>(pNode, kChildrenOffset + 0x8);
        const uint16_t capacity = At<uint16_t>(pNode, kChildrenOffset + 0x10);
        if (!pChildren)
            continue;

        for (uint16_t i = 0; i < capacity; ++i)
            if (pChildren[i])
                queue.push_back(pChildren[i]);
    }
    return nullptr;
}

void* FindByRtti(void* apStart, const char* acpRttiName, uint32_t aDepth = 0) noexcept
{
    if (!apStart || aDepth > 8)
        return nullptr;

    using TGetRtti = const char**(__fastcall*)(void*);
    auto** ppVTable = *static_cast<void***>(apStart);
    const char** ppRtti = static_cast<TGetRtti>(ppVTable[kVTableGetRttiSlot])(apStart);
    if (ppRtti && ppRtti[0] && std::strcmp(ppRtti[0], acpRttiName) == 0)
        return apStart;

    void* pNode = AsNode(apStart);
    if (!pNode)
        return nullptr;

    void** pChildren = At<void**>(pNode, kChildrenOffset + 0x8);
    const uint16_t capacity = At<uint16_t>(pNode, kChildrenOffset + 0x10);
    for (uint16_t i = 0; pChildren && i < capacity; ++i)
        if (void* pFound = pChildren[i] ? FindByRtti(pChildren[i], acpRttiName, aDepth + 1) : nullptr)
            return pFound;

    return nullptr;
}

bool FindBones(void* apRoot, BoneNodes& aNodes) noexcept
{
    aNodes.fill(nullptr);

    void* pSkeletonRoot = FindShallowest(apRoot, "NPC Root [Root]");
    if (!pSkeletonRoot)
        pSkeletonRoot = apRoot;

    // Each bone is searched inside the bone found for its parent, so only the real chain matches.
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        void* pSearchFrom = kBoneParents[i] < 0 ? pSkeletonRoot : aNodes[kBoneParents[i]];
        aNodes[i] = FindShallowest(pSearchFrom, kBoneNames[i]);
        if (!aNodes[i])
            return false;
    }
    return true;
}

// NiMatrix3 is row-major (data[row][col]), glm is column-major.
glm::mat3 ToGlm(const NiMatrix3& acMatrix) noexcept
{
    glm::mat3 result{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result[c][r] = acMatrix.data[r][c];
    return result;
}

NiMatrix3 FromGlm(const glm::mat3& acMatrix) noexcept
{
    NiMatrix3 result{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result.data[r][c] = acMatrix[c][r];
    return result;
}

glm::vec3 ToGlm(const NiPoint3& acPoint) noexcept
{
    return {acPoint.x, acPoint.y, acPoint.z};
}

NiPoint3 FromGlm(const glm::vec3& acPoint) noexcept
{
    NiPoint3 result{};
    result.x = acPoint.x;
    result.y = acPoint.y;
    result.z = acPoint.z;
    return result;
}

struct RemotePose
{
    std::array<glm::quat, VRPose::kBoneCount> Bones{};
};

std::shared_mutex s_posesLock;
TiltedPhoques::Map<uint32_t, RemotePose> s_poses;

/**
 * A bone as the renderer sees it: its node, its slot in the flattened bone array, or both.
 *
 * The witnesses record what the pointers pointed at when the skeleton was resolved. Skyrim hands freed nodes straight
 * back to new objects, so a rebuilt 3D can reuse the same addresses; a changed witness means the skeleton is gone and
 * nothing may be written through these pointers (TiltedEvolutionVR crashed writing a transform into a shader property
 * that had taken over a freed bone's memory).
 */
struct RigBone
{
    void* pNode = nullptr;
    uint8_t* pEntry = nullptr;
    void* NodeVTable = nullptr;
    void* EntryNode = nullptr;

    [[nodiscard]] bool IsIntact() const noexcept
    {
        if (pNode && *static_cast<void**>(pNode) != NodeVTable)
            return false;
        return !pEntry || *reinterpret_cast<void**>(pEntry + kBoneEntryNode) == EntryNode;
    }

    [[nodiscard]] const NiTransform& World() const noexcept
    {
        return pNode ? At<NiTransform>(pNode, kWorldOffset) : *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryWorld);
    }

    void WriteWorld(const NiTransform& acWorld) const noexcept
    {
        if (pNode)
            At<NiTransform>(pNode, kWorldOffset) = acWorld;
        if (pEntry)
            *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryWorld) = acWorld;
    }

    void WriteLocal(const NiTransform& acLocal) const noexcept
    {
        if (pNode)
            At<NiTransform>(pNode, kLocalOffset) = acLocal;
        if (pEntry)
            *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryLocal) = acLocal;
    }
};

RigBone MakeRigBone(void* apNode, uint8_t* apEntry) noexcept
{
    RigBone bone{apNode, apEntry};
    if (apNode)
        bone.NodeVTable = *static_cast<void**>(apNode);
    if (apEntry)
        bone.EntryNode = *reinterpret_cast<void**>(apEntry + kBoneEntryNode);
    return bone;
}

// The skeleton of one remote player, resolved once per 3D.
struct Rig
{
    void* pRoot = nullptr;
    std::array<RigBone, VRPose::kBoneCount> Bones{};
    // Everything below each posed bone (nodes and flattened bones), the posed bones further down the chain included.
    std::array<std::vector<RigBone>, VRPose::kBoneCount> Descendants{};
    std::chrono::steady_clock::time_point RetryAt{};
    bool Valid = false;
};

std::unordered_map<uint32_t, Rig> s_rigs; // frame end only

uint8_t* GetBoneArray(void* apTree, uint32_t& aOutCount) noexcept
{
    aOutCount = 0;
    if (!apTree || !IsReadable(static_cast<uint8_t*>(apTree) + kTreeBoneArray, sizeof(void*)))
        return nullptr;

    auto* pArray = At<uint8_t*>(apTree, kTreeBoneArray);
    if (!IsReadable(pArray, kBoneEntrySize))
        return nullptr;

    // The count sits near the array pointer at an offset that isn't measured, so a candidate is only accepted when every
    // entry below it is readable and points at a readable node or none.
    for (uint32_t offset = kChildrenOffset + 0x18; offset <= kTreeBoneArray + 0x20; offset += sizeof(uint32_t))
    {
        const uint32_t candidate = At<uint32_t>(apTree, offset);
        if (candidate == 0 || candidate > kMaxBones || !IsReadable(pArray, static_cast<size_t>(candidate) * kBoneEntrySize))
            continue;

        bool plausible = true;
        for (uint32_t i = 0; i < candidate && plausible; ++i)
        {
            void* pNode = *reinterpret_cast<void**>(pArray + i * kBoneEntrySize + kBoneEntryNode);
            plausible = !pNode || IsReadable(pNode, kNiAVObjectSize);
        }

        if (plausible)
        {
            aOutCount = candidate;
            return pArray;
        }
    }

    return nullptr;
}

uint8_t* FindEntry(uint8_t* apArray, uint32_t aCount, void* apNode) noexcept
{
    for (uint32_t i = 0; apArray && i < aCount; ++i)
        if (*reinterpret_cast<void**>(apArray + i * kBoneEntrySize + kBoneEntryNode) == apNode)
            return apArray + i * kBoneEntrySize;
    return nullptr;
}

void CollectNodeDescendants(void* apNode, std::vector<void*>& aOut, uint32_t aDepth = 0) noexcept
{
    void* pNode = aDepth < 64 ? AsNode(apNode) : nullptr;
    if (!pNode)
        return;

    void** pChildren = At<void**>(pNode, kChildrenOffset + 0x8);
    const uint16_t capacity = At<uint16_t>(pNode, kChildrenOffset + 0x10);
    for (uint16_t i = 0; pChildren && i < capacity; ++i)
    {
        if (!pChildren[i])
            continue;
        aOut.push_back(pChildren[i]);
        CollectNodeDescendants(pChildren[i], aOut, aDepth + 1);
    }
}

bool ResolveRig(void* apRoot, Rig& aRig) noexcept
{
    aRig = Rig{};
    aRig.pRoot = apRoot;

    BoneNodes nodes;
    if (!FindBones(apRoot, nodes))
        return false;

    uint32_t count = 0;
    uint8_t* pArray = GetBoneArray(FindByRtti(apRoot, "BSFlattenedBoneTree"), count);

    const auto readIndex = [](const uint8_t* apEntry, int aField) { return *reinterpret_cast<const int16_t*>(apEntry + kBoneEntryIndices + aField * sizeof(int16_t)); };

    // Which of the four indices is the parent: the one that maps the left forearm to the upper arm, and the hand to the
    // forearm.
    int parentField = -1;
    const uint8_t* pUpper = FindEntry(pArray, count, nodes[VRPose::kLeftUpperArm]);
    const uint8_t* pFore = FindEntry(pArray, count, nodes[VRPose::kLeftForearm]);
    const uint8_t* pHand = FindEntry(pArray, count, nodes[VRPose::kLeftHand]);
    if (pUpper && pFore && pHand)
    {
        const auto indexOf = [pArray](const uint8_t* apEntry) { return static_cast<int16_t>((apEntry - pArray) / kBoneEntrySize); };
        for (int field = 0; field < 4 && parentField < 0; ++field)
            if (readIndex(pFore, field) == indexOf(pUpper) && readIndex(pHand, field) == indexOf(pFore))
                parentField = field;
    }

    if (!pArray || parentField < 0)
    {
        // Nodes alone still pose the body and anything attached; flattened bones (fingers, face) then lag behind.
        spdlog::warn("VRBodySync: no usable flattened bone array under root {} (count {}, parent field {})", apRoot, count, parentField);
        pArray = nullptr;
        count = 0;
    }

    std::unordered_map<void*, uint8_t*> entryByNode;
    for (uint32_t i = 0; i < count; ++i)
        if (void* pNode = *reinterpret_cast<void**>(pArray + i * kBoneEntrySize + kBoneEntryNode))
            entryByNode.emplace(pNode, pArray + i * kBoneEntrySize);

    const auto entryFor = [&entryByNode](void* apNode) -> uint8_t*
    {
        const auto it = entryByNode.find(apNode);
        return it == entryByNode.end() ? nullptr : it->second;
    };

    for (uint32_t bone = 0; bone < VRPose::kBoneCount; ++bone)
    {
        aRig.Bones[bone] = MakeRigBone(nodes[bone], entryFor(nodes[bone]));

        // Nodes hung below the bone (weapon, shield, magic node, the next bones)...
        std::vector<RigBone>& descendants = aRig.Descendants[bone];
        std::unordered_map<const uint8_t*, bool> entriesSeen;
        std::vector<void*> childNodes;
        CollectNodeDescendants(nodes[bone], childNodes);
        for (void* pChild : childNodes)
        {
            uint8_t* pEntry = entryFor(pChild);
            descendants.push_back(MakeRigBone(pChild, pEntry));
            if (pEntry)
                entriesSeen[pEntry] = true;
        }

        // ...and the flattened bones below it (fingers, facial bones), found through each entry's parent chain.
        const uint8_t* pBoneEntry = aRig.Bones[bone].pEntry;
        for (uint32_t i = 0; pBoneEntry && i < count; ++i)
        {
            uint8_t* pEntry = pArray + i * kBoneEntrySize;
            if (pEntry == pBoneEntry || entriesSeen.count(pEntry))
                continue;

            int16_t walk = static_cast<int16_t>(i);
            for (uint32_t step = 0; step < 128; ++step)
            {
                walk = readIndex(pArray + static_cast<uint32_t>(walk) * kBoneEntrySize, parentField);
                if (walk < 0 || static_cast<uint32_t>(walk) >= count)
                    break;
                if (pArray + static_cast<uint32_t>(walk) * kBoneEntrySize == pBoneEntry)
                {
                    descendants.push_back(MakeRigBone(*reinterpret_cast<void**>(pEntry + kBoneEntryNode), pEntry));
                    break;
                }
            }
        }
    }

    aRig.Valid = true;
    spdlog::info("VRBodySync: resolved skeleton under root {}: {} flattened bones, {} carried by the spine, {} by the head, {} by the right hand", apRoot, count,
                 aRig.Descendants[VRPose::kSpine1].size(), aRig.Descendants[VRPose::kHead].size(), aRig.Descendants[VRPose::kRightHand].size());
    return true;
}

[[nodiscard]] bool RigIntact(const Rig& acRig) noexcept
{
    for (const auto& bone : acRig.Bones)
        if (!bone.IsIntact())
            return false;
    for (const auto& descendants : acRig.Descendants)
        for (const auto& bone : descendants)
            if (!bone.IsIntact())
                return false;
    return true;
}

// Posing an actor the renderer has culled tore its skin into black strips in TiltedEvolutionVR, so only actors in
// front of the headset are posed. A sphere around the chest is tested against a 50 degree cone, widened by the angle
// the sphere covers, so an actor close by counts as visible whatever the angle.
bool IsInView(const glm::vec3& acChest) noexcept
{
    constexpr float cHalfAngle = 50.f * glm::pi<float>() / 180.f;
    constexpr float cBodyRadius = 100.f;

    static int s_hmdState = 0; // 0 unchecked, 1 valid, -1 not the headset node on this build
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || s_hmdState < 0)
        return true;

    void* pHmd = At<void*>(pPlayer, kHmdNodeOffset);
    if (s_hmdState == 0)
    {
        const char* pName = IsReadable(pHmd, kNiAVObjectSize) ? GetName(pHmd) : nullptr;
        s_hmdState = pName && IsReadable(pName, 8) && std::strcmp(pName, "HmdNode") == 0 ? 1 : -1;
        if (s_hmdState < 0)
        {
            spdlog::warn("VRBodySync: PlayerCharacter+0x{:X} isn't the headset node, remote players are posed even out of view", kHmdNodeOffset);
            return true;
        }
    }
    if (!pHmd)
        return true;

    const auto& hmd = At<NiTransform>(pHmd, kWorldOffset);
    const glm::vec3 toChest = acChest - ToGlm(hmd.translate);
    const float distance = glm::length(toChest);
    if (distance <= cBodyRadius)
        return true;

    const glm::vec3 forward = ToGlm(hmd.rotate)[1]; // Skyrim: X right, Y forward, Z up
    const float offAxis = std::acos(glm::clamp(glm::dot(forward, toChest / distance), -1.f, 1.f));
    return offAxis < cHalfAngle + std::asin(cBodyRadius / distance);
}

void PoseActor(const Rig& acRig, const RemotePose& acPose) noexcept
{
    const glm::mat3 rootRotation = ToGlm(At<NiTransform>(acRig.pRoot, kWorldOffset).rotate);

    // Parents first. Each bone is turned about its own position, and that rigid motion is carried to everything below
    // it, posed children included, so a child's transform already holds its parents' motion when its own rotation is
    // solved. World transforms are written directly because nothing recomputes them from locals between the end of the
    // frame and the draw that uses them.
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        const RigBone& bone = acRig.Bones[i];
        const NiTransform current = bone.World();
        const glm::mat3 currentRotation = ToGlm(current.rotate);
        const glm::mat3 wantedRotation = rootRotation * glm::mat3_cast(acPose.Bones[i]);
        const glm::mat3 delta = wantedRotation * glm::transpose(currentRotation);
        const glm::vec3 pivot = ToGlm(current.translate);

        NiTransform wanted = current;
        wanted.rotate = FromGlm(wantedRotation);
        bone.WriteWorld(wanted);

        // local' = local * (world^-1 * world'). The position doesn't change, so only the rotation does.
        if (bone.pNode)
        {
            NiTransform local = At<NiTransform>(bone.pNode, kLocalOffset);
            local.rotate = FromGlm(ToGlm(local.rotate) * glm::transpose(currentRotation) * wantedRotation);
            bone.WriteLocal(local);
        }

        for (const RigBone& child : acRig.Descendants[i])
        {
            NiTransform world = child.World();
            world.rotate = FromGlm(delta * ToGlm(world.rotate));
            world.translate = FromGlm(pivot + delta * (ToGlm(world.translate) - pivot));
            child.WriteWorld(world);
        }
    }
}

} // namespace

namespace VRBodySync
{
void OnFrameEnd() noexcept
{
    TiltedPhoques::Vector<std::pair<uint32_t, RemotePose>> poses;
    {
        std::shared_lock lock(s_posesLock);
        for (const auto& [formId, pose] : s_poses)
            poses.emplace_back(formId, pose);
    }

    // Skeletons of players who left.
    for (auto it = s_rigs.begin(); it != s_rigs.end();)
    {
        const uint32_t formId = it->first;
        const bool stillPosed = std::any_of(poses.begin(), poses.end(), [formId](const auto& acEntry) { return acEntry.first == formId; });
        it = stillPosed ? std::next(it) : s_rigs.erase(it);
    }

    if (poses.empty())
        return;

    PerfCounterScope perfScope(PerfCounter::kVRPoseApply);
    const auto now = std::chrono::steady_clock::now();

    for (const auto& [formId, pose] : poses)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(formId));
        void* pRoot = pActor ? pActor->GetNiNode() : nullptr;
        if (!pRoot)
            continue;

        // A dead or downed body belongs to its ragdoll.
        if (pActor->actorState.IsDeadOrDying() || pActor->actorState.IsBleedingOut())
            continue;

        Rig& rig = s_rigs[formId];
        if (rig.pRoot != pRoot || !rig.Valid || !RigIntact(rig))
        {
            if (rig.pRoot == pRoot && now < rig.RetryAt)
                continue;
            if (!ResolveRig(pRoot, rig))
            {
                rig.RetryAt = now + std::chrono::seconds(1);
                continue;
            }
        }

        if (!IsInView(ToGlm(rig.Bones[VRPose::kSpine2].World().translate)))
            continue;

        PoseActor(rig, pose);
    }
}

bool CaptureLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose) noexcept
{
    aOutPose.HasData = false;

    if (!apPlayer)
        return false;

    void* pRoot = apPlayer->GetNiNode();
    if (!pRoot)
        return false;

    BoneNodes nodes;
    if (!FindBones(pRoot, nodes))
        return false;

    const glm::mat3 inverseRoot = glm::transpose(ToGlm(At<NiTransform>(pRoot, kWorldOffset).rotate));

    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        const glm::mat3 boneWorld = ToGlm(At<NiTransform>(nodes[i], kWorldOffset).rotate);
        aOutPose.Bones[i] = glm::normalize(glm::quat_cast(inverseRoot * boneWorld));
    }

    aOutPose.HasData = true;
    return true;
}

void SetRemotePose(Actor* apActor, const VRPose& acPose) noexcept
{
    if (!apActor)
        return;

    if (!acPose.HasData)
    {
        ClearRemotePose(apActor->formID);
        return;
    }

    RemotePose pose;
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
        pose.Bones[i] = acPose.Bones[i];

    std::unique_lock lock(s_posesLock);
    s_poses[apActor->formID] = pose;
}

void LogCastOrigin(Actor* apActor, uint32_t aCastingSource) noexcept
{
    // Spells from a remote VR player were reported leaving off the hand. The caster aims from the magic node, so log
    // where it is relative to the posed hand for the first few casts.
    static uint32_t s_logged = 0;
    if (!apActor || aCastingSource > 1 || s_logged >= 6)
        return;

    void* pRoot = apActor->GetNiNode();
    BoneNodes nodes;
    if (!pRoot || !FindBones(pRoot, nodes))
        return;

    ++s_logged;
    const bool left = aCastingSource == 0;
    void* pHand = nodes[left ? VRPose::kLeftHand : VRPose::kRightHand];
    void* pMagicNode = FindShallowest(pRoot, left ? "NPC L MagicNode [LMag]" : "NPC R MagicNode [RMag]");

    const auto& hand = At<NiTransform>(pHand, kWorldOffset).translate;
    if (!pMagicNode)
    {
        spdlog::info("CastDiag {:X}: {} hand at ({:.1f}, {:.1f}, {:.1f}), no magic node found", apActor->formID, left ? "left" : "right", hand.x, hand.y, hand.z);
        return;
    }

    void* pParent = At<void*>(pMagicNode, kParentOffset);
    const auto& magic = At<NiTransform>(pMagicNode, kWorldOffset).translate;
    const float distance = glm::distance(glm::vec3{hand.x, hand.y, hand.z}, glm::vec3{magic.x, magic.y, magic.z});
    spdlog::info("CastDiag {:X}: {} hand at ({:.1f}, {:.1f}, {:.1f}), magic node at ({:.1f}, {:.1f}, {:.1f}), {:.1f} units apart, magic node parent '{}', under the posed hand {}",
                 apActor->formID, left ? "left" : "right", hand.x, hand.y, hand.z, magic.x, magic.y, magic.z, distance, pParent ? GetName(pParent) : "none",
                 FindShallowest(pHand, left ? "NPC L MagicNode [LMag]" : "NPC R MagicNode [RMag]") == pMagicNode);
}

void ClearRemotePose(uint32_t aFormId) noexcept
{
    std::unique_lock lock(s_posesLock);
    s_poses.erase(aFormId);
}
} // namespace VRBodySync
