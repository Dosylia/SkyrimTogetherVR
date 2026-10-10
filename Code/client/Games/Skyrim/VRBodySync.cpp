#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/VRBodySync.h>
#include <Games/Skyrim/VRHaptics.h>

#include <Actor.h>
#include <Games/ActorExtension.h>
#include <PlayerCharacter.h>
#include <BSAnimationGraphManager.h>
#include <Components/BGSBipedObjectForm.h>
#include <Forms/TESObjectARMO.h>
#include <NetImmerse/NiNode.h>
#include <NetImmerse/NiTransform.h>

#include <glm/gtc/quaternion.hpp>

#include <PerfScope.h>

#include <mutex>
#include <shared_mutex>
#include <tlhelp32.h>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <map>
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
// NiAVObject::worldBound. A NiBound is a centre and a radius. 0xB0, straight after the world transform where SE keeps
// it, is previousWorld on VR; measured on 2026-10-06, the visible blade's bound is at 0xE4 (VR_HISTORY.md, P1).
// Read only with ReadWorldBound below, which refuses anything that is not the size and place of a weapon.
constexpr uint32_t kWorldBoundOffset = 0xE4;

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

// Where a weapon hangs, left then right (VRPose::HasWeapons). Both are found below the forearm: the shield node hangs
// off its twist bone rather than the hand.
constexpr std::array<const char*, 2> kAttachNames{"SHIELD", "WEAPON"};
constexpr std::array<const char*, 2> kHandNames{"NPC L Hand [LHnd]", "NPC R Hand [RHnd]"};

// PlayerCharacter's first-person hands, left then right: VR_NODE_DATA::NPCLHnd and NPCRHnd in CommonLibVR-NG, in the
// same block as the headset node below. Not measured here, and that library is not kept up to date, so each is
// checked by name before anything is read through it (FirstPersonHand).
constexpr std::array<uint32_t, 2> kFirstPersonHandOffsets{0x590, 0x598};

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
    // The owner's own hand positions relative to its root (see VRPose::HasHandCheck): ReachHands puts the copy's
    // hands there, and the hand measurement compares against them.
    bool HasHandCheck = false;
    glm::vec3 HandOffsets[2]{};
    // Where the owner's weapons are held relative to its hands, left then right (see VRPose::HasWeapons). Kept from
    // the last update that carried them: PlaceWeapons puts the copy's weapons there.
    bool HasWeapons = false;
    std::array<bool, 2> WeaponHeld{};
    std::array<glm::quat, 2> WeaponRotation{};
    std::array<glm::vec3, 2> WeaponOffset{};
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
    // The whole skeleton -- its root, every node below it and every flattened bone, each once -- for the whole-body
    // offset (see PoseActor). Empty when the skeleton root was not found.
    std::vector<RigBone> Body;
    // The weapon attach nodes below each hand ("SHIELD" left, "WEAPON" right) and everything hanging off them, for
    // PlaceWeapons. Empty when the hand has none.
    std::array<RigBone, 2> Attach{};
    std::array<std::vector<RigBone>, 2> AttachBelow{};
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

    // The whole skeleton, for the whole-body offset. The posed bones and what hangs below them are not all of it:
    // NPC COM and NPC Spine [Spn0] sit between the root and Spine1, and the waist is skinned to Spn0.
    if (void* pSkeleton = aRig.SkeletonRoot.pNode)
    {
        std::unordered_set<const uint8_t*> bodyEntries;
        std::vector<void*> bodyNodes{pSkeleton};
        CollectNodeDescendants(pSkeleton, bodyNodes);
        for (void* pNode : bodyNodes)
        {
            uint8_t* pEntry = entryFor(pNode);
            aRig.Body.push_back(MakeRigBone(pNode, pEntry));
            if (pEntry)
                bodyEntries.insert(pEntry);
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            uint8_t* pEntry = pArray + i * kBoneEntrySize;
            if (!bodyEntries.count(pEntry))
                aRig.Body.push_back(MakeRigBone(nullptr, pEntry));
        }
    }

    // The weapon attach nodes, by name below each forearm: "SHIELD" hangs off the forearm's twist bone, not the hand.
    for (size_t side = 0; side < 2; ++side)
    {
        void* pForearm = nodes[side == 0 ? VRPose::kLeftForearm : VRPose::kRightForearm];
        void* pAttach = pForearm ? FindShallowest(pForearm, kAttachNames[side]) : nullptr;
        if (!pAttach || !AsNode(pAttach))
            continue;
        aRig.Attach[side] = MakeRigBone(pAttach, entryFor(pAttach));
        std::vector<void*> below;
        CollectNodeDescendants(pAttach, below);
        for (void* pNode : below)
            aRig.AttachBelow[side].push_back(MakeRigBone(pNode, entryFor(pNode)));
    }

    aRig.FingersFound[0] = FindFingers(arrayInfo, aRig.Bones[VRPose::kLeftHand].pEntry, aRig.Fingers[0]);
    aRig.FingersFound[1] = FindFingers(arrayInfo, aRig.Bones[VRPose::kRightHand].pEntry, aRig.Fingers[1]);
    if (aLog && (!aRig.FingersFound[0] || !aRig.FingersFound[1]))
        spdlog::warn("VRBodySync: finger bones not found by shape under root {} (left {}, right {}); the hands stay on the animation pose", apRoot, aRig.FingersFound[0], aRig.FingersFound[1]);
    aRig.Valid = true;
    // Quiet when this is only picking up a node that was attached below a bone, which happens on every equip.
    if (aLog)
        spdlog::info("VRBodySync: resolved skeleton under root {}: {} flattened bones, {} carried by the spine, {} by the head, {} by the left hand, {} by the right hand, {} in the "
                     "whole body; weapon nodes {} left, {} right",
                     apRoot, count, aRig.Descendants[VRPose::kSpine1].size(), aRig.Descendants[VRPose::kHead].size(), aRig.Descendants[VRPose::kLeftHand].size(),
                     aRig.Descendants[VRPose::kRightHand].size(), aRig.Body.size(), aRig.Attach[0].pNode ? "found" : "not found", aRig.Attach[1].pNode ? "found" : "not found");
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
    for (const auto& bone : acRig.Body)
        if (!bone.IsIntact())
            return false;
    for (size_t side = 0; side < 2; ++side)
    {
        if (!acRig.Attach[side].IsIntact())
            return false;
        for (const auto& bone : acRig.AttachBelow[side])
            if (!bone.IsIntact())
                return false;
    }
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
    // Not Rig::Body: anything attached anywhere on the body (an arrow, an effect) would cost a whole new search, up to
    // 50 ms, and all a node missing from it misses is the whole-body offset.
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

    // The whole-body offset (a corpse being dragged, where the owner's body stands), applied once to every bone and to
    // everything hanging off them, before any rotation. It used to be added at each posed bone on top of what its
    // parent had already carried down to it -- spine, chest, collarbone, upper arm, forearm, hand: six times at the
    // hand. A 50-unit shift put the copy's hands 269 units behind it (the rig, 2026-10-04), and a dragged corpse came
    // out stretched along the drag ("the bodies deform drastically", Seen watching Emma carry one, 2026-10-03).
    // Nodes and flattened entries are counted separately: a bone can be listed under several parents, by either.
    //
    // It has to be the whole skeleton, not only the posed bones and what hangs below them. NPC Spine [Spn0], between
    // NPC COM and Spine1, is neither, and the waist is skinned to it: from the day the hips moved the body by 30 to 40
    // units, every copy's belly was pulled back towards where the body had been ("belly position of both of us is
    // buggy", 2026-10-04 15:58). Rig::Body is all of it, each node and entry once; the posed bones are the fallback
    // when the root was not found.
    if (cHasOffset && !acRig.Body.empty())
    {
        for (const RigBone& bone : acRig.Body)
        {
            if (bone.pNode)
            {
                NiTransform& world = At<NiTransform>(bone.pNode, kWorldOffset);
                world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
            }
            if (bone.pEntry)
            {
                NiTransform& world = *reinterpret_cast<NiTransform*>(bone.pEntry + kBoneEntryWorld);
                world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
            }
        }
    }
    else if (cHasOffset)
    {
        std::unordered_set<const void*> movedNodes;
        std::unordered_set<const void*> movedEntries;
        const auto shift = [&](const RigBone& acBone)
        {
            if (acBone.pNode && movedNodes.insert(acBone.pNode).second)
            {
                NiTransform& world = At<NiTransform>(acBone.pNode, kWorldOffset);
                world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
            }
            if (acBone.pEntry && movedEntries.insert(acBone.pEntry).second)
            {
                NiTransform& world = *reinterpret_cast<NiTransform*>(acBone.pEntry + kBoneEntryWorld);
                world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
            }
        };
        for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
        {
            shift(acRig.Bones[i]);
            for (const RigBone& below : acRig.Descendants[i])
                shift(below);
        }
    }

    // Parents first. Each bone is turned about its own position, and that rigid motion is carried to everything below
    // it, posed children included, so a child's transform already holds its parents' motion when its own rotation is
    // solved. World transforms are written directly because nothing recomputes them from locals between the end of the
    // frame and the draw that uses them.
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        const RigBone& bone = acRig.Bones[i];
        // Legs only when the sender's trackers drive them; otherwise the walk animation keeps them.
        // (A body being dragged is still dragged from the waist down: the offset above reached the legs too.)
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
            world.translate = FromGlm(pivot + delta * (ToGlm(world.translate) - pivot));
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

// Turns one posed bone about its own joint by a world rotation and carries everything below it along -- the same
// step PoseActor takes for each bone, for corrections made after the pose has been written.
void TurnBone(const Rig& acRig, const uint32_t aBone, const glm::mat3& acDelta) noexcept
{
    const RigBone& bone = acRig.Bones[aBone];
    const NiTransform current = bone.World();
    const glm::vec3 pivot = ToGlm(current.translate);
    const glm::mat3 wantedRotation = acDelta * ToGlm(current.rotate);

    NiTransform wanted = current;
    wanted.rotate = FromGlm(wantedRotation);
    bone.WriteWorld(wanted);
    if (bone.pNode)
    {
        NiTransform local = At<NiTransform>(bone.pNode, kLocalOffset);
        if (void* pParent = At<void*>(bone.pNode, kParentOffset))
        {
            local.rotate = FromGlm(glm::transpose(ToGlm(At<NiTransform>(pParent, kWorldOffset).rotate)) * wantedRotation);
            bone.WriteLocal(local);
        }
    }
    for (const RigBone& child : acRig.Descendants[aBone])
    {
        NiTransform world = child.World();
        world.rotate = FromGlm(acDelta * ToGlm(world.rotate));
        world.translate = FromGlm(pivot + acDelta * (ToGlm(world.translate) - pivot));
        child.WriteWorld(world);
    }
}

// The rotation that turns direction acFrom onto acTo (both unit length).
glm::mat3 RotationBetween(const glm::vec3& acFrom, const glm::vec3& acTo) noexcept
{
    const glm::vec3 axis = glm::cross(acFrom, acTo);
    const float sine = glm::length(axis);
    const float cosine = glm::clamp(glm::dot(acFrom, acTo), -1.f, 1.f);
    if (sine < 1e-5f)
        return glm::mat3(1.f); // already aligned, or exactly opposite (an arm never needs a half turn in one frame)
    return glm::mat3_cast(glm::angleAxis(std::atan2(sine, cosine), axis / sine));
}

// How far a rotation turns, in degrees.
float DegreesOf(const glm::mat3& acRotation) noexcept
{
    const float cosine = glm::clamp((acRotation[0][0] + acRotation[1][1] + acRotation[2][2] - 1.f) * 0.5f, -1.f, 1.f);
    return glm::degrees(std::acos(cosine));
}

// The copy's weapons where its owner holds them (VRPose::HasWeapons). The hands are already where the owner's are;
// this turns and moves each attach node, and the weapon hanging off it, to the grip the owner's first-person hand has
// relative to its body's hand. A grip more than 30 units from where this skeleton hangs the weapon is not a grip --
// an arm VRIK could not stretch to the controller -- and is left alone.
void PlaceWeapons(const Rig& acRig, const RemotePose& acPose, const uint32_t aFormId, const std::chrono::steady_clock::time_point aNow) noexcept
{
    if (!acPose.HasWeapons)
        return;

    static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_nextLog;
    for (size_t side = 0; side < 2; ++side)
    {
        const RigBone& attach = acRig.Attach[side];
        const RigBone& hand = acRig.Bones[side == 0 ? VRPose::kLeftHand : VRPose::kRightHand];
        if (!acPose.WeaponHeld[side] || !attach.pNode || (!hand.pNode && !hand.pEntry))
            continue;

        const NiTransform handWorld = hand.World();
        const glm::mat3 handRotation = ToGlm(handWorld.rotate);
        const glm::mat3 wantedRotation = handRotation * glm::mat3_cast(acPose.WeaponRotation[side]);
        const glm::vec3 wantedAt = ToGlm(handWorld.translate) + handRotation * (acPose.WeaponOffset[side] * handWorld.scale);
        const NiTransform current = attach.World();
        const glm::vec3 pivot = ToGlm(current.translate);
        const glm::mat3 delta = wantedRotation * glm::transpose(ToGlm(current.rotate));
        const float moved = glm::distance(wantedAt, pivot);
        if (!std::isfinite(moved) || !std::isfinite(wantedRotation[0][0]))
            continue;

        auto& nextLog = s_nextLog[(static_cast<uint64_t>(aFormId) << 1) | side];
        const bool cLog = aNow >= nextLog;
        if (cLog)
            nextLog = aNow + std::chrono::seconds(10);

        if (moved > 30.f)
        {
            if (cLog)
                spdlog::info("VRBodySync: actor {:X} {} weapon left where its skeleton hangs it: its owner holds it {:.1f} units from there", aFormId, side ? "right" : "left", moved);
            continue;
        }

        NiTransform placed = current;
        placed.rotate = FromGlm(wantedRotation);
        placed.translate = FromGlm(wantedAt);
        attach.WriteWorld(placed);
        for (const RigBone& below : acRig.AttachBelow[side])
        {
            NiTransform world = below.World();
            world.rotate = FromGlm(delta * ToGlm(world.rotate));
            world.translate = FromGlm(wantedAt + delta * (ToGlm(world.translate) - pivot));
            below.WriteWorld(world);
        }

        if (cLog)
            spdlog::info("VRBodySync: actor {:X} {} weapon placed where its owner holds it: turned {:.0f} degrees and moved {:.1f} units from where its skeleton hangs it", aFormId,
                         side ? "right" : "left", DegreesOf(delta), moved);
    }
}

// The copy's hands where the owner's are. The pose is joint rotations, and the same rotations on a different body
// put the hands elsewhere: the copy's arms are not the owner's length, and VRIK places the owner's body differently
// relative to the root than the copy's animation does. Measured on 2026-10-03, both screens: the copy's hands sat a
// median 27-32 units (about 40 cm) from where the owner held them, mostly too far forward, so hands that touched in
// life were most of a metre apart on screen -- and so was whatever they held or dragged. The owner sends where its
// hands are relative to its root (VRPose hand offsets); here shoulder and elbow are bent so each hand lands there
// (two-bone IK, keeping the elbow on the side it already bends to), and the hand then gets the owner's own
// orientation back.
void ReachHands(const Rig& acRig, const RemotePose& acPose) noexcept
{
    if (!acPose.HasHandCheck || !acRig.pRoot)
        return;

    const NiTransform& rootWorld = At<NiTransform>(acRig.pRoot, kWorldOffset);
    const glm::mat3 rootRotation = ToGlm(rootWorld.rotate);
    const glm::vec3 rootAt = ToGlm(rootWorld.translate);

    struct Arm
    {
        uint32_t Upper, Fore, Hand;
        int Side;
    };
    for (const Arm& arm : {Arm{VRPose::kLeftUpperArm, VRPose::kLeftForearm, VRPose::kLeftHand, 0}, Arm{VRPose::kRightUpperArm, VRPose::kRightForearm, VRPose::kRightHand, 1}})
    {
        const RigBone& upper = acRig.Bones[arm.Upper];
        const RigBone& fore = acRig.Bones[arm.Fore];
        const RigBone& hand = acRig.Bones[arm.Hand];
        if ((!upper.pNode && !upper.pEntry) || (!fore.pNode && !fore.pEntry) || (!hand.pNode && !hand.pEntry))
            continue;

        const glm::vec3 target = rootAt + rootRotation * acPose.HandOffsets[arm.Side];
        const glm::vec3 shoulder = ToGlm(upper.World().translate);
        const glm::vec3 elbow = ToGlm(fore.World().translate);
        const glm::vec3 wrist = ToGlm(hand.World().translate);

        // Fine-tuning only. The owner's rotations already give the arm its shape; what is left after the body is
        // placed is arm length, a few units. A bigger gap means the two disagree about where the body is, and pulling
        // the hand all the way folds the arm: Seen held his arms straight out and his copy had them crossed on its
        // chest (2026-10-04, 30 units pulled). Then the arm keeps the owner's pose untouched.
        constexpr float cMaxCorrection = 15.f;
        const float cGap = glm::distance(target, wrist);
        if (!std::isfinite(cGap) || cGap > cMaxCorrection)
        {
            static std::chrono::steady_clock::time_point s_nextFar{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_nextFar)
            {
                s_nextFar = now + std::chrono::seconds(10);
                spdlog::info("VRBodySync: a hand of the copy is {:.0f} units from where its owner's is; more than the {:.0f} fine-tuning, so the arm keeps the owner's pose", cGap,
                             cMaxCorrection);
            }
            continue;
        }
        const float upperLength = glm::distance(shoulder, elbow);
        const float foreLength = glm::distance(elbow, wrist);
        const glm::vec3 toTarget = target - shoulder;
        const float reach = glm::length(toTarget);
        // A bad read, not a hand: further from the shoulder than two arm lengths, or not a number at all.
        if (!std::isfinite(reach) || upperLength < 1.f || foreLength < 1.f || reach < 1.f || reach > 2.f * (upperLength + foreLength))
            continue;

        const float clamped = glm::clamp(reach, std::fabs(upperLength - foreLength) + 0.5f, upperLength + foreLength - 0.5f);
        const glm::vec3 along = toTarget / reach;

        // The plane the arm bends in, from how it bends now; a straight arm bends downwards, as an elbow does.
        glm::vec3 normal = glm::cross(elbow - shoulder, wrist - shoulder);
        if (glm::dot(normal, normal) < 1e-4f)
            normal = glm::cross(along, rootRotation * glm::vec3(0.f, 0.f, -1.f));
        if (glm::dot(normal, normal) < 1e-6f)
            continue;
        glm::vec3 side = glm::normalize(glm::cross(glm::normalize(normal), along));
        if (glm::dot(elbow - shoulder, side) < 0.f)
            side = -side;

        const float cosine = glm::clamp((upperLength * upperLength + clamped * clamped - foreLength * foreLength) / (2.f * upperLength * clamped), -1.f, 1.f);
        const float sine = std::sqrt(std::max(0.f, 1.f - cosine * cosine));
        const glm::vec3 elbowWanted = shoulder + upperLength * (cosine * along + sine * side);

        TurnBone(acRig, arm.Upper, RotationBetween(glm::normalize(elbow - shoulder), glm::normalize(elbowWanted - shoulder)));

        const glm::vec3 elbowNow = ToGlm(fore.World().translate);
        const glm::vec3 wristNow = ToGlm(hand.World().translate);
        const glm::vec3 wristWanted = shoulder + clamped * along;
        if (glm::distance(wristNow, elbowNow) < 1.f || glm::distance(wristWanted, elbowNow) < 1.f)
            continue;
        TurnBone(acRig, arm.Fore, RotationBetween(glm::normalize(wristNow - elbowNow), glm::normalize(wristWanted - elbowNow)));

        // The owner's own hand orientation, which the two turns above carried away.
        const glm::mat3 handWanted = rootRotation * glm::mat3_cast(acPose.Bones[arm.Hand]);
        TurnBone(acRig, arm.Hand, handWanted * glm::transpose(ToGlm(hand.World().rotate)));
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

// Where each remote body's hands and weapon attach nodes were drawn, recorded right after posing at the renderer's
// frame end. The touch check runs on the game thread and reads the scene, which the game rebuilds from the copy's
// own animation every frame; this says what was actually on screen, for comparing the two ("sword feeling still
// unsync", 2026-10-06).
struct DrawnGrip
{
    std::array<glm::vec3, 2> Hand{};   // left, right
    std::array<glm::vec3, 2> Attach{}; // "SHIELD", "WEAPON"
    std::array<bool, 2> HasHand{};
    std::array<bool, 2> HasAttach{};
    std::chrono::steady_clock::time_point At{};
};
std::mutex s_drawnLock;
std::unordered_map<uint32_t, DrawnGrip> s_drawn;

// Physics queue P0 (2026-10-06): what the Havok world holds for a copy -- one ragdoll bone's body and its drawn
// weapon's own body -- and which collision layers collide with which. Read only, once per copy per session. Layouts
// from PLANCK's static_asserts and CommonLibVR-NG; the filter info offset is the one to confirm by what it reads.
constexpr uint32_t kCollisionObjectOffset = 0x40; // NiAVObject::collisionObject
constexpr uint32_t kCollisionBodyOffset = 0x20;   // bhkNiCollisionObject::body (bhkWorldObject)
constexpr uint32_t kHavokBodyOffset = 0x10;       // bhkWorldObject::hkBody (hkpRigidBody)
constexpr uint32_t kHkWorldOffset = 0x10;         // hkpWorldObject::world
constexpr uint32_t kFilterInfoOffset = 0x4C;      // hkpWorldObject::collidable.broadPhaseHandle.collisionFilterInfo
constexpr uint32_t kMotionTypeOffset = 0x160;     // hkpEntity::motion.type
constexpr uint32_t kWorldFilterOffset = 0xD0;     // hkpWorld::collisionFilter
constexpr uint32_t kLayerBitfieldsOffset = 0x1D0; // bhkCollisionFilter::layerBitfields[64]

// The Havok body behind a node, or null. Every pointer is checked before it is followed: this runs once per copy.
uint8_t* HavokBodyOf(void* apNode) noexcept
{
    if (!apNode || !IsReadable(static_cast<uint8_t*>(apNode) + kCollisionObjectOffset, sizeof(void*)))
        return nullptr;
    void* pCollision = At<void*>(apNode, kCollisionObjectOffset);
    if (!pCollision || !IsReadable(pCollision, 0x28))
        return nullptr;
    void* pBody = At<void*>(pCollision, kCollisionBodyOffset);
    if (!pBody || !IsReadable(pBody, 0x18))
        return nullptr;
    auto* pHavok = At<uint8_t*>(pBody, kHavokBodyOffset);
    if (!pHavok || !IsReadable(pHavok, kMotionTypeOffset + 1))
        return nullptr;
    return pHavok;
}

// The same for a node known to be alive -- the rig's, which are checked by vtable every frame, or a weapon node this
// code holds a reference to -- without IsReadable. That is VirtualQuery, which in this process costs milliseconds a
// call (see IsReadable): with it on these paths the weapon bodies cost 26 ms before every physics step and the rig
// ran at 14 frames a second (2026-10-06). A node alive holds its collision object, which holds its body.
uint8_t* HavokBodyOfLive(void* apNode) noexcept
{
    void* pCollision = apNode ? At<void*>(apNode, kCollisionObjectOffset) : nullptr;
    void* pBody = pCollision ? At<void*>(pCollision, kCollisionBodyOffset) : nullptr;
    return pBody ? At<uint8_t*>(pBody, kHavokBodyOffset) : nullptr;
}

std::string DescribeHavokBody(const char* acpWhat, void* apNode, const uint8_t* apHavok) noexcept
{
    const uint32_t filter = At<uint32_t>(const_cast<uint8_t*>(apHavok), kFilterInfoOffset);
    const char* pName = GetName(apNode);
    return fmt::format("{} '{}': layer {}, group {}, collision {}, motion type {}, {}", acpWhat, pName && IsReadable(pName, 1) ? pName : "?", filter & 0x7F, filter >> 16,
                       (filter & (1u << 14)) ? "off" : "on", At<uint8_t>(const_cast<uint8_t*>(apHavok), kMotionTypeOffset),
                       At<void*>(const_cast<uint8_t*>(apHavok), kHkWorldOffset) ? "in the world" : "not in the world");
}

void LogPhysicsOnce(const uint32_t aFormId, const Rig& acRig) noexcept
{
    static std::unordered_set<uint32_t> s_logged;
    static bool s_worldLogged = false;
    if (s_logged.count(aFormId) || !acRig.Attach[1].pNode)
        return;

    // A drawn weapon hangs below the attach node; wait for one.
    uint8_t* pWeaponBody = nullptr;
    void* pWeaponNode = nullptr;
    for (const RigBone& below : acRig.AttachBelow[1])
        if (below.pNode && (pWeaponBody = HavokBodyOf(below.pNode)) != nullptr)
        {
            pWeaponNode = below.pNode;
            break;
        }
    if (acRig.AttachBelow[1].empty())
    {
        // Where the weapon hangs instead, once. In the rig on 2026-10-06 nothing was listed below the copy's "WEAPON"
        // node because the copy had never drawn (SetWeaponDrawn called the wrong VR virtual; see Actor.h).
        // Fifteen seconds after the copy was first posed, so a weapon drawn in the meantime is in place.
        static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_firstPosed;
        static std::unordered_set<uint32_t> s_whereLogged;
        const auto cNow = std::chrono::steady_clock::now();
        const auto firstPosed = s_firstPosed.emplace(aFormId, cNow).first->second;
        if (cNow - firstPosed < std::chrono::seconds(15) || !s_whereLogged.insert(aFormId).second)
            return;
        const auto childNames = [](void* apNode)
        {
            std::string names;
            void* pNode = apNode ? AsNode(apNode) : nullptr;
            if (!pNode)
                return std::string("(not a node)");
            void** pChildren = At<void**>(pNode, kChildrenOffset + 0x8);
            const uint16_t capacity = At<uint16_t>(pNode, kChildrenOffset + 0x10);
            for (uint16_t i = 0; pChildren && i < capacity; ++i)
                if (pChildren[i])
                {
                    const char* pName = GetName(pChildren[i]);
                    names += fmt::format(" '{}'", pName && IsReadable(pName, 1) ? pName : "?");
                }
            return names.empty() ? std::string(" none") : names;
        };
        void* pParent = At<void*>(acRig.Attach[1].pNode, kParentOffset);
        const char* pParentName = pParent ? GetName(pParent) : nullptr;
        spdlog::info("PhysicsProbe: actor {:X} weapon node '{}' (under '{}') has children:{}; the right hand has:{}", aFormId, GetName(acRig.Attach[1].pNode),
                     pParentName && IsReadable(pParentName, 1) ? pParentName : "?", childNames(acRig.Attach[1].pNode), childNames(acRig.Bones[VRPose::kRightHand].pNode));
        return;
    }
    s_logged.insert(aFormId);

    uint8_t* pBoneBody = nullptr;
    void* pBoneNode = nullptr;
    for (const RigBone& bone : acRig.Body)
        if (bone.pNode && (pBoneBody = HavokBodyOf(bone.pNode)) != nullptr)
        {
            pBoneNode = bone.pNode;
            break;
        }

    spdlog::info("PhysicsProbe: actor {:X} -- {}; {}", aFormId, pBoneBody ? DescribeHavokBody("ragdoll bone", pBoneNode, pBoneBody) : std::string("no ragdoll bone with a body"),
                 pWeaponBody ? DescribeHavokBody("drawn weapon", pWeaponNode, pWeaponBody) : fmt::format("no body under the drawn weapon ({} nodes)", acRig.AttachBelow[1].size()));

    // Which layers collide with which, from whichever body is in a world.
    if (s_worldLogged)
        return;
    for (uint8_t* pBody : {pBoneBody, pWeaponBody})
    {
        void* pWorld = pBody ? At<void*>(pBody, kHkWorldOffset) : nullptr;
        if (!pWorld || !IsReadable(static_cast<uint8_t*>(pWorld) + kWorldFilterOffset, sizeof(void*)))
            continue;
        void* pFilter = At<void*>(pWorld, kWorldFilterOffset);
        if (!pFilter || !IsReadable(static_cast<uint8_t*>(pFilter) + kLayerBitfieldsOffset, 64 * sizeof(uint64_t)))
            continue;
        s_worldLogged = true;
        constexpr std::array<uint32_t, 9> cLayers{4, 5, 8, 10, 30, 32, 33, 56, 1};
        std::string table;
        for (const uint32_t layer : cLayers)
        {
            const uint64_t bits = At<uint64_t>(pFilter, kLayerBitfieldsOffset + layer * sizeof(uint64_t));
            table += fmt::format(" | {}:", layer);
            for (const uint32_t other : cLayers)
                if ((bits >> other) & 1)
                    table += fmt::format(" {}", other);
        }
        spdlog::info("PhysicsProbe: layers that collide (clutter 4, weapon 5, biped 8, props 10, character capsule 30, dead body 32, biped without capsule 33, HIGGS 56, static 1){}",
                     table);
        break;
    }
}
// Physics queue P1 (2026-10-06): the other player's drawn weapon as a physical thing in this game.
//
// The copy's drawn weapon already has a rigid body of its own, which the game keeps out of the world. Measured in the
// rig: "drawn weapon 'Weapon  (00012EB7)': layer 8, group 1543, collision on, motion type 4, not in the world" --
// keyframed, on the biped layer, in the copy's own collision group (its ragdoll's). Layer 8 collides with clutter,
// weapons, bipeds, props and HIGGS's hands and weapons (56), and not with walls or character capsules; the group
// keeps it off its owner's own ragdoll. So that body is put in the world while the weapon is drawn and driven every
// frame to where the weapon is drawn, the way HIGGS drives the player's own (hand.cpp, MoveHandAndWeaponCollision).
//
// Lifetime: the weapon's node and its body wrapper are referenced while the body is in the world, so neither is freed
// under the physics. The body leaves the world at the first frame end at which its copy was not posed, the weapon is
// not fully drawn, the node no longer hangs under the copy's hand, or the copy's ragdoll is in another world.
constexpr uint32_t kBhkWorldOfHkpWorldOffset = 0x430;   // ahkpWorld::m_userData (bhkWorld)
constexpr uint32_t kWorldLockOffset = 0xC598;           // bhkWorld::worldLock
constexpr uint32_t kBodyTransformOffset = 0x170;        // hkpEntity::motion (0x150) .motionState.transform: rotation columns, then translation
constexpr uint32_t kRigidBodyTRotationOffset = 0x40;    // bhkRigidBodyT::rotation (hkQuaternion)
constexpr uint32_t kRigidBodyTTranslationOffset = 0x50; // bhkRigidBodyT::translation (hkVector4, Havok units)

using THkpWorldAddEntity = void*(void* apWorld, void* apEntity, int aActivation);
// Havok's removeEntity returns an hkBool, which MSVC returns through a hidden pointer after `this` (HIGGS calls it so).
using THkpWorldRemoveEntity = bool*(void* apWorld, bool* apResult, void* apEntity);
using THkpEntitySetPositionAndRotation = void(void* apEntity, const float* apPosition, const float* apRotation);
using TWorldLock = void(void* apLock);
using THkpEntityActivate = void(void* apEntity);
constexpr uint32_t kAngularVelocityOffset = 0x240; // hkpEntity::motion (0x150) .angularVelocity; linearVelocity just before it
// ahkpWorld::stepDeltaTime as bhkWorld::Update called it before the hook below; null until the hook is in.
using TStepDeltaTime = int(void* apHkpWorld, float aSeconds);
TStepDeltaTime* s_stepDeltaTime = nullptr;
constexpr uint32_t kBodyQuaternionOffset = 0x1E0;  // hkpEntity::motion.motionState.sweptTransform.rotation1, what the keyframe turns from
constexpr uint32_t kMaxAngularVelocityOffset = 0x21B; // hkpEntity::motion.motionState.maxAngularVelocity (hkUFloat8)
constexpr uint32_t kLinearDampingOffset = 0x214;      // hkpEntity::motion.motionState.linearDamping (hkHalf: a float's top 16 bits)
constexpr uint32_t kAngularDampingOffset = 0x216;     // .angularDamping (hkHalf)
constexpr uint32_t kTimeFactorOffset = 0x218;         // .timeFactor (hkHalf)
// hkpWorldObject::collidable.broadPhaseHandle.objectQualityType, two bytes before the filter info (0x4C, measured).
// Havok 2010: 0 fixed, 1 keyframed, 2 debris, 3 debris with simple TOI, 4 moving, 5 critical, 6 bullet, 9 keyframed reporting.
constexpr uint32_t kQualityTypeOffset = 0x4A;


float FromHalf(const uint16_t aHalf) noexcept
{
    const uint32_t bits = static_cast<uint32_t>(aHalf) << 16;
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
using THkRealToUFloat8 = bool(uint8_t& aOut, const float& acValue);

struct WeaponBody
{
    void* pNode = nullptr;     // the weapon's node, referenced
    void* pWrapper = nullptr;  // its bhkRigidBody, referenced
    uint8_t* pHavok = nullptr; // the hkpRigidBody inside it
    void* pBhkWorld = nullptr; // the world it was put in, checked (by name) when it was
    bool IsT = false;          // a bhkRigidBodyT, with its own offset from the node
    bool Seen = false;         // driven at this frame end
    bool Listening = false;    // our contact listener is on it
    uint8_t GameMaxAngularVelocity = 0; // the game's limit, put back when the body is let go
    uint16_t GameAngularDamping = 0;    // as the game had it (logged)
    int8_t GameQuality = 0;             // the game's collision quality (logged; 1, keyframed, already)
    uint32_t Driven = 0;         // frame ends since the last line about it
    float WorstGap = 0.f;        // units the body was from the drawn weapon at a frame end
    float WorstLag = 0.f;        // degrees its rotation was from the drawn weapon's
    float WorstTargetStep = 0.f; // degrees the target turned from one frame to the next
    uint32_t FramesOff = 0;      // frame ends with the body more than 10 degrees off
    glm::quat LastTarget{1.f, 0.f, 0.f, 0.f};
};
// Per copy and side (left, right). Touched only from the renderer's frame end.
std::unordered_map<uint64_t, WeaponBody> s_weaponBodies;

// What the weapon bodies touch (Physics queue P1, "contact, measured"): Havok's own contact callbacks on each of them.
// Havok raises them during its step, maybe on its worker threads, so they only write down what touched what, under a
// lock of their own that is never held while waiting for the world's; the frame end says it. Layout: Havok 2010's
// hkpContactListener (slots 0-2, as PLANCK overrides them and CommonLibVR-NG lists them) and its events.
constexpr uint32_t kEventBodiesOffset = 0x08;       // hkpCollisionEvent::bodies[2]
constexpr uint32_t kEventContactPointOffset = 0x28; // hkpContactPointEvent::contactPoint (hkContactPoint, position first)

using THkpEntityContactListener = void(void* apEntity, void* apListener);

struct WeaponContact
{
    const void* pBodies[2]{};
    uint32_t Filters[2]{};
    uint8_t MotionTypes[2]{};
    glm::vec3 Point{};          // Havok units
    glm::vec3 Velocities[2]{};  // each body's linear velocity, Havok units a second
    // Whether each body is the local player's: 1 one of HIGGS's own bodies (its hands, its body for the equipped
    // weapon), 2 held by one of them (HIGGS's grab constraint); where that HIGGS body is, Havok units.
    uint8_t HiggsKind[2]{};
    glm::vec3 HiggsAt[2]{};
};

// A clash: the other player's weapon met one of the local player's HIGGS bodies or something one of them holds,
// queued at the frame end and taken by CharacterService to be felt and sent.
struct PendingClash
{
    uint32_t FormId;
    uint8_t Side;
    glm::vec3 Point; // game units
    float Speed;     // game units a second
    glm::vec3 Hand;  // game units: the HIGGS body, which says which of the player's hands it was
    bool Start;      // the two started touching (a meeting); otherwise still touching
    bool Held;       // what met it is held up with HIGGS's grab, not one of HIGGS's own bodies
};
std::mutex s_clashesLock;
std::vector<PendingClash> s_clashes;
// HIGGS's layer: only the local player's hands and the body it gives the equipped weapon are on it (VR_TODO P0).
constexpr uint32_t kHiggsLayer = 56;
constexpr uint32_t kBodyTranslationOffset = 0x1A0; // hkpEntity::motion.motionState.transform.translation

bool IsHiggsBody(const uint8_t* apBody) noexcept
{
    return apBody && (*reinterpret_cast<const uint32_t*>(apBody + kFilterInfoOffset) & 0x7F) == kHiggsLayer;
}

// The HIGGS body a dynamic body is held by, through a constraint between the two (HIGGS's physics grab), or null.
// Called inside Havok's step, where constraints are not added or removed. Layouts from CommonLibVR-NG (hkpEntity,
// hkpConstraintInstance, hkConstraintInternal).
const uint8_t* HiggsBodyHolding(const uint8_t* apBody) noexcept
{
    const auto partner = [apBody](const uint8_t* apA, const uint8_t* apB) -> const uint8_t*
    {
        const uint8_t* pOther = apA == apBody ? apB : apA;
        return pOther != apBody && IsHiggsBody(pOther) ? pOther : nullptr;
    };
    // constraintsMaster: hkSmallArray<hkConstraintInternal> (data, uint16 size), 0x40 each, entities at +0x08 and +0x10.
    const uint8_t* pMaster = *reinterpret_cast<const uint8_t* const*>(apBody + 0x100);
    const uint16_t masterCount = *reinterpret_cast<const uint16_t*>(apBody + 0x108);
    if (pMaster && masterCount <= 32)
        for (uint16_t i = 0; i < masterCount; ++i)
        {
            const uint8_t* pInternal = pMaster + i * 0x40;
            if (const uint8_t* pHand = partner(*reinterpret_cast<const uint8_t* const*>(pInternal + 0x08), *reinterpret_cast<const uint8_t* const*>(pInternal + 0x10)))
                return pHand;
        }
    // constraintsSlave: hkArray<hkpConstraintInstance*> (data, int32 size), entities at +0x28 and +0x30.
    const auto* const* ppSlaves = *reinterpret_cast<const uint8_t* const* const*>(apBody + 0x110);
    const int32_t slaveCount = *reinterpret_cast<const int32_t*>(apBody + 0x118);
    if (ppSlaves && slaveCount > 0 && slaveCount <= 32)
        for (int32_t i = 0; i < slaveCount; ++i)
            if (const uint8_t* pInstance = ppSlaves[i])
                if (const uint8_t* pHand = partner(*reinterpret_cast<const uint8_t* const*>(pInstance + 0x28), *reinterpret_cast<const uint8_t* const*>(pInstance + 0x30)))
                    return pHand;
    return nullptr;
}
std::mutex s_contactsLock;
std::array<WeaponContact, 64> s_contacts{};
size_t s_contactCount = 0;

// Where the other players' weapon bodies were touched by this player's HIGGS bodies or by what they hold, lately
// (Havok units), under s_contactsLock: a hit this player lands at one of these points landed on a weapon, not on its
// owner (IsWeaponTouchAt).
struct WeaponTouch
{
    glm::vec3 Point{};
    std::chrono::steady_clock::time_point At{};
};
std::array<WeaponTouch, 32> s_recentTouches{};
size_t s_nextRecentTouch = 0;

std::atomic<uint32_t> s_contactCallbackCount{0};

void OnWeaponContactPoint(void*, const uint8_t* apEvent) noexcept
{
    ++s_contactCallbackCount;
    if (!apEvent)
        return;
    WeaponContact contact{};
    for (size_t i = 0; i < 2; ++i)
    {
        contact.pBodies[i] = *reinterpret_cast<void* const*>(apEvent + kEventBodiesOffset + i * sizeof(void*));
        if (!contact.pBodies[i])
            return;
        const auto* pBody = static_cast<const uint8_t*>(contact.pBodies[i]);
        contact.Filters[i] = *reinterpret_cast<const uint32_t*>(pBody + kFilterInfoOffset);
        contact.MotionTypes[i] = pBody[kMotionTypeOffset];
        const float* pVelocity = reinterpret_cast<const float*>(pBody + 0x230); // hkpEntity::motion.linearVelocity
        contact.Velocities[i] = glm::vec3{pVelocity[0], pVelocity[1], pVelocity[2]};
        // Motion types 1-3 and 6 are dynamic: only those can be held by HIGGS's grab constraint.
        const uint8_t motionType = contact.MotionTypes[i];
        const uint8_t* pHiggs = IsHiggsBody(pBody) ? pBody : (motionType >= 1 && motionType <= 3) || motionType == 6 ? HiggsBodyHolding(pBody) : nullptr;
        if (pHiggs)
        {
            contact.HiggsKind[i] = pHiggs == pBody ? 1 : 2;
            const float* pAt = reinterpret_cast<const float*>(pHiggs + kBodyTranslationOffset);
            contact.HiggsAt[i] = glm::vec3{pAt[0], pAt[1], pAt[2]};
        }
    }
    if (const float* pPoint = *reinterpret_cast<const float* const*>(apEvent + kEventContactPointOffset))
        contact.Point = {pPoint[0], pPoint[1], pPoint[2]};

    std::lock_guard lock(s_contactsLock);
    if (contact.HiggsKind[0] != 0 || contact.HiggsKind[1] != 0)
        s_recentTouches[s_nextRecentTouch++ % s_recentTouches.size()] = {contact.Point, std::chrono::steady_clock::now()};
    for (size_t i = 0; i < s_contactCount; ++i)
        if (s_contacts[i].pBodies[0] == contact.pBodies[0] && s_contacts[i].pBodies[1] == contact.pBodies[1])
        {
            s_contacts[i].Point = contact.Point;
            return;
        }
    if (s_contactCount < s_contacts.size())
        s_contacts[s_contactCount++] = contact;
}

void IgnoreContactEvent(void*, const void*) noexcept
{
}

// Slot 0 contactPointCallback, 1 collisionAddedCallback, 2 collisionRemovedCallback; the rest are never wanted here.
void* s_contactListenerVTable[8] = {reinterpret_cast<void*>(&OnWeaponContactPoint), reinterpret_cast<void*>(&IgnoreContactEvent), reinterpret_cast<void*>(&IgnoreContactEvent),
                                    reinterpret_cast<void*>(&IgnoreContactEvent),   reinterpret_cast<void*>(&IgnoreContactEvent), reinterpret_cast<void*>(&IgnoreContactEvent),
                                    reinterpret_cast<void*>(&IgnoreContactEvent),   reinterpret_cast<void*>(&IgnoreContactEvent)};
struct ContactListenerObject
{
    void** pVTable;
};
ContactListenerObject s_contactListener{s_contactListenerVTable};

// Where each weapon body is to be, set at the frame end and read right before each Havok step, which need not run on
// the same thread. A body comes off this list before it leaves the world (ReleaseWeaponBody), and the step's drive runs
// under the world's write lock, which the release also needs -- so the drive never touches a body being let go.
struct WeaponTarget
{
    alignas(16) float Position[4];
    alignas(16) float Rotation[4];
    // Where the last step was to leave it: where it should be when the next step begins.
    alignas(16) float StepPosition[4];
    alignas(16) float StepRotation[4];
    bool HasStep;
    uint8_t* pHavok;
    void* pBhkWorld; // checked when the body was added; the step's own world is matched against it
};
// Per 5 s line: how far bodies were from where the last step left them when the next began (moved by someone else
// between steps), how many were put back, and how far they were from their target right after a step.
std::atomic<uint32_t> s_reseats{0};
std::mutex s_stepStatsLock;
float s_worstMovedUnits = 0.f;
float s_worstMovedDegrees = 0.f;
float s_worstAfterStepUnits = 0.f;
float s_worstAfterStepDegrees = 0.f;
std::mutex s_targetsLock;
std::vector<WeaponTarget> s_targets;
std::atomic<uint32_t> s_stepDrives{0};
// Costs, per 5 s line: time in the drive before each step, time in the frame-end body work, and contact callbacks.
std::atomic<uint64_t> s_stepDriveNanos{0};
std::atomic<uint64_t> s_frameEndNanos{0};
std::atomic<uint64_t> s_lockNanos{0};     // of the drive: taking the world's write lock
std::atomic<uint64_t> s_activateNanos{0}; // of the drive: hkpEntity::activate
struct FrameEndTimer
{
    std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
    ~FrameEndTimer() { s_frameEndNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count(); }
};
std::atomic<uint32_t> s_stepSkips{0};
std::atomic<float> s_lastStepSeconds{0.f};

void SetWeaponTarget(uint8_t* apHavok, void* apBhkWorld, const float* apPosition, const float* apRotation) noexcept
{
    std::lock_guard lock(s_targetsLock);
    auto it = std::find_if(s_targets.begin(), s_targets.end(), [apHavok](const WeaponTarget& acTarget) { return acTarget.pHavok == apHavok; });
    if (it == s_targets.end())
    {
        s_targets.push_back({});
        it = s_targets.end() - 1;
        it->pHavok = apHavok;
        it->HasStep = false;
    }
    it->pBhkWorld = apBhkWorld;
    std::copy_n(apPosition, 4, it->Position);
    std::copy_n(apRotation, 4, it->Rotation);
}

void ForgetWeaponTarget(const uint8_t* apHavok) noexcept
{
    std::lock_guard lock(s_targetsLock);
    s_targets.erase(std::remove_if(s_targets.begin(), s_targets.end(), [apHavok](const WeaponTarget& acTarget) { return acTarget.pHavok == apHavok; }), s_targets.end());
}

const char* RttiNameOf(void* apObject) noexcept
{
    if (!apObject || !IsReadable(apObject, sizeof(void*)))
        return nullptr;
    auto** ppVTable = *static_cast<void***>(apObject);
    if (!ppVTable || !IsReadable(ppVTable + kVTableGetRttiSlot, sizeof(void*)))
        return nullptr;
    using TGetRtti = const char**(__fastcall*)(void*);
    const char** ppRtti = static_cast<TGetRtti>(ppVTable[kVTableGetRttiSlot])(apObject);
    return ppRtti && IsReadable(ppRtti, sizeof(void*)) && ppRtti[0] && IsReadable(ppRtti[0], 1) ? ppRtti[0] : nullptr;
}

// The bhkWorld that owns a Havok world, checked by name: its lock is about to be taken.
void* BhkWorldOf(void* apHkpWorld) noexcept
{
    if (!apHkpWorld || !IsReadable(static_cast<uint8_t*>(apHkpWorld) + kBhkWorldOfHkpWorldOffset, sizeof(void*)))
        return nullptr;
    void* pWorld = At<void*>(apHkpWorld, kBhkWorldOfHkpWorldOffset);
    const char* pName = RttiNameOf(pWorld);
    if (!pName || std::strncmp(pName, "bhkWorld", 8) != 0 || !IsReadable(static_cast<uint8_t*>(pWorld) + kWorldLockOffset, 8))
        return nullptr;
    return pWorld;
}

class WorldWriteLock
{
public:
    explicit WorldWriteLock(void* apBhkWorld) noexcept
    {
        POINTER_SKYRIMSE(TWorldLock, s_lockForWrite, 66977, 66977);
        if (apBhkWorld && s_lockForWrite.Get())
        {
            m_pLock = static_cast<uint8_t*>(apBhkWorld) + kWorldLockOffset;
            s_lockForWrite.Get()(m_pLock);
        }
    }
    ~WorldWriteLock() noexcept
    {
        POINTER_SKYRIMSE(TWorldLock, s_unlockForWrite, 66983, 66983);
        if (m_pLock && s_unlockForWrite.Get())
            s_unlockForWrite.Get()(m_pLock);
    }
    WorldWriteLock(const WorldWriteLock&) = delete;
    WorldWriteLock& operator=(const WorldWriteLock&) = delete;
    [[nodiscard]] bool Held() const noexcept { return m_pLock != nullptr; }

private:
    void* m_pLock = nullptr;
};

// Where the body has to be for the weapon to be where it is drawn: the node's world transform, times the body's own
// offset when it is a bhkRigidBodyT (HIGGS, GetRigidBodyTLocalTransform). Havok units.
void BodyTarget(const WeaponBody& acBody, const float aHavokScale, float* apPosition, float* apRotation) noexcept
{
    const NiTransform& node = At<NiTransform>(acBody.pNode, kWorldOffset);
    glm::mat3 rotation = ToGlm(node.rotate);
    glm::vec3 position = ToGlm(node.translate);
    if (acBody.IsT)
    {
        const float* pLocalRotation = reinterpret_cast<const float*>(static_cast<uint8_t*>(acBody.pWrapper) + kRigidBodyTRotationOffset);
        const float* pLocalTranslation = reinterpret_cast<const float*>(static_cast<uint8_t*>(acBody.pWrapper) + kRigidBodyTTranslationOffset);
        const glm::quat localRotation{pLocalRotation[3], pLocalRotation[0], pLocalRotation[1], pLocalRotation[2]};
        const glm::vec3 localTranslation = glm::vec3{pLocalTranslation[0], pLocalTranslation[1], pLocalTranslation[2]} / aHavokScale;
        position += rotation * (localTranslation * node.scale);
        rotation = rotation * glm::mat3_cast(localRotation);
    }
    const glm::quat quaternion = glm::normalize(glm::quat_cast(rotation));
    position *= aHavokScale;
    apPosition[0] = position.x;
    apPosition[1] = position.y;
    apPosition[2] = position.z;
    apPosition[3] = 0.f;
    apRotation[0] = quaternion.x;
    apRotation[1] = quaternion.y;
    apRotation[2] = quaternion.z;
    apRotation[3] = quaternion.w;
}

// hkMotionState::transform's rotation: three hkVector4 columns.
glm::quat BodyRotation(const uint8_t* apHavok) noexcept
{
    const float* pColumns = reinterpret_cast<const float*>(apHavok + kBodyTransformOffset);
    const glm::mat3 rotation{glm::vec3{pColumns[0], pColumns[1], pColumns[2]}, glm::vec3{pColumns[4], pColumns[5], pColumns[6]}, glm::vec3{pColumns[8], pColumns[9], pColumns[10]}};
    return glm::normalize(glm::quat_cast(rotation));
}

float DegreesBetween(const glm::quat& acA, const glm::quat& acB) noexcept
{
    const float cosHalf = std::min(1.f, std::fabs(glm::dot(acA, acB)));
    return glm::degrees(2.f * std::acos(cosHalf));
}

// Which hkpMotion the body really has: the motion type byte says keyframed, the motion object (its vtable) says what
// the solver does with it. VTABLE_hkp*Motion_0, ids 279525-279533 in the VR Address Library.
std::string DescribeMotion(const uint8_t* apHavok) noexcept
{
    POINTER_SKYRIMSE(void, s_motion, 279525, 279525);
    POINTER_SKYRIMSE(void, s_keyframed, 279526, 279526);
    POINTER_SKYRIMSE(void, s_maxSize, 279527, 279527);
    POINTER_SKYRIMSE(void, s_fixed, 279529, 279529);
    POINTER_SKYRIMSE(void, s_sphere, 279530, 279530);
    POINTER_SKYRIMSE(void, s_box, 279531, 279531);
    POINTER_SKYRIMSE(void, s_thinBox, 279532, 279532);
    POINTER_SKYRIMSE(void, s_character, 279533, 279533);
    const void* pVTable = *reinterpret_cast<void* const*>(apHavok + 0x150);
    const char* pClass = pVTable == s_keyframed.Get() ? "keyframed"
                         : pVTable == s_box.Get()     ? "box (dynamic)"
                         : pVTable == s_sphere.Get()  ? "sphere (dynamic)"
                         : pVTable == s_thinBox.Get() ? "thin box (dynamic)"
                         : pVTable == s_fixed.Get()   ? "fixed"
                         : pVTable == s_maxSize.Get() ? "max size"
                         : pVTable == s_character.Get() ? "character"
                         : pVTable == s_motion.Get()  ? "base"
                                                      : "unknown";
    const float* pInverse = reinterpret_cast<const float*>(apHavok + 0x150 + 0xD0); // inertiaAndMassInv
    const void* pSaved = *reinterpret_cast<void* const*>(apHavok + 0x150 + 0x128);  // savedMotion
    return fmt::format("motion object {}, inverse inertia ({:.3g}, {:.3g}, {:.3g}) and inverse mass {:.3g}, {} motion saved aside", pClass, pInverse[0], pInverse[1], pInverse[2],
                       pInverse[3], pSaved ? "a" : "no");
}

// hkSweptTransform::rotation1, the quaternion the keyframe turns from (x, y, z, w).
glm::quat BodyQuaternion(const uint8_t* apHavok) noexcept
{
    const float* pQuaternion = reinterpret_cast<const float*>(apHavok + kBodyQuaternionOffset);
    return glm::normalize(glm::quat{pQuaternion[3], pQuaternion[0], pQuaternion[1], pQuaternion[2]});
}

glm::vec3 BodyPosition(const uint8_t* apHavok, const float aHavokScale) noexcept
{
    const float* pTranslation = reinterpret_cast<const float*>(apHavok + kBodyTransformOffset + 0x30);
    return glm::vec3{pTranslation[0], pTranslation[1], pTranslation[2]} / aHavokScale;
}

// In and out of the world is said at most every ten seconds per weapon: a copy whose body flickers between usable
// and not would otherwise write two lines a frame.
bool SayAboutWeaponBody(const uint64_t aKey, const bool aIn) noexcept
{
    static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_quietUntil;
    const auto cNow = std::chrono::steady_clock::now();
    auto& quietUntil = s_quietUntil[aKey << 1 | (aIn ? 1 : 0)];
    if (cNow < quietUntil)
        return false;
    quietUntil = cNow + std::chrono::seconds(10);
    return true;
}

void ReleaseWeaponBody(const uint64_t aKey, WeaponBody& aBody, const char* acpWhy) noexcept
{
    if (!aBody.pHavok)
        return;
    ForgetWeaponTarget(aBody.pHavok);
    POINTER_SKYRIMSE(THkpWorldRemoveEntity, s_removeEntity, 60493, 60493);
    POINTER_SKYRIMSE(THkpEntityContactListener, s_removeContactListener, 60095, 60095);
    void* pHkpWorld = At<void*>(aBody.pHavok, kHkWorldOffset);
    if (pHkpWorld && s_removeEntity.Get())
    {
        WorldWriteLock lock(BhkWorldOf(pHkpWorld));
        // Read again under the lock: the game may have taken it out (a cell unloading) in the meantime.
        if (lock.Held() && At<void*>(aBody.pHavok, kHkWorldOffset) == pHkpWorld)
        {
            if (aBody.Listening && s_removeContactListener.Get())
                s_removeContactListener.Get()(aBody.pHavok, &s_contactListener);
            aBody.Listening = false;
            aBody.pHavok[kMaxAngularVelocityOffset] = aBody.GameMaxAngularVelocity;
            bool removed = false;
            s_removeEntity.Get()(pHkpWorld, &removed, aBody.pHavok);
        }
    }
    // Out of every world already: nothing simulates it, so the listener comes off without a lock.
    if (!At<void*>(aBody.pHavok, kHkWorldOffset))
    {
        if (aBody.Listening && s_removeContactListener.Get())
            s_removeContactListener.Get()(aBody.pHavok, &s_contactListener);
        aBody.pHavok[kMaxAngularVelocityOffset] = aBody.GameMaxAngularVelocity;
    }
    if (SayAboutWeaponBody(aKey, false))
        spdlog::info("VRWeaponBody: actor {:X} {} weapon body out of the world ({})", static_cast<uint32_t>(aKey >> 1), (aKey & 1) ? "right" : "left", acpWhy);
    static_cast<NiRefObject*>(aBody.pWrapper)->DecRef();
    static_cast<NiRefObject*>(aBody.pNode)->DecRef();
    aBody = {};
}

// Whether a node still hangs below another, a few levels up at most (the weapon's node sits two or three below the
// hand's "WEAPON" node). Once sheathed it hangs off the sheath instead.
bool HangsUnder(void* apNode, void* apAncestor) noexcept
{
    void* pNode = apNode;
    for (int depth = 0; pNode && depth < 6; ++depth)
    {
        pNode = At<void*>(pNode, kParentOffset);
        if (pNode == apAncestor)
            return true;
    }
    return false;
}

// Where the game's own animation has each side's weapon node this frame (left, right), read before the copy is posed;
// NaN where there is none. For measuring what, besides this code, moves a weapon body.
using AnimatedWeapons = std::array<glm::vec3, 2>;
AnimatedWeapons WeaponsAsAnimated(const Rig& acRig) noexcept
{
    FrameEndTimer timer;
    AnimatedWeapons animated;
    animated.fill(glm::vec3{std::numeric_limits<float>::quiet_NaN()});
    for (size_t side = 0; side < 2; ++side)
        for (const RigBone& below : acRig.AttachBelow[side])
            if (below.pNode && HavokBodyOfLive(below.pNode))
            {
                animated[side] = ToGlm(At<NiTransform>(below.pNode, kWorldOffset).translate);
                break;
            }
    return animated;
}

// Puts the copy's drawn weapons in the world and drives them to where they are drawn. Right after the copy is posed.
void UpdateWeaponBodies(const uint32_t aFormId, Actor* apActor, const Rig& acRig, const AnimatedWeapons* apAnimated = nullptr) noexcept
{
    FrameEndTimer timer;
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    POINTER_SKYRIMSE(THkpWorldAddEntity, s_addEntity, 9000001, 9000001);
    POINTER_SKYRIMSE(THkpEntitySetPositionAndRotation, s_setPositionAndRotation, 9000002, 9000002);
    POINTER_SKYRIMSE(THkpEntityContactListener, s_addContactListener, 60094, 60094);
    if (!s_stepDeltaTime || !s_havokScale.Get() || !s_addEntity.Get() || !s_setPositionAndRotation.Get())
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            spdlog::error("VRWeaponBody: a Havok address did not resolve (step hook {}, scale {}, addEntity {}, setPositionAndRotation {}); weapons stay out of the world",
                          s_stepDeltaTime != nullptr, s_havokScale.Get() != nullptr, s_addEntity.Get() != nullptr, s_setPositionAndRotation.Get() != nullptr);
        }
        return;
    }
    const float havokScale = *s_havokScale.Get();
    if (!(havokScale > 0.f))
        return;

    // The copy's ragdoll says which world it is in; nothing is added without one.
    void* pCopyWorld = nullptr;
    for (const RigBone& bone : acRig.Body)
        if (uint8_t* pBone = bone.pNode ? HavokBodyOfLive(bone.pNode) : nullptr)
            if ((pCopyWorld = At<void*>(pBone, kHkWorldOffset)) != nullptr)
                break;

    // Out: drawn, or drawn and asked to go back (a copy's own AI asks to sheathe, and its graph, which only its owner's
    // actions drive, may never do it: in the rig on 2026-10-06 a copy went "drawing" to "wantToSheathe" within 1.5 s
    // and still had the sword in its hand 30 s later), or on its way back. The node hanging under the hand decides.
    const uint32_t weaponState = apActor->actorState.flags2 >> 5 & 7;
    const bool cOut = weaponState >= 3 && weaponState <= 5;
    for (uint32_t side = 0; side < 2; ++side)
    {
        const uint64_t key = (static_cast<uint64_t>(aFormId) << 1) | side;
        void* pAttach = acRig.Attach[side].pNode;

        // The weapon's node and body as the rig has them now.
        void* pNode = nullptr;
        uint8_t* pHavok = nullptr;
        if (pAttach)
            for (const RigBone& below : acRig.AttachBelow[side])
                if (below.pNode && (pHavok = HavokBodyOfLive(below.pNode)) != nullptr)
                {
                    pNode = below.pNode;
                    break;
                }
        const bool cInHand = pNode && HangsUnder(pNode, pAttach);
        if (pNode && (!cOut || !cInHand || !pCopyWorld))
        {
            // Why a weapon with a body is left out, every ten seconds at most.
            static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_nextWhyOut;
            const auto cNow = std::chrono::steady_clock::now();
            auto& nextWhyOut = s_nextWhyOut[key];
            if (cNow >= nextWhyOut)
            {
                nextWhyOut = cNow + std::chrono::seconds(10);
                spdlog::info("VRWeaponBody: actor {:X} {} weapon has a body but stays out: weapon state {} (3 drawn, 4 asked to sheathe, 5 sheathing), {} the hand, {}", aFormId,
                             side ? "right" : "left", weaponState, cInHand ? "under" : "not under", pCopyWorld ? "its ragdoll in a world" : "no ragdoll in a world");
            }
        }
        if (!cOut || !cInHand)
        {
            pNode = nullptr;
            pHavok = nullptr;
        }

        auto it = s_weaponBodies.find(key);
        if (it != s_weaponBodies.end())
        {
            WeaponBody& body = it->second;
            void* pBodyWorld = At<void*>(body.pHavok, kHkWorldOffset);
            const char* pWhy = !cOut                               ? "the weapon is not out"
                               : body.pHavok != pHavok             ? "another weapon or none in the hand"
                               : !pCopyWorld || pBodyWorld != pCopyWorld ? "the copy is in another world, or the game took the body out"
                                                                   : nullptr;
            if (pWhy)
            {
                ReleaseWeaponBody(key, body, pWhy);
                s_weaponBodies.erase(it);
                it = s_weaponBodies.end();
            }
        }
        if (!pHavok || !pCopyWorld)
            continue;

        alignas(16) float position[4];
        alignas(16) float rotation[4];
        if (it == s_weaponBodies.end())
        {
            static std::unordered_set<uint64_t> s_saidBusy;
            if (At<void*>(pHavok, kHkWorldOffset) != nullptr)
            {
                // Someone else put it in a world; it is theirs.
                if (s_saidBusy.insert(key).second)
                    spdlog::info("VRWeaponBody: actor {:X} {} weapon body is already in a world; left alone", aFormId, side ? "right" : "left");
                continue;
            }
            // A failed add is tried again after five seconds, not every frame: the checks below are slow ones.
            static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_retryAt;
            const auto cNowAdd = std::chrono::steady_clock::now();
            if (const auto retry = s_retryAt.find(key); retry != s_retryAt.end() && cNowAdd < retry->second)
                continue;
            s_retryAt[key] = cNowAdd + std::chrono::seconds(5);
            void* pBhkWorld = BhkWorldOf(pCopyWorld);
            if (!pBhkWorld)
            {
                if (s_saidBusy.insert(key).second)
                    spdlog::warn("VRWeaponBody: actor {:X}: the world its ragdoll is in has no bhkWorld behind it; its weapon stays out", aFormId);
                continue;
            }

            WeaponBody body;
            body.pNode = pNode;
            body.pWrapper = At<void*>(At<void*>(pNode, kCollisionObjectOffset), kCollisionBodyOffset);
            body.pHavok = pHavok;
            body.pBhkWorld = pBhkWorld;
            const char* pWrapperClass = RttiNameOf(body.pWrapper);
            body.IsT = pWrapperClass && std::strcmp(pWrapperClass, "bhkRigidBodyT") == 0;
            if (!pWrapperClass || (std::strcmp(pWrapperClass, "bhkRigidBody") != 0 && !body.IsT) || !IsReadable(pHavok, kBodyQuaternionOffset + 0x10))
            {
                if (s_saidBusy.insert(key).second)
                    spdlog::warn("VRWeaponBody: actor {:X} {} weapon's body is a {}, not a rigid body; left out", aFormId, side ? "right" : "left", pWrapperClass ? pWrapperClass : "?");
                continue;
            }
            BodyTarget(body, havokScale, position, rotation);
            {
                WorldWriteLock lock(pBhkWorld);
                if (!lock.Held() || At<void*>(pHavok, kHkWorldOffset) != nullptr)
                    continue;
                static_cast<NiRefObject*>(body.pNode)->IncRef();
                static_cast<NiRefObject*>(body.pWrapper)->IncRef();
                body.GameQuality = static_cast<int8_t>(pHavok[kQualityTypeOffset]);
                s_setPositionAndRotation.Get()(pHavok, position, rotation);
                // The game gives its weapon body 31.6 rad/s at most; a drawn sword turns faster than that, and the
                // keyframe, capped there, never settled (up to 160 degrees off, 2026-10-06 -- while asked to turn not at
                // all the body held still to 0.1 degrees). HIGGS gives its own weapon bodies 500; so does this, while
                // the body is ours.
                body.GameMaxAngularVelocity = pHavok[kMaxAngularVelocityOffset];
                std::memcpy(&body.GameAngularDamping, pHavok + kAngularDampingOffset, sizeof(uint16_t));
                POINTER_SKYRIMSE(THkRealToUFloat8, s_toUFloat8, 9000010, 9000010);
                if (s_toUFloat8.Get())
                {
                    constexpr float cMaxAngularVelocity = 500.f;
                    s_toUFloat8.Get()(pHavok[kMaxAngularVelocityOffset], cMaxAngularVelocity);
                }
                const glm::quat cTarget{rotation[3], rotation[0], rotation[1], rotation[2]};
                const float setMatrix = DegreesBetween(BodyRotation(pHavok), cTarget);
                const float setQuaternion = DegreesBetween(BodyQuaternion(pHavok), cTarget);
                s_addEntity.Get()(pCopyWorld, pHavok, 1); // HK_ENTITY_ACTIVATION_DO_ACTIVATE
                spdlog::info("VRWeaponBody: actor {:X} {} weapon body turned as asked: {:.1f} degrees off (matrix) and {:.1f} (quaternion) once set, {:.1f} and {:.1f} once "
                             "added to the world; {}; its angular limit (hkUFloat8) {:#04x}, raised to {:#04x}; damping {:.3g} linear and {:.3g} angular (now {:.3g}), "
                             "time factor {:.3g}; collision quality {}, now {}; {} constraints as master, {} as slave, {} actions",
                             aFormId, side ? "right" : "left", setMatrix, setQuaternion, DegreesBetween(BodyRotation(pHavok), cTarget), DegreesBetween(BodyQuaternion(pHavok), cTarget),
                             DescribeMotion(pHavok), body.GameMaxAngularVelocity, pHavok[kMaxAngularVelocityOffset],
                             FromHalf(*reinterpret_cast<const uint16_t*>(pHavok + kLinearDampingOffset)), FromHalf(body.GameAngularDamping),
                             FromHalf(*reinterpret_cast<const uint16_t*>(pHavok + kAngularDampingOffset)), FromHalf(*reinterpret_cast<const uint16_t*>(pHavok + kTimeFactorOffset)),
                             body.GameQuality, static_cast<int8_t>(pHavok[kQualityTypeOffset]),
                             *reinterpret_cast<const uint16_t*>(pHavok + 0x100 + 0x8), *reinterpret_cast<const int32_t*>(pHavok + 0x110 + 0x8),
                             *reinterpret_cast<const uint16_t*>(pHavok + 0x2A0 + 0x8)); // hkpEntity constraintsMaster, constraintsSlave, actions (sizes)
                if (s_addContactListener.Get())
                {
                    s_addContactListener.Get()(pHavok, &s_contactListener);
                    body.Listening = true;
                }
            }
            body.Seen = true;
            s_weaponBodies[key] = body;
            s_retryAt.erase(key);
            const uint32_t filter = At<uint32_t>(pHavok, kFilterInfoOffset);
            const char* pName = GetName(pNode);
            const char* pClass = RttiNameOf(body.pWrapper);
            // Read back from where Havok keeps it: 0 here says the transform is read at the right offset.
            const float placedOff = glm::length(BodyPosition(pHavok, havokScale) - glm::vec3{position[0], position[1], position[2]} / havokScale);
            if (SayAboutWeaponBody(key, true))
                spdlog::info("VRWeaponBody: actor {:X} {} weapon '{}' ({}) is a body in the world now: layer {}, group {}, motion type {}; placed, it reads {:.1f} units from where it was put; "
                             "hkpRigidBody at {}",
                             aFormId, side ? "right" : "left", pName && IsReadable(pName, 1) ? pName : "?", pClass ? pClass : "?", filter & 0x7F, filter >> 16,
                             At<uint8_t>(pHavok, kMotionTypeOffset), placedOff, fmt::ptr(pHavok));
            continue;
        }

        // Where it is to be. The drive itself happens right before Havok's step (DriveWeaponBodiesBeforeStep), the way
        // HIGGS drives its weapon bodies. Driven from here, at the renderer's frame end, the velocity given was not the
        // one Havok integrated (2026-10-06, a 24-frame trace: the body turned toward the target one frame and back the
        // next, or not at all while 60 rad/s was set), and it never settled: 4-18 units and up to 160 degrees off.
        WeaponBody& body = it->second;
        body.Seen = true;
        BodyTarget(body, havokScale, position, rotation);
        SetWeaponTarget(body.pHavok, body.pBhkWorld, position, rotation);

        // How far the body is from where the weapon is drawn now, after the last step: what the drive leaves behind.
        const glm::vec3 wanted = glm::vec3{position[0], position[1], position[2]} / havokScale;
        const float gap = glm::length(BodyPosition(body.pHavok, havokScale) - wanted);
        const glm::quat target{rotation[3], rotation[0], rotation[1], rotation[2]};
        body.WorstGap = std::max(body.WorstGap, gap);
        const float lag = DegreesBetween(BodyRotation(body.pHavok), target);
        body.WorstLag = std::max(body.WorstLag, lag);
        if (lag > 10.f)
            ++body.FramesOff;
        if (body.Driven > 0)
            body.WorstTargetStep = std::max(body.WorstTargetStep, DegreesBetween(body.LastTarget, target));
        body.LastTarget = target;
        ++body.Driven;

        static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_nextGapLog;
        const auto cNow = std::chrono::steady_clock::now();
        auto& nextGapLog = s_nextGapLog[key];
        if (cNow >= nextGapLog)
        {
            nextGapLog = cNow + std::chrono::seconds(5);
            // Where its centre of mass is held during the steps: from the target, not read from the body now -- between
            // steps the game has it turned to where its animation holds the sword (2026-10-06), 20 units away.
            const float* pCentreLocal = reinterpret_cast<const float*>(body.pHavok + kBodyTransformOffset + 0x80);
            const glm::vec3 centre = wanted + glm::mat3_cast(target) * (glm::vec3{pCentreLocal[0], pCentreLocal[1], pCentreLocal[2]} / havokScale);
            const glm::vec3 animated = apAnimated ? (*apAnimated)[side] : glm::vec3{std::numeric_limits<float>::quiet_NaN()};
            const float fromAnimated = glm::length(BodyPosition(body.pHavok, havokScale) - animated);
            const uint32_t stepDrives = s_stepDrives.exchange(0);
            const uint32_t stepSkips = s_stepSkips.exchange(0);
            {
                std::lock_guard statsLock(s_stepStatsLock);
                spdlog::info("VRWeaponBody: over the last 5 s, right after each step up to {:.2f} units and {:.2f} degrees from where it was to be; between steps moved by "
                             "something else up to {:.1f} units and {:.1f} degrees, put back {} times",
                             s_worstAfterStepUnits, s_worstAfterStepDegrees, s_worstMovedUnits, s_worstMovedDegrees, s_reseats.exchange(0));
                s_worstAfterStepUnits = s_worstAfterStepDegrees = s_worstMovedUnits = s_worstMovedDegrees = 0.f;
            }
            spdlog::info("VRWeaponBody: cost over the last 5 s: {:.2f} ms driving before steps ({:.2f} taking the world lock, {:.2f} waking the bodies), {:.2f} ms at "
                         "frame ends, {} contact callbacks",
                         s_stepDriveNanos.exchange(0) / 1e6, s_lockNanos.exchange(0) / 1e6, s_activateNanos.exchange(0) / 1e6, s_frameEndNanos.exchange(0) / 1e6,
                         s_contactCallbackCount.exchange(0));
            spdlog::info("VRWeaponBody: actor {:X} {} weapon body {:.1f} units from where the weapon is drawn, {:.1f} from where the game's animation had it; its centre of "
                         "mass at ({:.1f}, {:.1f}, {:.1f}); over {} frames: up to {:.1f} units and {:.1f} degrees off ({} frames more than 10), the target turning up to "
                         "{:.1f} degrees a frame; "
                         "{} drives right before a step ({:.4f} s the last), {} steps skipped (world locked elsewhere)",
                         aFormId, side ? "right" : "left", gap, std::isfinite(fromAnimated) ? fromAnimated : -1.f, centre.x, centre.y, centre.z, body.Driven, body.WorstGap,
                         body.WorstLag, body.FramesOff, body.WorstTargetStep, stepDrives, s_lastStepSeconds.load(), stepSkips);
            body.Driven = 0;
            body.FramesOff = 0;
            body.WorstGap = body.WorstLag = body.WorstTargetStep = 0.f;
        }
    }
}

// Right before Havok steps a world: every weapon body in it is woken and keyframed to where its weapon was last drawn,
// over exactly the time this step covers. Called from inside bhkWorld::Update; the world's write lock is taken only
// when no other thread holds it or this one already does -- never waited on for a lock this thread may hold for
// reading -- and the step goes undriven otherwise (counted).
void DriveWeaponBodiesBeforeStep(void* apHkpWorld, const float aSeconds) noexcept
{
    if (!(aSeconds > 0.f))
        return;
    const auto cStart = std::chrono::steady_clock::now();
    struct AddTime
    {
        std::chrono::steady_clock::time_point Start;
        ~AddTime() { s_stepDriveNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count(); }
    } addTime{cStart};
    {
        std::lock_guard lock(s_targetsLock);
        if (s_targets.empty())
            return;
    }
    POINTER_SKYRIMSE(THkpEntitySetPositionAndRotation, s_setPositionAndRotation, 9000002, 9000002);
    POINTER_SKYRIMSE(THkpEntityActivate, s_activate, 60096, 60096);
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    if (!s_setPositionAndRotation.Get() || !s_activate.Get() || !s_havokScale.Get() || !(*s_havokScale.Get() > 0.f))
        return;

    // The world being stepped is alive; its bhkWorld is used only if a body of ours was added to that one (checked then).
    void* pBhkWorld = At<void*>(apHkpWorld, kBhkWorldOfHkpWorldOffset);
    {
        std::lock_guard targetsLock(s_targetsLock);
        if (!pBhkWorld || std::none_of(s_targets.begin(), s_targets.end(), [pBhkWorld](const WeaponTarget& acTarget) { return acTarget.pBhkWorld == pBhkWorld; }))
            return;
    }
    const auto* pLock = reinterpret_cast<const volatile uint32_t*>(static_cast<uint8_t*>(pBhkWorld) + kWorldLockOffset); // BSReadWriteLock: writer thread, lock word
    if (pLock[0] != GetCurrentThreadId() && pLock[1] != 0)
    {
        ++s_stepSkips;
        return;
    }
    const auto cBeforeLock = std::chrono::steady_clock::now();
    WorldWriteLock lock(pBhkWorld);
    s_lockNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cBeforeLock).count();
    if (!lock.Held())
        return;

    std::array<WeaponTarget, 8> targets;
    size_t count = 0;
    {
        std::lock_guard targetsLock(s_targetsLock);
        for (const WeaponTarget& target : s_targets)
            if (count < targets.size() && target.pBhkWorld == pBhkWorld && At<void*>(target.pHavok, kHkWorldOffset) == apHkpWorld)
                targets[count++] = target;
    }

    const float havokScale = *s_havokScale.Get();
    for (size_t i = 0; i < count; ++i)
    {
        WeaponTarget& target = targets[i];
        uint8_t* pHavok = target.pHavok;
        // Awake, as HIGGS keeps its own weapon bodies: a sleeping body ignores the velocity a keyframe gives it.
        const auto cBeforeActivate = std::chrono::steady_clock::now();
        s_activate.Get()(pHavok);
        s_activateNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cBeforeActivate).count();
        const glm::mat3 targetRotation = glm::mat3_cast(glm::quat{target.Rotation[3], target.Rotation[0], target.Rotation[1], target.Rotation[2]});
        const glm::vec3 targetPosition{target.Position[0], target.Position[1], target.Position[2]};
        const float* pColumns = reinterpret_cast<const float*>(pHavok + kBodyTransformOffset);
        const glm::mat3 bodyRotation{glm::vec3{pColumns[0], pColumns[1], pColumns[2]}, glm::vec3{pColumns[4], pColumns[5], pColumns[6]},
                                     glm::vec3{pColumns[8], pColumns[9], pColumns[10]}};
        const glm::vec3 bodyPosition{pColumns[12], pColumns[13], pColumns[14]};

        // Further than a sword's length from the target is a jump (a teleport, a load), not a swing: placed, not swept.
        if (!target.HasStep || glm::length(bodyPosition - targetPosition) / havokScale > 100.f)
        {
            s_setPositionAndRotation.Get()(pHavok, target.Position, target.Rotation);
            float* pLinear = reinterpret_cast<float*>(pHavok + kAngularVelocityOffset - 0x10);
            float* pAngular = reinterpret_cast<float*>(pHavok + kAngularVelocityOffset);
            std::fill_n(pLinear, 4, 0.f);
            std::fill_n(pAngular, 4, 0.f);
        }
        else
        {
            // Where the last step left it is where it starts. The game moves its weapon body to where its own
            // animation holds the sword now and then, between steps (bhkRigidBody's setters, from a worker thread;
            // 2026-10-06, every frame at 60 fps): driven from there, it swept 150 degrees through whatever was near in
            // one step. Put back instead, with no speed, and then moved only by as much as the drawn sword moved.
            const glm::mat3 startRotation = glm::mat3_cast(glm::quat{target.StepRotation[3], target.StepRotation[0], target.StepRotation[1], target.StepRotation[2]});
            const glm::vec3 startPosition{target.StepPosition[0], target.StepPosition[1], target.StepPosition[2]};
            glm::quat moved = glm::normalize(glm::quat_cast(bodyRotation * glm::transpose(startRotation)));
            const float movedDegrees = glm::degrees(glm::angle(moved.w < 0.f ? -moved : moved));
            const float movedUnits = glm::length(bodyPosition - startPosition) / havokScale;
            {
                std::lock_guard statsLock(s_stepStatsLock);
                s_worstMovedUnits = std::max(s_worstMovedUnits, movedUnits);
                s_worstMovedDegrees = std::max(s_worstMovedDegrees, movedDegrees);
            }
            if (movedUnits > 1.f || movedDegrees > 3.f)
            {
                s_setPositionAndRotation.Get()(pHavok, target.StepPosition, target.StepRotation);
                ++s_reseats;
            }

            // The velocities that take it from there to the target over this step, from rotation matrices (handed to
            // applyHardKeyFrame as a quaternion, it sat ~145 degrees off about one axis in every build).
            glm::quat turn = glm::normalize(glm::quat_cast(targetRotation * glm::transpose(startRotation)));
            if (turn.w < 0.f)
                turn = -turn;
            const float angle = glm::angle(turn);
            const glm::vec3 spin = angle > 1e-5f ? glm::axis(turn) * (angle / aSeconds) : glm::vec3{};
            const float* pCentreLocal = reinterpret_cast<const float*>(pHavok + kBodyTransformOffset + 0x80);
            const glm::vec3 centreLocal{pCentreLocal[0], pCentreLocal[1], pCentreLocal[2]};
            const glm::vec3 linear = ((targetPosition + targetRotation * centreLocal) - (startPosition + startRotation * centreLocal)) / aSeconds;
            float* pLinear = reinterpret_cast<float*>(pHavok + kAngularVelocityOffset - 0x10);
            float* pAngular = reinterpret_cast<float*>(pHavok + kAngularVelocityOffset);
            pLinear[0] = linear.x, pLinear[1] = linear.y, pLinear[2] = linear.z, pLinear[3] = 0.f;
            pAngular[0] = spin.x, pAngular[1] = spin.y, pAngular[2] = spin.z, pAngular[3] = 0.f;
        }

        // This step leaves it at the target.
        {
            std::lock_guard targetsLock(s_targetsLock);
            for (WeaponTarget& stored : s_targets)
                if (stored.pHavok == pHavok)
                {
                    std::copy_n(target.Position, 4, stored.StepPosition);
                    std::copy_n(target.Rotation, 4, stored.StepRotation);
                    stored.HasStep = true;
                }
        }
        ++s_stepDrives;
    }
    s_lastStepSeconds = aSeconds;
}

// The call to ahkpWorld::stepDeltaTime in bhkWorld::Update, VR 0xDFB722 (HIGGS's src/hooks.cpp, prePhysicsStepHookLoc:
// it hooks the same call, and whichever of the two patches second calls the other). The call is replaced only if it
// is a call; whatever it called is called after the drive.

// Right after the step: how far each weapon body ended from where the step was to take it.
void MeasureWeaponBodiesAfterStep(void* apHkpWorld) noexcept
{
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    if (!s_havokScale.Get() || !(*s_havokScale.Get() > 0.f))
        return;
    void* pBhkWorld = At<void*>(apHkpWorld, kBhkWorldOfHkpWorldOffset);
    {
        std::lock_guard targetsLock(s_targetsLock);
        if (!pBhkWorld || std::none_of(s_targets.begin(), s_targets.end(), [pBhkWorld](const WeaponTarget& acTarget) { return acTarget.pBhkWorld == pBhkWorld; }))
            return;
    }
    const auto* pLock = reinterpret_cast<const volatile uint32_t*>(static_cast<uint8_t*>(pBhkWorld) + kWorldLockOffset);
    if (pLock[0] != GetCurrentThreadId() && pLock[1] != 0)
        return;
    WorldWriteLock lock(pBhkWorld);
    if (!lock.Held())
        return;
    std::lock_guard targetsLock(s_targetsLock);
    for (const WeaponTarget& target : s_targets)
    {
        if (!target.HasStep || target.pBhkWorld != pBhkWorld || At<void*>(target.pHavok, kHkWorldOffset) != apHkpWorld)
            continue;
        const float* pColumns = reinterpret_cast<const float*>(target.pHavok + kBodyTransformOffset);
        const glm::mat3 bodyRotation{glm::vec3{pColumns[0], pColumns[1], pColumns[2]}, glm::vec3{pColumns[4], pColumns[5], pColumns[6]},
                                     glm::vec3{pColumns[8], pColumns[9], pColumns[10]}};
        const glm::mat3 stepRotation = glm::mat3_cast(glm::quat{target.StepRotation[3], target.StepRotation[0], target.StepRotation[1], target.StepRotation[2]});
        glm::quat off = glm::normalize(glm::quat_cast(bodyRotation * glm::transpose(stepRotation)));
        const float offDegrees = glm::degrees(glm::angle(off.w < 0.f ? -off : off));
        const float offUnits = glm::length(glm::vec3{pColumns[12], pColumns[13], pColumns[14]} - glm::vec3{target.StepPosition[0], target.StepPosition[1], target.StepPosition[2]}) /
                               *s_havokScale.Get();
        std::lock_guard statsLock(s_stepStatsLock);
        s_worstAfterStepUnits = std::max(s_worstAfterStepUnits, offUnits);
        s_worstAfterStepDegrees = std::max(s_worstAfterStepDegrees, offDegrees);
    }
}

int HookStepDeltaTime(void* apHkpWorld, float aSeconds)
{
    DriveWeaponBodiesBeforeStep(apHkpWorld, aSeconds);
    const int result = s_stepDeltaTime(apHkpWorld, aSeconds);
    MeasureWeaponBodiesAfterStep(apHkpWorld);
    return result;
}

#ifdef SKYRIMVR
TiltedPhoques::Initializer s_stepHook(
    []()
    {
        VersionDbPtr<uint8_t> callSite(9000009);
        uint8_t* pCall = callSite.Get();
        if (!pCall || *pCall != 0xE8)
        {
            spdlog::error("VRWeaponBody: the call to Havok's step is not where it was expected; weapons stay out of the world");
            return;
        }
        TiltedPhoques::SwapCall(pCall, s_stepDeltaTime, &HookStepDeltaTime);
        spdlog::info("VRWeaponBody: weapon bodies are driven right before Havok's step (call at {}, which called {})", fmt::ptr(pCall), fmt::ptr(s_stepDeltaTime));
    });
#endif

// Physics queue P2 (2026-10-06): the copy's ragdoll driven toward its drawn pose, not its animation. Its ragdoll bodies
// are what everything collides with, and they were 83-98 units from where its hands are drawn and 2-3 from where its
// own animation had them ("VRRagdoll" lines, the rig). The behaviour graph hands the ragdoll driver the animation pose
// in the generator output's pose track just before hkbRagdollDriver::driveToPose; PLANCK hooks the same call to steer
// its active ragdolls. For a remote player's copy that track gets, bone by bone, the pose drawn at the last frame end.
//
// The frame end records, per copy, the drawn world transform of every animation-skeleton bone that has a node of the
// same name; the hook turns those into the track's parent-relative transforms, parents first, so each such bone's
// model-space transform is the drawn one. Bones with no node keep the animation's. Units are measured once per copy
// (the track's model-to-world against the copy's position), never assumed; anything unexpected and the track is left
// as the game made it.
constexpr uint32_t kDriverCharacterOffset = 0x80;     // hkbRagdollDriver::character (PLANCK's static_assert)
constexpr uint32_t kCharacterSetupOffset = 0x50;      // hkbCharacter::setup
constexpr uint32_t kCharacterWorldFromModel = 0x88;   // hkbCharacter::worldFromModel (hkQsTransform*)
constexpr uint32_t kSetupAnimationSkeleton = 0x20;    // hkbCharacterSetup::m_animationSkeleton
constexpr uint32_t kSkeletonParents = 0x18;           // hkaSkeleton::parentIndices (hkArray<int16>: data, size)
constexpr uint32_t kSkeletonBones = 0x28;             // hkaSkeleton::bones (hkArray<hkaBone>, 0x10 each, name first)
constexpr uint32_t kQsTransformSize = 0x30;           // hkQsTransform: translation, rotation (x, y, z, w), scale

struct DrawnPose
{
    std::vector<NiTransform> World;  // by animation bone
    std::vector<uint8_t> Has;        // 1 where the bone has a drawn node
    glm::vec3 ActorAt{};             // the copy's own position (the game's), for the units
    uint32_t FormId = 0;
    std::chrono::steady_clock::time_point At{};
};
std::mutex s_drawnPosesLock;
std::unordered_map<const void*, DrawnPose> s_drawnPoses; // by hkbCharacter
std::atomic<uint32_t> s_drawnPoseCount{0};
// hkbRagdollDriver::postPhysics as called before our hook went on its call (null until then; see HookPostPhysicsOnce).
using TPostPhysics = bool(void* apDriver, const void* apContext, void* apInOut);
TPostPhysics* s_postPhysicsNext = nullptr;

// Per copy, on the frame-end thread only: which hkbCharacter is its, and the node of each of its animation bones.
struct CharacterLink
{
    void* pRoot = nullptr;
    void* pCharacter = nullptr;
    std::vector<void*> BoneNodes;
};
std::unordered_map<uint32_t, CharacterLink> s_characterLinks;

void* CharacterOf(Actor* apActor) noexcept
{
    BSAnimationGraphManager* pManager = nullptr;
    void* pCharacter = nullptr;
    if (apActor->animationGraphHolder.GetBSAnimationGraph(&pManager) && pManager)
    {
        const uint32_t index = pManager->ResolveGraphIndex();
        if (index < pManager->animationGraphs.size)
            if (BShkbAnimationGraph* pGraph = pManager->animationGraphs.Get(index))
                pCharacter = &pGraph->character;
    }
    if (pManager)
        pManager->Release();
    return pCharacter;
}

void HookPostPhysicsOnce() noexcept;

void RecordDrawnPose(const uint32_t aFormId, Actor* apActor, const Rig& acRig) noexcept
{
#ifdef SKYRIMVR
    HookPostPhysicsOnce();
    if (!s_postPhysicsNext)
        return;
#else
    return;
#endif
    CharacterLink& link = s_characterLinks[aFormId];
    if (link.pRoot != acRig.pRoot || !link.pCharacter)
    {
        // Once per 3D: the slow lookups (a search of the skeleton per bone) are not for every frame.
        link = {};
        link.pRoot = acRig.pRoot;
        link.pCharacter = CharacterOf(apActor);
        void* pSetup = link.pCharacter ? At<void*>(link.pCharacter, kCharacterSetupOffset) : nullptr;
        void* pSkeleton = pSetup ? At<void*>(pSetup, kSetupAnimationSkeleton) : nullptr;
        if (!pSkeleton || !IsReadable(static_cast<uint8_t*>(pSkeleton) + kSkeletonBones, 0x10))
        {
            link.pCharacter = nullptr;
            return;
        }
        const int32_t count = At<int32_t>(pSkeleton, kSkeletonBones + 0x8);
        const uint8_t* pBones = At<uint8_t*>(pSkeleton, kSkeletonBones);
        if (count <= 0 || count > 512 || !pBones || !IsReadable(pBones, static_cast<size_t>(count) * 0x10))
        {
            link.pCharacter = nullptr;
            return;
        }
        uint32_t matched = 0;
        link.BoneNodes.assign(count, nullptr);
        for (int32_t i = 0; i < count; ++i)
        {
            const auto* pName = reinterpret_cast<const char*>(*reinterpret_cast<const uintptr_t*>(pBones + i * 0x10) & ~uintptr_t{1});
            if (pName && IsReadable(pName, 2))
                if ((link.BoneNodes[i] = FindShallowest(acRig.pRoot, pName)) != nullptr)
                    ++matched;
        }
        spdlog::info("VRRagdoll: actor {:X}: {} of its {} animation bones have a node to take the drawn pose from", aFormId, matched, count);
    }

    DrawnPose pose;
    pose.World.resize(link.BoneNodes.size());
    pose.Has.resize(link.BoneNodes.size());
    for (size_t i = 0; i < link.BoneNodes.size(); ++i)
        if (void* pNode = link.BoneNodes[i])
        {
            pose.World[i] = At<NiTransform>(pNode, kWorldOffset);
            pose.Has[i] = 1;
        }
    pose.ActorAt = glm::vec3{apActor->position.x, apActor->position.y, apActor->position.z};
    pose.FormId = aFormId;
    const auto cNow = std::chrono::steady_clock::now();
    pose.At = cNow;

    std::lock_guard lock(s_drawnPosesLock);
    s_drawnPoses[link.pCharacter] = std::move(pose);
    // Old ones go: a copy no longer posed leaves its entry to age out here.
    for (auto it = s_drawnPoses.begin(); it != s_drawnPoses.end();)
        it = cNow - it->second.At > std::chrono::seconds(2) ? s_drawnPoses.erase(it) : std::next(it);
    s_drawnPoseCount = static_cast<uint32_t>(s_drawnPoses.size());
}

struct QsPose
{
    glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 Translation{};
    float Scale = 1.f;
};

QsPose Compose(const QsPose& acParent, const QsPose& acLocal) noexcept
{
    return QsPose{acParent.Rotation * acLocal.Rotation, acParent.Translation + acParent.Rotation * (acLocal.Translation * acParent.Scale), acParent.Scale * acLocal.Scale};
}

QsPose Inverse(const QsPose& acPose) noexcept
{
    const glm::quat inverse = glm::inverse(acPose.Rotation);
    const float inverseScale = acPose.Scale != 0.f ? 1.f / acPose.Scale : 1.f;
    return QsPose{inverse, inverse * (-acPose.Translation) * inverseScale, inverseScale};
}

QsPose ReadQs(const float* apQs) noexcept
{
    return QsPose{glm::normalize(glm::quat{apQs[7], apQs[4], apQs[5], apQs[6]}), glm::vec3{apQs[0], apQs[1], apQs[2]}, apQs[8]};
}

std::atomic<uint32_t> s_ragdollPosesWritten{0};

// The generator output's pose track (track 2) when it is dense hkQsTransforms, one per bone; null otherwise.
float* PoseTrackOf(void* apGeneratorOutput, int16_t& aCount) noexcept
{
    aCount = 0;
    auto* pTracks = apGeneratorOutput ? At<uint8_t*>(apGeneratorOutput, 0) : nullptr;
    if (!pTracks || At<int32_t>(pTracks, 0x4) <= 2)
        return nullptr;
    const uint8_t* pHeader = pTracks + 0x10 + 2 * 0x10;
    const int16_t numData = *reinterpret_cast<const int16_t*>(pHeader + 0x2);
    const int16_t dataOffset = *reinterpret_cast<const int16_t*>(pHeader + 0x4);
    const int16_t elementSize = *reinterpret_cast<const int16_t*>(pHeader + 0x6);
    const int8_t flags = *reinterpret_cast<const int8_t*>(pHeader + 0xC);
    const int8_t type = *reinterpret_cast<const int8_t*>(pHeader + 0xD);
    if (type != 1 || (flags & 0x6) || elementSize != kQsTransformSize || numData <= 0 || numData > 512)
        return nullptr;
    aCount = numData;
    return reinterpret_cast<float*>(pTracks + dataOffset);
}

// Whether this ragdoll driver's character is a copy whose drawn pose is being fed to its ragdoll.
bool IsDrivenCharacter(const void* apDriver) noexcept
{
    if (s_drawnPoseCount == 0 || !apDriver)
        return false;
    const void* pCharacter = At<void*>(const_cast<void*>(apDriver), kDriverCharacterOffset);
    std::lock_guard lock(s_drawnPosesLock);
    const auto it = s_drawnPoses.find(pCharacter);
    return it != s_drawnPoses.end() && std::chrono::steady_clock::now() - it->second.At <= std::chrono::milliseconds(250);
}

// Puts the drawn pose in the track; what was there before goes into aSaved, to be put back once the ragdoll is driven.
bool PutDrawnPoseIntoTrack(void* apDriver, void* apGeneratorOutput, std::vector<uint8_t>& aSaved) noexcept
{
    if (s_drawnPoseCount == 0 || !apDriver || !apGeneratorOutput)
        return false;
    void* pCharacter = At<void*>(apDriver, kDriverCharacterOffset);
    if (!pCharacter)
        return false;
    DrawnPose drawn;
    {
        std::lock_guard lock(s_drawnPosesLock);
        const auto it = s_drawnPoses.find(pCharacter);
        if (it == s_drawnPoses.end() || std::chrono::steady_clock::now() - it->second.At > std::chrono::milliseconds(250))
            return false;
        drawn = it->second;
    }

    // The tracks: the pose (2) must be dense hkQsTransforms, one per animation bone; model-to-world (0) as well.
    auto* pTracks = At<uint8_t*>(apGeneratorOutput, 0);
    if (!pTracks || At<int32_t>(pTracks, 0x4) <= 2)
        return false;
    const auto header = [pTracks](const int aTrack) { return pTracks + 0x10 + aTrack * 0x10; };
    const uint8_t* pPoseHeader = header(2);
    const int16_t numData = *reinterpret_cast<const int16_t*>(pPoseHeader + 0x2);
    const int16_t dataOffset = *reinterpret_cast<const int16_t*>(pPoseHeader + 0x4);
    const int16_t elementSize = *reinterpret_cast<const int16_t*>(pPoseHeader + 0x6);
    const float onFraction = *reinterpret_cast<const float*>(pPoseHeader + 0x8);
    const int8_t flags = *reinterpret_cast<const int8_t*>(pPoseHeader + 0xC);
    const int8_t type = *reinterpret_cast<const int8_t*>(pPoseHeader + 0xD);
    void* pSetup = At<void*>(pCharacter, kCharacterSetupOffset);
    void* pSkeleton = pSetup ? At<void*>(pSetup, kSetupAnimationSkeleton) : nullptr;
    if (!pSkeleton || type != 1 || (flags & 0x6) || elementSize != kQsTransformSize || !(onFraction > 0.f) || numData != static_cast<int16_t>(drawn.World.size()) ||
        At<int32_t>(pSkeleton, kSkeletonParents + 0x8) != numData)
        return false;
    auto* pPose = reinterpret_cast<float*>(pTracks + dataOffset);
    const auto* pParents = At<const int16_t*>(pSkeleton, kSkeletonParents);

    const uint8_t* pWorldHeader = header(0);
    const float* pWorldFromModel = nullptr;
    if (*reinterpret_cast<const int8_t*>(pWorldHeader + 0xD) == 1 && *reinterpret_cast<const float*>(pWorldHeader + 0x8) > 0.f)
        pWorldFromModel = reinterpret_cast<const float*>(pTracks + *reinterpret_cast<const int16_t*>(pWorldHeader + 0x4));
    else
        pWorldFromModel = At<const float*>(pCharacter, kCharacterWorldFromModel);
    if (!pWorldFromModel || !pParents)
        return false;
    const QsPose worldFromModel = ReadQs(pWorldFromModel);

    // The track's units, from where it has the copy against where the game does.
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    const float havokScale = s_havokScale.Get() ? *s_havokScale.Get() : 0.f;
    float scale = 0.f;
    if (glm::length(worldFromModel.Translation - drawn.ActorAt) < 64.f)
        scale = 1.f;
    else if (havokScale > 0.f && glm::length(worldFromModel.Translation / havokScale - drawn.ActorAt) < 64.f)
        scale = havokScale;
    static std::unordered_set<uint32_t> s_saidUnits;
    if (scale == 0.f)
    {
        if (s_saidUnits.insert(drawn.FormId).second)
            spdlog::warn("VRRagdoll: actor {:X}: its ragdoll's model-to-world ({:.1f}, {:.1f}, {:.1f}) is not where the copy is ({:.1f}, {:.1f}, {:.1f}) in either unit; "
                         "left to its animation",
                         drawn.FormId, worldFromModel.Translation.x, worldFromModel.Translation.y, worldFromModel.Translation.z, drawn.ActorAt.x, drawn.ActorAt.y, drawn.ActorAt.z);
        return false;
    }
    if (s_saidUnits.insert(drawn.FormId).second)
        spdlog::info("VRRagdoll: actor {:X}: its ragdoll is driven toward its drawn pose from now on ({} bones, the track in {} units)", drawn.FormId, numData,
                     scale == 1.f ? "game" : "Havok");

    aSaved.assign(reinterpret_cast<const uint8_t*>(pPose), reinterpret_cast<const uint8_t*>(pPose) + static_cast<size_t>(numData) * kQsTransformSize);
    const QsPose modelFromWorld = Inverse(worldFromModel);
    std::vector<QsPose> model(numData);
    for (int16_t i = 0; i < numData; ++i)
    {
        float* pLocal = pPose + i * (kQsTransformSize / sizeof(float));
        const int16_t parent = pParents[i];
        const QsPose parentModel = parent >= 0 && parent < i ? model[parent] : QsPose{};
        if (!drawn.Has[i])
        {
            model[i] = Compose(parentModel, ReadQs(pLocal));
            continue;
        }
        const NiTransform& world = drawn.World[i];
        QsPose drawnWorld{glm::normalize(glm::quat_cast(ToGlm(world.rotate))), ToGlm(world.translate) * scale, world.scale};
        QsPose target = Compose(modelFromWorld, drawnWorld);
        const QsPose local = Compose(Inverse(parentModel), target);
        if (!std::isfinite(local.Translation.x) || !std::isfinite(local.Translation.y) || !std::isfinite(local.Translation.z) || !std::isfinite(local.Rotation.w))
        {
            model[i] = Compose(parentModel, ReadQs(pLocal));
            continue;
        }
        pLocal[0] = local.Translation.x, pLocal[1] = local.Translation.y, pLocal[2] = local.Translation.z;
        pLocal[4] = local.Rotation.x, pLocal[5] = local.Rotation.y, pLocal[6] = local.Rotation.z, pLocal[7] = local.Rotation.w;
        // Scale stays the animation's.
        target.Scale = parentModel.Scale * pLocal[8];
        model[i] = target;
    }
    ++s_ragdollPosesWritten;
    return true;
}

// The call to hkbRagdollDriver::driveToPose, VR 0xB266AB (PLANCK's driveToPoseHookLoc; it hooks the same call, and
// whichever of the two patches second calls the other). Replaced only if it is a call.
using TDriveToPose = void(void* apDriver, float aDeltaTime, const void* apContext, void* apGeneratorOutput);
TDriveToPose* s_driveToPose = nullptr;

// The track is also the animation the game poses the copy's skeleton from, so it is the drawn pose only while the
// ragdoll is driven, and the game's own again straight after. Left in (2026-10-06), it fed the drawn pose into itself:
// the copy's drawn hands went from a median 65 to 131 units off its owner's ("hands within 80", live-weapon-grip).
void HookDriveToPose(void* apDriver, float aDeltaTime, const void* apContext, void* apGeneratorOutput)
{
    thread_local std::vector<uint8_t> t_saved;
    const bool cWrote = PutDrawnPoseIntoTrack(apDriver, apGeneratorOutput, t_saved);
    s_driveToPose(apDriver, aDeltaTime, apContext, apGeneratorOutput);
    if (cWrote)
    {
        int16_t count = 0;
        if (float* pPose = PoseTrackOf(apGeneratorOutput, count); pPose && static_cast<size_t>(count) * kQsTransformSize == t_saved.size())
            std::memcpy(pPose, t_saved.data(), t_saved.size());
    }
}

// After the step the game writes the ragdoll's pose back into the animation, and PLANCK blends it in: for a driven copy
// the animation is put back as it was, so the ragdoll never moves what is drawn. Outside PLANCK's hook on that call
// (moved there once the game runs), so its blend is undone too.

bool HookPostPhysicsOutside(void* apDriver, const void* apContext, void* apInOut)
{
    if (!IsDrivenCharacter(apDriver))
        return s_postPhysicsNext(apDriver, apContext, apInOut);
    thread_local std::vector<uint8_t> t_saved;
    int16_t count = 0;
    float* pPose = PoseTrackOf(apInOut, count);
    if (pPose)
        t_saved.assign(reinterpret_cast<const uint8_t*>(pPose), reinterpret_cast<const uint8_t*>(pPose) + static_cast<size_t>(count) * kQsTransformSize);
    const bool cResult = s_postPhysicsNext(apDriver, apContext, apInOut);
    int16_t countAfter = 0;
    if (pPose && PoseTrackOf(apInOut, countAfter) == pPose && countAfter == count)
        std::memcpy(pPose, t_saved.data(), t_saved.size());
    return cResult;
}

// Once the game runs (every plugin loaded): the call to hkbRagdollDriver::postPhysics, VR 0xB268DC (PLANCK's
// postPhysicsHookLoc), goes to the hook above first. Only if it is a call.
void HookPostPhysicsOnce() noexcept
{
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;
    VersionDbPtr<uint8_t> callSite(9000012);
    uint8_t* pCall = callSite.Get();
    if (!pCall || *pCall != 0xE8)
    {
        spdlog::error("VRRagdoll: the call to postPhysics is not where it was expected; the drawn pose is not fed to ragdolls");
        s_drawnPoseCount = 0;
        return;
    }
    s_postPhysicsNext = TiltedPhoques::GetCall<TPostPhysics*>(pCall);
    TiltedPhoques::PutCall(pCall, &HookPostPhysicsOutside);
    spdlog::info("VRRagdoll: after the step a driven copy's animation is put back as it was (call at {}, which called {})", fmt::ptr(pCall), fmt::ptr(s_postPhysicsNext));
}

#ifdef SKYRIMVR
// Installed before the plugins, so PLANCK's hook on the same call wraps ours: ours writes the drawn pose right before
// the drive, after PLANCK's own work. Installed around PLANCK's instead (once the game runs), PLANCK's work copies the
// game's animation pose over the track and the ragdoll hands went from 8-14 units off the drawn hands to 84-90 (rig,
// 2026-10-06 22:25 and 22:30, as on the first try the same morning). Left inside.
TiltedPhoques::Initializer s_driveToPoseHook(
    []()
    {
        VersionDbPtr<uint8_t> callSite(9000011);
        uint8_t* pCall = callSite.Get();
        if (!pCall || *pCall != 0xE8)
        {
            spdlog::error("VRRagdoll: the call to driveToPose is not where it was expected; ragdolls follow their animation");
            return;
        }
        TiltedPhoques::SwapCall(pCall, s_driveToPose, &HookDriveToPose);
        spdlog::info("VRRagdoll: remote players' ragdolls are driven toward their drawn pose (call at {}, which called {})", fmt::ptr(pCall), fmt::ptr(s_driveToPose));
    });
#endif

// What the weapon bodies touched since the last frame end: each body touched said once, and again ten seconds later.
void SayWeaponContacts() noexcept
{
    std::array<WeaponContact, 64> contacts;
    size_t count = 0;
    {
        std::lock_guard lock(s_contactsLock);
        count = s_contactCount;
        std::copy_n(s_contacts.begin(), count, contacts.begin());
        s_contactCount = 0;
    }
    if (count == 0)
        return;

    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    const float havokScale = s_havokScale.Get() ? *s_havokScale.Get() : 0.f;
    static std::unordered_map<const void*, std::chrono::steady_clock::time_point> s_lastSaid; // by the other body
    static uint64_t s_total = 0;
    const auto cNow = std::chrono::steady_clock::now();
    for (size_t i = 0; i < count; ++i)
    {
        const WeaponContact& contact = contacts[i];
        // Which of the two is the weapon: the listener is only on weapon bodies, but both could be.
        uint64_t key = 0;
        size_t ours = 2;
        for (const auto& [bodyKey, body] : s_weaponBodies)
            for (size_t side = 0; side < 2 && ours == 2; ++side)
                if (contact.pBodies[side] == body.pHavok)
                {
                    key = bodyKey;
                    ours = side;
                }
        if (ours == 2)
            continue;
        ++s_total;
        const size_t other = 1 - ours;
        // One of the local player's HIGGS bodies, or held by one: a clash when this weapon starts being touched by the
        // player's side -- nothing of the player's on it for a quarter second before -- and at most one every quarter
        // second per weapon. A hand resting on the blade touches it every step: counted every quarter second, that was
        // ~45 clashes in a run, each a sound and a pulse, at 1-15 units a second (rig, 2026-10-06). Counted per pair
        // of bodies, a hand lifting off for a moment while the held sword stayed on the blade was a new meeting each
        // time it came back: 6 clashes, four at 1-4 units a second, during 96 touches every 5 s (rig, 20:33).
        if (contact.HiggsKind[other] != 0 && havokScale > 0.f)
        {
            static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_lastTouch;
            static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_nextClash;
            auto& lastTouch = s_lastTouch[key];
            const bool cStarts = cNow - lastTouch >= std::chrono::milliseconds(250);
            lastTouch = cNow;
            auto& nextClash = s_nextClash[key];
            // And fast enough to be a blow: a hand resting between the copy's two blades brushed each about once a
            // second at 0-5 units a second, a new meeting every time (11 in a run, rig 21:28), while 98 of the 103
            // meetings of the first real fight were faster than 20 (6 at 10 or less). A slower one is a touch.
            constexpr float cMeetingSpeed = 10.f;
            const float speed = glm::length(contact.Velocities[0] - contact.Velocities[1]) / havokScale;
            if (cStarts && speed >= cMeetingSpeed && cNow >= nextClash)
            {
                nextClash = cNow + std::chrono::milliseconds(250);
                std::lock_guard clashesLock(s_clashesLock);
                if (s_clashes.size() < 16)
                    s_clashes.push_back({static_cast<uint32_t>(key >> 1), static_cast<uint8_t>(key & 1), contact.Point / havokScale, speed, contact.HiggsAt[other] / havokScale, true,
                                         contact.HiggsKind[other] == 2});
            }
            else
            {
                // Still touching: a blade resting on his blocks it all the same (the defender's rule), said every
                // tenth of a second, neither felt nor heard nor sent.
                static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_nextTouch;
                auto& nextTouch = s_nextTouch[key];
                if (cNow >= nextTouch)
                {
                    nextTouch = cNow + std::chrono::milliseconds(100);
                    std::lock_guard clashesLock(s_clashesLock);
                    if (s_clashes.size() < 16)
                        s_clashes.push_back({static_cast<uint32_t>(key >> 1), static_cast<uint8_t>(key & 1), contact.Point / havokScale, 0.f, contact.HiggsAt[other] / havokScale, false,
                                             contact.HiggsKind[other] == 2});
                }
            }
        }
        auto& lastSaid = s_lastSaid[contact.pBodies[other]];
        if (cNow - lastSaid < std::chrono::seconds(10))
            continue;
        lastSaid = cNow;
        const glm::vec3 point = havokScale > 0.f ? contact.Point / havokScale : glm::vec3{};
        spdlog::info("VRWeaponBody: actor {:X} {} weapon touches a body on layer {} (group {}, motion type {}{}) at ({:.1f}, {:.1f}, {:.1f}); {} contacts so far",
                     static_cast<uint32_t>(key >> 1), (key & 1) ? "right" : "left", contact.Filters[other] & 0x7F, contact.Filters[other] >> 16, contact.MotionTypes[other],
                     contact.HiggsKind[other] == 1 ? ", one of the player's HIGGS bodies" : contact.HiggsKind[other] == 2 ? ", held by the player's HIGGS hand" : "", point.x, point.y,
                     point.z, s_total);
    }
}

// Physics queue P2 (2026-10-06): where the copy's ragdoll hands are -- the bodies Havok collides with -- against its hands
// as drawn (the owner's VR pose) and as its own animation had them this frame. The ragdoll is driven toward the
// animation (PLANCK, at driveToPose), and the VR pose is only drawn at the frame end, so the bodies are expected at the
// animation. Every five seconds per copy.
void LogRagdollHands(const uint32_t aFormId, const Rig& acRig, const std::array<glm::vec3, 2>& acAnimatedHands) noexcept
{
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextLog;
    const auto cNow = std::chrono::steady_clock::now();
    auto& nextLog = s_nextLog[aFormId];
    if (cNow < nextLog)
        return;
    nextLog = cNow + std::chrono::seconds(5);
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    if (!s_havokScale.Get() || !(*s_havokScale.Get() > 0.f))
        return;

    std::string text;
    for (size_t side = 0; side < 2; ++side)
    {
        const RigBone& hand = acRig.Bones[side == 0 ? VRPose::kLeftHand : VRPose::kRightHand];
        uint8_t* pBody = hand.pNode ? HavokBodyOfLive(hand.pNode) : nullptr;
        if (!pBody)
        {
            text += fmt::format("{} no body; ", side == 0 ? "left" : "right");
            continue;
        }
        const glm::vec3 body = BodyPosition(pBody, *s_havokScale.Get());
        const glm::vec3 drawn = ToGlm(hand.World().translate);
        text += fmt::format("{} {:.1f} units from where it is drawn and {:.1f} from where the animation had it ({}, motion type {}); ", side == 0 ? "left" : "right",
                            glm::length(body - drawn), glm::length(body - acAnimatedHands[side]), At<void*>(pBody, kHkWorldOffset) ? "in the world" : "not in the world",
                            pBody[kMotionTypeOffset]);
    }
    spdlog::info("VRRagdoll: actor {:X} ragdoll hands: {}{} poses written into ragdoll tracks since the last line", aFormId, text, s_ragdollPosesWritten.exchange(0));
}

// Bodies of copies not posed at this frame end leave the world.
void ReleaseUnseenWeaponBodies() noexcept
{
    FrameEndTimer timer;
    SayWeaponContacts();
    for (auto it = s_weaponBodies.begin(); it != s_weaponBodies.end();)
    {
        if (!it->second.Seen)
        {
            ReleaseWeaponBody(it->first, it->second, "its copy was not posed");
            it = s_weaponBodies.erase(it);
            continue;
        }
        it->second.Seen = false;
        ++it;
    }
}

TiltedPhoques::Vector<uint32_t> s_touchCandidates;

// When the owner's bones last moved a dead body here (ApplyRemotePoses). See ObserveRemoteBodyMotion.
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_bodyPosedByOwnerAt;

// How close the nearer of the player's two hands is to any node of a body, in units; very large when it cannot be
// told. Every node, not only the root: a dragon is dragged by a wing or the tail, a long way from its root.
float NearestHandToBody(void* apBodyRoot) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    Holding mine;
    if (!apBodyRoot || !pPlayer || !ResolveHolding(pPlayer, mine) || (!mine.RightHand.Valid && !mine.LeftHand.Valid))
        return 1e9f;

    float nearest = 1e9f;
    TiltedPhoques::Vector<void*> queue;
    queue.push_back(apBodyRoot);
    for (size_t head = 0; head < queue.size() && head < 1024; ++head)
    {
        void* pNode = AsNode(queue[head]);
        if (!pNode)
            continue;
        const glm::vec3 at = ToGlm(At<NiTransform>(pNode, kWorldOffset).translate);
        const Segment point{at, at, true};
        for (const Segment* pHand : {&mine.RightHand, &mine.LeftHand})
            if (pHand->Valid)
                nearest = std::min(nearest, SegmentDistance(*pHand, point));

        void** pChildren = At<void**>(pNode, kChildrenOffset + 0x8);
        const uint16_t capacity = At<uint16_t>(pNode, kChildrenOffset + 0x10);
        if (!pChildren)
            continue;
        for (uint16_t i = 0; i < capacity; ++i)
            if (pChildren[i])
                queue.push_back(pChildren[i]);
    }
    return nearest;
}

// ---------------------------------------------------------------------------------------------------------------
// Blades as segments, for the blade stop (StopLocalBlades). A weapon's blade runs along its node's +Y (VR_HISTORY P1:
// the visible blade's bound and the body's centre of mass both lie there), from the grip to as far as its bound
// reaches: an iron sword's bound is 31.9 units around a point 21.9 along +Y, so 53.8.
struct Blade
{
    glm::vec3 Grip{};
    glm::vec3 Axis{}; // unit length
    float Length = 0.f;
};

// How far a weapon's blade reaches along its node's +Y, read where the node's world transform and its bound agree:
// before anything of ours has moved it this frame (the bound is the game's, from its own update).
struct BladeExtent
{
    void* pNode = nullptr; // the weapon's own node, hanging under WEAPON or SHIELD
    float Length = 0.f;
    float Radius = 0.f;
    float Along = 0.f;
};

// False for what is not held at one end and long along +Y: no bound, or one centred off that axis (a shield).
bool MeasureBlade(void* apNode, BladeExtent& aOut) noexcept
{
    const NiTransform& world = At<NiTransform>(apNode, kWorldOffset);
    const glm::vec3 grip = ToGlm(world.translate);
    const NiBoundRead bound = ReadWorldBound(apNode, grip);
    const glm::vec3 axis = ToGlm(world.rotate)[1];
    const float axisLength = glm::length(axis);
    if (!bound.Valid || bound.Radius < 8.f || bound.Radius > 150.f || !(axisLength > 0.5f))
        return false;
    const float along = glm::dot(bound.Centre - grip, axis / axisLength);
    if (!(along >= 0.4f * bound.Radius))
        return false;
    aOut = BladeExtent{apNode, along + bound.Radius, bound.Radius, along};
    return true;
}

Blade BladeAt(const BladeExtent& acExtent) noexcept
{
    const NiTransform& world = At<NiTransform>(acExtent.pNode, kWorldOffset);
    return Blade{ToGlm(world.translate), glm::normalize(ToGlm(world.rotate)[1]), acExtent.Length};
}

bool IsFinite(const glm::vec3& acValue) noexcept
{
    return std::isfinite(acValue.x) && std::isfinite(acValue.y) && std::isfinite(acValue.z);
}

// Which side of his blade ours is on: the sign of our blade's direction against the plane through our grip and his
// blade's line, whose unit normal is put in aNormal. It changes sign when our blade, or our grip, passes through his
// line. 0 when the plane is not defined (our grip on his line).
float SideOf(const Blade& acOurs, const Blade& acHis, glm::vec3& aNormal) noexcept
{
    const glm::vec3 normal = glm::cross(acHis.Grip - acOurs.Grip, acHis.Axis);
    const float length = glm::length(normal);
    if (!(length > 1.f))
        return 0.f;
    aNormal = normal / length;
    return glm::dot(acOurs.Axis, aNormal);
}

// Where two lines come closest: aT along the first (from acA along acU) and aV along the second (from acB along acW),
// both directions of unit length. False when they are within about 10 degrees of parallel.
bool LineParameters(const glm::vec3& acA, const glm::vec3& acU, const glm::vec3& acB, const glm::vec3& acW, float& aT, float& aV) noexcept
{
    const float b = glm::dot(acU, acW);
    const float denominator = 1.f - b * b;
    if (!(denominator >= 0.03f))
        return false;
    const glm::vec3 r = acA - acB;
    const float d = glm::dot(acU, r);
    const float e = glm::dot(acW, r);
    aT = (b * e - d) / denominator;
    aV = (e - b * d) / denominator;
    return std::isfinite(aT) && std::isfinite(aV);
}

float DistanceToBlade(const glm::vec3& acPoint, const Blade& acBlade) noexcept
{
    const float t = glm::clamp(glm::dot(acPoint - acBlade.Grip, acBlade.Axis), 0.f, acBlade.Length);
    return glm::distance(acPoint, acBlade.Grip + acBlade.Axis * t);
}

// The copies' drawn blades at this frame end, once they are posed (RecordCopyBlades). Frame end only.
struct CopyBlade
{
    uint64_t Key = 0; // form id << 1 | side (0 left, 1 right)
    Blade Segment{};
};
std::vector<CopyBlade> s_copyBlades;
} // namespace

namespace VRBodySync
{
// The blade stop, after HandParries below; OnFrameEnd calls these.
std::array<BladeExtent, 2> MeasureCopyBlades(const Rig& acRig) noexcept;
void RecordCopyBlades(uint32_t aFormId, Actor* apActor, const std::array<BladeExtent, 2>& acExtents) noexcept;
void StopLocalBlades(std::chrono::steady_clock::time_point aNow) noexcept;

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

    // Measurement: what this check reads of the other body, against where it was last drawn. Near zero means it
    // measures what is on screen; tens of units mean it measures the copy's own animation instead.
    static std::chrono::steady_clock::time_point s_nextGapLog{};
    if (const auto gapNow = std::chrono::steady_clock::now(); gapNow >= s_nextGapLog)
    {
        for (const uint32_t formId : candidates)
        {
            Actor* pOther = Cast<Actor>(TESForm::GetById(formId));
            Holding theirs;
            if (!pOther || !ResolveHolding(pOther, theirs))
                continue;
            DrawnGrip drawn;
            {
                std::lock_guard drawnLock(s_drawnLock);
                const auto it = s_drawn.find(formId);
                if (it == s_drawn.end())
                    continue;
                drawn = it->second;
            }
            const auto handGap = [&drawn](const Segment& acHand, const size_t aSide)
            { return acHand.Valid && drawn.HasHand[aSide] ? glm::distance(acHand.A, drawn.Hand[aSide]) : -1.f; };
            const auto attachGap = [&drawn](const HeldSide& acSide, const size_t aSide)
            {
                if (!drawn.HasAttach[aSide] || !StillTheSame(acSide.pAttach, acSide.pAttachVTable))
                    return -1.f;
                return glm::distance(ToGlm(At<NiTransform>(acSide.pAttach, kWorldOffset).translate), drawn.Attach[aSide]);
            };
            s_nextGapLog = gapNow + std::chrono::seconds(5);
            spdlog::info("VRWeaponTouch: {:X} as this check reads it, against where it was drawn {} ms before: right hand {:.1f} units away, left hand {:.1f}, "
                         "right weapon {:.1f}, left weapon {:.1f} (-1: nothing to compare)",
                         formId, std::chrono::duration_cast<std::chrono::milliseconds>(gapNow - drawn.At).count(), handGap(theirs.RightHand, 1), handGap(theirs.LeftHand, 0),
                         attachGap(theirs.Right, 1), attachGap(theirs.Left, 0));
            break;
        }
    }

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

    s_copyBlades.clear();
    if (poses.empty())
    {
        StopLocalBlades(std::chrono::steady_clock::now());
        ReleaseUnseenWeaponBodies();
        return;
    }

    PerfCounterScope perfScope(PerfCounter::kVRPoseApply);
    const auto now = std::chrono::steady_clock::now();

    // Per copy, every 30 s: how many frames it was posed and why the others were not. A copy that never shows its
    // owner's pose said nothing about it before (2026-10-04, the hands test: posed or not could not be told).
    static std::unordered_map<uint32_t, std::map<std::string, uint32_t>> s_tally;
    static std::chrono::steady_clock::time_point s_nextTally = now + std::chrono::seconds(30);
    if (now >= s_nextTally)
    {
        s_nextTally = now + std::chrono::seconds(30);
        for (const auto& [id, counts] : s_tally)
        {
            std::string text;
            for (const auto& [why, count] : counts)
                text += fmt::format("{}{} {}", text.empty() ? "" : ", ", why, count);
            spdlog::info("VRBodySync: actor {:X} over the last 30 s: {}", id, text);
        }
        s_tally.clear();
    }

    for (const auto& [formId, pose] : poses)
    {
        auto& tally = s_tally[formId];
        Actor* pActor = Cast<Actor>(TESForm::GetById(formId));
        void* pRoot = pActor ? pActor->GetNiNode() : nullptr;
        if (!pRoot)
        {
            ++tally["no 3D"];
            continue;
        }

        // A dead or downed player copy belongs to its ragdoll: posing it fights the death animation and the body
        // ends up standing in the air. A dead NPC is the opposite case. Its owner only sends bones for it while it
        // is being moved over there (dragged, thrown, shoved), and that movement is the whole point of this, so
        // those bones are applied.
        // A dead body being moved by its owner, set below. Every way the pose for one can be skipped after the "posing
        // body" line is named for it: that line used to be the only one, and it is logged *before* four checks that each
        // skip the pose without a word. On 2026-09-30 Seen's side logged "posing body FF0010E1" for Emma's dragged troll
        // and he saw it not move at all -- which of the four it was could not be told.
        bool isBody = false;
        const auto whyNotPosed = [&isBody, &now, &tally, formId](const char* acpWhy, const float aValue = 0.f)
        {
            ++tally[acpWhy];
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
            {
                ++tally["a downed or dead player"];
                continue;
            }

            // Only a properly dead body. A downed one (bleeding out) gets back up, and a dying one is playing its
            // death animation; writing bones into either fights the game. Seen reported exactly that on
            // 2026-09-23: "npcs that get downed but cant be killed dont stand back up, they just lay on the floor
            // weirdly". The sender no longer sends for those, and this refuses them even if an old client does.
            if (!pActor->actorState.IsDead())
            {
                ++tally["dying or downed"];
                continue;
            }
            isBody = true;
            s_bodyPosedByOwnerAt[formId] = now;

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
            {
                ++tally["waiting to retry its skeleton"];
                continue;
            }
            if (!ResolveRig(pRoot, rig, cGone))
            {
                rig.RetryAt = now + std::chrono::seconds(1);
                whyNotPosed("its skeleton could not be matched to the bones being sent");
                continue;
            }
            rig.RestructureAt = now + std::chrono::milliseconds(200);
        }

        // A body being moved over there is looked for where its owner has it. Its own bones are where its ragdoll left
        // it here -- where it lay before the drag -- so checked there, a dragged body was skipped whenever that old spot
        // was out of view, and showed at it: Seen, 2026-10-06 20:03, "the npc disappeared for a brief moment" while Emma
        // dragged 1018F9 ("out of view 46, posed 5" in 30 s, "not posed this time: out of view" every few seconds).
        glm::vec3 viewPoint = ToGlm(rig.Bones[VRPose::kSpine2].World().translate);
        if (isBody && pose.HasRootPosition && std::isfinite(pose.RootPosition.x) && std::isfinite(pose.RootPosition.y) && std::isfinite(pose.RootPosition.z))
            viewPoint += pose.RootPosition - ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
        if (!IsInView(viewPoint))
        {
            // Its weapon stays a body behind your back too, where the game's own animation has it: out of the world
            // and back in every time he leaves your view would be churn for nothing.
            UpdateWeaponBodies(formId, pActor, rig);
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
                glm::vec3 delta = wanted - ToGlm(pelvis.World().translate);
                // Without leg trackers, where the body stands but not how high: the legs are this copy's walk
                // animation, and lowering it to the owner's crouch would sink the feet. Measured in play on both
                // screens (2026-10-04, 327 samples): the copy's body stood a median 22-28 units in front of where the
                // owner's VRIK body was, and every hand 20-25 units too far forward with it. The real body stands
                // behind its root in the owner's game too, so this is also where it is to be hit.
                if (!pose.HasLegs)
                {
                    const glm::mat3 rootRotation = ToGlm(rootWorld.rotate);
                    glm::vec3 local = glm::transpose(rootRotation) * delta;
                    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextBodyGap;
                    auto& nextGap = s_nextBodyGap[formId];
                    if (now >= nextGap)
                    {
                        nextGap = now + std::chrono::seconds(10);
                        spdlog::info("VRBodySync: actor {:X} body stands ({:.1f}, {:.1f}) from where its owner's does (right, forward); moved there", formId, local.x, local.y);
                    }
                    local.z = 0.f;
                    delta = rootRotation * local;
                }
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

        const AnimatedWeapons animatedWeapons = WeaponsAsAnimated(rig);
        const auto animatedAt = [&rig](const uint32_t aBone)
        {
            const RigBone& bone = rig.Bones[aBone];
            return bone.pNode || bone.pEntry ? ToGlm(bone.World().translate) : glm::vec3{std::numeric_limits<float>::quiet_NaN()};
        };
        const std::array<glm::vec3, 2> animatedHands{animatedAt(VRPose::kLeftHand), animatedAt(VRPose::kRightHand)};
        const std::array<BladeExtent, 2> bladeExtents = MeasureCopyBlades(rig);
        PoseActor(rig, pose, worldOffset);
        ReachHands(rig, pose);
        PlaceWeapons(rig, pose, formId, now);
        {
            DrawnGrip drawn;
            drawn.At = now;
            for (size_t side = 0; side < 2; ++side)
            {
                const RigBone& hand = rig.Bones[side == 0 ? VRPose::kLeftHand : VRPose::kRightHand];
                if (hand.pNode || hand.pEntry)
                {
                    drawn.HasHand[side] = true;
                    drawn.Hand[side] = ToGlm(hand.World().translate);
                }
                if (rig.Attach[side].pNode)
                {
                    drawn.HasAttach[side] = true;
                    drawn.Attach[side] = ToGlm(rig.Attach[side].World().translate);
                }
            }
            std::lock_guard drawnLock(s_drawnLock);
            s_drawn[formId] = drawn;
        }
        LogPhysicsOnce(formId, rig);
        UpdateWeaponBodies(formId, pActor, rig, &animatedWeapons);
        RecordCopyBlades(formId, pActor, bladeExtents);
        LogRagdollHands(formId, rig, animatedHands);
        if (!isBody && pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer())
            RecordDrawnPose(formId, pActor, rig);
        // Where the drawn weapon is, for the rig's physics tests (live-check "DO drop ... onto copy weapon"), with its
        // axes: which of them runs along the blade is not known yet.
        if (rig.Attach[1].pNode && !rig.AttachBelow[1].empty())
        {
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextWeaponLog;
            auto& nextWeaponLog = s_nextWeaponLog[formId];
            if (now >= nextWeaponLog)
            {
                nextWeaponLog = now + std::chrono::seconds(5);
                const NiTransform& weapon = rig.Attach[1].World();
                const glm::mat3 axes = ToGlm(weapon.rotate);
                spdlog::info("PhysicsProbe: actor {:X} right weapon drawn at ({:.1f}, {:.1f}, {:.1f}); its x axis ({:.2f}, {:.2f}, {:.2f}), y ({:.2f}, {:.2f}, {:.2f}), z ({:.2f}, {:.2f}, {:.2f})",
                             formId, weapon.translate.x, weapon.translate.y, weapon.translate.z, axes[0].x, axes[0].y, axes[0].z, axes[1].x, axes[1].y, axes[1].z, axes[2].x,
                             axes[2].y, axes[2].z);
            }
        }
        ++tally["posed"];

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

    StopLocalBlades(now);
    ReleaseUnseenWeaponBodies();
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

// The local player's first-person hand, left (0) or right (1), or null when this build's PlayerCharacter does not
// carry it where we look. Checked by name the first time, like the headset node; after that, one plain read a frame
// says it is still the same node (a node's name is one pooled string, so the same node keeps the same pointer).
void* FirstPersonHand(PlayerCharacter* apPlayer, const size_t aSide) noexcept
{
    static std::array<int, 2> s_state{}; // 0 unchecked, 1 the hand, -1 not the hand on this build
    static std::array<const char*, 2> s_name{};
    if (!apPlayer || s_state[aSide] < 0)
        return nullptr;

    void* pHand = At<void*>(apPlayer, kFirstPersonHandOffsets[aSide]);
    if (!pHand)
        return nullptr;

    if (s_state[aSide] == 0)
    {
        const char* pName = IsReadable(pHand, kNiAVObjectSize) ? GetName(pHand) : nullptr;
        const bool cReadable = pName && IsReadable(pName, 8);
        s_state[aSide] = cReadable && std::strcmp(pName, kHandNames[aSide]) == 0 ? 1 : -1;
        s_name[aSide] = pName;
        if (s_state[aSide] < 0)
        {
            spdlog::warn("VRBodySync: PlayerCharacter+0x{:X} is not the first-person {} hand (it is {}); where this player holds weapons is not sent", kFirstPersonHandOffsets[aSide],
                         aSide ? "right" : "left", cReadable ? pName : "unreadable");
            return nullptr;
        }
        spdlog::info("VRBodySync: first-person {} hand found at PlayerCharacter+0x{:X}", aSide ? "right" : "left", kFirstPersonHandOffsets[aSide]);
    }

    return GetName(pHand) == s_name[aSide] ? pHand : nullptr;
}

// Whether this hand holds what parries: a weapon in the hand, or for the left hand a worn shield (the game keeps a
// shield as worn armour, not as the hand's object). Not a spell, a torch or nothing.
bool HandParries(const uint8_t aSide) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || aSide > 1)
        return false;
    if (const TESForm* pHeld = pPlayer->GetEquippedWeapon(aSide); pHeld && pHeld->formType == FormType::Weapon)
        return true;
    if (aSide != 0)
        return false;
    // Read at most once a second: it walks the whole inventory, and a blade resting on his is weighed ten times a
    // second.
    static bool s_shield = false;
    static std::chrono::steady_clock::time_point s_readAt{};
    if (const auto cNow = std::chrono::steady_clock::now(); cNow - s_readAt >= std::chrono::seconds(1))
    {
        s_readAt = cNow;
        const Inventory shields = pPlayer->GetInventory(
            [](TESForm& aForm)
            {
                return aForm.formType == FormType::Armor &&
                       (static_cast<TESObjectARMO&>(aForm).slotType & static_cast<uint32_t>(BGSBipedObjectForm::Part::Shield)) != 0;
            });
        s_shield = std::any_of(shields.Entries.begin(), shields.Entries.end(), [](const Inventory::Entry& acEntry) { return acEntry.IsWorn(); });
    }
    return s_shield;
}

// ---------------------------------------------------------------------------------------------------------------
// The blade stop (VR_TODO "How your equipped sword meets his: (1)"). Emma, 2026-10-08, after a fight with Seen: "if I
// meet my enemy sword I shouldn't be able to still go past it, or it defeats the purpose"; 2026-10-10: start it.
//
// Both swords are keyframed to hands, the player's to the controller and the copy's to its owner's pose, so Havok sees
// them touch (the clash) and neither can push the other: the blade the player saw went straight through his. Here, at
// the frame end, once every copy is posed, each drawn blade is a segment (Blade), and the moment the player's crosses
// one of his it is held on the side it came from: what is drawn of the player's weapon is turned about the grip until
// it rests on his blade, for as long as the hand is past it. It lets go when the hand comes back, when the touch
// slides off the end of either blade or reaches the player's own hand, or when it would have to turn more than 60
// degrees. Only world transforms are written, which the game draws anew from the hand every frame, plus the grip sent
// to the other player (DrawnGripRotation), so he sees it rest on his blade too; the controller and the hand are not
// touched. A hit that blade lands on him meanwhile is dropped (IsBladeStoppedAt): the blade he sees never reached him.
constexpr float kBladeMaxTurnDegrees = 60.f;
constexpr float kBladeGuard = 8.f; // units from the grip: nearer than that, his blade is at the player's hand
constexpr float kBladeRest = 1.5f; // units ours is held off his blade's line, about half of two blades' thickness
constexpr float kBladeSlack = 2.f; // units past the end of either blade a touch still counts

std::array<BladeExtent, 2> MeasureCopyBlades(const Rig& acRig) noexcept
{
    std::array<BladeExtent, 2> extents{};
    for (size_t side = 0; side < 2; ++side)
        if (void* pAttach = acRig.Attach[side].pNode)
            for (const RigBone& below : acRig.AttachBelow[side])
                if (below.pNode && At<void*>(below.pNode, kParentOffset) == pAttach && MeasureBlade(below.pNode, extents[side]))
                    break;
    return extents;
}

void RecordCopyBlades(const uint32_t aFormId, Actor* apActor, const std::array<BladeExtent, 2>& acExtents) noexcept
{
    // Drawn, asked to sheathe or sheathing, as for its weapon bodies (UpdateWeaponBodies).
    const uint32_t weaponState = apActor->actorState.flags2 >> 5 & 7;
    if (weaponState < 3 || weaponState > 5)
        return;
    static std::unordered_map<uint64_t, int> s_saidLength;
    for (uint32_t side = 0; side < 2; ++side)
    {
        if (!acExtents[side].pNode)
            continue;
        const CopyBlade blade{(static_cast<uint64_t>(aFormId) << 1) | side, BladeAt(acExtents[side])};
        if (!IsFinite(blade.Segment.Grip) || !IsFinite(blade.Segment.Axis))
            continue;
        s_copyBlades.push_back(blade);
        if (int& said = s_saidLength[blade.Key]; said != static_cast<int>(acExtents[side].Length))
        {
            said = static_cast<int>(acExtents[side].Length);
            spdlog::info("BladeStop: {:X}'s {} weapon reaches {:.1f} units from its grip (a bound of {:.1f} around a point {:.1f} along it)", aFormId, side ? "right" : "left",
                         acExtents[side].Length, acExtents[side].Radius, acExtents[side].Along);
        }
    }
}

struct LocalStop
{
    // The first-person attach node (WEAPON, SHIELD), searched twice a second as CaptureWeapons does.
    void* pAttach = nullptr;
    void* pAttachVTable = nullptr;
    std::chrono::steady_clock::time_point SearchAt{};
    BladeExtent Extent{}; // measured on frames it was not turned
    int SaidLength = -1;
    // Which side of each copy blade ours was on at the last frame end, while free, to see it cross one.
    std::unordered_map<uint64_t, float> LastSide;
    // The blade it rests on (0: free), the side of it ours is held on (SideOf's sign), and what to say on letting go.
    uint64_t Key = 0;
    float HeldSide = 0.f;
    std::chrono::steady_clock::time_point Since{};
    float WorstDegrees = 0.f;
    uint32_t Frames = 0;
    uint32_t NotDrawnAnew = 0;
    // This frame end's turn, and the weapon node's rotation as written, to tell whether the game drew it anew since.
    bool Turned = false;
    NiMatrix3 WrittenRoot{};
};
std::array<LocalStop, 2> s_localStops{};

// What other threads read: the grip as sent (CaptureWeapons, from the game's update) and the hit check (the game's
// damage code).
struct SharedStop
{
    bool Turned = false;
    glm::mat3 Turn{1.f};
    NiMatrix3 WrittenAttach{};
    Blade Real{}; // the blade where the hand holds it, not where it is drawn
    uint32_t On = 0;
    std::chrono::steady_clock::time_point DropHitsUntil{};
};
std::mutex s_sharedStopsLock;
std::array<SharedStop, 2> s_sharedStops{};

// The weapon drawn in that hand, as the player sees it: the node hanging under the first-person attach node, read
// from its children every frame (a weapon swapped between two searches is then never a stale pointer). Null when
// nothing hangs there.
void* LocalWeaponNode(PlayerCharacter* apPlayer, LocalStop& aStop, const size_t aSide, const std::chrono::steady_clock::time_point aNow) noexcept
{
    const bool cLost = aStop.pAttach && !StillTheSame(aStop.pAttach, aStop.pAttachVTable);
    if (aNow >= aStop.SearchAt || cLost)
    {
        aStop.SearchAt = aNow + std::chrono::milliseconds(500);
        aStop.pAttach = nullptr;
        aStop.pAttachVTable = nullptr;
        if (void* pHand = FirstPersonHand(apPlayer, aSide))
        {
            void* pForearm = At<void*>(pHand, kParentOffset);
            void* pAttach = FindShallowest(pForearm ? pForearm : pHand, kAttachNames[aSide]);
            if (pAttach && AsNode(pAttach))
            {
                aStop.pAttach = pAttach;
                aStop.pAttachVTable = *static_cast<void**>(pAttach);
            }
        }
    }
    if (!aStop.pAttach)
        return nullptr;
    void** pChildren = At<void**>(aStop.pAttach, kChildrenOffset + 0x8);
    const uint16_t slots = At<uint16_t>(aStop.pAttach, kChildrenOffset + 0x10);
    for (uint16_t i = 0; pChildren && i < slots && i < 8; ++i)
        if (pChildren[i])
            return pChildren[i];
    return nullptr;
}

const CopyBlade* FindCopyBlade(const uint64_t aKey) noexcept
{
    for (const CopyBlade& blade : s_copyBlades)
        if (blade.Key == aKey)
            return &blade;
    return nullptr;
}

void LetGo(const size_t aSide, LocalStop& aStop, const char* acpWhy, const std::chrono::steady_clock::time_point aNow) noexcept
{
    if (aStop.Key)
        spdlog::info("BladeStop: our {} blade let go of {:X}'s {} blade after {} ms: {}; it was held up to {:.0f} degrees from where the hand had it, over {} frame ends ({} "
                     "of them not drawn anew by the game)",
                     aSide ? "right" : "left", static_cast<uint32_t>(aStop.Key >> 1), aStop.Key & 1 ? "right" : "left",
                     std::chrono::duration_cast<std::chrono::milliseconds>(aNow - aStop.Since).count(), acpWhy, aStop.WorstDegrees, aStop.Frames, aStop.NotDrawnAnew);
    aStop.Key = 0;
    aStop.Turned = false;
    aStop.LastSide.clear();
}

// Turns the attach node and everything below it about the grip. Only while a blade rests on another, and walked live
// from the attach node each time: no list of nodes is kept from one frame to the next.
void TurnDrawnWeapon(void* apAttach, const glm::vec3& acPivot, const glm::mat3& acTurn) noexcept
{
    static std::vector<void*> s_nodes;
    s_nodes.clear();
    s_nodes.push_back(apAttach);
    CollectNodeDescendants(apAttach, s_nodes);
    for (void* pNode : s_nodes)
    {
        NiTransform& world = At<NiTransform>(pNode, kWorldOffset);
        world.rotate = FromGlm(acTurn * ToGlm(world.rotate));
        world.translate = FromGlm(acPivot + acTurn * (ToGlm(world.translate) - acPivot));
    }
}

void StopLocalBlades(const std::chrono::steady_clock::time_point aNow) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const uint32_t weaponState = pPlayer ? pPlayer->actorState.flags2 >> 5 & 7 : 0;
    const bool cDrawn = pPlayer && weaponState >= 3 && weaponState <= 5 && !pPlayer->actorState.IsDeadOrDying();

    std::array<SharedStop, 2> shared{};
    for (size_t side = 0; side < 2; ++side)
    {
        LocalStop& stop = s_localStops[side];
        void* pNode = cDrawn ? LocalWeaponNode(pPlayer, stop, side, aNow) : nullptr;
        const TESForm* pHeld = pNode ? pPlayer->GetEquippedWeapon(static_cast<uint32_t>(side)) : nullptr;
        if (!pNode || !pHeld || pHeld->formType != FormType::Weapon)
        {
            LetGo(side, stop, "nothing is drawn in that hand", aNow);
            continue;
        }

        // Not drawn anew since our last turn (it still has the rotation we wrote): what is drawn is that turn still,
        // and nothing is weighed until the game draws it from the hand again.
        const NiTransform& world = At<NiTransform>(pNode, kWorldOffset);
        if (stop.Turned && std::memcmp(&world.rotate, &stop.WrittenRoot, sizeof(NiMatrix3)) == 0)
        {
            ++stop.NotDrawnAnew;
            std::lock_guard lock(s_sharedStopsLock);
            shared[side] = s_sharedStops[side];
            continue;
        }
        stop.Turned = false;

        // Measured where nothing of ours has turned it: the game's transform and its bound agree.
        if (!stop.Key || stop.Extent.pNode != pNode)
        {
            if (!MeasureBlade(pNode, stop.Extent))
            {
                LetGo(side, stop, "the weapon has no blade to measure", aNow);
                stop.Extent = BladeExtent{};
                continue;
            }
            if (stop.SaidLength != static_cast<int>(stop.Extent.Length))
            {
                stop.SaidLength = static_cast<int>(stop.Extent.Length);
                spdlog::info("BladeStop: our {} weapon {:X} reaches {:.1f} units from the grip (a bound of {:.1f} around a point {:.1f} along it)", side ? "right" : "left",
                             pHeld->formID, stop.Extent.Length, stop.Extent.Radius, stop.Extent.Along);
            }
        }
        const Blade ours = BladeAt(stop.Extent);
        if (!IsFinite(ours.Grip) || !IsFinite(ours.Axis))
        {
            LetGo(side, stop, "the weapon's transform is not usable", aNow);
            continue;
        }

        // Free: did it pass through one of his blades since the last frame end, within the length of both?
        if (!stop.Key)
        {
            std::unordered_map<uint64_t, float> sides;
            for (const CopyBlade& his : s_copyBlades)
            {
                if (glm::distance(his.Segment.Grip, ours.Grip) > ours.Length + his.Segment.Length + 20.f)
                    continue;
                glm::vec3 normal{};
                const float sideNow = SideOf(ours, his.Segment, normal);
                const float sign = sideNow > 0.f ? 1.f : sideNow < 0.f ? -1.f : 0.f;
                sides[his.Key] = sign;
                const auto before = stop.LastSide.find(his.Key);
                if (stop.Key || sign == 0.f || before == stop.LastSide.end() || before->second == 0.f || before->second == sign)
                    continue;
                float t = 0.f, v = 0.f;
                const bool cLines = LineParameters(ours.Grip, glm::normalize(ours.Axis - sideNow * normal), his.Segment.Grip, his.Segment.Axis, t, v);
                const char* pNotHeld = !cLines                                                   ? "the blades lie along each other"
                                       : t < kBladeGuard                                         ? "his blade passed at the hand"
                                       : t > ours.Length + kBladeSlack                           ? "his blade passed beyond our tip"
                                       : v < -kBladeSlack || v > his.Segment.Length + kBladeSlack ? "ours passed beyond his blade's ends"
                                                                                                 : nullptr;
                if (pNotHeld)
                {
                    // Crossings that are not held, said at most four times a second: what a test or a fight shows of the
                    // geometry (grips, axes, where along each blade).
                    static std::chrono::steady_clock::time_point s_nextNotHeld{};
                    if (aNow >= s_nextNotHeld)
                    {
                        s_nextNotHeld = aNow + std::chrono::milliseconds(250);
                        spdlog::info("BladeStop: our {} blade crossed the line of {:X}'s {} blade, not held: {} ({:.0f} along ours of {:.0f}, {:.0f} along his of {:.0f}; ours "
                                     "from ({:.0f}, {:.0f}, {:.0f}) along ({:.2f}, {:.2f}, {:.2f}), his from ({:.0f}, {:.0f}, {:.0f}) along ({:.2f}, {:.2f}, {:.2f}))",
                                     side ? "right" : "left", static_cast<uint32_t>(his.Key >> 1), his.Key & 1 ? "right" : "left", pNotHeld, t, ours.Length, v,
                                     his.Segment.Length, ours.Grip.x, ours.Grip.y, ours.Grip.z, ours.Axis.x, ours.Axis.y, ours.Axis.z, his.Segment.Grip.x,
                                     his.Segment.Grip.y, his.Segment.Grip.z, his.Segment.Axis.x, his.Segment.Axis.y, his.Segment.Axis.z);
                    }
                    continue;
                }
                stop.Key = his.Key;
                stop.HeldSide = before->second;
                stop.Since = aNow;
                stop.WorstDegrees = 0.f;
                stop.Frames = 0;
                stop.NotDrawnAnew = 0;
                const glm::vec3 at = his.Segment.Grip + his.Segment.Axis * v;
                spdlog::info("BladeStop: our {} blade met {:X}'s {} blade at ({:.0f}, {:.0f}, {:.0f}), {:.0f} units from our grip and {:.0f} from his; held on this side of it",
                             side ? "right" : "left", static_cast<uint32_t>(his.Key >> 1), his.Key & 1 ? "right" : "left", at.x, at.y, at.z, t, v);
            }
            stop.LastSide.swap(sides);
        }

        // Held: turned about the grip into the plane of his blade, resting on it on the side it came from.
        if (stop.Key)
        {
            const char* pWhy = nullptr;
            glm::mat3 turn{1.f};
            float degrees = 0.f;
            const CopyBlade* pHis = FindCopyBlade(stop.Key);
            glm::vec3 normal{};
            const float sideNow = pHis ? SideOf(ours, pHis->Segment, normal) : 0.f;
            float t = 0.f, v = 0.f;
            if (!pHis)
                pWhy = "his blade is no longer drawn here";
            else if (sideNow == 0.f)
                pWhy = "the hand is on his blade's line";
            else if (sideNow * stop.HeldSide > 0.f)
                pWhy = "the hand came back";
            else if (!LineParameters(ours.Grip, glm::normalize(ours.Axis - sideNow * normal), pHis->Segment.Grip, pHis->Segment.Axis, t, v))
                pWhy = "the blades lie along each other";
            else if (t < kBladeGuard)
                pWhy = "his blade reached the hand";
            else if (t > ours.Length + kBladeSlack)
                pWhy = "it slid off the end of ours";
            else if (v < -kBladeSlack || v > pHis->Segment.Length + kBladeSlack)
                pWhy = "it slid off the end of his";
            else
            {
                const glm::vec3 held = glm::normalize(glm::normalize(ours.Axis - sideNow * normal) + stop.HeldSide * (kBladeRest / t) * normal);
                turn = RotationBetween(ours.Axis, held);
                degrees = DegreesOf(turn);
                if (!(degrees <= kBladeMaxTurnDegrees))
                    pWhy = "the hand pushed it past (more than 60 degrees)";
            }
            if (pWhy)
            {
                LetGo(side, stop, pWhy, aNow);
                continue;
            }

            TurnDrawnWeapon(stop.pAttach, ours.Grip, turn);
            stop.Turned = true;
            stop.WrittenRoot = world.rotate;
            stop.WorstDegrees = std::max(stop.WorstDegrees, degrees);
            ++stop.Frames;
            shared[side].Turned = true;
            shared[side].Turn = turn;
            shared[side].WrittenAttach = At<NiTransform>(stop.pAttach, kWorldOffset).rotate;
            shared[side].Real = ours;
            shared[side].On = static_cast<uint32_t>(stop.Key >> 1);
            shared[side].DropHitsUntil = aNow + std::chrono::milliseconds(300);
        }
    }

    std::lock_guard lock(s_sharedStopsLock);
    for (size_t side = 0; side < 2; ++side)
    {
        // A hit along a blade that has just been let go still counts as stopped for its last 300 ms.
        const auto cKeepHits = s_sharedStops[side].DropHitsUntil;
        const uint32_t cKeepOn = s_sharedStops[side].On;
        const Blade cKeepReal = s_sharedStops[side].Real;
        s_sharedStops[side] = shared[side];
        if (!shared[side].Turned && aNow < cKeepHits)
        {
            s_sharedStops[side].DropHitsUntil = cKeepHits;
            s_sharedStops[side].On = cKeepOn;
            s_sharedStops[side].Real = cKeepReal;
        }
    }
}

// The grip as drawn, from the first-person attach node's rotation as the game's update reads it: turned the way the
// blade stop turned it at the last frame end, unless it still has that turn (not drawn anew since).
glm::mat3 DrawnGripRotation(const size_t aSide, const NiMatrix3& acRead) noexcept
{
    std::lock_guard lock(s_sharedStopsLock);
    const SharedStop& stop = s_sharedStops[aSide];
    if (!stop.Turned || std::memcmp(&acRead, &stop.WrittenAttach, sizeof(NiMatrix3)) == 0)
        return ToGlm(acRead);
    return stop.Turn * ToGlm(acRead);
}

bool IsBladeStoppedAt(const uint32_t aCopyFormId, const glm::vec3& acPoint) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    // No point to go by (none written, or not a number): any blade resting on his counts.
    const bool cNoPoint = !IsFinite(acPoint) || glm::dot(acPoint, acPoint) < 1.f;
    std::lock_guard lock(s_sharedStopsLock);
    for (const SharedStop& stop : s_sharedStops)
        if (stop.On == aCopyFormId && cNow < stop.DropHitsUntil && (cNoPoint || DistanceToBlade(acPoint, stop.Real) <= 20.f))
            return true;
    return false;
}

bool TakeClash(Clash& aOut) noexcept
{
    PendingClash pending{};
    {
        std::lock_guard lock(s_clashesLock);
        if (s_clashes.empty())
            return false;
        pending = s_clashes.front();
        s_clashes.erase(s_clashes.begin());
    }
    aOut.FormId = pending.FormId;
    aOut.Side = pending.Side;
    aOut.Point = pending.Point;
    aOut.Speed = pending.Speed;
    aOut.Start = pending.Start;
    aOut.Heard = false;

    // Which of the local hands it was: the one nearer the HIGGS body (the hand, or the hand holding what met it). Every
    // HIGGS body is one of the two hands', so there is no limit: a hand moved 84 units in one frame left its body more
    // than 40 units from both hand nodes (rig, 2026-10-06), and the nearer one was still the right one.
    aOut.OwnSide = 2;
    float nearest = std::numeric_limits<float>::max();
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
        for (uint8_t side = 0; side < 2; ++side)
            if (void* pHand = FirstPersonHand(pPlayer, side))
            {
                const float distance = glm::length(ToGlm(At<NiTransform>(pHand, kWorldOffset).translate) - pending.Hand);
                if (distance < nearest)
                {
                    nearest = distance;
                    aOut.OwnSide = side;
                    aOut.HandDistance = distance;
                }
            }
    // Only a weapon or a shield parries (Emma, 2026-10-09): 5 of her 7 blocks that evening were his sword on a hand
    // that held no blade, and a hand is a body part, so his blade on it is a hit. Something held up with HIGGS's grab
    // counts as held.
    aOut.Parries = pending.Held || HandParries(aOut.OwnSide);
    if (!pending.Start || !aOut.Parries)
        return true;
    FeelClash(aOut.OwnSide);
    aOut.Heard = SoundClash(aOut.Point);
    SparkClash(aOut.Point);
    return true;
}

void FeelClash(const uint8_t aSide) noexcept
{
    if (aSide < 2)
        VRHaptics::Pulse(aSide == 1, 3999);
}

bool PlaceCorpse(Actor* apActor, const glm::vec3& acWanted) noexcept
{
    // A corpse is drawn from its ragdoll, and moving the reference (ForcePosition, MoveTo, SetPosition, the console)
    // leaves the ragdoll where this game dropped it: the two screens showed a body in two places, and a body dragged
    // over there fell back to its old spot here when the dragging stopped (2026-10-04, `live-corpse-place`; Seen,
    // 2026-10-06 20:03). The ragdoll is its rigid bodies, so they are moved, every one by the same offset, as PLANCK
    // warps a ragdoll (`rb->getRigidMotion()->setTransform`, `updateMovedBodyInfo`, velocities zeroed, its main.cpp).
    struct Placed
    {
        std::chrono::steady_clock::time_point NextAt{};
        std::chrono::steady_clock::time_point MovedAt{};
        bool SayAfter = false;
    };
    static std::unordered_map<uint32_t, Placed> s_placed;
    if (!apActor)
        return false;
    const auto cNow = std::chrono::steady_clock::now();
    auto& placed = s_placed[apActor->formID];
    if (cNow < placed.NextAt)
        return false;
    placed.NextAt = cNow + std::chrono::milliseconds(500);

    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    POINTER_SKYRIMSE(THkpEntitySetPositionAndRotation, s_setPositionAndRotation, 9000002, 9000002);
    POINTER_SKYRIMSE(THkpEntityActivate, s_activate, 60096, 60096);
    const float cScale = s_havokScale.Get() ? *s_havokScale.Get() : 0.f;
    void* pRoot = apActor->GetNiNode();
    if (!(cScale > 0.f) || !pRoot || !s_setPositionAndRotation.Get())
        return false;

    // The ragdoll's bodies: a live tree, so no IsReadable (milliseconds a call here), only null checks. Dynamic and in
    // a world only; the first found from the root is the ragdoll's root (pelvis or centre of mass).
    TiltedPhoques::Vector<uint8_t*> bodies;
    void* pHkpWorld = nullptr;
    TiltedPhoques::Vector<void*> queue;
    queue.push_back(pRoot);
    for (size_t head = 0; head < queue.size() && head < 512; ++head)
    {
        void* pObject = queue[head];
        if (uint8_t* pBody = HavokBodyOfLive(pObject))
        {
            const uint8_t motionType = pBody[kMotionTypeOffset];
            void* pWorld = At<void*>(pBody, kHkWorldOffset);
            if (pWorld && ((motionType >= 1 && motionType <= 3) || motionType == 6) && (!pHkpWorld || pWorld == pHkpWorld) && bodies.size() < 64)
            {
                pHkpWorld = pWorld;
                bodies.push_back(pBody);
            }
        }
        void* pNode = AsNode(pObject);
        void** pChildren = pNode ? At<void**>(pNode, kChildrenOffset + 0x8) : nullptr;
        const uint16_t capacity = pNode ? At<uint16_t>(pNode, kChildrenOffset + 0x10) : 0;
        for (uint16_t i = 0; pChildren && i < capacity; ++i)
            if (pChildren[i])
                queue.push_back(pChildren[i]);
    }
    if (bodies.empty())
        return false;

    const float* pRootAt = reinterpret_cast<const float*>(bodies[0] + kBodyTranslationOffset);
    const glm::vec3 cRagdollRoot = glm::vec3{pRootAt[0], pRootAt[1], pRootAt[2]} / cScale;
    const float cApart = glm::length(glm::vec2{acWanted.x - cRagdollRoot.x, acWanted.y - cRagdollRoot.y});

    if (placed.SayAfter && cNow - placed.MovedAt >= std::chrono::seconds(1))
    {
        placed.SayAfter = false;
        spdlog::info("CorpseDiag: {:X}'s ragdoll now lies {:.0f} units from where its owner's corpse is", apActor->formID, cApart);
    }

    // The same 64 units as the reference: a settled ragdoll is not pulled around every frame.
    if (cApart <= 64.f)
        return false;

    // Its root put just above where the owner has the body (a reference stands on the ground), to settle from there.
    constexpr float cLift = 20.f;
    const glm::vec3 cOffset{acWanted.x - cRagdollRoot.x, acWanted.y - cRagdollRoot.y, acWanted.z + cLift - cRagdollRoot.z};
    const glm::vec3 cHavokOffset = cOffset * cScale;

    WorldWriteLock lock(BhkWorldOf(pHkpWorld));
    if (!lock.Held())
        return false;
    uint32_t moved = 0;
    for (uint8_t* pBody : bodies)
    {
        // Still in that world now that it is locked.
        if (At<void*>(pBody, kHkWorldOffset) != pHkpWorld)
            continue;
        const float* pAt = reinterpret_cast<const float*>(pBody + kBodyTranslationOffset);
        const float position[4] = {pAt[0] + cHavokOffset.x, pAt[1] + cHavokOffset.y, pAt[2] + cHavokOffset.z, 0.f};
        float rotation[4];
        std::memcpy(rotation, pBody + kBodyQuaternionOffset, sizeof(rotation));
        s_setPositionAndRotation.Get()(pBody, position, rotation);
        std::memset(pBody + kAngularVelocityOffset - 0x10, 0, 16); // linear velocity
        std::memset(pBody + kAngularVelocityOffset, 0, 16);
        if (s_activate.Get())
            s_activate.Get()(pBody);
        ++moved;
    }
    placed.MovedAt = cNow;
    placed.SayAfter = true;
    spdlog::info("CorpseDiag: {:X}'s ragdoll lay {:.0f} units from where its owner's corpse is; moved there ({} bodies)", apActor->formID, cApart, moved);
    return moved > 0;
}

bool IsWeaponTouchAt(const glm::vec3& acPoint) noexcept
{
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    if (!s_havokScale.Get() || !(*s_havokScale.Get() > 0.f))
        return false;
    const float cScale = *s_havokScale.Get();
    const glm::vec3 cHavokPoint = acPoint * cScale;
    // The same contact point PLANCK reads, so it matches to a rounding; 3 units of slack.
    const float cTolerance = 3.f * cScale;
    const auto cNow = std::chrono::steady_clock::now();
    std::lock_guard lock(s_contactsLock);
    for (const auto& touch : s_recentTouches)
        if (touch.At.time_since_epoch().count() && cNow - touch.At < std::chrono::milliseconds(500) && glm::distance(touch.Point, cHavokPoint) <= cTolerance)
            return true;
    return false;
}

bool SparkClash(const glm::vec3& acPoint) noexcept
{
    // The game's own sparks for a one-handed blade on metal (Skyrim.esm WPNBlade1HandVsMetaImpact, 0x4BB52), at the
    // point, for a second. Asked for by Emma and Seen after the first fight (2026-10-06: "no blocking animation with
    // sparks or whatever").
    using TSpawnParticle = void*(TESObjectCELL* apCell, float aLifetime, const char* apModel, const NiPoint3& acRotation, const NiPoint3& acPosition, float aScale,
                                 uint32_t aFlags, void* apTarget);
    POINTER_SKYRIMSE(TSpawnParticle, s_spawnParticle, 29218, 29218); // BSTempEffectParticle::Spawn (CommonLibVR-NG)
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!s_spawnParticle.Get() || !pCell)
        return false;
    NiPoint3 rotation;
    rotation.x = rotation.y = rotation.z = 0.f;
    NiPoint3 position;
    position.x = acPoint.x;
    position.y = acPoint.y;
    position.z = acPoint.z;
    const bool cSpawned = s_spawnParticle.Get()(pCell, 1.f, "Effects\\ImpactEffects\\FXMetalSparkImpactSlice.nif", rotation, position, 1.f, 7, nullptr) != nullptr;
    static std::chrono::steady_clock::time_point s_nextSaid{};
    if (const auto cNow = std::chrono::steady_clock::now(); cNow >= s_nextSaid)
    {
        s_nextSaid = cNow + std::chrono::seconds(5);
        spdlog::info("Clash: sparks at ({:.0f}, {:.0f}, {:.0f}){}", acPoint.x, acPoint.y, acPoint.z, cSpawned ? "" : " not shown (the game made no effect)");
    }
    return cSpawned;
}

bool SoundClash(const glm::vec3& acPoint) noexcept
{
    // Skyrim.esm WPNBlockBlade1HandVsOtherSD: the game's own sound for a one-handed blade blocked by a weapon (what the
    // sound marker WPNBlockBladeVsOther, 0x137D0, plays).
    constexpr uint32_t kBladeBlockSound = 0x3C73C;
    constexpr uint8_t kSoundDescriptorType = 0x80; // FormType SNDR (CommonLibVR-NG FormTypes.h)
    constexpr uint32_t kSoundDescriptorInterface = 0x20; // BGSSoundDescriptorForm's BSISoundDescriptor

    // BSSoundHandle (CommonLibVR-NG): sound id, "assume success", state.
    struct SoundHandle
    {
        uint32_t SoundId = 0xFFFFFFFF;
        bool AssumeSuccess = false;
        uint8_t Pad05 = 0;
        uint16_t Pad06 = 0;
        uint32_t State = 0;
    };
    using TGetAudioManager = void*();
    using TGetSoundHandle = bool(void* apManager, SoundHandle& aHandle, void* apDescriptor, uint32_t aFlags);
    using TSetPosition = bool(SoundHandle* apHandle, NiPoint3 aPosition);
    using TPlay = bool(SoundHandle* apHandle);
    POINTER_SKYRIMSE(TGetAudioManager, s_getAudioManager, 66391, 66391);
    POINTER_SKYRIMSE(TGetSoundHandle, s_getSoundHandle, 66404, 66404);
    POINTER_SKYRIMSE(TSetPosition, s_setPosition, 66370, 66370);
    POINTER_SKYRIMSE(TPlay, s_play, 66355, 66355);
    if (!s_getAudioManager.Get() || !s_getSoundHandle.Get() || !s_setPosition.Get() || !s_play.Get())
        return false;

    TESForm* pForm = TESForm::GetById(kBladeBlockSound);
    if (!pForm || static_cast<uint8_t>(pForm->formType) != kSoundDescriptorType)
        return false;
    void* pManager = s_getAudioManager.Get()();
    if (!pManager)
        return false;
    SoundHandle handle{};
    // 0x1A: the flags CommonLibVR-NG's BSAudioManager::Play passes.
    if (!s_getSoundHandle.Get()(pManager, handle, reinterpret_cast<uint8_t*>(pForm) + kSoundDescriptorInterface, 0x1A))
        return false;
    NiPoint3 position;
    position.x = acPoint.x;
    position.y = acPoint.y;
    position.z = acPoint.z;
    s_setPosition.Get()(&handle, position);
    return s_play.Get()(&handle);
}

// Where this player's weapons are held, relative to the third-person hands the rest of the pose is read from
// (VRPose::HasWeapons). In VR the weapon the player sees and swings hangs off the first-person hand, at the angle VR
// holds it; the third-person body's own attach node holds it the skeleton's way, and that is what the copy on the
// other screen showed. The attach nodes are searched twice a second, as ResolveHolding does, and read in between.
void CaptureWeapons(PlayerCharacter* apPlayer, void* apRoot, const BoneNodes& acNodes, VRPose& aOutPose, const std::chrono::steady_clock::time_point aNow) noexcept
{
    struct Side
    {
        void* pAttach = nullptr; // first person: the one that is seen and swung
        void* pAttachVTable = nullptr;
        void* pBody = nullptr; // third person: where the copy would hang it, for the measurement
        void* pBodyVTable = nullptr;
    };
    static std::array<Side, 2> s_sides{};
    static std::chrono::steady_clock::time_point s_searchAt{};
    static void* s_searchedRoot = nullptr;

    if (aNow >= s_searchAt || s_searchedRoot != apRoot)
    {
        s_searchAt = aNow + std::chrono::milliseconds(500);
        s_searchedRoot = apRoot;
        for (size_t side = 0; side < 2; ++side)
        {
            Side& found = s_sides[side];
            found = Side{};
            // Below the forearm, the hand's parent: the shield node hangs off the forearm's twist bone.
            if (void* pHand = FirstPersonHand(apPlayer, side))
            {
                void* pForearm = At<void*>(pHand, kParentOffset);
                void* pAttach = FindShallowest(pForearm ? pForearm : pHand, kAttachNames[side]);
                if (pAttach && AsNode(pAttach))
                {
                    found.pAttach = pAttach;
                    found.pAttachVTable = *static_cast<void**>(pAttach);
                }
            }
            if (void* pForearm = acNodes[side == 0 ? VRPose::kLeftForearm : VRPose::kRightForearm])
            {
                if (void* pBody = FindShallowest(pForearm, kAttachNames[side]))
                {
                    found.pBody = pBody;
                    found.pBodyVTable = *static_cast<void**>(pBody);
                }
            }
        }
    }

    std::array<bool, 2> held{};
    std::array<Quaternion_NetQuantize, 2> rotation{};
    std::array<glm::vec3, 2> offset{};
    static std::array<std::chrono::steady_clock::time_point, 2> s_nextLog{};
    for (size_t side = 0; side < 2; ++side)
    {
        const Side& found = s_sides[side];
        void* pHand = acNodes[side == 0 ? VRPose::kLeftHand : VRPose::kRightHand];
        // Nothing hanging off the attach node: nothing held in that hand.
        if (!pHand || !StillTheSame(found.pAttach, found.pAttachVTable) || (ChildFingerprintOf(found.pAttach) & 0xFFFF) == 0)
            continue;

        const NiTransform& hand = At<NiTransform>(pHand, kWorldOffset);
        const NiTransform& grip = At<NiTransform>(found.pAttach, kWorldOffset);
        // As drawn: a blade resting on his (the blade stop) is sent resting there, so he sees it stopped too.
        const glm::mat3 gripRotation = DrawnGripRotation(side, grip.rotate);
        const glm::mat3 inverseHand = glm::transpose(ToGlm(hand.rotate));
        const glm::vec3 at = inverseHand * (ToGlm(grip.translate) - ToGlm(hand.translate)) / std::max(hand.scale, 0.05f);
        const bool cLog = aNow >= s_nextLog[side];
        if (cLog)
            s_nextLog[side] = aNow + std::chrono::seconds(10);
        // The first-person hand is the controller; the body's hand is where VRIK got the arm to. Past a hand's length
        // apart, the arm did not reach and the grip means nothing for the copy.
        if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) || glm::length(at) > 40.f)
        {
            if (cLog)
                spdlog::info("VRBodySync: local {} weapon is {:.1f} units from the body's hand; where it is held is not sent", side ? "right" : "left", glm::length(at));
            continue;
        }

        held[side] = true;
        rotation[side] = glm::normalize(glm::quat_cast(inverseHand * gripRotation));
        offset[side] = at;

        // The measurement: how far the copy's weapon was from this one before it was sent.
        if (cLog && StillTheSame(found.pBody, found.pBodyVTable))
        {
            const NiTransform& body = At<NiTransform>(found.pBody, kWorldOffset);
            spdlog::info("VRBodySync: local {} weapon held {:.0f} degrees and {:.1f} units from where the body's own {} node has it; sent", side ? "right" : "left",
                         DegreesOf(ToGlm(grip.rotate) * glm::transpose(ToGlm(body.rotate))), glm::distance(ToGlm(grip.translate), ToGlm(body.translate)), kAttachNames[side]);
        }
    }

    // Sent when the grip changes or once a second, like the fingers; the receiver keeps the last.
    static std::array<bool, 2> s_lastHeld{};
    static std::array<Quaternion_NetQuantize, 2> s_lastRotation{};
    static std::array<glm::vec3, 2> s_lastOffset{};
    static std::chrono::steady_clock::time_point s_lastSentAt{};
    bool changed = held != s_lastHeld;
    for (size_t side = 0; side < 2 && !changed; ++side)
        changed = held[side] && (rotation[side] != s_lastRotation[side] || glm::distance(offset[side], s_lastOffset[side]) > 0.05f);

    aOutPose.HasWeapons = false;
    if (!changed && aNow - s_lastSentAt < std::chrono::seconds(1))
        return;

    s_lastHeld = held;
    s_lastRotation = rotation;
    s_lastOffset = offset;
    s_lastSentAt = aNow;
    aOutPose.HasWeapons = true;
    aOutPose.WeaponHeld = held;
    aOutPose.WeaponRotation = rotation;
    for (size_t side = 0; side < 2; ++side)
        for (int axis = 0; axis < 3; ++axis)
            aOutPose.WeaponOffset[side][axis] = offset[side][axis];
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
    // The owner's own hands relative to the root, with every pose: the receiver puts its copy's hands there
    // (ReachHands), and measures against them. Once a second until 2026-10-04, when they were only measured.
    aOutPose.HasHandCheck = false;
    if (nodes[VRPose::kLeftHand] && nodes[VRPose::kRightHand])
    {
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

    // Sent always, not only with leg trackers (2026-10-04): VRIK stands the body some way back from the headset, the
    // copy's animation stands it over its feet, and the receiver uses the horizontal part to stand the copy where the
    // owner's body is (see ApplyRemotePoses). The height is applied only while the legs are tracked.
    aOutPose.HasHips = false;
    if (nodes[VRPose::kPelvis])
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

    CaptureWeapons(apPlayer, pRoot, nodes, aOutPose, now);

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
bool ObserveRemoteBodyMotion(Actor* apActor, const glm::vec3& acOwnerPosition) noexcept
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
        glm::vec3 OwnerLast{};
        std::chrono::steady_clock::time_point SampledAt{};
        std::chrono::steady_clock::time_point OwnerMovedAt{};
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
        watch.OwnerLast = acOwnerPosition;
        watch.Placed = true;
        return false;
    }

    // Looked at every 100 ms, as the owner's side looks at its own bodies (CaptureBodyPose, at the snapshot rate): the
    // 4 units below were between two frames until 2026-10-06, 240 units a second at 60 fps, which a hand dragging a
    // body never reaches -- Seen dragged one of Emma's bodies at 20:03 and his game never noticed (no hand-off line all
    // evening), while the rig's test moved it in 15-unit jumps and always passed.
    if (now - watch.SampledAt < std::chrono::milliseconds(100))
        return false;
    watch.SampledAt = now;

    const float step = glm::distance(at, watch.Last);
    watch.Last = at;

    // Moved by its owner: its updates moved it (the position they give, or its bones). That is the other side
    // carrying it, not a hand here. On 2026-10-03 (20:54-20:56) the corpse of the Mistwatch dragon, moved that way,
    // made Seen's game ask for it twice and Emma's take it back each time: "a dead blood dragon spawns out of nowhere".
    if (glm::distance(acOwnerPosition, watch.OwnerLast) >= 4.f)
        watch.OwnerMovedAt = now;
    watch.OwnerLast = acOwnerPosition;
    const auto posed = s_bodyPosedByOwnerAt.find(apActor->formID);
    const bool cOwnerMovesIt = now - watch.OwnerMovedAt < std::chrono::milliseconds(1500) ||
                               (posed != s_bodyPosedByOwnerAt.end() && now - posed->second < std::chrono::milliseconds(1500));
    if (cOwnerMovesIt)
    {
        watch.Travelled = 0.f;
        watch.LastMoved = {};
        watch.Reported = false;
        return false;
    }

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

    // A third of a second of being moved, with one of this player's hands on it: the caller asks for the body, so
    // that this side becomes the one that sends it (see CharacterService::RunRemoteUpdates). The hand is the proof:
    // a body also moves here by itself -- a corpse falling or settling after its owner let go of it -- and on
    // 2026-10-03 (20:55) the Mistwatch dragon's corpse, which nobody here was holding, was asked for that way.
    bool cGrabbed = false;
    if (movingFor >= 300 && !watch.Reported)
    {
        watch.Reported = true;
        const float cHand = NearestHandToBody(pRoot);
        cGrabbed = cHand <= 40.f;
        if (!cGrabbed)
            spdlog::info("Body {:X} is moving here, but no hand of this player's is on it (nearest {:.0f} units); left to its owner", apActor->formID,
                         std::min(cHand, 99999.f));
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

    if (acPose.HasWeapons)
    {
        pose.HasWeapons = true;
        for (size_t side = 0; side < 2; ++side)
        {
            pose.WeaponHeld[side] = acPose.WeaponHeld[side];
            pose.WeaponRotation[side] = acPose.WeaponRotation[side];
            pose.WeaponOffset[side] = glm::vec3{acPose.WeaponOffset[side][0], acPose.WeaponOffset[side][1], acPose.WeaponOffset[side][2]};
        }
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
