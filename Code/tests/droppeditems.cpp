#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/glm.hpp>

#include <catch2/catch.hpp>

#include <Messages/RequestDroppedItemAdd.h>
#include <Messages/RequestDroppedItemRemove.h>
#include <Messages/NotifyDroppedItem.h>
#include <Messages/NotifyDroppedItemRemoved.h>
#include <Messages/RequestDroppedItemMove.h>
#include <Messages/NotifyDroppedItemMove.h>

using namespace TiltedPhoques;

// The messages behind remembered drops and moved items (2026-09-30). The drops bot pair sends three of them end to end; the
// Announcement flag -- how a player's earlier drops are told apart from a fresh one -- only travels when the game reads
// the player's dropped-item list, which no bot can do, so it is checked here.

namespace
{
Inventory::Entry EnchantedDagger()
{
    Inventory::Entry item{};
    item.BaseId = GameId(0, 0x12EB7);
    item.Count = -1;
    item.ExtraHealth = 1.3f;
    item.ExtraEnchantId = GameId(0, 0x4605A);
    item.ExtraEnchantCharge = 500;
    return item;
}
} // namespace

TEST_CASE("a drop and an announcement survive the wire, and stay different", "[droppeditems]")
{
    for (const bool announcement : {false, true})
    {
        RequestDroppedItemAdd sent{};
        sent.Item = EnchantedDagger();
        sent.CellId = GameId(0, 0x16BDB);
        sent.WorldSpaceId = GameId(0, 0x3C);
        sent.Position = glm::vec3(141472.5f, -27222.25f, -13092.125f);
        sent.Rotation = glm::vec3(0.1f, -1.5f, 6.2f);
        sent.Announcement = announcement;

        Buffer buffer(1 << 12);
        Buffer::Writer writer(&buffer);
        sent.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        RequestDroppedItemAdd back{};
        back.DeserializeRaw(reader);

        INFO("announcement " << announcement);
        REQUIRE(back.Announcement == announcement);
        REQUIRE(back.Item == sent.Item);
        REQUIRE(back.CellId == sent.CellId);
        REQUIRE(back.WorldSpaceId == sent.WorldSpaceId);
        REQUIRE(back.Position == sent.Position);
        REQUIRE(back.Rotation == sent.Rotation);
    }
}

TEST_CASE("the server's copy of an item carries its enchantment and its id", "[droppeditems]")
{
    NotifyDroppedItem sent{};
    sent.Id = 4242;
    sent.Item = EnchantedDagger();
    sent.Item.Count = 3;
    sent.CellId = GameId(0, 0x16BDB);
    sent.Position = glm::vec3(1.f, 2.f, 3.f);
    sent.Rotation = glm::vec3(0.f, 0.f, 3.14159f);

    Buffer buffer(1 << 12);
    Buffer::Writer writer(&buffer);
    sent.SerializeRaw(writer);
    Buffer::Reader reader(&buffer);
    NotifyDroppedItem back{};
    back.DeserializeRaw(reader);

    REQUIRE(back.Id == 4242);
    REQUIRE(back.Item == sent.Item);
    REQUIRE(back.Item.ExtraEnchantId == GameId(0, 0x4605A));
    REQUIRE(back.WorldSpaceId == GameId{}); // an interior: no worldspace
    REQUIRE(back.Rotation == sent.Rotation);
}

TEST_CASE("a pick-up names the item by id alone", "[droppeditems]")
{
    for (const uint32_t id : {1u, 300u, 0xFFFFFFFFu})
    {
        RequestDroppedItemRemove request{};
        request.Id = id;
        Buffer buffer(64);
        Buffer::Writer writer(&buffer);
        request.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        RequestDroppedItemRemove back{};
        back.DeserializeRaw(reader);
        REQUIRE(back.Id == id);

        NotifyDroppedItemRemoved notify{};
        notify.Id = id;
        Buffer buffer2(64);
        Buffer::Writer writer2(&buffer2);
        notify.SerializeRaw(writer2);
        Buffer::Reader reader2(&buffer2);
        NotifyDroppedItemRemoved back2{};
        back2.DeserializeRaw(reader2);
        REQUIRE(back2.Id == id);
    }
}

TEST_CASE("a move keeps its turn exactly and says whether the item was put down", "[droppeditems]")
{
    // The turn goes as three floats, not packed like the position: a packed angle loses a few degrees, which an item
    // lying on a table shows at once.
    for (const bool atRest : {false, true})
    {
        RequestDroppedItemMove request{};
        request.Id = 77;
        request.Position = glm::vec3(-5120.f, 20480.5f, 300.25f);
        request.Rotation = glm::vec3(0.0123f, 3.0f, 6.28f);
        request.AtRest = atRest;

        Buffer buffer(256);
        Buffer::Writer writer(&buffer);
        request.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        RequestDroppedItemMove back{};
        back.DeserializeRaw(reader);

        INFO("at rest " << atRest);
        REQUIRE(back.Id == 77);
        REQUIRE(back.Position == request.Position);
        REQUIRE(back.Rotation == request.Rotation);
        REQUIRE(back.AtRest == atRest);

        NotifyDroppedItemMove notify{};
        notify.Id = 0xFFFFFFFFu;
        notify.Position = request.Position;
        notify.Rotation = request.Rotation;
        notify.AtRest = atRest;

        Buffer buffer2(256);
        Buffer::Writer writer2(&buffer2);
        notify.SerializeRaw(writer2);
        Buffer::Reader reader2(&buffer2);
        NotifyDroppedItemMove back2{};
        back2.DeserializeRaw(reader2);

        REQUIRE(back2.Id == 0xFFFFFFFFu);
        REQUIRE(back2.Position == notify.Position);
        REQUIRE(back2.Rotation == notify.Rotation);
        REQUIRE(back2.AtRest == atRest);
    }
}
