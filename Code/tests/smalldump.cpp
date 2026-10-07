// The small crash dump (client/SmallDump.h), written of this test process: a real minidump, small enough for a
// report, and the folder pruned to the newest few.

#include <catch2/catch.hpp>

#include <SmallDump.h>

#include <atomic>
#include <fstream>
#include <thread>
#include <vector>

TEST_CASE("A small crash dump is a real minidump and stays small", "[smalldump]")
{
    const auto folder = std::filesystem::temp_directory_path() / L"tp-smalldump-test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    DWORD error = 0;
    const auto path = folder / L"crash_UTC_2026-10-07_08-00-00.small.dmp";
    const uint64_t size = SmallDump::Write(path, nullptr, nullptr, error);
    INFO("error " << error);
    REQUIRE(size > 0);
    // The launcher takes small dumps under 4 MB.
    CHECK(size < 4ull * 1024 * 1024);

    char signature[4]{};
    std::ifstream(path, std::ios::binary).read(signature, 4);
    CHECK(std::string(signature, 4) == "MDMP");

    SECTION("too big a dump is written again with only the crashing thread's stack")
    {
        // A few threads waiting, each with a stack of its own, as the game has a hundred.
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i)
            threads.emplace_back([&stop] {
                volatile char stack[64 * 1024];
                for (auto& c : stack)
                    c = 1;
                while (!stop)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const auto full = folder / L"full.small.dmp";
        const uint64_t fullSize = SmallDump::Write(full, nullptr, nullptr, error);
        static int s_restarts = 0;
        s_restarts = 0;
        const auto smaller = folder / L"smaller.small.dmp";
        // A target of one byte: always the second pass.
        const uint64_t smallerSize = SmallDump::Write(smaller, nullptr, nullptr, error, []() noexcept { ++s_restarts; }, 1);
        stop = true;
        for (auto& t : threads)
            t.join();

        INFO("no extra threads " << size << ", with eight " << fullSize << ", second pass " << smallerSize);
        CHECK(s_restarts == 2);
        REQUIRE(smallerSize > 0);
        // Eight 64 KB stacks left out (each at least 48 KB of it in the full dump).
        CHECK(smallerSize + 8 * 48 * 1024 < fullSize);
        char head[4]{};
        std::ifstream(smaller, std::ios::binary).read(head, 4);
        CHECK(std::string(head, 4) == "MDMP");
    }

    SECTION("only the newest three are kept, and nothing else is touched")
    {
        for (const auto* name : {L"crash_UTC_2026-10-01_08-00-00.small.dmp", L"crash_UTC_2026-10-02_08-00-00.small.dmp",
                                 L"crash_UTC_2026-10-03_08-00-00.small.dmp", L"crash_UTC_2026-10-04_08-00-00.dmp", L"tp_client.log"})
            std::ofstream(folder / name) << "x";
        SmallDump::Prune(folder);
        CHECK_FALSE(std::filesystem::exists(folder / L"crash_UTC_2026-10-01_08-00-00.small.dmp"));
        CHECK(std::filesystem::exists(folder / L"crash_UTC_2026-10-02_08-00-00.small.dmp"));
        CHECK(std::filesystem::exists(folder / L"crash_UTC_2026-10-03_08-00-00.small.dmp"));
        CHECK(std::filesystem::exists(path));
        CHECK(std::filesystem::exists(folder / L"crash_UTC_2026-10-04_08-00-00.dmp"));
        CHECK(std::filesystem::exists(folder / L"tp_client.log"));
    }
    std::filesystem::remove_all(folder);
}
