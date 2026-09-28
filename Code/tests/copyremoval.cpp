#include <catch2/catch.hpp>

#include <CopyRemovalPolicy.h>

#include <deque>

using namespace CopyRemovalPolicy;

// The property that matters: a remote copy held back for safety must never be able to accumulate.
//
// Twice on 2026-09-27 a change to this decision filled Emma's world with live copies and collapsed her frame
// rate in VR, because "wait until the game releases it" has no bound when the game never releases it. These
// tests exist so the bound is checked by a machine in under a second instead of by her losing an evening.

TEST_CASE("a copy nothing holds is freed at once", "[copyremoval]")
{
    REQUIRE(OnRemoved(0, 0) == Action::DeleteNow);
    REQUIRE(OnRemoved(0, kMaxWaiting - 1) == Action::DeleteNow);
    REQUIRE(OnRemoved(0, kMaxWaiting) == Action::DeleteNow);
}

TEST_CASE("a copy something still holds waits, while there is room", "[copyremoval]")
{
    REQUIRE(OnRemoved(3, 0) == Action::WaitDisabled);
    REQUIRE(OnRemoved(1, kMaxWaiting - 1) == Action::WaitDisabled);
}

TEST_CASE("at the limit it is freed anyway, holders or not", "[copyremoval]")
{
    REQUIRE(OnRemoved(3, kMaxWaiting) == Action::DeleteNow);
    REQUIRE(OnRemoved(999, kMaxWaiting + 50) == Action::DeleteNow);
}

TEST_CASE("the waiting list cannot grow past the bound, however the game behaves", "[copyremoval]")
{
    // The exact failure of 2026-09-27: the handle count never reaches zero because the holders are the game's
    // own lists. Five hundred removals in a row, none of them ever released.
    std::deque<int> waiting;
    for (int i = 0; i < 500; ++i)
    {
        if (OnRemoved(3, waiting.size()) == Action::WaitDisabled)
            waiting.push_back(i);

        // Nothing is ever released by the game, so the only way out is the bound.
        REQUIRE(waiting.size() <= kMaxWaiting);
    }

    REQUIRE(waiting.size() == kMaxWaiting);
}

TEST_CASE("a waiting copy is released once its handles clear", "[copyremoval]")
{
    REQUIRE(ShouldRelease(0, 10, false));
    REQUIRE_FALSE(ShouldRelease(2, 10, false));
}

TEST_CASE("a waiting copy gives up eventually", "[copyremoval]")
{
    REQUIRE_FALSE(ShouldRelease(2, kMaxWaitMs - 1, false));
    REQUIRE(ShouldRelease(2, kMaxWaitMs, false));
}

TEST_CASE("the oldest go when the list is over its bound", "[copyremoval]")
{
    REQUIRE(ShouldRelease(5, 0, true));
}
