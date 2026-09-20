#pragma once

#include <array>
#include <atomic>
#include <chrono>

//! Per-frame profiling for stutter hunting. Sections time themselves with PerfScope, and World::Update
//! logs the slowest one when a frame spikes, to tell mod hitches from game hitches.
struct PerfFrame
{
    const char* SlowestSection = nullptr;
    double SlowestMs = 0.0;

    static PerfFrame& Get() noexcept
    {
        static PerfFrame s_frame;
        return s_frame;
    }

    void Record(const char* acpName, double aMs) noexcept
    {
        if (aMs > SlowestMs)
        {
            SlowestMs = aMs;
            SlowestSection = acpName;
        }
    }

    void Reset() noexcept
    {
        SlowestSection = nullptr;
        SlowestMs = 0.0;
    }
};

//! Main thread only.
struct PerfScope
{
    explicit PerfScope(const char* acpName) noexcept
        : m_pName(acpName)
        , m_start(std::chrono::steady_clock::now())
    {
    }

    ~PerfScope() noexcept
    {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
        PerfFrame::Get().Record(m_pName, ms);
    }

    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;

private:
    const char* m_pName;
    std::chrono::steady_clock::time_point m_start;
};

//! Time spent in game-side work the mod triggers or hooks. Unlike PerfScope these can run on game worker threads
//! (the animation update), so they only add to atomic totals. World::Update reports them every 30 s.
enum class PerfCounter : uint8_t
{
    kVRPoseApply,    // remote VR pose written into the skeleton (animation update hook)
    kInventoryApply, // full inventory rebuild of a spawned or respawned actor
    kActorSpawn,     // creating a remote actor
    kEquipmentSnapshot, // the once-a-second read of the local player's worn items (large modlist inventories)
    kCount
};

struct PerfCounters
{
    struct Slot
    {
        std::atomic<uint64_t> Nanoseconds{0};
        std::atomic<uint32_t> Calls{0};
    };

    static std::array<Slot, static_cast<size_t>(PerfCounter::kCount)>& Get() noexcept
    {
        static std::array<Slot, static_cast<size_t>(PerfCounter::kCount)> s_slots;
        return s_slots;
    }
};

struct PerfCounterScope
{
    explicit PerfCounterScope(PerfCounter aCounter) noexcept
        : m_counter(aCounter)
        , m_start(std::chrono::steady_clock::now())
    {
    }

    ~PerfCounterScope() noexcept
    {
        auto& slot = PerfCounters::Get()[static_cast<size_t>(m_counter)];
        slot.Nanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
        ++slot.Calls;
    }

    PerfCounterScope(const PerfCounterScope&) = delete;
    PerfCounterScope& operator=(const PerfCounterScope&) = delete;

private:
    PerfCounter m_counter;
    std::chrono::steady_clock::time_point m_start;
};
