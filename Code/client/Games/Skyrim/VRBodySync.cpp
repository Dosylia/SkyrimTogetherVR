#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/VRBodySync.h>

#include <Actor.h>
#include <PlayerCharacter.h>
#include <NetImmerse/NiNode.h>
#include <NetImmerse/NiTransform.h>

#include <glm/gtc/quaternion.hpp>

#include <mutex>
#include <shared_mutex>

// Everything here reads NetImmerse objects through raw offsets (CommonLibVR-NG layouts) rather
// than the client's NiAVObject/NiNode structs, because the VR NiAVObject is 0x138 bytes instead of
// SE's 0x110 and the client structs only model the SE size.
namespace
{
constexpr uint32_t kNameOffset = 0x10;   // NiObjectNET::name (BSFixedString -> const char*)
constexpr uint32_t kParentOffset = 0x30; // NiAVObject::parent
constexpr uint32_t kLocalOffset = 0x48;  // NiAVObject::local
constexpr uint32_t kWorldOffset = 0x7C;  // NiAVObject::world
#ifdef SKYRIMVR
constexpr uint32_t kChildrenOffset = 0x138; // NiNode::children (NiTObjectArray)
#else
constexpr uint32_t kChildrenOffset = 0x110;
#endif
constexpr uint32_t kVTableAsNodeSlot = 3;         // NiObject::AsNode

constexpr std::array<const char*, VRPose::kBoneCount> kBoneNames{
    "NPC Spine1 [Spn1]",   "NPC Spine2 [Spn2]",   "NPC Neck [Neck]",       "NPC Head [Head]",
    "NPC L Clavicle [LClv]", "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]",
    "NPC R Clavicle [RClv]", "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]",
};

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

// Parent bone index for each entry of kBoneNames (-1: searched under "NPC Root [Root]").
constexpr std::array<int8_t, VRPose::kBoneCount> kBoneParents{-1, 0, 1, 2, 1, 4, 5, 6, 1, 8, 9, 10};

// Breadth-first search for the shallowest node with this name below apStart. Armour with physics
// (SMP/3BA, common in FUS) embeds its own copies of skeleton nodes such as "NPC L Hand"; a plain
// first-match search mixed real skeleton bones with those copies, which stretched the remote arms.
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

bool FindBones(void* apRoot, BoneNodes& aNodes) noexcept
{
    aNodes.fill(nullptr);

    void* pSkeletonRoot = FindShallowest(apRoot, "NPC Root [Root]");
    if (!pSkeletonRoot)
        pSkeletonRoot = apRoot;

    // Walk the real chain: every bone is searched inside the bone found for its parent.
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        void* pSearchFrom = kBoneParents[i] < 0 ? pSkeletonRoot : aNodes[kBoneParents[i]];
        aNodes[i] = FindShallowest(pSearchFrom, kBoneNames[i]);
        if (!aNodes[i])
            return false;
    }
    return true;
}


void LogBoneChain(const char* acpWhere, void* apRoot, const BoneNodes& acNodes) noexcept
{
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        void* pParent = At<void*>(acNodes[i], kParentOffset);
        const auto& world = At<NiTransform>(acNodes[i], kWorldOffset);
        spdlog::info("VRBodySync[{}]: {} (parent {}) world pos ({:.1f}, {:.1f}, {:.1f}) scale {:.2f}", acpWhere, kBoneNames[i], pParent ? GetName(pParent) : "none",
                     world.translate.x, world.translate.y, world.translate.z, world.scale);
    }
    const auto& forearm = At<NiTransform>(acNodes[VRPose::kLeftForearm], kWorldOffset).translate;
    const auto& hand = At<NiTransform>(acNodes[VRPose::kLeftHand], kWorldOffset).translate;
    const float dx = hand.x - forearm.x, dy = hand.y - forearm.y, dz = hand.z - forearm.z;
    spdlog::info("VRBodySync[{}]: left forearm->hand distance {:.1f}", acpWhere, std::sqrt(dx * dx + dy * dy + dz * dz));
}

// NiMatrix3 is row-major (data[row][col]); glm is column-major. Converting through glm keeps the
// matrix->quat->matrix round trip consistent whatever the handedness turns out to be.
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

// world = parent.world * local (NetImmerse: R = Rp * Rl, T = Tp + Rp * (Tl * Sp), S = Sp * Sl)
void UpdateWorldFromParent(void* apObject) noexcept
{
    void* pParent = At<void*>(apObject, kParentOffset);
    if (!pParent)
        return;

    const auto& parentWorld = At<NiTransform>(pParent, kWorldOffset);
    const auto& local = At<NiTransform>(apObject, kLocalOffset);
    auto& world = At<NiTransform>(apObject, kWorldOffset);

    const glm::mat3 parentRotation = ToGlm(parentWorld.rotate);
    const glm::vec3 localTranslate{local.translate.x, local.translate.y, local.translate.z};
    const glm::vec3 translate = glm::vec3{parentWorld.translate.x, parentWorld.translate.y, parentWorld.translate.z} + parentRotation * (localTranslate * parentWorld.scale);

    world.rotate = FromGlm(parentRotation * ToGlm(local.rotate));
    world.translate.x = translate.x;
    world.translate.y = translate.y;
    world.translate.z = translate.z;
    world.scale = parentWorld.scale * local.scale;
}

void UpdateSubtree(void* apObject, uint32_t aDepth) noexcept
{
    if (!apObject || aDepth > 64)
        return;

    UpdateWorldFromParent(apObject);

    void* pNode = AsNode(apObject);
    if (!pNode)
        return;

    void** pChildren = At<void**>(pNode, kChildrenOffset + 0x8);
    const uint16_t capacity = At<uint16_t>(pNode, kChildrenOffset + 0x10);
    if (!pChildren)
        return;

    for (uint16_t i = 0; i < capacity; ++i)
        UpdateSubtree(pChildren[i], aDepth + 1);
}

struct RemotePose
{
    std::array<glm::quat, VRPose::kBoneCount> Bones{};
};

std::shared_mutex s_posesLock;
TiltedPhoques::Map<uint32_t, RemotePose> s_poses;

void ApplyRemotePose(Actor* apActor) noexcept
{
    static bool s_loggedHook = false;
    if (!std::exchange(s_loggedHook, true))
        spdlog::info("VRBodySync: animation update hook is running (first actor {:X})", apActor->formID);

    RemotePose pose;
    {
        std::shared_lock lock(s_posesLock);
        const auto it = s_poses.find(apActor->formID);
        if (it == s_poses.end())
            return;
        pose = it->second;
    }

    void* pRoot = apActor->GetNiNode();
    if (!pRoot)
    {
        static bool s_loggedNoRoot = false;
        if (!std::exchange(s_loggedNoRoot, true))
            spdlog::warn("VRBodySync: remote actor {:X} has a pose but no 3D root yet", apActor->formID);
        return;
    }

    BoneNodes nodes;
    if (!FindBones(pRoot, nodes))
    {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
            spdlog::warn("VRBodySync: remote actor {:X} skeleton is missing some upper-body bones, pose not applied", apActor->formID);
        return;
    }

    static bool s_loggedApply = false;
    if (!std::exchange(s_loggedApply, true))
        spdlog::info("VRBodySync: applying remote VR pose to actor {:X}", apActor->formID);

    const glm::mat3 rootRotation = ToGlm(At<NiTransform>(pRoot, kWorldOffset).rotate);

    // Parent-before-child order: each bone's parent world transform is already final when we
    // derive the bone's local rotation from its desired root-relative rotation.
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        void* pBone = nodes[i];
        void* pParent = At<void*>(pBone, kParentOffset);
        if (!pParent)
            continue;

        const glm::mat3 desiredWorld = rootRotation * glm::mat3_cast(pose.Bones[i]);
        const glm::mat3 parentWorld = ToGlm(At<NiTransform>(pParent, kWorldOffset).rotate);

        At<NiTransform>(pBone, kLocalOffset).rotate = FromGlm(glm::transpose(parentWorld) * desiredWorld);
        UpdateWorldFromParent(pBone);
    }

    // Let the engine refresh the whole actor (world transforms of every node, skin instances and
    // bounds) from the new local rotations. Recomputing only the bone world transforms moved rigidly
    // attached items (torch, shield, weapons) but left skinned armour/body meshes on the animation
    // pose - the "stretched arms" seen in testing. NiAVObject::Update = SE 68900 (VR csv 0xc9bc10).
    struct NiUpdateData
    {
        float time = 0.f;
        uint32_t flags = 0;
    } updateData;
    TP_THIS_FUNCTION(TNiAVObjectUpdate, void, void, NiUpdateData&);
    POINTER_SKYRIMSE(TNiAVObjectUpdate, s_niAVObjectUpdate, 0, 68900);
    TiltedPhoques::ThisCall(s_niAVObjectUpdate, pRoot, updateData);

    static bool s_loggedChain = false;
    if (!std::exchange(s_loggedChain, true))
        LogBoneChain("remote", pRoot, nodes);
}

} // namespace

// Per-actor animation graph update: SE 36372 (VR 0x5e2010, name DB Character::sub_1405D9990),
// (Actor*, float delta). NPCs - including remote players - never go through Actor::UpdateAnimation
// (SE 36370): the process update inlines it and calls this function directly (VR 0x14070883e), and
// UpdateAnimation calls it as well. Both earlier hook points (vtable slot 0x7D, then a detour on
// 36370) therefore never ran for remote players. The pose is applied right after the graph update.
TP_THIS_FUNCTION(TUpdateAnimation, void, Actor, float aDelta);
static TUpdateAnimation* RealUpdateAnimation = nullptr;

void TP_MAKE_THISCALL(HookUpdateAnimation, Actor, float aDelta)
{
    TiltedPhoques::ThisCall(RealUpdateAnimation, apThis, aDelta);

    if (apThis)
        ApplyRemotePose(apThis);
}

static TiltedPhoques::Initializer s_vrBodySyncHooks(
    []()
    {
        POINTER_SKYRIMSE(TUpdateAnimation, s_updateAnimation, 0, 36372);

        RealUpdateAnimation = s_updateAnimation.Get();

        TP_HOOK(&RealUpdateAnimation, HookUpdateAnimation);
    });

namespace VRBodySync
{
bool CaptureLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose) noexcept
{
    aOutPose.HasData = false;

    if (!apPlayer)
        return false;

    void* pRoot = apPlayer->GetNiNode();
    if (!pRoot)
        return false;

    BoneNodes nodes;
    static bool s_loggedCapture = false;
    if (!FindBones(pRoot, nodes))
    {
        if (!std::exchange(s_loggedCapture, true))
        {
            for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
                if (!nodes[i])
                    spdlog::warn("VRBodySync: local player skeleton has no bone '{}', VR pose not sent", kBoneNames[i]);
        }
        return false;
    }

    if (!std::exchange(s_loggedCapture, true))
    {
        spdlog::info("VRBodySync: capturing local VR pose from player skeleton root {}", pRoot);
        LogBoneChain("local", pRoot, nodes);
    }

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

    static bool s_loggedReceive = false;
    if (!std::exchange(s_loggedReceive, true))
        spdlog::info("VRBodySync: received first remote VR pose, actor {:X}", apActor->formID);

    RemotePose pose;
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
        pose.Bones[i] = acPose.Bones[i];

    std::unique_lock lock(s_posesLock);
    s_poses[apActor->formID] = pose;
}

void ClearRemotePose(uint32_t aFormId) noexcept
{
    std::unique_lock lock(s_posesLock);
    s_poses.erase(aFormId);
}
} // namespace VRBodySync
