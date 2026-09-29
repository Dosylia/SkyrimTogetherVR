#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Structs/AnimationVariables.h>

using namespace TiltedPhoques;

// AnimationVariables is the only *stateful* thing on the wire. Everything else describes where a body is now;
// this describes what changed since the last packet, and the receiver reconstructs the rest from what it
// already had. That makes it the one format where a decoding bug does not show up as a glitch on one frame --
// it shows up as a body stuck in a state nobody sent, for as long as the variable does not change again.
//
// It is also entirely hand-rolled: booleans bit-packed into a string by hand, integers and floats sent only
// when they differ, and a change table whose bits have to line up with three separate counts. The bit-packing
// loops run one byte at a time with a shifting mask, which is exactly the code that breaks on counts that are
// not a multiple of eight.
//
// Written 2026-09-28. The format had no test.

namespace
{
AnimationVariables Make(const std::initializer_list<bool> aBooleans, const std::initializer_list<uint32_t> aIntegers,
                        const std::initializer_list<float> aFloats)
{
    AnimationVariables vars;
    vars.Booleans.assign(aBooleans.begin(), aBooleans.end());
    vars.Integers.assign(aIntegers.begin(), aIntegers.end());
    vars.Floats.assign(aFloats.begin(), aFloats.end());
    return vars;
}

//! Send `acNow` against `acPrevious`, and apply the result to `aReceiver` -- exactly what the two ends do.
void Transmit(const AnimationVariables& acNow, const AnimationVariables& acPrevious, AnimationVariables& aReceiver)
{
    Buffer buffer(1 << 16);

    Buffer::Writer writer(&buffer);
    acNow.GenerateDiff(acPrevious, writer);

    Buffer::Reader reader(&buffer);
    aReceiver.ApplyDiff(reader);
}
} // namespace

TEST_CASE("a first packet carries everything", "[animvars]")
{
    // Nothing has been sent yet, so the sender diffs against an empty set and the receiver starts empty. Every
    // value has to arrive, because there is no earlier state to fall back on.
    const AnimationVariables sent = Make({true, false, true, true}, {1, 2, 3}, {1.5f, -2.25f});

    AnimationVariables received;
    Transmit(sent, AnimationVariables{}, received);

    REQUIRE(received == sent);
}

TEST_CASE("an empty set survives", "[animvars]")
{
    const AnimationVariables sent;

    AnimationVariables received = Make({true}, {7}, {7.f});
    Transmit(sent, AnimationVariables{}, received);

    REQUIRE(received.Booleans.empty());
    REQUIRE(received.Integers.empty());
    REQUIRE(received.Floats.empty());
}

TEST_CASE("bit-packed booleans survive every count around a byte boundary", "[animvars]")
{
    // The packing loop walks one byte at a time with a mask that stops at 0x100, and the array it writes is
    // (size + 7) / 8 bytes. Counts either side of a multiple of eight are where that arithmetic goes wrong, and
    // a boolean read back wrong is an animation flag -- sneaking, sprinting, weapon drawn -- stuck on or off.
    for (size_t count = 0; count <= 70; ++count)
    {
        AnimationVariables sent;
        sent.Booleans.resize(count);
        for (size_t i = 0; i < count; ++i)
            sent.Booleans[i] = (i % 3) == 0; // an irregular pattern, so a shift by one is visible

        AnimationVariables received;
        Transmit(sent, AnimationVariables{}, received);

        INFO("boolean count " << count);
        REQUIRE(received.Booleans.size() == count);
        for (size_t i = 0; i < count; ++i)
        {
            INFO("bit " << i);
            REQUIRE(received.Booleans[i] == sent.Booleans[i]);
        }
    }
}

TEST_CASE("a value that did not change is kept, not cleared", "[animvars]")
{
    // The whole point of the format: unchanged integers and floats are not on the wire at all, so the receiver
    // has to hold on to what it had. If it did not, every variable would drop to zero the moment it stopped
    // changing, which for an animation float means a limb snapping to a default pose whenever it settles.
    const AnimationVariables first = Make({true, false}, {10, 20, 30}, {1.f, 2.f, 3.f});

    AnimationVariables received;
    Transmit(first, AnimationVariables{}, received);
    REQUIRE(received == first);

    AnimationVariables second = first;
    second.Integers[1] = 99;
    second.Floats[2] = -7.5f;

    Transmit(second, first, received);

    REQUIRE(received == second);
    REQUIRE(received.Integers[0] == 10); // untouched, and never sent
    REQUIRE(received.Floats[0] == 1.f);
}

TEST_CASE("a run of packets leaves both ends agreeing", "[animvars]")
{
    // What actually happens in play: a state every frame, each diffed against the one before. The sender's idea
    // of "previous" is the last state it sent, and the receiver's state has to track it exactly, forever -- any
    // single step that loses a value stays lost, because later packets only describe what changed after it.
    AnimationVariables previous;
    AnimationVariables received;

    for (uint32_t step = 0; step < 64; ++step)
    {
        AnimationVariables now = Make({(step & 1) != 0, (step & 2) != 0, (step & 4) != 0},
                                      {step, step * 7u, 0xFFFFFFFFu - step},
                                      {static_cast<float>(step) * 0.25f, -static_cast<float>(step), 3.5f});

        Transmit(now, previous, received);

        INFO("step " << step);
        REQUIRE(received == now);

        previous = now;
    }
}

TEST_CASE("the counts can change mid-stream", "[animvars]")
{
    // A different actor, or a creature with a different animation graph, has a different number of variables.
    // When the counts change nothing from the previous state can be trusted, so everything must be re-sent.
    const AnimationVariables small = Make({true}, {1}, {1.f});

    AnimationVariables received;
    Transmit(small, AnimationVariables{}, received);
    REQUIRE(received == small);

    const AnimationVariables large = Make({true, true, false, true}, {5, 6, 7, 8}, {9.f, 10.f, 11.f});
    Transmit(large, small, received);
    REQUIRE(received == large);

    // And back down again, which is the case that leaves stale entries behind if the receiver only grows.
    Transmit(small, large, received);
    REQUIRE(received == small);
}

TEST_CASE("extreme values survive intact", "[animvars]")
{
    // Integers go through a VarInt and floats through WriteFloat. Both are asserted exactly: an animation float
    // that arrives approximately right is a limb in approximately the right place on every remote body.
    const AnimationVariables sent = Make({true, false},
                                         {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu},
                                         {0.f, -0.f, 1.0f, -1.0f, 3.4028235e38f, 1.1754944e-38f, 0.1f});

    AnimationVariables received;
    Transmit(sent, AnimationVariables{}, received);

    REQUIRE(received.Integers.size() == sent.Integers.size());
    for (size_t i = 0; i < sent.Integers.size(); ++i)
    {
        INFO("integer " << i);
        REQUIRE(received.Integers[i] == sent.Integers[i]);
    }
    for (size_t i = 0; i < sent.Floats.size(); ++i)
    {
        INFO("float " << i);
        REQUIRE(received.Floats[i] == Approx(sent.Floats[i]).epsilon(0.0));
    }
}

TEST_CASE("equality notices a change in any of the three vectors", "[animvars]")
{
    // GenerateDiff decides what to send by comparing against the previous state, so a comparison that missed a
    // field would stop that field travelling the moment nothing else changed.
    const AnimationVariables base = Make({true, false}, {1, 2}, {1.f, 2.f});

    AnimationVariables other = base;
    other.Booleans[1] = true;
    REQUIRE(other != base);

    other = base;
    other.Integers[0] = 42;
    REQUIRE(other != base);

    other = base;
    other.Floats[1] = 42.f;
    REQUIRE(other != base);

    other = base;
    REQUIRE(other == base);
}
