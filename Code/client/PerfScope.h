#pragma once

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
