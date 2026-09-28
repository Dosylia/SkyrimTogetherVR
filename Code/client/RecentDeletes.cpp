#include <TiltedOnlinePCH.h>

#include <RecentDeletes.h>

#include <chrono>

namespace RecentDeletes
{
namespace
{
Entry s_ring[kCount]{};
std::atomic<uint32_t> s_next{0};
} // namespace

namespace
{
TiltedPhoques::String DescribeClaims(const uint32_t aFlags, const uint32_t aHandles)
{
    TiltedPhoques::String out;
    const auto add = [&out](const char* acpWhat)
    {
        if (!out.empty())
            out += ", ";
        out += acpWhat;
    };

    if (aFlags & kPlayerCombatTarget)
        add("the player is fighting it");
    if (aFlags & kOtherCombatTarget)
        add("another actor is fighting it");
    if (aFlags & kIsPlayerTeammate)
        add("it is a teammate");
    if (aFlags & kHasProcess)
        add("still in the AI process lists");
    if (aFlags & kHas3D)
        add("still has 3D");
    if (aFlags & kDeadOrDying)
        add("dead or dying");
    if (out.empty())
        add("nothing obvious");

    out += fmt::format(", {} outstanding handle(s)", aHandles).c_str();
    return out;
}
} // namespace

void Record(const void* apActor, const uint32_t aFormId, const uint32_t aHandles, const uint32_t aRefCount, const uint32_t aFlags) noexcept
{
    const uint32_t slot = s_next.fetch_add(1, std::memory_order_relaxed) % kCount;
    s_ring[slot].Address = reinterpret_cast<uint64_t>(apActor);
    s_ring[slot].FormId = aFormId;
    s_ring[slot].Handles = aHandles;
    s_ring[slot].RefCount = aRefCount;
    s_ring[slot].Flags = aFlags;
    s_ring[slot].Stamp = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    // Said as it happens, not only on a crash: an actor deleted while somebody still holds a handle to it is a
    // use-after-free waiting to be collected, whether or not this session is the one that collects it.
    if (aHandles > 0 || (aFlags & (kPlayerCombatTarget | kOtherCombatTarget)))
        spdlog::warn("DeleteClaim: deleting {:X} while something still holds it -- {}", aFormId, DescribeClaims(aFlags, aHandles));
}

void Report(const uint64_t* apRegisters, const size_t aRegisterCount) noexcept
{
    const uint64_t now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint32_t written = s_next.load(std::memory_order_relaxed);
    if (written == 0)
    {
        spdlog::error("RecentDeletes: nothing has been deleted this session.");
        return;
    }

    const uint32_t count = written < kCount ? written : kCount;
    spdlog::error("RecentDeletes: the last {} actors this client deleted, newest first, checked against every crashing register.", count);

    bool matched = false;
    for (uint32_t i = 1; i <= count; ++i)
    {
        const Entry& e = s_ring[(written - i) % kCount];
        if (!e.Address)
            continue;

        // steady_clock ticks are nanoseconds on MSVC; this only has to be readable, not exact.
        const uint64_t ago = now > e.Stamp ? (now - e.Stamp) / 1000000ull : 0ull;
        int inRegister = -1;
        for (size_t r = 0; r < aRegisterCount; ++r)
            if (apRegisters[r] == e.Address)
            {
                inRegister = static_cast<int>(r);
                break;
            }
        if (inRegister >= 0)
            matched = true;

        static const char* kNames[] = {"Rax", "Rcx", "Rdx", "Rbx", "Rsi", "Rdi", "Rbp", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
        spdlog::error("    {:#018x}  form {:X}  {} ms ago  [{}]{}", e.Address, e.FormId, ago, DescribeClaims(e.Flags, e.Handles),
                      inRegister >= 0 ? fmt::format("   <<<< STILL IN {}, THE CRASHING CODE WAS USING IT", kNames[inRegister]) : "");
    }

    if (!matched)
        spdlog::error("    none of them is in any crashing register: this crash is not our actor deletion.");
}
} // namespace RecentDeletes
