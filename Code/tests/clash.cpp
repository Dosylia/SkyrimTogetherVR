#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/glm.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClashRequest.h>
#include <Messages/NotifyClash.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/NotifyHealthChangeBroadcast.h>

using namespace TiltedPhoques;

TEST_CASE("a clash keeps who, which hand, where, how fast and when", "[clash]")
{
    for (const uint8_t side : {uint8_t{0}, uint8_t{1}})
        for (const uint8_t own : {uint8_t{0}, uint8_t{1}, uint8_t{2}})
        {
            ClashRequest request{};
            request.OtherId = 0x123456;
            request.OtherSide = side;
            request.OwnSide = own;
            request.Point = glm::vec3(140885.5f, -32894.25f, -10469.75f);
            request.Speed = 312.5f;
            request.Tick = 0x1234'5678'9ABCull;

            Buffer buffer(128);
            Buffer::Writer writer(&buffer);
            request.SerializeRaw(writer);
            Buffer::Reader reader(&buffer);
            ClashRequest back{};
            back.DeserializeRaw(reader);

            INFO("side " << int(side) << ", own " << int(own));
            REQUIRE(back == request);

            NotifyClash notify{};
            notify.FromId = 0xFFFFFFFFu;
            notify.OtherId = request.OtherId;
            notify.OtherSide = side;
            notify.OwnSide = own;
            notify.Point = request.Point;
            notify.Speed = request.Speed;
            notify.Tick = request.Tick;

            Buffer buffer2(128);
            Buffer::Writer writer2(&buffer2);
            notify.SerializeRaw(writer2);
            Buffer::Reader reader2(&buffer2);
            NotifyClash back2{};
            back2.DeserializeRaw(reader2);
            REQUIRE(back2 == notify);
        }
}

TEST_CASE("a hit keeps when it happened", "[clash]")
{
    RequestHealthChangeBroadcast request{};
    request.Id = 0x100016;
    request.DeltaHealth = -30.f;
    request.Tick = 0x1234'5678'9ABCull;
    Buffer buffer(64);
    Buffer::Writer writer(&buffer);
    request.SerializeRaw(writer);
    Buffer::Reader reader(&buffer);
    RequestHealthChangeBroadcast back{};
    back.DeserializeRaw(reader);
    REQUIRE(back == request);

    NotifyHealthChangeBroadcast notify{};
    notify.Id = request.Id;
    notify.DeltaHealth = request.DeltaHealth;
    notify.AttackerPlayerId = 3;
    notify.Tick = request.Tick;
    Buffer buffer2(64);
    Buffer::Writer writer2(&buffer2);
    notify.SerializeRaw(writer2);
    Buffer::Reader reader2(&buffer2);
    NotifyHealthChangeBroadcast back2{};
    back2.DeserializeRaw(reader2);
    REQUIRE(back2 == notify);
}

TEST_CASE("a claim says whether it is for the claimant's own follower", "[clash]")
{
    for (const bool follower : {false, true})
    {
        RequestOwnershipClaim claim{};
        claim.ServerId = 0x1A;
        claim.ExpectedOwnershipEpoch = 3;
        claim.Follower = follower;
        Buffer buffer(32);
        Buffer::Writer writer(&buffer);
        claim.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        RequestOwnershipClaim back{};
        back.DeserializeRaw(reader);
        REQUIRE(back == claim);
    }
}
