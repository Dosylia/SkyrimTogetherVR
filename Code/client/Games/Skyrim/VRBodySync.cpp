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

// NetImmerse objects are read through raw offsets: the VR NiAVObject is 0x138 bytes, while the
// client's structs only model the SE size (0x110).
namespace
{
constexpr uint32_t kNameOffset = 0x10;   // NiObjectNET::name
constexpr uint32_t kParentOffset = 0x30; // NiAVObject::parent
constexpr uint32_t kLocalOffset = 0x48;  // NiAVObject::local
constexpr uint32_t kWorldOffset = 0x7C;  // NiAVObject::world
#ifdef SKYRIMVR
constexpr uint32_t kChildrenOffset = 0x138; // NiNode::children
#else
constexpr uint32_t kChildrenOffset = 0x110;
#endif
constexpr uint32_t kVTableAsNodeSlot = 3; // NiObject::AsNode

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

// Breadth-first search for the shallowest node with this name. Physics armour (SMP/3BA) carries its
// own copies of skeleton nodes like "NPC L Hand"; picking one of those stretched the remote arms.
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

// world = parent.world * local (R = Rp * Rl, T = Tp + Rp * (Tl * Sp), S = Sp * Sl)
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

struct RemotePose
{
    std::array<glm::quat, VRPose::kBoneCount> Bones{};
};

std::shared_mutex s_posesLock;
TiltedPhoques::Map<uint32_t, RemotePose> s_poses;

void ApplyRemotePose(Actor* apActor) noexcept
{
    RemotePose pose;
    {
        std::shared_lock lock(s_posesLock);
        const auto it = s_poses.find(apActor->formID);
        if (it == s_poses.end())
            return;
        pose = it->second;
    }

    PerfCounterScope perfScope(PerfCounter::kVRPoseApply);

    void* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return;

    BoneNodes nodes;
    if (!FindBones(pRoot, nodes))
        return;

    const glm::mat3 rootRotation = ToGlm(At<NiTransform>(pRoot, kWorldOffset).rotate);

    // Parents come first, so each parent's world transform is final before its child is solved.
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

    // Let the engine refresh the whole actor (NiAVObject::Update, SE 68900). Updating only the bones
    // moved attached items but left skinned body and armour meshes on the animation pose.
    struct NiUpdateData
    {
        float time = 0.f;
        uint32_t flags = 0;
    } updateData;
    TP_THIS_FUNCTION(TNiAVObjectUpdate, void, void, NiUpdateData&);
    POINTER_SKYRIMSE(TNiAVObjectUpdate, s_niAVObjectUpdate, 0, 68900);
    TiltedPhoques::ThisCall(s_niAVObjectUpdate, pRoot, updateData);
}

} // namespace

// Per-actor animation graph update (SE 36372). NPCs, remote players included, never go through
// Actor::UpdateAnimation: the process update calls this directly. The pose is applied right after.
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
    // Spells from a remote VR player were reported leaving slightly off the hand. The caster aims from the magic
    // node, so log where it is relative to the posed hand for the first few casts.
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
