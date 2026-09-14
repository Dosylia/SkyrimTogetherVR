#include <TiltedOnlinePCH.h>

#include "World.h"
#include <PerfScope.h>

#include <Services/DiscoveryService.h>
#include <Services/InputService.h>
#include <Services/TransportService.h>
#include <Services/RunnerService.h>
#include <Services/ImguiService.h>
#include <Services/PapyrusService.h>
#include <Services/DiscordService.h>
#include <Services/ObjectService.h>
#include <Services/QuestService.h>
#include <Services/ActorValueService.h>
#include <Services/InventoryService.h>
#include <Services/MagicService.h>
#include <Services/CommandService.h>
#include <Services/CalendarService.h>
#include <Services/StringCacheService.h>
#include <Services/PlayerService.h>
#include <Services/CombatService.h>
#include <Services/WeatherService.h>
#include <Services/MapService.h>
#include <Services/VRConnectService.h>

#include <Events/PreUpdateEvent.h>
#include <Events/UpdateEvent.h>

#include <ModCompat/BehaviorVar.h>  

World::World()
    : m_runner(m_dispatcher)
    , m_transport(*this, m_dispatcher)
    , m_modSystem(m_dispatcher)
    , m_lastFrameTime{std::chrono::high_resolution_clock::now()}
{
    ctx().emplace<ImguiService>();
    ctx().emplace<DiscoveryService>(*this, m_dispatcher);
    ctx().emplace<OverlayService>(*this, m_transport, m_dispatcher);
    ctx().emplace<InputService>(ctx().at<OverlayService>());
    ctx().emplace<CharacterService>(*this, m_dispatcher, m_transport);
    ctx().emplace<DebugService>(m_dispatcher, *this, m_transport, ctx().at<ImguiService>());
    ctx().emplace<PapyrusService>(m_dispatcher);
    ctx().emplace<DiscordService>(m_dispatcher);
    ctx().emplace<ObjectService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CalendarService>(*this, m_dispatcher, m_transport);
    ctx().emplace<QuestService>(*this, m_dispatcher);
    ctx().emplace<PartyService>(*this, m_dispatcher, m_transport);
    ctx().emplace<ActorValueService>(*this, m_dispatcher, m_transport);
    ctx().emplace<InventoryService>(*this, m_dispatcher, m_transport);
    ctx().emplace<MagicService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CommandService>(*this, m_transport, m_dispatcher);
    ctx().emplace<PlayerService>(*this, m_dispatcher, m_transport);
    ctx().emplace<StringCacheService>(m_dispatcher);
    ctx().emplace<CombatService>(*this, m_transport, m_dispatcher);
    ctx().emplace<WeatherService>(*this, m_transport, m_dispatcher);
    ctx().emplace<MapService>(*this, m_dispatcher, m_transport);
#ifdef SKYRIMVR
    ctx().emplace<VRConnectService>(*this, m_dispatcher, m_transport);
#endif

    BehaviorVar::Get()->Init();
}

World::~World() = default;

void World::Update() noexcept
{
    const auto cNow = std::chrono::high_resolution_clock::now();
    const auto cDelta = cNow - m_lastFrameTime;
    m_lastFrameTime = cNow;

    const auto cDeltaSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(cDelta).count();

    PerfFrame::Get().Reset();
    const auto cUpdateStart = std::chrono::steady_clock::now();

    m_dispatcher.trigger(PreUpdateEvent(cDeltaSeconds));

    // Force run this before so we get the tasks scheduled to run
    {
        PerfScope perfScope("RunnerService tasks");
        m_runner.OnUpdate(UpdateEvent(cDeltaSeconds));
    }
    m_dispatcher.trigger(UpdateEvent(cDeltaSeconds));

    const double frameMs = cDeltaSeconds * 1000.0;
    const double updateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cUpdateStart).count();

    // A slow mod update is ours to fix, so it is reported as it happens (at most once a second). Slow frames in
    // general are summed up in ReportPerformance instead of one line per spike.
    if (updateMs > 5.0)
    {
        static std::chrono::steady_clock::time_point s_lastReport;
        const auto now = std::chrono::steady_clock::now();
        if (now - s_lastReport >= std::chrono::seconds(1))
        {
            const auto& perf = PerfFrame::Get();
            spdlog::warn("Mod update took {:.1f} ms, slowest section {} {:.1f} ms", updateMs, perf.SlowestSection ? perf.SlowestSection : "none", perf.SlowestMs);
            s_lastReport = now;
        }
    }

    ReportPerformance(frameMs, updateMs);
}

void World::ReportPerformance(double aFrameMs, double aUpdateMs) noexcept
{
    constexpr auto kInterval = std::chrono::seconds(30);

    // Frame gaps over 2 s are loading screens.
    if (aFrameMs < 2000.0)
    {
        m_perfFrameTimes.push_back(static_cast<float>(aFrameMs));
        m_perfUpdateTotalMs += aUpdateMs;
        m_perfUpdateMaxMs = std::max(m_perfUpdateMaxMs, aUpdateMs);
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_perfIntervalStart == std::chrono::steady_clock::time_point{})
        m_perfIntervalStart = now;
    if (now - m_perfIntervalStart < kInterval || m_perfFrameTimes.empty())
        return;

    const size_t frames = m_perfFrameTimes.size();
    double totalMs = 0.0;
    size_t over50 = 0;
    for (float ms : m_perfFrameTimes)
    {
        totalMs += ms;
        over50 += ms > 50.f;
    }
    std::sort(m_perfFrameTimes.begin(), m_perfFrameTimes.end());
    const auto percentile = [this, frames](double aFraction) { return m_perfFrameTimes[std::min(frames - 1, static_cast<size_t>(frames * aFraction))]; };
    const double averageMs = totalMs / frames;

    std::string hooks;
    constexpr std::array<const char*, static_cast<size_t>(PerfCounter::kCount)> kNames{"VR pose", "inventory apply", "actor spawn"};
    auto& slots = PerfCounters::Get();
    for (size_t i = 0; i < slots.size(); ++i)
    {
        const uint64_t nanoseconds = slots[i].Nanoseconds.exchange(0);
        const uint32_t calls = slots[i].Calls.exchange(0);
        if (calls)
            hooks += fmt::format(", {} {:.2f} ms/frame ({} calls)", kNames[i], nanoseconds / 1e6 / frames, calls);
    }

    spdlog::info("Perf last {} s: {} frames, avg {:.1f} ms ({:.0f} fps), p95 {:.1f} ms, p99 {:.1f} ms, {} frames over 50 ms; mod update avg {:.2f} ms, max {:.1f} ms{}",
                 std::chrono::duration_cast<std::chrono::seconds>(now - m_perfIntervalStart).count(), frames, averageMs, 1000.0 / averageMs, percentile(0.95),
                 percentile(0.99), over50, m_perfUpdateTotalMs / frames, m_perfUpdateMaxMs, hooks);

    m_perfFrameTimes.clear();
    m_perfUpdateTotalMs = 0.0;
    m_perfUpdateMaxMs = 0.0;
    m_perfIntervalStart = now;
}

RunnerService& World::GetRunner() noexcept
{
    return m_runner;
}

TransportService& World::GetTransport() noexcept
{
    return m_transport;
}

ModSystem& World::GetModSystem() noexcept
{
    return m_modSystem;
}

uint64_t World::GetTick() const noexcept
{
    return m_transport.GetClock().GetCurrentTick();
}

void World::Create() noexcept
{
    if (!entt::locator<World>::has_value())
    {
        entt::locator<World>::emplace();
    }
}

World& World::Get() noexcept
{
    return entt::locator<World>::value();
}
