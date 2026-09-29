#include <TiltedCore/Buffer.hpp>

#include <catch2/catch.hpp>

#include <Structs/Quaternion_NetQuantize.h>

// Quaternion_NetQuantize drops the largest component and quantises the other three to 10 bits. That is lossy by
// design, which is fine -- but the codec has to be *stable*: once a value has been through it, going through it
// again must not keep changing it. Everything upstream assumes that. VRBodySync only sends a pose when the
// quantised bones differ from the last quantised bones it sent, and VRPose::operator== compares packed forms,
// so a codec that never settles would mean poses that always look changed and are always re-sent.
//
// Written 2026-09-28 because the first VRPose round-trip test failed and the cause had to be established before
// deciding whether the bug was in the pose or in the test.

TEST_CASE("packing is stable once a value has been through it", "[quantize]")
{
    // A spread of orientations, including ones where two components are nearly equal in magnitude -- the awkward
    // case for "smallest three", because which component is dropped can change.
    const glm::quat cases[] = {
        glm::quat(1.f, 0.f, 0.f, 0.f),
        glm::normalize(glm::quat(1.f, 0.01f, 0.02f, 0.03f)),
        glm::normalize(glm::quat(0.5f, 0.5f, 0.5f, 0.5f)),
        glm::normalize(glm::quat(0.7071f, 0.7071f, 0.f, 0.f)),
        glm::normalize(glm::quat(0.f, 1.f, 0.f, 0.f)),
        glm::normalize(glm::quat(-0.6f, 0.6f, 0.4f, 0.33f)),
        glm::normalize(glm::quat(0.13f, 0.14f, 0.15f, 0.97f)),
    };

    for (size_t i = 0; i < std::size(cases); ++i)
    {
        Quaternion_NetQuantize q;
        q = cases[i];

        Quaternion_NetQuantize once;
        once.Unpack(q.Pack());

        Quaternion_NetQuantize twice;
        twice.Unpack(once.Pack());

        INFO("case " << i);
        // The first pass may move the value; the second must not.
        REQUIRE(once.Pack() == twice.Pack());
    }
}

TEST_CASE("a value that came off the wire survives being sent again", "[quantize]")
{
    // This is what a relay does: receive a pose, hold it, send it on. If that is not a fixed point, a body's
    // bones drift a little every hop.
    Quaternion_NetQuantize original;
    original = glm::normalize(glm::quat(1.f, 0.13f, 0.26f, 0.39f));

    Quaternion_NetQuantize current;
    current.Unpack(original.Pack());

    const uint32_t settled = current.Pack();
    for (int hop = 0; hop < 8; ++hop)
    {
        Quaternion_NetQuantize next;
        next.Unpack(current.Pack());
        current = next;
        INFO("hop " << hop);
        REQUIRE(current.Pack() == settled);
    }
}
