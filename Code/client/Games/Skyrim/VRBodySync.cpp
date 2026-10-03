#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/VRBodySync.h>
#include <Games/Skyrim/VRHaptics.h>

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
// NiAVObject::worldBound, straight after the world transform (0x7C + 0x34). A NiBound is a centre and a radius.
// Read only with ValidWorldBound below, which refuses anything that is not the size and place of a weapon: the
// offset is inferred from the two transforms either side of it rather than measured, and a wrong read must
// announce itself rather than quietly put a blade through the floor.
constexpr uint32_t kWorldBoundOffset = 0xB0;

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

// Regions VirtualQuery has already found readable, kept while a ReadableRegionScope is open on this thread.
//
// VirtualQuery is a system call, and a slow one in a big process: it works out how far the region around the address
// reaches, and the game's heap regions only grow. GetBoneArray asks it about every entry of a skeleton's bone list
// for each candidate count, a few hundred calls for one answer, and that one step was the whole cost of a local
// skeleton search: 44 ms one minute into a session, 49 ms after ten, 178 ms after twenty (2026-10-03, with the
// bone search itself at 0.0 ms over 135 nodes), and 210 to 284 ms in the three sessions of the night before that
// reached the ten-minute re-search. Almost every entry lies in a region an earlier call has just described.
//
// Only for the length of one scope: a region can be released at any time, and an answer older than the work that
// asked for it is not one to trust.
struct ReadableRegion
{
    uintptr_t Begin = 0;
    uintptr_t End = 0;
};
thread_local std::array<ReadableRegion, 16>* t_pReadableRegions = nullptr;
thread_local size_t t_nextReadableRegion = 0;

struct ReadableRegionScope
{
    std::array<ReadableRegion, 16> Regions{};
    std::array<ReadableRegion, 16>* pOuter = nullptr;

    ReadableRegionScope() noexcept
        : pOuter(t_pReadableRegions)
    {
        if (!pOuter)
            t_pReadableRegions = &Regions;
    }
    ~ReadableRegionScope()
    {
        if (!pOuter)
            t_pReadableRegions = nullptr;
    }
};

// Only used while resolving a skeleton, never per frame: VirtualQuery is a system call.
bool IsReadable(const void* apPointer, size_t aSize) noexcept
{
    if (!apPointer)
        return false;

    const auto cBegin = reinterpret_cast<uintptr_t>(apPointer);
    if (t_pReadableRegions)
        for (const ReadableRegion& region : *t_pReadableRegions)
            if (cBegin >= region.Begin && cBegin + aSize <= region.End)
                return true;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apPointer, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return false;

    constexpr DWORD cReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & cReadable))
        return false;

    if (t_pReadableRegions)
    {
        const auto cRegionBegin = reinterpret_cast<uintptr_t>(info.BaseAddress);
        (*t_pReadableRegions)[t_nextReadableRegion++ % t_pReadableRegions->size()] = ReadableRegion{cRegionBegin, cRegionBegin + info.RegionSize};
    }

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

// How many objects hang under this one, counting itself, up to aCap. The same walk as FindShallowest.
size_t CountNodes(void* apStart, const size_t aCap) noexcept
{
    if (!apStart)
        return 0;

    TiltedPhoques::Vector<void*> queue;
    queue.push_back(apStart);
    size_t head = 0;
    for (; head < queue.size() && head < aCap; ++head)
    {
        void* pNode = AsNode(queue[head]);
        if (!pNode)
            continue;

        void** pChildren = At<void**>(pNode, kChildrenOffset + 0x8);
        const uint16_t capacity = At<uint16_t>(pNode, kChildrenOffset + 0x10);
        for (uint16_t i = 0; pChildren && i < capacity; ++i)
            if (pChildren[i])
                queue.push_back(pChildren[i]);
    }
    return head;
}

// TEMPORARY (2026-10-03): what makes a search slow. The ten-minute re-search cost 210, 284 and 239 ms in the three
// sessions that reached it on 2026-10-02/03, where searches early in a session cost 6 to 23 ms: something keeps
// adding to the player's 3D. Called only after a slow search.
void DescribeBigTree(void* apRoot, const double aBonesMs, const double aArrayMs, const double aTotalMs) noexcept
{
    struct Branch
    {
        const char* pName;
        size_t Count;
    };
    TiltedPhoques::Vector<Branch> branches;
    if (void* pNode = AsNode(apRoot))
    {
        void** pChildren = At<void**>(pNode, kChildrenOffset + 0x8);
        const uint16_t capacity = At<uint16_t>(pNode, kChildrenOffset + 0x10);
        for (uint16_t i = 0; pChildren && i < capacity; ++i)
            if (pChildren[i])
                branches.push_back(Branch{GetName(pChildren[i]), CountNodes(pChildren[i], 400000)});
    }
    std::sort(branches.begin(), branches.end(), [](const Branch& a, const Branch& b) { return a.Count > b.Count; });

    std::string biggest;
    for (size_t i = 0; i < branches.size() && i < 4; ++i)
        biggest += fmt::format("{}{} ({})", i ? ", " : "", branches[i].pName ? branches[i].pName : "?", branches[i].Count);
    void* pSkeletonRoot = FindShallowest(apRoot, "NPC Root [Root]");
    spdlog::info("VRBodySync: slow skeleton search, {:.1f} ms ({:.1f} ms finding the bones, {:.1f} ms checking the flattened bone list): {} objects under "
                 "the body's root, {} under NPC Root; {} branches at the top, the biggest {}",
                 aTotalMs, aBonesMs, aArrayMs, CountNodes(apRoot, 400000), CountNodes(pSkeletonRoot, 400000), branches.size(), biggest);
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
    // The whole pose is a position and nothing else (see VRPose::NoBones): a skeleton the bone search cannot read.
    bool NoBones = false;
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
    // The owner's own hand positions, for the measurement only (see VRPose::HasHandCheck). Never posed from.
    bool HasHandCheck = false;
    glm::vec3 HandOffsets[2]{};
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
    ReadableRegionScope regions; // a few hundred readability questions, almost all about the same few regions
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
// Move a whole 3D tree by an offset, without knowing a single bone's name.
//
// For a body VRPose::NoBones describes: a Dwemer automaton, a spider, anything whose skeleton the name search
// cannot read. The renderer follows the bones and not the root -- writing the root alone moved nothing at all,
// which is the lesson of 2026-09-24 -- so every node under it is shifted too, and the flattened bone tree with
// them where there is one. Rotations are left exactly as the local animation has them; only where the thing is
// changes, which is all a drag is.
void ShiftTree(void* apRoot, const glm::vec3& acWorldOffset) noexcept
{
    if (!apRoot)
        return;

    NiTransform& rootWorld = At<NiTransform>(apRoot, kWorldOffset);
    rootWorld.translate = FromGlm(ToGlm(rootWorld.translate) + acWorldOffset);

    TiltedPhoques::Vector<void*> queue;
    queue.push_back(apRoot);
    for (size_t head = 0; head < queue.size() && head < 8192; ++head)
    {
        void* pObject = queue[head];
        if (head > 0)
        {
            NiTransform& world = At<NiTransform>(pObject, kWorldOffset);
            world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
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

    // Bones that were flattened away exist only in the tree's own array and have no node to walk to.
    uint32_t count = 0;
    if (uint8_t* pArray = GetBoneArray(FindByRtti(apRoot, "BSFlattenedBoneTree"), count))
    {
        for (uint32_t i = 0; i < count && i < kMaxBones; ++i)
        {
            uint8_t* pEntry = pArray + static_cast<size_t>(i) * kBoneEntrySize;
            if (At<void*>(pEntry, kBoneEntryNode))
                continue; // already shifted above, through its node
            NiTransform& world = At<NiTransform>(pEntry, kBoneEntryWorld);
            world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
        }
    }
}

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
    const bool cHasOffset = glm::dot(acWorldOffset, acWorldOffset) > 0.0001f;

    // Parents first. Each bone is turned about its own position, and that rigid motion is carried to everything below
    // it, posed children included, so a child's transform already holds its parents' motion when its own rotation is
    // solved. World transforms are written directly because nothing recomputes them from locals between the end of the
    // frame and the draw that uses them.
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        const RigBone& bone = acRig.Bones[i];
        // Legs only when the sender's trackers drive them; otherwise the walk animation keeps them.
        if (i >= VRPose::kUpperBoneCount && !acPose.HasLegs)
        {
            // ...but a body being dragged is still being dragged from the waist down. A corpse never carries legs
            // (HasLegs is false for one), so the offset below reached the spine and the arms and stopped there:
            // the top half went with the hands and the bottom half stayed on the floor, which is the body
            // stretching into a long thing that Seen watched on 2026-09-26 at 09:43.
            //
            // Only the pelvis is shifted, with everything under it -- the thighs, calves and feet are its
            // descendants, so shifting them again by their own index would move them twice.
            if (cHasOffset && i == VRPose::kPelvis && (bone.pNode || bone.pEntry))
            {
                NiTransform pelvisWorld = bone.World();
                pelvisWorld.translate = FromGlm(ToGlm(pelvisWorld.translate) + acWorldOffset);
                bone.WriteWorld(pelvisWorld);

                for (const RigBone& below : acRig.Descendants[i])
                {
                    if (!below.pNode && !below.pEntry)
                        continue;
                    NiTransform world = below.World();
                    world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
                    below.WriteWorld(world);
                }
            }
            continue;
        }
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


// ---------------------------------------------------------------------------------------------------------------
// Weapon touch: feeling somebody else's blade against yours.
//
// Emma, 2026-09-26: "when my hand goes against Lydia's axe I do not feel it, even when I have a sword". Nothing in
// her list does this. PLANCK gives an NPC's *body*; the parry mod only fires on an incoming attack and scales its
// pulse by the stamina the parry costs, so resting a blade on a held axe is silent by design; HIGGS is hands
// against grabbable objects. And for another *player's* weapon nothing but this mod could do it anyway, because
// only this mod knows where that weapon is.
//
// The probe of the same day proved a pulse reaches the controllers (29 of them, both hands), so this is built on
// a fact rather than an assumption about OpenVR's input model.
//
// Nothing here guesses at geometry. A weapon is a line from the hand's attach node to the far end of whatever is
// attached below it, and that far end is *measured* by walking the children and taking the most distant one --
// using the same node offsets the body sync has been reading correctly all week. A weapon with no child nodes
// falls back to a point at the node, and says so once, so the log tells us which case the game actually gives.
struct Segment
{
    glm::vec3 A{};
    glm::vec3 B{};
    bool Valid = false;
};

// Distance between two line segments. The usual clamped-parameter solution; degenerate segments (a point) fall
// out of it correctly because the denominators are guarded.
float SegmentDistance(const Segment& acP, const Segment& acQ) noexcept
{
    const glm::vec3 d1 = acP.B - acP.A;
    const glm::vec3 d2 = acQ.B - acQ.A;
    const glm::vec3 r = acP.A - acQ.A;
    const float a = glm::dot(d1, d1);
    const float e = glm::dot(d2, d2);
    const float f = glm::dot(d2, r);

    float s = 0.f, t = 0.f;
    constexpr float cEps = 1e-5f;

    if (a <= cEps && e <= cEps)
        return glm::length(acP.A - acQ.A);

    if (a <= cEps)
    {
        t = glm::clamp(f / e, 0.f, 1.f);
    }
    else
    {
        const float c = glm::dot(d1, r);
        if (e <= cEps)
        {
            s = glm::clamp(-c / a, 0.f, 1.f);
        }
        else
        {
            const float b = glm::dot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > cEps ? glm::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
            t = (b * s + f) / e;
            if (t < 0.f)
            {
                t = 0.f;
                s = glm::clamp(-c / a, 0.f, 1.f);
            }
            else if (t > 1.f)
            {
                t = 1.f;
                s = glm::clamp((b - c) / a, 0.f, 1.f);
            }
        }
    }

    return glm::length((acP.A + d1 * s) - (acQ.A + d2 * t));
}

// How long the thing in this hand is, and which way it points.
//
// The first attempt walked the child nodes and took the most distant one. On Emma's session of 2026-09-26 17:19
// that found nothing at all -- "'WEAPON' has nothing measurable below it" -- and the weapon collapsed to a point
// at the grip. That is why the contact felt random: a sword was a dot in her fist, so it only fired when her
// *hilt* came within 14 cm of Lydia's, and hitting the axe with the middle of the blade did nothing.
//
// A weapon mesh is usually one geometry attached at the grip: no child nodes to find, because the blade lives in
// the vertices. What does describe those vertices is the node's world bound -- a centre and a radius. The centre
// of a sword's bound sits down the middle of the blade, so the grip and the centre together give both the
// direction and, doubled, the length.
struct NiBoundRead
{
    glm::vec3 Centre{};
    float Radius = 0.f;
    bool Valid = false;
};

NiBoundRead ReadWorldBound(void* apObject, const glm::vec3& acGrip) noexcept
{
    NiBoundRead out;
    if (!apObject)
        return out;

    const glm::vec3 centre = ToGlm(At<NiPoint3>(apObject, kWorldBoundOffset));
    const float radius = At<float>(apObject, kWorldBoundOffset + 0xC);

    // What a weapon actually looks like. A dagger is about 20 units, a greatsword about 90; anything outside
    // this is not a bound, it is whatever else lives at that offset.
    if (!std::isfinite(radius) || radius < 4.f || radius > 300.f)
        return out;
    if (!std::isfinite(centre.x) || !std::isfinite(centre.y) || !std::isfinite(centre.z))
        return out;
    if (glm::distance(centre, acGrip) > 400.f)
        return out;

    out.Centre = centre;
    out.Radius = radius;
    out.Valid = true;
    return out;
}

// Find the world bound by its shape, because guessing its offset was wrong.
//
// 0xB0 -- straight after the world transform, which is where SE keeps it -- reads centre (-0.3, -0.9, 0.1) and
// radius **-0.9** on Seen's VR client. A negative radius is not a radius, and that triple is almost exactly the
// node's first rotation axis, so the read lands inside the transform. VR's NiAVObject is 0x28 bytes larger than
// SE's and the bound is not where SE keeps it.
//
// Rather than pick another number and ship it, this walks the object once and reports every offset whose four
// floats actually look like a bound around this weapon: a centre within a few hundred units of the grip and a
// radius the size of a blade. One session names the offset; then it becomes a constant and this goes away.
void ReportWeaponShape(void* apAttach, const glm::vec3& acGrip, const NiBoundRead& acAtGuess) noexcept
{
    static bool s_said = false;
    if (s_said)
        return;
    s_said = true;

    // Once per session, so a VirtualQuery is affordable -- and required, because this walks past the end of what
    // the client models. Reading unmapped memory is how a diagnostic becomes the bug it was sent to find.
    if (!IsReadable(apAttach, kNiAVObjectSize + 0x40))
    {
        spdlog::warn("VRWeaponTouch shape: this node is not readable far enough to look for a bound; not looking.");
        return;
    }

    const glm::mat3 rot = ToGlm(At<NiTransform>(apAttach, kWorldOffset).rotate);
    spdlog::info("VRWeaponTouch shape: grip ({:.1f}, {:.1f}, {:.1f}); the guess at +{:X} was {}; node axes x({:.2f}, {:.2f}, {:.2f}) y({:.2f}, {:.2f}, {:.2f}) z({:.2f}, {:.2f}, {:.2f})", acGrip.x,
                 acGrip.y, acGrip.z, kWorldBoundOffset, acAtGuess.Valid ? "accepted" : "rejected", rot[0][0], rot[0][1], rot[0][2], rot[1][0], rot[1][1], rot[1][2], rot[2][0], rot[2][1], rot[2][2]);

    // The attach node alone found nothing on three separate sessions (2026-09-26 19:19, 19:38 and 09-27 09:07),
    // so the geometry hanging under it is the next place to look: a mesh's bound belongs to the mesh, not to the
    // empty node it is parented to. Each candidate is checked for readability of its own, because a child here
    // may be any NetImmerse object and not all of them are the size of an NiAVObject.
    const auto scan = [&acGrip](void* pObject, const char* acpWhat, std::string& aHits)
    {
        if (!IsReadable(pObject, kNiAVObjectSize + 0x40))
            return;
        for (uint32_t offset = 0x80; offset + 0x10 <= kNiAVObjectSize; offset += 4)
        {
            const glm::vec3 centre = ToGlm(At<NiPoint3>(pObject, offset));
            const float radius = At<float>(pObject, offset + 0xC);
            if (!std::isfinite(radius) || radius < 4.f || radius > 300.f)
                continue;
            if (!std::isfinite(centre.x) || !std::isfinite(centre.y) || !std::isfinite(centre.z))
                continue;
            const float away = glm::distance(centre, acGrip);
            if (away > 400.f)
                continue;
            aHits += fmt::format("{} +{:X}: centre {:.0f} from the grip, radius {:.1f}; ", acpWhat, offset, away, radius);
        }
    };

    std::string hits;
    scan(apAttach, "attach", hits);

    // One level of children is enough: the weapon mesh is parented straight to the attach node.
    int child = 0;
    if (void* pNode = AsNode(apAttach))
    {
        void** pChildren = At<void**>(pNode, kChildrenOffset + 0x8);
        const uint16_t capacity = At<uint16_t>(pNode, kChildrenOffset + 0x10);
        for (uint16_t i = 0; pChildren && i < capacity && child < 6; ++i)
        {
            if (!pChildren[i])
                continue;
            ++child;
            const char* pName = GetName(pChildren[i]);
            scan(pChildren[i], pName && *pName ? pName : "child", hits);
        }
    }

    if (hits.empty())
        spdlog::warn("VRWeaponTouch shape: nothing in the attach node or its {} children looks like a bounding sphere. The blade cannot be measured this way.", child);
    else
        spdlog::info("VRWeaponTouch shape: offsets that do look like a bound -- {}", hits);
}

// What one actor is holding, plus the hands themselves -- "my hand against her axe" is the request, and a hand
// is just a very short weapon.
//
// The searching and the reading are deliberately separated. The first version of this did four full skeleton
// searches per nearby actor **per frame**, each one a breadth-first walk making a virtual call on every node it
// touched -- and its own comment claimed it did not. For sixteen actors at 45 fps that is millions of virtual
// calls a second, on objects other threads create and destroy as the game streams actors in and out. Emma's
// crash of 2026-09-26 18:26 is the first in eight days and thirty-three crashes with HIGGS on the stack, and it
// landed 68 ms after two actors were deleted during her respawn. That is not proof, and the stack has no frame
// of ours on it, but walking freed node trees at that rate is a defect whether or not it caused that crash.
//
// So: the search runs at most twice a second, and what it stores is checked before it is trusted. Between
// searches only plain memory is read -- a transform and a bound -- which costs nothing and cannot call into a
// freed object.
struct HeldSide
{
    void* pAttach = nullptr;   // the WEAPON / SHIELD node, if anything is equipped
    void* pAttachVTable = nullptr;
    void* pTipNode = nullptr;  // a distinct far node (a bow limb), when the weapon has one
    void* pTipVTable = nullptr;
    void* pHand = nullptr;
    void* pHandVTable = nullptr;
    bool UseBound = false;     // no far node: the tip comes from the world bound instead
};

struct Holding
{
    HeldSide Right{};
    HeldSide Left{};
    Segment RightWeapon{};
    Segment LeftWeapon{};
    Segment RightHand{};
    Segment LeftHand{};
    std::chrono::steady_clock::time_point SearchAt{};
    void* pRoot = nullptr;
};

// Skyrim hands a freed node's memory straight to the next allocation, so a pointer that is still non-null may
// belong to something else entirely. The vtable it had when it was found is the cheapest witness to that.
bool StillTheSame(void* apNode, void* apVTable) noexcept
{
    return apNode && apVTable && *static_cast<void**>(apNode) == apVTable;
}

void SearchSide(void* apRoot, const char* acpAttachName, const char* acpHandName, HeldSide& aSide) noexcept
{
    aSide = HeldSide{};

    if (void* pHand = FindShallowest(apRoot, acpHandName))
    {
        aSide.pHand = pHand;
        aSide.pHandVTable = *static_cast<void**>(pHand);
    }

    void* pAttach = FindShallowest(apRoot, acpAttachName);
    if (!pAttach)
        return;

    aSide.pAttach = pAttach;
    aSide.pAttachVTable = *static_cast<void**>(pAttach);

    // A weapon made of several nodes (a bow's limbs, a staff's head) has a far node to track, which beats a
    // bounding sphere because it is an actual position. One made of a single mesh does not, and its blade is
    // described only by the bound -- which is the case Emma hit: "'WEAPON' has nothing measurable below it".
    const glm::vec3 grip = ToGlm(At<NiTransform>(pAttach, kWorldOffset).translate);
    float best = 0.f;

    TiltedPhoques::Vector<void*> queue;
    queue.push_back(pAttach);
    for (size_t head = 0; head < queue.size() && head < 256; ++head)
    {
        void* pObject = queue[head];
        if (head > 0)
        {
            const float d = glm::distance(grip, ToGlm(At<NiTransform>(pObject, kWorldOffset).translate));
            if (d > best && d < 400.f)
            {
                best = d;
                aSide.pTipNode = pObject;
            }
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

    if (aSide.pTipNode && best > 8.f)
    {
        aSide.pTipVTable = *static_cast<void**>(aSide.pTipNode);
        return;
    }

    aSide.pTipNode = nullptr;
    aSide.UseBound = false; // see ReadSide: nothing is read from a bound on the per-frame path
    ReportWeaponShape(pAttach, grip, NiBoundRead{});
}

// Per frame. Nothing here searches, and nothing here makes a virtual call.
void ReadSide(const HeldSide& acSide, Segment& aWeapon, Segment& aHand) noexcept
{
    aWeapon.Valid = false;
    aHand.Valid = false;

    if (StillTheSame(acSide.pHand, acSide.pHandVTable))
    {
        const glm::vec3 p = ToGlm(At<NiTransform>(acSide.pHand, kWorldOffset).translate);
        if (std::isfinite(p.x))
        {
            aHand.A = p;
            aHand.B = p;
            aHand.Valid = true;
        }
    }

    if (!StillTheSame(acSide.pAttach, acSide.pAttachVTable))
        return;

    const glm::vec3 grip = ToGlm(At<NiTransform>(acSide.pAttach, kWorldOffset).translate);
    if (!std::isfinite(grip.x))
        return;

    glm::vec3 tip = grip;

    if (StillTheSame(acSide.pTipNode, acSide.pTipVTable))
    {
        tip = ToGlm(At<NiTransform>(acSide.pTipNode, kWorldOffset).translate);
    }
    else
    {
        // No far node, so no blade -- and deliberately no bounding-sphere read here any more.
        //
        // That read was added at 17:43 on 2026-09-26 and is the one thing present in the builds that crashed and
        // absent from the ones that did not: f8c31b6 ran two hours clean and bb5b10b one hour clean, both without
        // it; a469765 and 52cbc9b crashed three times between them, always within half a second of remote actors
        // being deleted. It was a raw sixteen-byte read at an offset inferred rather than measured, running every
        // frame on every weapon node near the player, and Seen's log had already reported it returning a negative
        // radius -- so it was buying nothing and costing sessions.
        //
        // The hand segment still covers this side, which is what bb5b10b did for an hour without incident. The
        // blade comes back when SearchSide's one-shot scan names the real offset.
        return;
    }

    if (!std::isfinite(tip.x) || glm::distance(grip, tip) > 400.f)
        return;

    aWeapon.A = grip;
    aWeapon.B = tip;
    aWeapon.Valid = true;
}

// The cache, and the lock it should always have had.
//
// This mod's actor work runs on a thread pool -- eight distinct thread ids in one of Emma's sessions, and the
// code around it says so in as many words ("Serialize, Actors are multi-threaded" in BehaviorVar::Patch; a
// shared_mutex over the pose map here). The first version of this cache was a bare std::unordered_map that
// every one of those threads inserted into and erased from, with no lock -- while the little vector of
// candidates beside it *was* locked, so the hazard was recognised and then missed on the bigger structure.
//
// Concurrent insert and erase on an unordered_map corrupts the heap. A corrupted heap does not fault where it
// was corrupted; it faults later, in whatever code next walks the damaged allocation, reading values like
// 0xFFFFFFFFFFFFFFFF. That is why the crashes of 2026-09-26 landed at four different addresses inside the game
// with no frame of ours on the stack, and why they looked like the game's fault.
//
// It is returned by value now, so nothing holds a pointer into the map after the lock is released.
std::mutex s_heldLock;

bool ResolveHolding(Actor* apActor, Holding& aOut) noexcept
{
    static std::unordered_map<uint32_t, Holding> s_held;

    if (!apActor)
        return false;
    void* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return false;

    const auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(s_heldLock);

    Holding& held = s_held[apActor->formID];

    // Twice a second, or whenever the 3D was rebuilt under us. Everything else is reads.
    if (held.pRoot != pRoot || now >= held.SearchAt)
    {
        held.pRoot = pRoot;
        held.SearchAt = now + std::chrono::milliseconds(500);
        SearchSide(pRoot, "WEAPON", "NPC R Hand [RHnd]", held.Right);
        SearchSide(pRoot, "SHIELD", "NPC L Hand [LHnd]", held.Left);
    }

    ReadSide(held.Right, held.RightWeapon, held.RightHand);
    ReadSide(held.Left, held.LeftWeapon, held.LeftHand);

    if (s_held.size() > 64)
    {
        for (auto it = s_held.begin(); it != s_held.end();)
            it = (it->second.SearchAt + std::chrono::seconds(10) < now) ? s_held.erase(it) : std::next(it);
    }

    aOut = held;
    return true;
}

// Everything the local player could touch this frame, filled on the game thread and read at the end of it.
std::mutex s_touchLock;
TiltedPhoques::Vector<uint32_t> s_touchCandidates;

} // namespace

namespace VRBodySync
{
// TEMPORARY placement note: this runs from the game thread, where the actor list is safe to walk, rather than at
// the renderer's frame end where the body sync lives. A frame-old transform is nothing for a haptic pulse.
void BeginWeaponTouch() noexcept
{
    std::lock_guard lock(s_touchLock);
    s_touchCandidates.clear();
}

void ConsiderForWeaponTouch(Actor* apActor) noexcept
{
    if (!apActor)
        return;

    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || apActor == pPlayer)
        return;

    // Only what is within reach. A blade is under two metres and an arm is less; 300 units of slack is generous
    // and keeps the per-frame work to the two or three actors actually next to you.
    const glm::vec3 delta = static_cast<glm::vec3>(apActor->position) - static_cast<glm::vec3>(pPlayer->position);
    if (glm::dot(delta, delta) > 300.f * 300.f)
        return;

    std::lock_guard lock(s_touchLock);
    if (s_touchCandidates.size() < 16)
        s_touchCandidates.push_back(apActor->formID);
}

void EndWeaponTouch() noexcept
{
    TiltedPhoques::Vector<uint32_t> candidates;
    {
        std::lock_guard lock(s_touchLock);
        candidates = s_touchCandidates;
    }
    if (candidates.empty())
        return;

    // One thread at a time through the whole pass. Another already doing it means this frame's answer is
    // already being worked out; a second copy of it is waste, not safety.
    static std::mutex s_passLock;
    std::unique_lock pass(s_passLock, std::try_to_lock);
    if (!pass.owns_lock())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    // Not while dying, respawning or loading. The 3D is being torn down and rebuilt through all of those, which
    // is exactly where Emma's crash of 18:26 sat -- 0.7 s after "PlayerService: respawning player" and 68 ms
    // after two actors were deleted. Nothing about feeling a blade is worth a frame taken during a teardown.
    if (pPlayer->actorState.IsDeadOrDying() || pPlayer->actorState.IsBleedingOut())
        return;

    Holding mine;
    if (!ResolveHolding(pPlayer, mine))
        return;

    // How close counts as contact. A weapon mesh is thinner than this, but the segment is a line down its middle
    // and both blades have width; 10 units is about 14 cm, which is a touch rather than a near miss.
    constexpr float cTouch = 10.f;

    struct Side
    {
        const Segment* pWeapon;
        const Segment* pHand;
        bool RightHand;
    };
    const Side sides[2] = {{&mine.RightWeapon, &mine.RightHand, true}, {&mine.LeftWeapon, &mine.LeftHand, false}};

    // Safe only because the pass above is serialised.
    static float s_lastDistance[2] = {1e9f, 1e9f};
    static std::chrono::steady_clock::time_point s_nextLog{};
    const auto now = std::chrono::steady_clock::now();

    for (const Side& side : sides)
    {
        if (!side.pWeapon->Valid && !side.pHand->Valid)
            continue;

        float nearest = 1e9f;
        uint32_t nearestId = 0;
        const char* pWhat = "?";

        for (const uint32_t formId : candidates)
        {
            Actor* pOther = Cast<Actor>(TESForm::GetById(formId));
            if (!pOther)
                continue;
            Holding theirs;
            if (!ResolveHolding(pOther, theirs))
                continue;

            for (const Segment* pTheir : {&theirs.RightWeapon, &theirs.LeftWeapon, &theirs.RightHand, &theirs.LeftHand})
            {
                if (!pTheir->Valid)
                    continue;
                for (const Segment* pMineSide : {side.pWeapon, side.pHand})
                {
                    if (!pMineSide->Valid)
                        continue;
                    const float d = SegmentDistance(*pMineSide, *pTheir);
                    if (d < nearest)
                    {
                        nearest = d;
                        nearestId = formId;
                        pWhat = pMineSide == side.pWeapon ? "weapon" : "hand";
                    }
                }
            }
        }

        const int slot = side.RightHand ? 0 : 1;
        const float previous = s_lastDistance[slot];
        s_lastDistance[slot] = nearest;

        if (nearest > cTouch)
            continue;

        // Strength from how hard the contact is: how fast the gap is closing, plus how deep the overlap is. A
        // blade resting against another still buzzes faintly, which is the point -- it is touch, not a hit.
        const float closing = glm::max(0.f, previous - nearest);
        const float depth = (cTouch - nearest) / cTouch;
        const float strength = glm::clamp(0.25f + depth * 0.45f + closing * 0.08f, 0.f, 1.f);

        VRHaptics::Pulse(side.RightHand, static_cast<uint16_t>(strength * 3999.f));

        if (now >= s_nextLog)
        {
            s_nextLog = now + std::chrono::seconds(5);
            spdlog::info("VRWeaponTouch: {} {} is {:.1f} units from something on {:X}, closing {:.1f}; pulsing at {:.2f}", side.RightHand ? "right" : "left", pWhat, nearest, nearestId, closing,
                         strength);
        }
    }
}

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
        // A dead body being moved by its owner, set below. Every way the pose for one can be skipped after the "posing
        // body" line is named for it: that line used to be the only one, and it is logged *before* four checks that each
        // skip the pose without a word. On 2026-09-30 Seen's side logged "posing body FF0010E1" for Emma's dragged troll
        // and he saw it not move at all -- which of the four it was could not be told.
        bool isBody = false;
        const auto whyNotPosed = [&isBody, &now, formId](const char* acpWhy, const float aValue = 0.f)
        {
            if (!isBody)
                return;
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextWhy;
            auto& next = s_nextWhy[formId];
            if (now < next)
                return;
            next = now + std::chrono::seconds(5);
            spdlog::info("VRBodySync: body {:X} not posed this time: {} ({:.0f})", formId, acpWhy, aValue);
        };

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
            isBody = true;

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

        // A body with no readable skeleton, sent as a position and nothing else. There is no rig to resolve and
        // none is needed: shift the whole tree by however far the owner has it from where this side does.
        if (pose.NoBones)
        {
            if (!pose.HasRootPosition)
                continue;

            const glm::vec3 wanted{pose.RootPosition[0], pose.RootPosition[1], pose.RootPosition[2]};
            if (!std::isfinite(wanted.x) || !std::isfinite(wanted.y) || !std::isfinite(wanted.z))
                continue;

            const glm::vec3 offset = wanted - ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
            // Half a cell apart is a desync, not a drag.
            if (glm::dot(offset, offset) > 2048.f * 2048.f || glm::dot(offset, offset) < 0.0001f)
                continue;

            ShiftTree(pRoot, offset);

            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextBoneless;
            auto& nextBoneless = s_nextBoneless[formId];
            if (now >= nextBoneless)
            {
                nextBoneless = now + std::chrono::seconds(5);
                spdlog::info("VRBodySync: body {:X} has no humanoid skeleton; moved its whole tree {:.1f} units to where its owner has it", formId, glm::length(offset));
            }
            continue;
        }

        if (cGone || cRestructured)
        {
            if (cGone && rig.pRoot == pRoot && now < rig.RetryAt)
                continue;
            if (!ResolveRig(pRoot, rig, cGone))
            {
                rig.RetryAt = now + std::chrono::seconds(1);
                whyNotPosed("its skeleton could not be matched to the bones being sent");
                continue;
            }
            rig.RestructureAt = now + std::chrono::milliseconds(200);
        }

        if (!IsInView(ToGlm(rig.Bones[VRPose::kSpine2].World().translate)))
        {
            whyNotPosed("out of view");
            continue;
        }

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
            whyNotPosed("a bone the renderer cannot use");
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
                {
                    // Bent but not moved: the prime suspect for a dragged body that "did not move at all", since each
                    // side's ragdoll drops a corpse on its own and the two can land well apart.
                    whyNotPosed("more than 2048 units from where its owner has it, so bent in place and not moved; units apart",
                                glm::length(worldOffset));
                    worldOffset = glm::vec3{};
                }
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

        // Hands, measured after posing so it reports what is actually being shown, and against the owner's own
        // numbers. Both sides are the distance from the same person's 3D root in that root's own space, so the
        // three axes are directly comparable: a difference in z is a hand held at the wrong height, a difference
        // in x or y is one held too far forward or out to the side.
        if (pose.HasHandCheck && (rig.Bones[VRPose::kLeftHand].pNode || rig.Bones[VRPose::kLeftHand].pEntry))
        {
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextHandLog;
            auto& nextHandLog = s_nextHandLog[formId];
            if (now >= nextHandLog)
            {
                nextHandLog = now + std::chrono::seconds(5);
                const NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
                const glm::mat3 inverseRoot = glm::transpose(ToGlm(rootWorld.rotate));
                const glm::vec3 rootAt = ToGlm(rootWorld.translate);
                const glm::vec3 copyLeft = inverseRoot * (ToGlm(rig.Bones[VRPose::kLeftHand].World().translate) - rootAt);
                const glm::vec3 copyRight = inverseRoot * (ToGlm(rig.Bones[VRPose::kRightHand].World().translate) - rootAt);
                spdlog::info("VRBodySync: actor {:X} hands -- owner L({:.1f}, {:.1f}, {:.1f}) R({:.1f}, {:.1f}, {:.1f}); copy L({:.1f}, {:.1f}, {:.1f}) R({:.1f}, {:.1f}, {:.1f})", formId,
                             pose.HandOffsets[0].x, pose.HandOffsets[0].y, pose.HandOffsets[0].z, pose.HandOffsets[1].x, pose.HandOffsets[1].y, pose.HandOffsets[1].z, copyLeft.x, copyLeft.y,
                             copyLeft.z, copyRight.x, copyRight.y, copyRight.z);
            }
        }
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
    // Every step up the parents and every bone is checked with IsReadable; within one description the answers are
    // reused. Unshared, this was the 10 to 18 ms "RunRemotePlayerDiag" warning each time a player copy appeared
    // (2026-10-03), up to 80 ms in older logs. See IsReadable.
    ReadableRegionScope regions;
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
    // a cached node's vtable or name changes (the game reuses freed node memory), or after 60 s as a safety net.
    //
    // The safety net was 5 s, and the search it repeats -- up to 8192 nodes per bone, a string compare at each, for 19
    // bones -- costs about 50 ms on a FUS body. Emma's session of 2026-09-30 has that as a "Mod update took ~50 ms,
    // slowest section RunLocalUpdates" warning 300 times, on an exact 5.0 s period: the dropped frames that showed up
    // in every 30 s window as "6 frames over 50 ms". The 5 s net was only there for a freed node reused by another
    // node of the same type, which the vtable check cannot see; the name check does, for one read per bone per frame.
    //
    // Sixty seconds was still a stall once a minute whenever the search happened to be slow: one whole session on
    // 2026-10-02 had it at 52 to 77 ms nine times running (others were 4 to 20 ms; what makes the difference is not
    // known). The net is ten minutes now. Nothing has ever been seen to need it: the root, vtable and name checks
    // are what catch a rebuilt or freed skeleton.
    //
    // Those checks do trip more often than once a minute, though. With nobody in the headset, every menu opened
    // through DevBench was followed by a search: 68 in one minute of opening and closing menus, up to 22.7 ms each,
    // and the log line (which now says why) named the reason every time: the body's root node changed. The player
    // has two bodies in VR and the game shows the other one while a menu is up. So two roots are remembered, not
    // one, and going back to a root already searched costs the per-frame checks and nothing else.
    struct LocalCache
    {
        void* pRoot = nullptr;
        BoneNodes Nodes{};
        std::array<void*, VRPose::kBoneCount> VTables{};
        std::array<const char*, VRPose::kBoneCount> Names{};
        bool LegsFound = false;
        std::array<FingerEntries, 2> Fingers{};
        std::array<bool, 2> FingersFound{};
        void* pSkeletonRoot = nullptr;
        std::chrono::steady_clock::time_point RefreshAt{};
        std::chrono::steady_clock::time_point UsedAt{};
    };
    static std::array<LocalCache, 2> s_caches;
    const auto now = std::chrono::steady_clock::now();
    LocalCache* pCache = nullptr;
    for (LocalCache& cache : s_caches)
        if (cache.pRoot == pRoot)
            pCache = &cache;
    const bool cKnownRoot = pCache != nullptr;
    if (!pCache)
    {
        // An empty slot first, then the one used longest ago. A root whose search fails leaves its slot empty, and
        // it fails again on every frame it is the current one; taking "longest ago" alone would hand it the good
        // slot on its second frame.
        if (!s_caches[0].pRoot)
            pCache = &s_caches[0];
        else if (!s_caches[1].pRoot)
            pCache = &s_caches[1];
        else
            pCache = s_caches[0].UsedAt <= s_caches[1].UsedAt ? &s_caches[0] : &s_caches[1];
    }
    LocalCache& s_local = *pCache;
    s_local.UsedAt = now;

    const char* pWhy = nullptr;
    if (!cKnownRoot)
        pWhy = "this root node was not one of the two remembered";
    else if (now >= s_local.RefreshAt)
        pWhy = "ten minutes passed";
    bool cached = pWhy == nullptr;
    for (uint32_t i = 0; cached && i < VRPose::kBoneCount; ++i)
        if (s_local.Nodes[i] && (*static_cast<void**>(s_local.Nodes[i]) != s_local.VTables[i] || GetName(s_local.Nodes[i]) != s_local.Names[i]))
        {
            cached = false;
            pWhy = *static_cast<void**>(s_local.Nodes[i]) != s_local.VTables[i] ? "a bone node became another kind of object" : "a bone node changed its name";
        }
    if (!cached)
    {
        const auto cSearchStarted = std::chrono::steady_clock::now();
        BoneNodes nodes;
        bool legsFound = false;
        if (!FindBones(pRoot, nodes, &legsFound))
        {
            s_local.pRoot = nullptr;
            return false;
        }
        const double cBonesMs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - cSearchStarted).count() / 1000.0;
        s_local.pRoot = pRoot;
        s_local.Nodes = nodes;
        s_local.LegsFound = legsFound;
        s_local.pSkeletonRoot = FindSkeletonRoot(pRoot);
        const auto cArrayStarted = std::chrono::steady_clock::now();
        const BoneArrayInfo arrayInfo = ResolveBoneArray(pRoot, nodes, false);
        const double cArrayMs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - cArrayStarted).count() / 1000.0;
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
        {
            s_local.VTables[i] = nodes[i] ? *static_cast<void**>(nodes[i]) : nullptr;
            s_local.Names[i] = nodes[i] ? GetName(nodes[i]) : nullptr;
        }
        s_local.RefreshAt = now + std::chrono::minutes(10);

        // Measured, so the next session says whether this was the 50 ms hitch.
        const auto cTookMs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - cSearchStarted).count() / 1000.0;
        if (cTookMs >= 40.0)
            DescribeBigTree(pRoot, cBonesMs, cArrayMs, cTookMs);
        static std::chrono::steady_clock::time_point s_nextSearchLog{};
        static uint32_t s_searches = 0;
        static double s_worstMs = 0.0;
        ++s_searches;
        s_worstMs = std::max(s_worstMs, cTookMs);
        if (now >= s_nextSearchLog)
        {
            s_nextSearchLog = now + std::chrono::seconds(60);
            spdlog::info("VRBodySync: local skeleton searched in {:.1f} ms because {} ({} searches since the last report, worst {:.1f} ms; two roots are remembered, each for 10 min or until a bone node changes)",
                         cTookMs, pWhy ? pWhy : "?", s_searches, s_worstMs);
            s_searches = 0;
            s_worstMs = 0.0;
        }
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
    // The owner's own hands, about once a second, so the receiver can compare its copy against the person it is a
    // copy of rather than against whoever happens to be looking. See VRPose::HasHandCheck.
    aOutPose.HasHandCheck = false;
    if (nodes[VRPose::kLeftHand] && nodes[VRPose::kRightHand])
    {
        static std::chrono::steady_clock::time_point s_nextHandCheck{};
        if (now >= s_nextHandCheck)
        {
            s_nextHandCheck = now + std::chrono::seconds(1);
            const glm::vec3 rootAt = ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
            const glm::vec3 left = inverseRoot * (ToGlm(At<NiTransform>(nodes[VRPose::kLeftHand], kWorldOffset).translate) - rootAt);
            const glm::vec3 right = inverseRoot * (ToGlm(At<NiTransform>(nodes[VRPose::kRightHand], kWorldOffset).translate) - rootAt);
            if (std::isfinite(left.z) && std::isfinite(right.z) && glm::dot(left, left) < 512.f * 512.f && glm::dot(right, right) < 512.f * 512.f)
            {
                aOutPose.HasHandCheck = true;
                aOutPose.LeftHandOffset[0] = left.x;
                aOutPose.LeftHandOffset[1] = left.y;
                aOutPose.LeftHandOffset[2] = left.z;
                aOutPose.RightHandOffset[0] = right.x;
                aOutPose.RightHandOffset[1] = right.y;
                aOutPose.RightHandOffset[2] = right.z;
            }
        }
    }

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

// Measurement only: nothing here claims anything or changes what anyone sees.
//
// Dragging a corpse is only sent by the client that **owns** it (see AnimationSystem: "a body this machine owns").
// The thing that used to claim a body you had grabbed, RunBodyGrabUpdates, was removed on 2026-09-24 because its
// test -- "a dead body more than 32 units from its network position" -- is true of corpses that have merely
// settled, and it fired 207 times across 58 bodies in one session and crashed the receiver. So since that removal,
// dragging a body you do not own is invisible to everybody, by construction. That is the 20:45 Lurker of
// 2026-09-25, and it needs no log to explain.
//
// Before anything claims a body again, this says what a safe test would look like. A settling ragdoll comes to
// rest in a second or two; a body someone is dragging keeps moving for as long as they drag it. So what gets
// measured is *sustained* motion within arm's reach -- how long, how far, and how close -- and the numbers decide
// the threshold rather than the other way round.
bool ObserveRemoteBodyMotion(Actor* apActor) noexcept
{
    if (!apActor || !apActor->actorState.IsDead())
        return false;

    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;

    void* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return false;

    const glm::vec3 at = ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
    const glm::vec3 playerAt = ToGlm(static_cast<NiPoint3>(pPlayer->position));
    const float toPlayer = glm::distance(at, playerAt);
    // Beyond this nobody is touching it by hand, so it is not worth a thought.
    if (toPlayer > 400.f)
        return false;

    struct Watch
    {
        glm::vec3 Last{};
        bool Placed = false;
        std::chrono::steady_clock::time_point MovingSince{};
        std::chrono::steady_clock::time_point LastMoved{};
        std::chrono::steady_clock::time_point NextLog{};
        float Travelled = 0.f;
        std::chrono::steady_clock::time_point TouchedAt{};
        bool Reported = false; // this grab has been reported to the caller
    };
    static std::unordered_map<uint32_t, Watch> s_watch;

    const auto now = std::chrono::steady_clock::now();
    for (auto it = s_watch.begin(); it != s_watch.end();)
        it = now - it->second.TouchedAt > std::chrono::seconds(30) ? s_watch.erase(it) : std::next(it);

    Watch& watch = s_watch[apActor->formID];
    watch.TouchedAt = now;

    if (!watch.Placed)
    {
        watch.Last = at;
        watch.Placed = true;
        return false;
    }

    const float step = glm::distance(at, watch.Last);
    watch.Last = at;

    // The same 4 units CaptureBodyPose uses: a settled ragdoll still twitches.
    if (step >= 4.f)
    {
        if (now - watch.LastMoved > std::chrono::milliseconds(500))
        {
            watch.MovingSince = now;
            watch.Travelled = 0.f;
        }
        watch.LastMoved = now;
        watch.Travelled += step;
    }
    else if (now - watch.LastMoved > std::chrono::milliseconds(500))
    {
        watch.Travelled = 0.f;
        watch.Reported = false;
        return false;
    }

    const auto movingFor = std::chrono::duration_cast<std::chrono::milliseconds>(now - watch.MovingSince).count();

    // A third of a second of being moved is a hand on it, not a ragdoll settling: the caller asks for the body, so that
    // this side becomes the one that sends it (see CharacterService::RunRemoteUpdates).
    bool cGrabbed = false;
    if (movingFor >= 300 && !watch.Reported)
    {
        watch.Reported = true;
        cGrabbed = true;
    }

    if (movingFor < 1000 || now < watch.NextLog)
        return cGrabbed;

    watch.NextLog = now + std::chrono::seconds(5);
    spdlog::info("BodyGrabDiag: remote body {:X} has been moving here for {} ms, {:.0f} units travelled, {:.0f} from the player. Nobody owns it on this side, so nothing of this is being sent.",
                 apActor->formID, movingFor, watch.Travelled, toPlayer);
    return cGrabbed;
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
        bool Boneless = false; // a skeleton this bone search cannot read; the root position is sent on its own
        bool LegsFound = false;
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
        bool legsFound = false;
        if (!FindBones(pRoot, nodes, &legsFound))
        {
            // Not a person. The bone search is by human bone name, so a Dwarven sphere, a spider or a centurion
            // never gets past it -- and until 2026-09-26 that meant nothing at all was sent for one, so dragging
            // an automaton did nothing on the other screen. Where it is can still be sent; only how it is bent
            // cannot. See VRPose::NoBones.
            capture.pRoot = pRoot;
            capture.Nodes = BoneNodes{};
            capture.VTables = {};
            capture.Boneless = true;
            capture.RefreshAt = now + std::chrono::seconds(5);
            capture.Sent = false;
        }
        else
        {
            capture.pRoot = pRoot;
            capture.Nodes = nodes;
            capture.Boneless = false;
            capture.LegsFound = legsFound;
            for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
                capture.VTables[i] = nodes[i] ? *static_cast<void**>(nodes[i]) : nullptr;
            capture.RefreshAt = now + std::chrono::seconds(5);
            capture.Sent = false;
        }
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

    // A skeleton with no readable bones: the root position is the whole message. It is sent every time it is
    // asked for rather than on change, because the change test below is a comparison of bones there are none of.
    if (capture.Boneless)
    {
        if (!capture.Sent)
            spdlog::info("VRBodySync: body {:X} is being moved here and has no humanoid skeleton; sending where it is, not how it is bent", apActor->formID);
        capture.Sent = true;

        aOutPose.NoBones = true;
        aOutPose.HasLegs = false;
        aOutPose.HasFingers = false;
        aOutPose.HasScale = false;
        aOutPose.HasRootPosition = true;
        aOutPose.RootPosition[0] = rootPosition.x;
        aOutPose.RootPosition[1] = rootPosition.y;
        aOutPose.RootPosition[2] = rootPosition.z;
        aOutPose.HasData = true;
        return true;
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

    // The legs as well, encoded like the upper body. Without them the other screen posed the top half from these
    // bones and left the legs bent however its own ragdoll had them, only shifted along: "the bodies start deforming
    // drastically" when Emma carried the bandit 45B51 on 2026-10-03 (09:28), seen from Seen's side.
    if (capture.LegsFound)
    {
        for (uint32_t i = VRPose::kUpperBoneCount; i < VRPose::kBoneCount; ++i)
        {
            const glm::mat3 boneWorld = ToGlm(At<NiTransform>(capture.Nodes[i], kWorldOffset).rotate);
            aOutPose.Bones[i] = glm::normalize(glm::quat_cast(inverseRoot * boneWorld));
        }
    }
    aOutPose.HasLegs = capture.LegsFound;
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
    pose.NoBones = acPose.NoBones;
    pose.HasLegs = acPose.NoBones ? false : acPose.HasLegs;
    if (!acPose.NoBones)
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

    pose.HasHandCheck = acPose.HasHandCheck;
    if (acPose.HasHandCheck)
    {
        pose.HandOffsets[0] = glm::vec3{acPose.LeftHandOffset[0], acPose.LeftHandOffset[1], acPose.LeftHandOffset[2]};
        pose.HandOffsets[1] = glm::vec3{acPose.RightHandOffset[0], acPose.RightHandOffset[1], acPose.RightHandOffset[2]};
    }
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
