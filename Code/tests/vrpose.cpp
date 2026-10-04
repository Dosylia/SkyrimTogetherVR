#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <Structs/VRPose.h>

using namespace TiltedPhoques;

// VRPose has carried the VR body across the wire all week and has never had a test.
//
// Two bugs came out of that. The hip offset shipped on 2026-09-25 did nothing for a day, because a new field
// needs the message, the sender, the interpolation rebuild *and* the receiver, and the third was missed. Then
// `NoBones` was added on 2026-09-26 by hand-editing the serialiser, the deserialiser and the comparison in three
// separate places -- a wire format changed by hand, with nothing checking that a pose survives the trip.
//
// These tests do not know anything about the game. They write a pose, read it back, and require the two to
// agree, over every combination of the optional fields. That is the property the whole feature rests on.

namespace
{
VRPose RoundTrip(const VRPose& acPose)
{
    Buffer buffer(4096);

    Buffer::Writer writer(&buffer);
    acPose.Serialize(writer);

    Buffer::Reader reader(&buffer);
    VRPose out{};
    out.Deserialize(reader);
    return out;
}

//! Distinct, recognisable values, so a field that lands in the wrong place is obvious rather than plausible.
Quaternion_NetQuantize MakeQuat(const float aSeed)
{
    // Assigned rather than constructed: Quaternion_NetQuantize takes a glm::quat through operator= only.
    Quaternion_NetQuantize q;
    q = glm::normalize(glm::quat(1.f, aSeed * 0.01f, aSeed * 0.02f, aSeed * 0.03f));
    return q;
}

VRPose MakeFullPose()
{
    VRPose pose{};
    pose.HasData = true;
    pose.HasLegs = true;
    for (size_t i = 0; i < VRPose::kBoneCount; ++i)
        pose.Bones[i] = MakeQuat(static_cast<float>(i + 1));

    pose.HasFingers = true;
    for (size_t i = 0; i < VRPose::kFingerBoneCount; ++i)
        pose.Fingers[i] = MakeQuat(static_cast<float>(i + 40));

    pose.HasScale = true;
    pose.RootScale = 1234;

    pose.HasRootPosition = true;
    pose.RootPosition[0] = 1234.5f;
    pose.RootPosition[1] = -6789.25f;
    pose.RootPosition[2] = 42.125f;

    pose.HasHips = true;
    pose.HipOffset[0] = 1.5f;
    pose.HipOffset[1] = -2.25f;
    pose.HipOffset[2] = 3.75f;

    pose.HasHandCheck = true;
    pose.LeftHandOffset[0] = -10.5f;
    pose.LeftHandOffset[1] = 11.25f;
    pose.LeftHandOffset[2] = -12.125f;
    pose.RightHandOffset[0] = 20.5f;
    pose.RightHandOffset[1] = -21.25f;
    pose.RightHandOffset[2] = 22.125f;

    // A sword in the right hand and nothing in the left: the shape a held weapon takes.
    pose.HasWeapons = true;
    pose.WeaponHeld = {false, true};
    pose.WeaponRotation[1] = MakeQuat(77.f);
    pose.WeaponOffset[1][0] = 1.25f;
    pose.WeaponOffset[1][1] = -3.5f;
    pose.WeaponOffset[1][2] = 7.75f;

    return pose;
}
} // namespace

TEST_CASE("an empty pose survives the wire", "[vrpose]")
{
    const VRPose pose{};
    const VRPose back = RoundTrip(pose);

    REQUIRE_FALSE(back.HasData);
    REQUIRE(back == pose);
}

TEST_CASE("a full pose survives the wire", "[vrpose]")
{
    const VRPose pose = MakeFullPose();
    const VRPose back = RoundTrip(pose);

    REQUIRE(back == pose);
    REQUIRE(back.HasLegs);
    REQUIRE(back.HasFingers);
    REQUIRE(back.HasScale);
    REQUIRE(back.HasRootPosition);
    REQUIRE(back.HasHips);
    REQUIRE(back.HasHandCheck);
    REQUIRE(back.HasWeapons);
    REQUIRE_FALSE(back.WeaponHeld[0]);
    REQUIRE(back.WeaponHeld[1]);
}

TEST_CASE("the positional fields are exact, not quantised", "[vrpose]")
{
    // These are world coordinates and root-relative offsets. They are written as raw bits precisely because
    // quantising them would move a dragged body further than the drag being sent.
    VRPose pose = MakeFullPose();
    pose.RootPosition[0] = 141472.5f;
    pose.RootPosition[1] = -27222.25f;
    pose.RootPosition[2] = -13092.125f;

    const VRPose back = RoundTrip(pose);

    REQUIRE(back.RootPosition[0] == Approx(141472.5f).epsilon(0.0));
    REQUIRE(back.RootPosition[1] == Approx(-27222.25f).epsilon(0.0));
    REQUIRE(back.RootPosition[2] == Approx(-13092.125f).epsilon(0.0));
    REQUIRE(back.HipOffset[1] == Approx(-2.25f).epsilon(0.0));
    REQUIRE(back.LeftHandOffset[2] == Approx(-12.125f).epsilon(0.0));
    REQUIRE(back.RightHandOffset[0] == Approx(20.5f).epsilon(0.0));
    REQUIRE(back.WeaponOffset[1][1] == Approx(-3.5f).epsilon(0.0));
    REQUIRE(back.WeaponOffset[1][2] == Approx(7.75f).epsilon(0.0));
}

TEST_CASE("every combination of the optional fields survives", "[vrpose]")
{
    // Eight independent flags, so 256 shapes. Each one is a wire layout of its own, because the serialiser writes
    // a field only when its flag is set -- which is exactly where a hand-edited format goes wrong.
    for (uint32_t mask = 0; mask < 256; ++mask)
    {
        VRPose pose = MakeFullPose();
        pose.NoBones = (mask & 1) != 0;
        pose.HasLegs = (mask & 2) != 0;
        pose.HasFingers = (mask & 4) != 0;
        pose.HasScale = (mask & 8) != 0;
        pose.HasRootPosition = (mask & 16) != 0;
        pose.HasHips = (mask & 32) != 0;
        pose.HasWeapons = (mask & 64) != 0;
        pose.WeaponHeld[0] = (mask & 128) != 0;

        const VRPose back = RoundTrip(pose);

        INFO("field mask " << mask);
        REQUIRE(back.NoBones == pose.NoBones);
        REQUIRE(back.HasFingers == pose.HasFingers);
        REQUIRE(back.HasScale == pose.HasScale);
        REQUIRE(back.HasRootPosition == pose.HasRootPosition);
        REQUIRE(back.HasHips == pose.HasHips);
        REQUIRE(back.HasHandCheck == pose.HasHandCheck);
        REQUIRE(back.HasWeapons == pose.HasWeapons);
        if (pose.HasWeapons)
        {
            REQUIRE(back.WeaponHeld[0] == pose.WeaponHeld[0]);
            REQUIRE(back.WeaponHeld[1] == pose.WeaponHeld[1]);
        }

        // A boneless pose carries no bones at all, so its leg flag means nothing on arrival.
        if (!pose.NoBones)
            REQUIRE(back.HasLegs == pose.HasLegs);

        REQUIRE(back == pose);
    }
}

TEST_CASE("a boneless pose still carries where the body is", "[vrpose]")
{
    // The Dwemer automaton case: no humanoid skeleton to read, so only the root position is sent, and the
    // receiver shifts the whole node tree by it.
    VRPose pose{};
    pose.HasData = true;
    pose.NoBones = true;
    pose.HasRootPosition = true;
    pose.RootPosition[0] = 10442.25f;
    pose.RootPosition[1] = -10342.5f;
    pose.RootPosition[2] = -2656.75f;

    const VRPose back = RoundTrip(pose);

    REQUIRE(back.NoBones);
    REQUIRE(back.HasRootPosition);
    REQUIRE(back.RootPosition[0] == Approx(10442.25f).epsilon(0.0));
    REQUIRE(back.RootPosition[2] == Approx(-2656.75f).epsilon(0.0));
    REQUIRE(back == pose);
}

TEST_CASE("legs are only read when they were written", "[vrpose]")
{
    // The lower-body bones exist in the struct whether or not trackers drive them. Only kUpperBoneCount of them
    // go on the wire without HasLegs, and reading more than was written is how a stream desynchronises.
    VRPose pose = MakeFullPose();
    pose.HasLegs = false;

    const VRPose back = RoundTrip(pose);

    REQUIRE_FALSE(back.HasLegs);
    for (size_t i = 0; i < VRPose::kUpperBoneCount; ++i)
    {
        INFO("upper bone " << i);
        REQUIRE(static_cast<glm::quat>(back.Bones[i]).x == Approx(static_cast<glm::quat>(pose.Bones[i]).x).margin(0.01));
    }
}

TEST_CASE("two poses that differ in one field are not equal", "[vrpose]")
{
    // operator== decides whether a pose is worth sending again. If it ignored a field, that field would stop
    // travelling the moment nothing else changed -- silently, which is the worst way for sync to fail.
    const VRPose base = MakeFullPose();

    auto differsBy = [&base](auto&& aMutate)
    {
        VRPose other = base;
        aMutate(other);
        return !(other == base);
    };

    REQUIRE(differsBy([](VRPose& p) { p.RootScale = 999; }));
    REQUIRE(differsBy([](VRPose& p) { p.RootPosition[1] += 1.f; }));
    REQUIRE(differsBy([](VRPose& p) { p.HipOffset[2] += 1.f; }));
    REQUIRE(differsBy([](VRPose& p) { p.LeftHandOffset[0] += 1.f; }));
    REQUIRE(differsBy([](VRPose& p) { p.RightHandOffset[1] += 1.f; }));
    REQUIRE(differsBy([](VRPose& p) { p.NoBones = !p.NoBones; }));
    REQUIRE(differsBy([](VRPose& p) { p.HasFingers = !p.HasFingers; }));
    REQUIRE(differsBy([](VRPose& p) { p.Bones[VRPose::kHead] = MakeQuat(99.f); }));
    REQUIRE(differsBy([](VRPose& p) { p.HasWeapons = false; }));
    REQUIRE(differsBy([](VRPose& p) { p.WeaponHeld[1] = false; }));
    REQUIRE(differsBy([](VRPose& p) { p.WeaponRotation[1] = MakeQuat(5.f); }));
    REQUIRE(differsBy([](VRPose& p) { p.WeaponOffset[1][2] += 1.f; }));
}
