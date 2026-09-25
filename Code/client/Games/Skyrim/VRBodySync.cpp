#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/VRBodySync.h>

#include <Actor.h>
#include <Games/ActorExtension.h>
#include <PlayerCharacter.h>
#include <NetImmerse/NiNode.h>
#include <NetImmerse/NiTransform.h>

#include <glm/gtc/quaternion.hpp>

#include <PerfScope.h>

#include <mutex>
#include <shared_mutex>
#include <tlhelp32.h>
#include <cwctype>
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
    // Lower body (VRPose::HasLegs). The pelvis hangs off NPC COM, beside the spine, so it is searched from the root.
    "NPC Pelvis [Pelv]",   "NPC L Thigh [LThg]",  "NPC L Calf [LClf]",     "NPC L Foot [Lft ]",
    "NPC R Thigh [RThg]",  "NPC R Calf [RClf]",   "NPC R Foot [Rft ]",
};

// Parent bone index for each entry of kBoneNames (-1: searched under "NPC Root [Root]").
constexpr std::array<int8_t, VRPose::kBoneCount> kBoneParents{-1, 0, 1, 2, 1, 4, 5, 6, 1, 8, 9, 10, -1, 12, 13, 14, 12, 16, 17};

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

// The skeleton root below the actor's 3D: every bone hangs off it, and its world scale is the body's size.
void* FindSkeletonRoot(void* apRoot) noexcept
{
    void* pSkeletonRoot = FindShallowest(apRoot, "NPC Root [Root]");
    return pSkeletonRoot ? pSkeletonRoot : apRoot;
}

// The upper body is required; the legs are optional (a skeleton without them is posed above the waist only).
bool FindBones(void* apRoot, BoneNodes& aNodes, bool* apLegsFound = nullptr) noexcept
{
    aNodes.fill(nullptr);

    void* pSkeletonRoot = FindShallowest(apRoot, "NPC Root [Root]");
    if (!pSkeletonRoot)
        pSkeletonRoot = apRoot;

    bool legsFound = true;
    // Each bone is searched inside the bone found for its parent, so only the real chain matches.
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        void* pSearchFrom = kBoneParents[i] < 0 ? pSkeletonRoot : aNodes[kBoneParents[i]];
        aNodes[i] = pSearchFrom ? FindShallowest(pSearchFrom, kBoneNames[i]) : nullptr;
        if (aNodes[i])
            continue;
        if (i < VRPose::kUpperBoneCount)
            return false;
        legsFound = false;
    }
    if (apLegsFound)
        *apLegsFound = legsFound;
    return true;
}

// SkyrimVR FBT (Nexus 185070) drives the hips and feet from SteamVR body trackers through its SKSE plugin. The legs
// are only sent while that plugin is loaded here: without it they follow the walk animation on both sides anyway,
// and sending them would pin the other side's copy to this side's animation frame. The plugin is SkyrimVR-FBT.dll
// (1.0.3, checked 2026-09-20); "fbt" or "fullbody" anywhere in a module name also counts, and the match is logged.
bool FullBodyTrackingActive() noexcept
{
    static std::chrono::steady_clock::time_point s_nextCheck;
    static bool s_active = false;
    static std::string s_module;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_nextCheck)
        return s_active;
    s_nextCheck = now + std::chrono::seconds(5);

    bool active = false;
    std::string matched;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (hSnapshot != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Module32FirstW(hSnapshot, &entry); ok && !active; ok = Module32NextW(hSnapshot, &entry))
        {
            std::string name;
            for (const wchar_t* p = entry.szModule; *p; ++p)
                name += *p < 128 ? static_cast<char>(std::towlower(*p)) : '?';
            if (name.rfind("skyrimtogether", 0) == 0)
                continue;
            if (name.find("fbt") != std::string::npos || name.find("fullbody") != std::string::npos)
            {
                active = true;
                matched = name;
            }
        }
        CloseHandle(hSnapshot);
    }

    if (active != s_active)
    {
        if (active)
            spdlog::info("VRBodySync: full body tracking plugin '{}' is loaded, the legs are sent with the pose", matched);
        else
            spdlog::info("VRBodySync: full body tracking plugin '{}' is gone, the legs are no longer sent", s_module);
    }
    s_active = active;
    if (active)
        s_module = matched;
    return active;
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

// Defined further down, with the rig.
uint8_t* GetBoneArray(void* apTree, uint32_t& aOutCount) noexcept;
uint8_t* FindEntry(uint8_t* apArray, uint32_t aCount, void* apNode) noexcept;

// The flattened bone array of a skeleton and which of an entry's four indices is its parent.
struct BoneArrayInfo
{
    uint8_t* pArray = nullptr;
    uint32_t Count = 0;
    int ParentField = -1;
};

int16_t ReadEntryIndex(const uint8_t* apEntry, int aField) noexcept
{
    return *reinterpret_cast<const int16_t*>(apEntry + kBoneEntryIndices + aField * sizeof(int16_t));
}

BoneArrayInfo ResolveBoneArray(void* apRoot, const BoneNodes& acNodes, bool aLog) noexcept
{
    BoneArrayInfo info;
    info.pArray = GetBoneArray(FindByRtti(apRoot, "BSFlattenedBoneTree"), info.Count);

    // Which of the four indices is the parent: the one that maps the left forearm to the upper arm, and the hand to the
    // forearm.
    const uint8_t* pUpper = FindEntry(info.pArray, info.Count, acNodes[VRPose::kLeftUpperArm]);
    const uint8_t* pFore = FindEntry(info.pArray, info.Count, acNodes[VRPose::kLeftForearm]);
    const uint8_t* pHand = FindEntry(info.pArray, info.Count, acNodes[VRPose::kLeftHand]);
    if (pUpper && pFore && pHand)
    {
        const auto indexOf = [&info](const uint8_t* apEntry) { return static_cast<int16_t>((apEntry - info.pArray) / kBoneEntrySize); };
        for (int field = 0; field < 4 && info.ParentField < 0; ++field)
            if (ReadEntryIndex(pFore, field) == indexOf(pUpper) && ReadEntryIndex(pHand, field) == indexOf(pFore))
                info.ParentField = field;
    }

    if (!info.pArray || info.ParentField < 0)
    {
        // Nodes alone still pose the body and anything attached; flattened bones (fingers, face) then lag behind.
        if (aLog)
            spdlog::warn("VRBodySync: no usable flattened bone array under root {} (count {}, parent field {})", apRoot, info.Count, info.ParentField);
        info = BoneArrayInfo{};
    }
    return info;
}

// The finger bones of one hand: five chains of three flattened entries (Finger00 > 01 > 02), thumb first in the
// vanilla skeleton, in array order. Flattened entries carry no name here, so they are found by shape: an entry under
// the hand with a child that has a child. Weapon, shield and magic nodes hang off the hand as single entries.
using FingerEntries = std::array<uint8_t*, VRPose::kFingersPerHand * VRPose::kBonesPerFinger>;

bool FindFingers(const BoneArrayInfo& acInfo, const uint8_t* apHandEntry, FingerEntries& aOut) noexcept
{
    aOut.fill(nullptr);
    if (!acInfo.pArray || !apHandEntry || acInfo.ParentField < 0)
        return false;
    const auto entryAt = [&acInfo](uint32_t aIndex) { return acInfo.pArray + aIndex * kBoneEntrySize; };
    const auto parentOf = [&](uint32_t aIndex) { return ReadEntryIndex(entryAt(aIndex), acInfo.ParentField); };
    const auto firstChildOf = [&](int16_t aParent) -> int16_t
    {
        for (uint32_t i = 0; i < acInfo.Count; ++i)
            if (parentOf(i) == aParent)
                return static_cast<int16_t>(i);
        return -1;
    };
    const int16_t handIndex = static_cast<int16_t>((apHandEntry - acInfo.pArray) / kBoneEntrySize);

    uint32_t found = 0;
    for (uint32_t i = 0; i < acInfo.Count; ++i)
    {
        if (parentOf(i) != handIndex)
            continue;
        const int16_t second = firstChildOf(static_cast<int16_t>(i));
        const int16_t third = second >= 0 ? firstChildOf(second) : -1;
        if (second < 0 || third < 0)
            continue;
        if (found == VRPose::kFingersPerHand)
        {
            found = 0; // a sixth chain: not the shape expected, give up rather than guess
            break;
        }
        aOut[found * VRPose::kBonesPerFinger + 0] = entryAt(i);
        aOut[found * VRPose::kBonesPerFinger + 1] = entryAt(static_cast<uint32_t>(second));
        aOut[found * VRPose::kBonesPerFinger + 2] = entryAt(static_cast<uint32_t>(third));
        ++found;
    }
    if (found != VRPose::kFingersPerHand)
    {
        aOut.fill(nullptr);
        return false;
    }
    return true;
}

// Finger rotations relative to the parent bone, read from the rendered (world) transforms: the hand node for the
// first bone of each finger, the previous entry for the others.
void ReadFingerRotations(const FingerEntries& acFingers, const glm::mat3& acHandWorld, glm::quat* apOut) noexcept
{
    for (size_t f = 0; f < VRPose::kFingersPerHand; ++f)
    {
        glm::mat3 parent = acHandWorld;
        for (size_t k = 0; k < VRPose::kBonesPerFinger; ++k)
        {
            const uint8_t* pEntry = acFingers[f * VRPose::kBonesPerFinger + k];
            const glm::mat3 world = ToGlm(reinterpret_cast<const NiTransform*>(pEntry + kBoneEntryWorld)->rotate);
            apOut[f * VRPose::kBonesPerFinger + k] = glm::normalize(glm::quat_cast(glm::transpose(parent) * world));
            parent = world;
        }
    }
}

// Writes finger rotations below an already posed hand: each entry's world follows its parent, keeping the entry's
// own local offset and the world scale it had.
void WriteFingerRotations(const FingerEntries& acFingers, const NiTransform& acHandWorld, const glm::quat* apRotations) noexcept
{
    for (size_t f = 0; f < VRPose::kFingersPerHand; ++f)
    {
        NiTransform parent = acHandWorld;
        for (size_t k = 0; k < VRPose::kBonesPerFinger; ++k)
        {
            uint8_t* pEntry = acFingers[f * VRPose::kBonesPerFinger + k];
            NiTransform& local = *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryLocal);
            NiTransform& world = *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryWorld);
            const glm::mat3 parentRotation = ToGlm(parent.rotate);
            const glm::mat3 localRotation = glm::mat3_cast(apRotations[f * VRPose::kBonesPerFinger + k]);
            local.rotate = FromGlm(localRotation);
            world.rotate = FromGlm(parentRotation * localRotation);
            world.translate = FromGlm(ToGlm(parent.translate) + parentRotation * (ToGlm(local.translate) * parent.scale));
            parent = world;
        }
    }
}

struct RemotePose
{
    bool HasLegs = false;
    std::array<glm::quat, VRPose::kBoneCount> Bones{};
    // Kept from the last update that carried them (see VRPose::HasFingers).
    bool HasFingers = false;
    std::array<glm::quat, VRPose::kFingerBoneCount> Fingers{};
    // Kept from the last update that carried it (see VRPose::HasScale).
    bool HasScale = false;
    float RootScale = 1.f;
    // Only for a dead body being moved (see VRPose::HasRootPosition). Not kept: when the sender stops, the body
    // goes back to its own ragdoll rather than being pinned where it last was.
    bool HasRootPosition = false;
    glm::vec3 RootPosition{};
    // Where the sender's hips are relative to its root (see VRPose::HasHips). Not kept either: when the sender's
    // trackers stop driving the legs the body should go back to its own animation rather than hold the last crouch.
    bool HasHips = false;
    glm::vec3 HipOffset{};
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
// Direct children of one node: how many slots the array holds and how many are filled. Plain reads, no virtual call
// and no VirtualQuery, so unlike IsReadable this is cheap enough to run every frame. Enough to notice something being
// attached below a bone, which is what the cached descendant list cannot see by itself.
uint32_t ChildFingerprintOf(void* apChildOwner) noexcept
{
    if (!apChildOwner)
        return 0;

    void** pChildren = At<void**>(apChildOwner, kChildrenOffset + 0x8);
    const uint16_t slots = At<uint16_t>(apChildOwner, kChildrenOffset + 0x10);
    if (!pChildren)
        return slots;

    uint32_t filled = 0;
    for (uint16_t i = 0; i < slots; ++i)
        filled += pChildren[i] != nullptr;

    return (static_cast<uint32_t>(slots) << 16) | filled;
}

struct RigBone
{
    void* pNode = nullptr;
    uint8_t* pEntry = nullptr;
    void* NodeVTable = nullptr;
    void* EntryNode = nullptr;
    // The node under which children hang, and what hung there when the skeleton was resolved.
    void* pChildOwner = nullptr;
    uint32_t ChildFingerprint = 0;

    [[nodiscard]] bool IsIntact() const noexcept
    {
        if (pNode && *static_cast<void**>(pNode) != NodeVTable)
            return false;
        return !pEntry || *reinterpret_cast<void**>(pEntry + kBoneEntryNode) == EntryNode;
    }

    [[nodiscard]] bool ChildrenUnchanged() const noexcept
    {
        return ChildFingerprintOf(pChildOwner) == ChildFingerprint;
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
    {
        bone.NodeVTable = *static_cast<void**>(apNode);
        bone.pChildOwner = AsNode(apNode);
        bone.ChildFingerprint = ChildFingerprintOf(bone.pChildOwner);
    }
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
    // Finger entries below each hand (left, right); found only when the flattened array is usable.
    std::array<FingerEntries, 2> Fingers{};
    std::array<bool, 2> FingersFound{};
    // "NPC Root [Root]": its local scale is set to the sender's body size.
    RigBone SkeletonRoot{};
    std::chrono::steady_clock::time_point RetryAt{};
    // Re-resolving because something attached is rate limited: an effect that attaches and detaches every frame would
    // otherwise rebuild the whole skeleton every frame.
    std::chrono::steady_clock::time_point RestructureAt{};
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

bool ResolveRig(void* apRoot, Rig& aRig, bool aLog = true) noexcept
{
    aRig = Rig{};
    aRig.pRoot = apRoot;
    aRig.SkeletonRoot = MakeRigBone(FindSkeletonRoot(apRoot), nullptr);

    BoneNodes nodes;
    if (!FindBones(apRoot, nodes))
        return false;

    const BoneArrayInfo arrayInfo = ResolveBoneArray(apRoot, nodes, aLog);
    uint8_t* pArray = arrayInfo.pArray;
    const uint32_t count = arrayInfo.Count;
    const int parentField = arrayInfo.ParentField;
    const auto readIndex = [](const uint8_t* apEntry, int aField) { return ReadEntryIndex(apEntry, aField); };

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
        if (!nodes[bone])
            continue; // a leg bone this skeleton lacks; never posed

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

    aRig.FingersFound[0] = FindFingers(arrayInfo, aRig.Bones[VRPose::kLeftHand].pEntry, aRig.Fingers[0]);
    aRig.FingersFound[1] = FindFingers(arrayInfo, aRig.Bones[VRPose::kRightHand].pEntry, aRig.Fingers[1]);
    if (aLog && (!aRig.FingersFound[0] || !aRig.FingersFound[1]))
        spdlog::warn("VRBodySync: finger bones not found by shape under root {} (left {}, right {}); the hands stay on the animation pose", apRoot, aRig.FingersFound[0], aRig.FingersFound[1]);
    aRig.Valid = true;
    // Quiet when this is only picking up a node that was attached below a bone, which happens on every equip.
    if (aLog)
        spdlog::info("VRBodySync: resolved skeleton under root {}: {} flattened bones, {} carried by the spine, {} by the head, {} by the left hand, {} by the right hand", apRoot, count,
                     aRig.Descendants[VRPose::kSpine1].size(), aRig.Descendants[VRPose::kHead].size(), aRig.Descendants[VRPose::kLeftHand].size(),
                     aRig.Descendants[VRPose::kRightHand].size());
    return true;
}

[[nodiscard]] bool RigIntact(const Rig& acRig) noexcept
{
    if (!acRig.SkeletonRoot.IsIntact())
        return false;
    for (const auto& bone : acRig.Bones)
        if (!bone.IsIntact())
            return false;
    for (const auto& descendants : acRig.Descendants)
        for (const auto& bone : descendants)
            if (!bone.IsIntact())
                return false;
    return true;
}

// The descendant lists are a snapshot taken when the skeleton was resolved. Anything attached afterwards - the art of a
// readied spell hangs off the magic node, which is itself below the hand - is missing from them, so the hand was posed
// to the VR pose while the spell stayed where the animation had left it, floating beside the hand. Notice the
// attachment and resolve the skeleton again so the new node is carried too. Only call this on an intact rig: it reads
// through the cached pointers.
[[nodiscard]] bool RigStructureUnchanged(const Rig& acRig) noexcept
{
    for (const auto& bone : acRig.Bones)
        if (!bone.ChildrenUnchanged())
            return false;
    for (const auto& descendants : acRig.Descendants)
        for (const auto& bone : descendants)
            if (!bone.ChildrenUnchanged())
                return false;
    return true;
}

// Posing an actor the renderer has culled tore its skin into black strips in TiltedEvolutionVR, so only actors in
// front of the headset are posed. A sphere around the chest is tested against a 50 degree cone, widened by the angle
// the sphere covers, so an actor close by counts as visible whatever the angle.
// The headset's world transform, or null when this build's PlayerCharacter does not carry it where we look.
const NiTransform* HeadsetTransform() noexcept
{
    static int s_hmdState = 0; // 0 unchecked, 1 valid, -1 not the headset node on this build
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || s_hmdState < 0)
        return nullptr;

    void* pHmd = At<void*>(pPlayer, kHmdNodeOffset);
    if (s_hmdState == 0)
    {
        const char* pName = IsReadable(pHmd, kNiAVObjectSize) ? GetName(pHmd) : nullptr;
        s_hmdState = pName && IsReadable(pName, 8) && std::strcmp(pName, "HmdNode") == 0 ? 1 : -1;
        if (s_hmdState < 0)
        {
            spdlog::warn("VRBodySync: PlayerCharacter+0x{:X} isn't the headset node, remote players are posed even out of view", kHmdNodeOffset);
            return nullptr;
        }
    }
    if (!pHmd)
        return nullptr;
    return &At<NiTransform>(pHmd, kWorldOffset);
}

bool IsInView(const glm::vec3& acChest) noexcept
{
    constexpr float cHalfAngle = 50.f * glm::pi<float>() / 180.f;
    constexpr float cBodyRadius = 100.f;

    const NiTransform* pHmdTransform = HeadsetTransform();
    if (!pHmdTransform)
        return true;
    const NiTransform& hmd = *pHmdTransform;
    const glm::vec3 toChest = acChest - ToGlm(hmd.translate);
    const float distance = glm::length(toChest);
    if (distance <= cBodyRadius)
        return true;

    const glm::vec3 forward = ToGlm(hmd.rotate)[1]; // Skyrim: X right, Y forward, Z up
    const float offAxis = std::acos(glm::clamp(glm::dot(forward, toChest / distance), -1.f, 1.f));
    return offAxis < cHalfAngle + std::asin(cBodyRadius / distance);
}

// A transform the renderer can still use: a real rotation, a sane scale, no infinities. A skinned body whose bone
// transform fails this is not drawn at all, while the actor itself stays perfectly healthy, which is what the other
// player sees as "he is there, I can hit him, I cannot see him".
bool IsUsable(const NiTransform& acTransform) noexcept
{
    const glm::mat3 rotation = ToGlm(acTransform.rotate);
    for (int column = 0; column < 3; ++column)
    {
        const float length = glm::length(rotation[column]);
        if (!std::isfinite(length) || length < 0.9f || length > 1.1f)
            return false;
    }
    if (!std::isfinite(acTransform.scale) || acTransform.scale < 0.05f || acTransform.scale > 20.f)
        return false;
    return std::isfinite(acTransform.translate.x) && std::isfinite(acTransform.translate.y) && std::isfinite(acTransform.translate.z);
}

// The bones we write to, as the renderer will read them. False means this body is about to be, or already is,
// undrawable and we must take our hands off it.
bool IsBodyUsable(const Rig& acRig) noexcept
{
    if (!acRig.pRoot || !IsUsable(At<NiTransform>(acRig.pRoot, kWorldOffset)))
        return false;
    for (uint32_t i = 0; i < VRPose::kUpperBoneCount; ++i)
    {
        const RigBone& bone = acRig.Bones[i];
        if ((bone.pNode || bone.pEntry) && !IsUsable(bone.World()))
            return false;
    }
    return true;
}

// The top of the tree a node hangs from. Two bodies drawn in the same world share it; one that has been left in a
// subtree the world no longer holds does not.
void* SceneTopOf(void* apNode) noexcept
{
    void* pWalk = apNode;
    for (uint32_t depth = 0; pWalk && depth < 64; ++depth)
    {
        void* pParent = At<void*>(pWalk, kParentOffset);
        if (!pParent || !IsReadable(pParent, kNiAVObjectSize))
            break;
        pWalk = pParent;
    }
    return pWalk;
}

//! acWorldOffset shifts the whole skeleton, for a corpse being dragged elsewhere. It has to reach every bone:
//! this writes world transforms directly and the skinned body follows the bones, not the root.
void PoseActor(const Rig& acRig, const RemotePose& acPose, const glm::vec3& acWorldOffset) noexcept
{
    // Body size: the skeleton root's local scale is set so that its world scale matches the sender's. Only the local
    // is written; the game carries it into every bone's world transform on its next update, before the rotations
    // below are written on top. Written only when it differs, so a copy that already matches is left alone.
    if (acPose.HasScale && acRig.SkeletonRoot.pNode)
    {
        NiTransform& local = At<NiTransform>(acRig.SkeletonRoot.pNode, kLocalOffset);
        const NiTransform& world = At<NiTransform>(acRig.SkeletonRoot.pNode, kWorldOffset);
        const float parentScale = local.scale > 0.001f ? world.scale / local.scale : 1.f;
        const float wanted = parentScale > 0.001f ? acPose.RootScale / parentScale : local.scale;
        if (wanted > 0.05f && wanted < 20.f && std::fabs(local.scale - wanted) > 0.003f)
            local.scale = wanted;
    }

    const glm::mat3 rootRotation = ToGlm(At<NiTransform>(acRig.pRoot, kWorldOffset).rotate);

    // Parents first. Each bone is turned about its own position, and that rigid motion is carried to everything below
    // it, posed children included, so a child's transform already holds its parents' motion when its own rotation is
    // solved. World transforms are written directly because nothing recomputes them from locals between the end of the
    // frame and the draw that uses them.
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        const RigBone& bone = acRig.Bones[i];
        // Legs only when the sender's trackers drive them; otherwise the walk animation keeps them.
        if (i >= VRPose::kUpperBoneCount && !acPose.HasLegs)
            continue;
        if (!bone.pNode && !bone.pEntry)
            continue;
        const NiTransform current = bone.World();
        const glm::mat3 currentRotation = ToGlm(current.rotate);
        const glm::mat3 wantedRotation = rootRotation * glm::mat3_cast(acPose.Bones[i]);
        const glm::mat3 delta = wantedRotation * glm::transpose(currentRotation);
        const glm::vec3 pivot = ToGlm(current.translate);

        NiTransform wanted = current;
        wanted.rotate = FromGlm(wantedRotation);
        wanted.translate = FromGlm(ToGlm(wanted.translate) + acWorldOffset);
        bone.WriteWorld(wanted);

        // The local rotation is computed from scratch every frame: the parent's world rotation, inverted, times the
        // world rotation we want. It used to be built by multiplying the previous frame's local by a correction,
        // which never re-derives anything and so never sheds the error it picks up. Thirty times a second for
        // minutes on end, with more bones since the fingers arrived, that drift turns a bone matrix into something
        // that is no longer a rotation, and a skinned body hanging off such a bone stops being drawn while the
        // actor beside it stays perfect. Deriving it fresh cannot drift, and corrects a bone that already has.
        if (bone.pNode)
        {
            NiTransform local = At<NiTransform>(bone.pNode, kLocalOffset);
            void* pParent = At<void*>(bone.pNode, kParentOffset);
            if (pParent)
                local.rotate = FromGlm(glm::transpose(ToGlm(At<NiTransform>(pParent, kWorldOffset).rotate)) * wantedRotation);
            else
                local.rotate = FromGlm(ToGlm(local.rotate) * glm::transpose(currentRotation) * wantedRotation);
            bone.WriteLocal(local);
        }

        for (const RigBone& child : acRig.Descendants[i])
        {
            NiTransform world = child.World();
            world.rotate = FromGlm(delta * ToGlm(world.rotate));
            world.translate = FromGlm(pivot + delta * (ToGlm(world.translate) - pivot) + acWorldOffset);
            child.WriteWorld(world);
        }
    }

    if (acPose.HasFingers)
    {
        constexpr size_t cPerHand = VRPose::kFingersPerHand * VRPose::kBonesPerFinger;
        if (acRig.FingersFound[0])
            WriteFingerRotations(acRig.Fingers[0], acRig.Bones[VRPose::kLeftHand].World(), acPose.Fingers.data());
        if (acRig.FingersFound[1])
            WriteFingerRotations(acRig.Fingers[1], acRig.Bones[VRPose::kRightHand].World(), acPose.Fingers.data() + cPerHand);
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

        // A dead or downed player copy belongs to its ragdoll: posing it fights the death animation and the body
        // ends up standing in the air. A dead NPC is the opposite case. Its owner only sends bones for it while it
        // is being moved over there (dragged, thrown, shoved), and that movement is the whole point of this, so
        // those bones are applied.
        if (pActor->actorState.IsDeadOrDying() || pActor->actorState.IsBleedingOut())
        {
            const ActorExtension* pExtension = pActor->GetExtension();
            if (!pExtension || pExtension->IsRemotePlayer() || pExtension->IsLocalPlayer())
                continue;

            // Only a properly dead body. A downed one (bleeding out) gets back up, and a dying one is playing its
            // death animation; writing bones into either fights the game. Seen reported exactly that on
            // 2026-09-23: "npcs that get downed but cant be killed dont stand back up, they just lay on the floor
            // weirdly". The sender no longer sends for those, and this refuses them even if an old client does.
            if (!pActor->actorState.IsDead())
                continue;

            // The other half of the sender's line, so one log shows the whole chain.
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextSaid;
            auto& next = s_nextSaid[formId];
            if (now >= next)
            {
                next = now + std::chrono::seconds(5);
                spdlog::info("VRBodySync: posing body {:X} from its owner's bones (someone is moving it over there)", formId);
            }
        }

        Rig& rig = s_rigs[formId];
        const bool cGone = rig.pRoot != pRoot || !rig.Valid || !RigIntact(rig);
        // Only worth asking once the rig is known good, and not more than a few times a second.
        const bool cRestructured = !cGone && now >= rig.RestructureAt && !RigStructureUnchanged(rig);

        if (cGone || cRestructured)
        {
            if (cGone && rig.pRoot == pRoot && now < rig.RetryAt)
                continue;
            if (!ResolveRig(pRoot, rig, cGone))
            {
                rig.RetryAt = now + std::chrono::seconds(1);
                continue;
            }
            rig.RestructureAt = now + std::chrono::milliseconds(200);
        }

        if (!IsInView(ToGlm(rig.Bones[VRPose::kSpine2].World().translate)))
            continue;

        // A body whose bones are no longer usable is not drawn, and writing more of our own transforms into it
        // keeps it that way. Let go: the game's animation rewrites those bones within a frame and the body comes
        // back, without the other player having to reconnect. Said once every ten seconds per actor.
        if (!IsBodyUsable(rig))
        {
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextComplaint;
            auto& next = s_nextComplaint[formId];
            if (now >= next)
            {
                next = now + std::chrono::seconds(10);
                const NiTransform& spine = rig.Bones[VRPose::kSpine2].World();
                spdlog::warn("VRBodySync: actor {:X} has a bone the renderer cannot use (spine scale {:.3f}, rotation row {:.4f}); not posing it until the game puts it right",
                             formId, spine.scale, glm::length(ToGlm(spine.rotate)[0]));
            }
            continue;
        }

        // Placing the root first, so the bone world transforms below are built on the right origin. Written here,
        // at the renderer's frame end, for the same reason the bones are: the local ragdoll has already had its
        // turn this frame and would otherwise drag the body back.
        // How far the whole body has to move to be where its owner has it. Applied to every bone by PoseActor,
        // because the renderer follows the bones and not the root: writing the root alone moved nothing at all,
        // which is why nobody could see a dragged body on 2026-09-24.
        glm::vec3 worldOffset{};
        if (pose.HasRootPosition)
        {
            const glm::vec3& wanted = pose.RootPosition;
            if (std::isfinite(wanted.x) && std::isfinite(wanted.y) && std::isfinite(wanted.z))
            {
                NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
                worldOffset = wanted - ToGlm(rootWorld.translate);
                // A body half a cell away is a desync, not a drag, and hauling the skeleton that far would look
                // worse than leaving it where it is.
                if (glm::dot(worldOffset, worldOffset) > 2048.f * 2048.f)
                    worldOffset = glm::vec3{};
                else
                    rootWorld.translate = wanted; // NiPoint3 derives from glm::vec3
            }
        }

        // Hips. The pose is rotations, so a crouch or a lean -- which move the body relative to the root without
        // changing any rotation -- never arrived, and the hip tracker did nothing while the feet followed. Moving
        // every bone puts the hip joint where the sender's is; the thigh and calf rotations being written anyway
        // then swing the feet from there. It has to be the whole body: the spine hangs off NPC COM beside the
        // pelvis rather than below it, so shifting the pelvis alone would pull the legs off the torso.
        if (pose.HasHips)
        {
            const RigBone& pelvis = rig.Bones[VRPose::kPelvis];
            if (pelvis.pNode || pelvis.pEntry)
            {
                const NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
                const glm::vec3 wanted = ToGlm(rootWorld.translate) + ToGlm(rootWorld.rotate) * pose.HipOffset;
                const glm::vec3 delta = wanted - ToGlm(pelvis.World().translate);
                // A hip a body-length away from where this copy has it is a bad read, not a crouch.
                if (std::isfinite(delta.x) && std::isfinite(delta.y) && std::isfinite(delta.z) && glm::dot(delta, delta) < 256.f * 256.f)
                {
                    worldOffset += delta;

                    // The other half of the measurement. Compare with the sender's "local hips" line: the offset
                    // should be the same, and the correction is how far this copy's own animation had the hips
                    // from where they belong.
                    static std::chrono::steady_clock::time_point s_lastHipLog{};
                    if (now - s_lastHipLog >= std::chrono::seconds(5))
                    {
                        s_lastHipLog = now;
                        spdlog::info("VRBodySync: actor {:X} hips wanted at ({:.1f}, {:.1f}, {:.1f}) from its root, moving the body by {:.1f}", formId, pose.HipOffset.x, pose.HipOffset.y,
                                     pose.HipOffset.z, glm::length(delta));
                    }
                }
            }
        }

        PoseActor(rig, pose, worldOffset);
    }
}

float HeadsetAngleTo(const NiPoint3& acPosition) noexcept
{
    const NiTransform* pHmdTransform = HeadsetTransform();
    if (!pHmdTransform)
        return -1.f;

    const glm::vec3 toTarget = ToGlm(acPosition) - ToGlm(pHmdTransform->translate);
    const float distance = glm::length(toTarget);
    if (distance < 1.f)
        return 0.f;

    const glm::vec3 forward = ToGlm(pHmdTransform->rotate)[1]; // Skyrim: X right, Y forward, Z up
    const float offAxis = std::acos(glm::clamp(glm::dot(forward, toTarget / distance), -1.f, 1.f));
    return offAxis * 180.f / glm::pi<float>();
}

std::string DescribeBody(Actor* apActor) noexcept
{
    void* pRoot = apActor ? apActor->GetNiNode() : nullptr;
    if (!pRoot)
        return "no 3D";
    void* pSkeletonRoot = FindSkeletonRoot(pRoot);
    const uint32_t fingerprint = ChildFingerprintOf(AsNode(pRoot));
    const NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
    const NiTransform& skeletonWorld = At<NiTransform>(pSkeletonRoot, kWorldOffset);
    // Two things can hide a body that is otherwise perfect, and this tells them apart.
    //
    // One: the 3D hangs in a subtree that is no longer part of the scene. It still has a parent, so "parent yes"
    // proved nothing; what counts is whether walking up reaches the world. The depth and the topmost node's name
    // say so, and they can be compared between a body that is drawn and one that is not.
    //
    // Two: the bones we pose have collapsed. We write world transforms into them every frame, thirty times a
    // second, and more of them since the fingers arrived. A bone whose world scale has drifted to nothing, or
    // whose rotation is no longer a rotation, takes the skinned body with it while everything attached to the
    // actor still draws, which is exactly what "I see his spells but not his body" looks like. The scale of two
    // posed bones and the length of a row of the spine's rotation (1.000 for a true rotation) show that drift.
    void* pWalk = pRoot;
    uint32_t depth = 0;
    const char* pTopName = "?";
    while (depth < 64)
    {
        void* pParent = At<void*>(pWalk, kParentOffset);
        if (!pParent || !IsReadable(pParent, kNiAVObjectSize))
            break;
        pWalk = pParent;
        ++depth;
        if (const char* pName = GetName(pWalk); pName && IsReadable(pName, 2))
            pTopName = pName;
    }
    // The player is always drawn, so whatever tree the player hangs from is the one the world draws.
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    void* pPlayerRoot = pPlayer ? pPlayer->GetNiNode() : nullptr;
    const char* pSharesScene = pPlayerRoot ? (SceneTopOf(pPlayerRoot) == pWalk ? "yes" : "NO") : "unknown";

    BoneNodes nodes;
    float spineScale = -1.f, headScale = -1.f, spineRowLength = -1.f;
    if (FindBones(pRoot, nodes))
    {
        if (nodes[VRPose::kSpine2])
        {
            const NiTransform& spine = At<NiTransform>(nodes[VRPose::kSpine2], kWorldOffset);
            spineScale = spine.scale;
            spineRowLength = glm::length(ToGlm(spine.rotate)[0]);
        }
        if (nodes[VRPose::kHead])
            headScale = At<NiTransform>(nodes[VRPose::kHead], kWorldOffset).scale;
    }

    // Render state, which no probe has ever looked at.
    //
    // Every cause proposed for the invisible body so far has been *actor* state -- health, scale, the invisibility
    // value, bone drift -- and this probe disproved each one by reading perfect every time. A body can be a
    // flawless actor and still not be drawn, and what decides that is the fade the renderer applies to the node.
    //
    // The offset is not guessed. CommonLibVR puts BSFadeNode's own data at +0x128, but VR's NiNode keeps its
    // children at +0x138, so +0x128 is still inside NiNode here and cannot be the fade: taking it on trust would
    // be the fifth wrong offset on this bug. Instead the words just past the end of a VR NiNode are printed for
    // the copy *and* for the player, whose body is always drawn. Whatever the player reads as "visible" is the
    // value to look for, and the word that differs when a body vanishes is the fade.
    // Why the other player's palm does not land where yours does.
    //
    // VRPose carries bone **rotations** only, and PoseActor keeps each bone's own translation, so the copy's hand
    // ends up wherever the receiver's own skeleton puts it once the sent rotations are applied. Three things can
    // move it from where the sender had it, and they need different fixes, so guessing between them is no use:
    //   1. the skeleton root sits at a different height (VRIK height calibration moves it by translation)
    //   2. the bones are different lengths (a different skeleton, or a scale this does not capture)
    //   3. VRIK moved the hand itself by translation, which rotations can never reproduce
    // Printing the copy's numbers next to the player's own tells them apart: same root height and same arm
    // lengths but a different hand height means (3); a different root height means (1); different arm lengths
    // mean (2). Measured against the 3D root, so the actor's world position does not enter into it.
    const auto describeReach = [](void* apRoot, void* apSkeletonRoot) -> std::string
    {
        if (!apRoot)
            return "none";

        BoneNodes reachNodes;
        if (!FindBones(apRoot, reachNodes))
            return "bones not found";

        const NiTransform& rootTransform = At<NiTransform>(apRoot, kWorldOffset);
        const glm::vec3 rootPos = ToGlm(rootTransform.translate);

        const auto heightOf = [&](void* apNode) -> float { return apNode ? ToGlm(At<NiTransform>(apNode, kWorldOffset).translate).z - rootPos.z : -999.f; };
        const auto lengthBetween = [&](void* apA, void* apB) -> float
        {
            if (!apA || !apB)
                return -1.f;
            return glm::length(ToGlm(At<NiTransform>(apA, kWorldOffset).translate) - ToGlm(At<NiTransform>(apB, kWorldOffset).translate));
        };

        const float skeletonLocalZ = apSkeletonRoot ? At<NiTransform>(apSkeletonRoot, kLocalOffset).translate.z : -999.f;

        return fmt::format("skelRootLocalZ {:.1f}, head {:.1f}, Lhand {:.1f}, Rhand {:.1f}, upperarm {:.1f}, forearm {:.1f}", skeletonLocalZ, heightOf(reachNodes[VRPose::kHead]),
                           heightOf(reachNodes[VRPose::kLeftHand]), heightOf(reachNodes[VRPose::kRightHand]),
                           lengthBetween(reachNodes[VRPose::kLeftUpperArm], reachNodes[VRPose::kLeftForearm]),
                           lengthBetween(reachNodes[VRPose::kLeftForearm], reachNodes[VRPose::kLeftHand]));
    };

    std::string renderWords;
    std::string playerWords;
    const auto dumpFloats = [](void* apNode, std::string& aOut)
    {
        if (!apNode || !IsReadable(apNode, 0x170))
        {
            aOut = "unreadable";
            return;
        }
        for (uint32_t offset = 0x140; offset < 0x160; offset += 4)
        {
            const float value = At<float>(apNode, offset);
            aOut += fmt::format("{}{:X}={:.3f}", aOut.empty() ? "" : " ", offset, std::isfinite(value) ? value : -999.f);
        }
    };
    dumpFloats(pRoot, renderWords);
    dumpFloats(pPlayerRoot, playerWords);

    return fmt::format("3D root {} with {} of {} child slots filled, root world scale {:.3f} at ({:.0f}, {:.0f}, {:.0f}), skeleton root {} world scale {:.3f}; "
                       "{} parents up to '{}' (same scene as the player: {}); spine scale {:.3f} rotation row {:.4f}, head scale {:.3f}; "
                       "copy words [{}]; player words [{}]; copy reach [{}]; player reach [{}]",
                       pRoot, fingerprint & 0xFFFF, fingerprint >> 16, rootWorld.scale, rootWorld.translate.x, rootWorld.translate.y, rootWorld.translate.z,
                       pSkeletonRoot == pRoot ? "missing" : "found", skeletonWorld.scale, depth, pTopName, pSharesScene, spineScale, spineRowLength, headScale, renderWords, playerWords,
                       describeReach(pRoot, pSkeletonRoot == pRoot ? nullptr : pSkeletonRoot), describeReach(pPlayerRoot, pPlayerRoot ? FindSkeletonRoot(pPlayerRoot) : nullptr));
}

bool CaptureLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose) noexcept
{
    aOutPose.HasData = false;

    if (!apPlayer)
        return false;

    void* pRoot = apPlayer->GetNiNode();
    if (!pRoot)
        return false;

    // The bones are looked up once per 3D and kept: the search walks the whole skeleton, armour and physics nodes
    // included, and ran at every send (30 times a second) before. The cache is dropped when the root changes, when
    // a cached node's vtable changes (the game reuses freed node memory), or after 5 s as a safety net.
    static struct
    {
        void* pRoot = nullptr;
        BoneNodes Nodes{};
        std::array<void*, VRPose::kBoneCount> VTables{};
        bool LegsFound = false;
        std::array<FingerEntries, 2> Fingers{};
        std::array<bool, 2> FingersFound{};
        void* pSkeletonRoot = nullptr;
        std::chrono::steady_clock::time_point RefreshAt{};
    } s_local;
    const auto now = std::chrono::steady_clock::now();
    bool cached = s_local.pRoot == pRoot && now < s_local.RefreshAt;
    for (uint32_t i = 0; cached && i < VRPose::kBoneCount; ++i)
        if (s_local.Nodes[i] && *static_cast<void**>(s_local.Nodes[i]) != s_local.VTables[i])
            cached = false;
    if (!cached)
    {
        BoneNodes nodes;
        bool legsFound = false;
        if (!FindBones(pRoot, nodes, &legsFound))
        {
            s_local.pRoot = nullptr;
            return false;
        }
        s_local.pRoot = pRoot;
        s_local.Nodes = nodes;
        s_local.LegsFound = legsFound;
        s_local.pSkeletonRoot = FindSkeletonRoot(pRoot);
        const BoneArrayInfo arrayInfo = ResolveBoneArray(pRoot, nodes, false);
        const uint8_t* pLeftHand = FindEntry(arrayInfo.pArray, arrayInfo.Count, nodes[VRPose::kLeftHand]);
        const uint8_t* pRightHand = FindEntry(arrayInfo.pArray, arrayInfo.Count, nodes[VRPose::kRightHand]);
        s_local.FingersFound[0] = FindFingers(arrayInfo, pLeftHand, s_local.Fingers[0]);
        s_local.FingersFound[1] = FindFingers(arrayInfo, pRightHand, s_local.Fingers[1]);
        static bool s_fingersNoted = false;
        if (!s_fingersNoted)
        {
            s_fingersNoted = true;
            spdlog::info("VRBodySync: local finger bones {} (left {}, right {}); {} flattened bones", s_local.FingersFound[0] && s_local.FingersFound[1] ? "found" : "not found by shape",
                         s_local.FingersFound[0], s_local.FingersFound[1], arrayInfo.Count);
        }
        for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
            s_local.VTables[i] = nodes[i] ? *static_cast<void**>(nodes[i]) : nullptr;
        s_local.RefreshAt = now + std::chrono::seconds(5);
    }
    const BoneNodes& nodes = s_local.Nodes;
    const bool legsFound = s_local.LegsFound;
    const bool cLegs = legsFound && FullBodyTrackingActive();

    const glm::mat3 inverseRoot = glm::transpose(ToGlm(At<NiTransform>(pRoot, kWorldOffset).rotate));

    const uint32_t boneCount = cLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
    for (uint32_t i = 0; i < boneCount; ++i)
    {
        const glm::mat3 boneWorld = ToGlm(At<NiTransform>(nodes[i], kWorldOffset).rotate);
        aOutPose.Bones[i] = glm::normalize(glm::quat_cast(inverseRoot * boneWorld));
    }

    aOutPose.HasLegs = cLegs;

    // Hips: where the pelvis sits relative to the 3D root, in root space. Crouching, leaning and hip sway all move
    // it and none of them change a rotation, which is why a hip tracker did nothing on the other screen while the
    // feet followed. Sent only while the legs are tracked -- without trackers the pelvis is wherever the walk
    // animation put it, and the receiver's own copy of that animation already agrees.
    aOutPose.HasHips = false;
    if (cLegs && nodes[VRPose::kPelvis])
    {
        const NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
        const glm::vec3 pelvis = ToGlm(At<NiTransform>(nodes[VRPose::kPelvis], kWorldOffset).translate);
        const glm::vec3 offset = inverseRoot * (pelvis - ToGlm(rootWorld.translate));
        // A body is a couple of hundred units tall. Anything beyond that is a bone read mid-update, and sending it
        // would throw the copy across the room.
        if (std::isfinite(offset.x) && std::isfinite(offset.y) && std::isfinite(offset.z) && glm::dot(offset, offset) < 512.f * 512.f)
        {
            aOutPose.HasHips = true;
            aOutPose.HipOffset[0] = offset.x;
            aOutPose.HipOffset[1] = offset.y;
            aOutPose.HipOffset[2] = offset.z;

            // Measured, not assumed. If the hips still look wrong next session, this line says whether the sender
            // ever saw them move: crouch and stand, and the height should change by something like the distance
            // actually crouched. If it does not move, the hip tracker is not reaching the pelvis node at all and
            // the problem is upstream of this protocol.
            static std::chrono::steady_clock::time_point s_lastHipLog{};
            if (now - s_lastHipLog >= std::chrono::seconds(5))
            {
                s_lastHipLog = now;
                spdlog::info("VRBodySync: local hips at ({:.1f}, {:.1f}, {:.1f}) from the root", offset.x, offset.y, offset.z);
            }
        }
    }

    // Fingers: only when both hands' bones are known, and only when they changed since the last send or a second has
    // passed (so a player who arrives later still gets them).
    aOutPose.HasFingers = false;
    if (s_local.FingersFound[0] && s_local.FingersFound[1])
    {
        static std::array<Quaternion_NetQuantize, VRPose::kFingerBoneCount> s_lastSent{};
        static std::chrono::steady_clock::time_point s_lastSentAt{};
        constexpr size_t cPerHand = VRPose::kFingersPerHand * VRPose::kBonesPerFinger;
        std::array<glm::quat, VRPose::kFingerBoneCount> rotations{};
        ReadFingerRotations(s_local.Fingers[0], ToGlm(At<NiTransform>(nodes[VRPose::kLeftHand], kWorldOffset).rotate), rotations.data());
        ReadFingerRotations(s_local.Fingers[1], ToGlm(At<NiTransform>(nodes[VRPose::kRightHand], kWorldOffset).rotate), rotations.data() + cPerHand);
        std::array<Quaternion_NetQuantize, VRPose::kFingerBoneCount> quantized{};
        for (size_t i = 0; i < VRPose::kFingerBoneCount; ++i)
            quantized[i] = rotations[i];
        if (quantized != s_lastSent || now - s_lastSentAt >= std::chrono::seconds(1))
        {
            s_lastSent = quantized;
            s_lastSentAt = now;
            aOutPose.HasFingers = true;
            aOutPose.Fingers = quantized;
        }
    }

    // Body size: the skeleton root's world scale, sent when it changes or once a second. Logged the first time and
    // whenever it changes, so the log says what VRIK made of this body.
    aOutPose.HasScale = false;
    if (s_local.pSkeletonRoot)
    {
        static uint16_t s_lastScaleSent = 0;
        static std::chrono::steady_clock::time_point s_lastScaleSentAt{};
        const float worldScale = At<NiTransform>(s_local.pSkeletonRoot, kWorldOffset).scale;
        const uint16_t quantized = static_cast<uint16_t>(glm::clamp(worldScale * 1000.f, 50.f, 20000.f) + 0.5f);
        if (quantized != s_lastScaleSent || now - s_lastScaleSentAt >= std::chrono::seconds(1))
        {
            if (quantized != s_lastScaleSent)
                spdlog::info("VRBodySync: local body scale {:.3f} (skeleton root world scale, actor scale field {})", worldScale, apPlayer->scale);
            s_lastScaleSent = quantized;
            s_lastScaleSentAt = now;
            aOutPose.HasScale = true;
            aOutPose.RootScale = quantized;
        }
    }

    aOutPose.HasData = true;
    return true;
}

bool CaptureBodyPose(Actor* apActor, VRPose& aOutPose) noexcept
{
    aOutPose.HasData = false;

    if (!apActor)
        return false;

    void* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return false;

    // Same caching as the local player, one entry per body: the bone search walks the whole skeleton and would
    // otherwise run at every send. Entries for bodies nobody has touched in a while are dropped.
    struct BodyCapture
    {
        void* pRoot = nullptr;
        BoneNodes Nodes{};
        std::array<void*, VRPose::kBoneCount> VTables{};
        std::chrono::steady_clock::time_point RefreshAt{};
        std::array<Quaternion_NetQuantize, VRPose::kUpperBoneCount> LastSent{};
        std::chrono::steady_clock::time_point LastChangeAt{};
        std::chrono::steady_clock::time_point TouchedAt{};
        glm::vec3 LastPosition{};
        std::chrono::steady_clock::time_point MovedAt{};
        bool Placed = false;
        bool Sent = false;
    };
    static std::unordered_map<uint32_t, BodyCapture> s_bodies;

    const auto now = std::chrono::steady_clock::now();
    for (auto it = s_bodies.begin(); it != s_bodies.end();)
        it = now - it->second.TouchedAt > std::chrono::seconds(30) ? s_bodies.erase(it) : std::next(it);

    BodyCapture& capture = s_bodies[apActor->formID];
    capture.TouchedAt = now;

    bool cached = capture.pRoot == pRoot && now < capture.RefreshAt;
    for (uint32_t i = 0; cached && i < VRPose::kUpperBoneCount; ++i)
        if (capture.Nodes[i] && *static_cast<void**>(capture.Nodes[i]) != capture.VTables[i])
            cached = false;

    if (!cached)
    {
        BoneNodes nodes;
        if (!FindBones(pRoot, nodes))
        {
            capture.pRoot = nullptr;
            return false;
        }
        capture.pRoot = pRoot;
        capture.Nodes = nodes;
        for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
            capture.VTables[i] = nodes[i] ? *static_cast<void**>(nodes[i]) : nullptr;
        capture.RefreshAt = now + std::chrono::seconds(5);
        capture.Sent = false;
    }

    // Bones alone are a bad signal for "someone is handling this body": a corpse settling on the ground, or an
    // actor still playing an animation, changes its bones without anyone touching it. That is why this fired 204
    // times in one session on 2026-09-23 when nobody had dragged much of anything. What a grab really does is
    // move the body, so the gate is displacement of the 3D root, not bone movement.
    const glm::vec3 rootPosition = ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
    if (capture.Placed)
    {
        constexpr float cMovedSquared = 4.f * 4.f; // a settled ragdoll still twitches a little
        const glm::vec3 delta = rootPosition - capture.LastPosition;
        if (glm::dot(delta, delta) >= cMovedSquared)
            capture.MovedAt = now;
    }
    capture.LastPosition = rootPosition;
    capture.Placed = true;

    // Two seconds after it comes to rest, this body stops costing anything at all.
    if (now - capture.MovedAt >= std::chrono::seconds(2))
    {
        capture.Sent = false;
        return false;
    }

    const glm::mat3 inverseRoot = glm::transpose(ToGlm(At<NiTransform>(pRoot, kWorldOffset).rotate));

    std::array<Quaternion_NetQuantize, VRPose::kUpperBoneCount> quantized{};
    for (uint32_t i = 0; i < VRPose::kUpperBoneCount; ++i)
    {
        const glm::mat3 boneWorld = ToGlm(At<NiTransform>(capture.Nodes[i], kWorldOffset).rotate);
        quantized[i] = glm::normalize(glm::quat_cast(inverseRoot * boneWorld));
    }

    // A corpse that has settled costs nothing on the wire. Sending carries on for two seconds after the body comes
    // to rest rather than stopping dead: the receiver only buffers two points, so the last pose has to be repeated
    // a few times to be the one that lands. After that the body is left to its own ragdoll, which is where it was
    // already heading on both sides.
    const bool cChanged = !capture.Sent || quantized != capture.LastSent;
    if (cChanged)
    {
        // One line each time a body starts moving again, so a session log says whether this ever happened at all.
        // Without it the feature is invisible: a grab that works and a grab that does nothing look identical.
        if (!capture.Sent || now - capture.LastChangeAt >= std::chrono::seconds(2))
            spdlog::info("VRBodySync: body {:X} is being moved here, sending its bones to the other players", apActor->formID);
        capture.LastChangeAt = now;
    }
    else if (now - capture.LastChangeAt >= std::chrono::seconds(2))
        return false;

    capture.LastSent = quantized;
    capture.Sent = true;

    for (uint32_t i = 0; i < VRPose::kUpperBoneCount; ++i)
        aOutPose.Bones[i] = quantized[i];

    aOutPose.HasLegs = false;
    aOutPose.HasFingers = false;
    aOutPose.HasScale = false;

    // Where the body is, not just how it is bent. The receiver cannot get this from the reference: a corpse is
    // carried by its ragdoll, and moving the reference leaves the visible body behind.
    aOutPose.HasRootPosition = true;
    aOutPose.RootPosition[0] = rootPosition.x;
    aOutPose.RootPosition[1] = rootPosition.y;
    aOutPose.RootPosition[2] = rootPosition.z;

    aOutPose.HasData = true;
    return true;
}

void SetRemotePose(Actor* apActor, const VRPose& acPose) noexcept
{
    if (!apActor)
        return;

    if (!acPose.HasData)
    {
        // Without pose data the arms fall back to the animation, which is what a sword held up in the vanilla idle
        // on the other player looks like. Say so, at most every 10 s per actor, so the log tells whether the pose
        // never arrived or arrived and was overwritten.
        // Only players send a pose, so only a remote player without one is worth a line.
        if (apActor->GetExtension()->IsRemotePlayer() && !apActor->actorState.IsDeadOrDying())
        {
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextNote;
            const auto now = std::chrono::steady_clock::now();
            auto& next = s_nextNote[apActor->formID];
            if (now >= next)
            {
                next = now + std::chrono::seconds(10);
                spdlog::warn("VRBodySync: no VR pose data for remote actor {:X}, its arms follow the animation", apActor->formID);
            }
        }
        ClearRemotePose(apActor->formID);
        return;
    }

    std::unique_lock lock(s_posesLock);
    RemotePose& pose = s_poses[apActor->formID];
    pose.HasLegs = acPose.HasLegs;
    for (uint32_t i = 0; i < (acPose.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount); ++i)
        pose.Bones[i] = acPose.Bones[i];
    if (acPose.HasFingers)
    {
        pose.HasFingers = true;
        for (size_t i = 0; i < VRPose::kFingerBoneCount; ++i)
            pose.Fingers[i] = acPose.Fingers[i];
    }
    if (acPose.HasScale)
    {
        const float scale = acPose.RootScale / 1000.f;
        if (!pose.HasScale || std::fabs(pose.RootScale - scale) > 0.0005f)
            spdlog::info("VRBodySync: body scale of remote actor {:X} is {:.3f}", apActor->formID, scale);
        pose.HasScale = true;
        pose.RootScale = scale;
    }

    // Deliberately not sticky: the sender only fills this while a dead body is being moved, and when it stops the
    // body should settle on its own ragdoll rather than stay pinned to the last place it was dragged to.
    pose.HasRootPosition = acPose.HasRootPosition;
    if (acPose.HasRootPosition)
        pose.RootPosition = glm::vec3{acPose.RootPosition[0], acPose.RootPosition[1], acPose.RootPosition[2]};

    pose.HasHips = acPose.HasHips;
    if (acPose.HasHips)
        pose.HipOffset = glm::vec3{acPose.HipOffset[0], acPose.HipOffset[1], acPose.HipOffset[2]};
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
uint64_t SkeletonMotionFingerprint(Actor* apActor) noexcept
{
    void* pRoot = apActor ? apActor->GetNiNode() : nullptr;
    if (!pRoot)
        return 0;

    // The same walk the pose code uses; every NiAVObject below the root carries a local transform at kLocalOffset.
    std::vector<void*> nodes;
    CollectNodeDescendants(pRoot, nodes);

    uint64_t hash = 1469598103934665603ull;
    size_t hashed = 0;
    for (void* pNode : nodes)
    {
        const auto* pBytes = reinterpret_cast<const uint8_t*>(&At<NiTransform>(pNode, kLocalOffset));
        for (size_t i = 0; i < sizeof(NiTransform); ++i)
        {
            hash ^= pBytes[i];
            hash *= 1099511628211ull;
        }
        if (++hashed >= 64)
            break;
    }
    return hash;
}
} // namespace VRBodySync
