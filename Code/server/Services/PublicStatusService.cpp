#include <Services/PublicStatusService.h>

#include <Events/UpdateEvent.h>
#include <Game/Player.h>
#include <GameServer.h>
#include <World.h>

#include <console/Setting.h>

#include <cmath>
#include <ctime>

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

extern Console::Setting<uint32_t> uMaxPlayerCount;

namespace
{
Console::Setting bPublicStatus{"LiveServices:bPublicStatus", "Publish this server's status (name, player count, character names and where they are) on the urSovngarde website's public server page. Needs the URSOVNGARDE_SERVER_KEY environment variable", false};
Console::StringSetting sPublicAddress{"LiveServices:sPublicAddress", "The address players type to join this server (host:port), shown on the public server page", ""};
Console::StringSetting sPublicStatusUrl{"LiveServices:sPublicStatusUrl", "Where the public server's status is sent", "https://ursovngarde-hub.ursovngarde.workers.dev/servers/public/status"};

constexpr char kKeyVariable[] = "URSOVNGARDE_SERVER_KEY";

// The hub and the website count a status older than 180 seconds as an offline server. Ten seconds while someone plays
// keeps the map moving; a minute when nobody is on keeps the hub's free plan for everything else (Emma, 2026-10-10).
constexpr auto kBusyInterval = std::chrono::seconds(10);
constexpr auto kIdleInterval = std::chrono::seconds(60);
// Someone joining or leaving is pushed at once, but never sooner than this after the previous push.
constexpr auto kMinimumGap = std::chrono::seconds(2);
constexpr auto kTimeout = std::chrono::seconds(5);
// The "online": false push at shutdown: short, the server is closing.
constexpr auto kLastTimeout = std::chrono::seconds(2);
constexpr auto kFailureLogInterval = std::chrono::minutes(5);

// The website shows at most 64 players and the hub takes at most 16 KB; player_count still counts everyone.
constexpr size_t kMaxListedPlayers = 64;
constexpr size_t kMaxNameLength = 40;

// Tamriel, the only worldspace the page's map draws: Skyrim.esm's 0x3C.
constexpr uint32_t kTamrielBaseId = 0x3C;

void AppendJsonString(std::string& aOut, std::string_view aText)
{
    aOut += '"';
    for (const unsigned char c : aText)
    {
        switch (c)
        {
        case '"': aOut += "\\\""; break;
        case '\\': aOut += "\\\\"; break;
        case '\n': aOut += "\\n"; break;
        case '\r': aOut += "\\r"; break;
        case '\t': aOut += "\\t"; break;
        default:
            if (c < 0x20)
                aOut += fmt::format("\\u{:04x}", c);
            else
                aOut += static_cast<char>(c);
        }
    }
    aOut += '"';
}

std::string IsoTime(std::chrono::system_clock::time_point aTime)
{
    const std::time_t time = std::chrono::system_clock::to_time_t(aTime);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string EnvironmentVariable(const char* acpName)
{
#ifdef _WIN32
    char* pValue = nullptr;
    size_t length = 0;
    std::string value;
    if (_dupenv_s(&pValue, &length, acpName) == 0 && pValue)
        value = pValue;
    free(pValue);
    return value;
#else
    const char* pValue = std::getenv(acpName);
    return pValue ? pValue : "";
#endif
}

bool SameFilename(std::string_view aLeft, std::string_view aRight)
{
    return std::ranges::equal(aLeft, aRight, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

// A worldspace id is in this server's mod ids, which are handed out as clients join, so Skyrim.esm's id is looked up.
bool IsTamriel(World& aWorld, const GameId& acWorldSpaceId)
{
    if (acWorldSpaceId.BaseId != kTamrielBaseId)
        return false;

    for (const auto& entry : aWorld.ctx().at<ModsComponent>().GetStandardMods())
    {
        if (SameFilename({entry.first.c_str(), entry.first.size()}, "Skyrim.esm"))
            return entry.second.id == acWorldSpaceId.ModId;
    }
    return false;
}

// "https://host/path" as httplib wants it: the scheme and host for the client, the path for the request.
bool SplitUrl(std::string_view aUrl, std::string& aOrigin, std::string& aPath)
{
    const auto scheme = aUrl.find("://");
    if (scheme == std::string_view::npos)
        return false;
    const auto path = aUrl.find('/', scheme + 3);
    aOrigin = std::string(aUrl.substr(0, path));
    aPath = path == std::string_view::npos ? "/" : std::string(aUrl.substr(path));
    return aOrigin.size() > scheme + 3;
}
} // namespace

PublicStatusService::PublicStatusService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&PublicStatusService::OnUpdate>(this))
    , m_random(std::random_device{}())
{
    if (!bPublicStatus)
        return;

    // From the environment, never from STServer.ini: the ini gets copied around with the server folder.
    m_key = EnvironmentVariable(kKeyVariable);
    if (m_key.empty())
    {
        spdlog::error("bPublicStatus is on but the {} environment variable is not set: this server's status is not published", kKeyVariable);
        return;
    }

    if (!SplitUrl(sPublicStatusUrl.value(), m_origin, m_path))
    {
        spdlog::error("sPublicStatusUrl '{}' is not a URL: this server's status is not published", sPublicStatusUrl.value());
        return;
    }

    if (std::string_view(sPublicAddress.value()).empty())
        spdlog::warn("bPublicStatus is on but sPublicAddress is empty: the public server page will show no address to join");

    m_startedAt = IsoTime(std::chrono::system_clock::now());
    m_enabled = true;
    m_thread = std::thread(&PublicStatusService::Run, this);

    spdlog::info("Publishing this server's status (name, player count, character names and where they are) to {}, every {} s while players are on, every {} s when nobody is",
                 m_origin, kBusyInterval.count(), kIdleInterval.count());
}

PublicStatusService::~PublicStatusService() noexcept
{
    if (!m_thread.joinable())
        return;

    {
        std::scoped_lock lock(m_mutex);
        // Only once a status went out: m_header holds what it said.
        if (!m_header.empty())
            m_pending = BuildOfflineStatus();
        m_stop = true;
    }
    m_wake.notify_one();
    m_thread.join();
}

void PublicStatusService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    if (!m_enabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    const uint32_t playerCount = m_world.GetPlayerManager().Count();

    // Someone joining or leaving shows on the page at once rather than at the next push.
    if (playerCount != m_lastPlayerCount)
    {
        m_lastPlayerCount = playerCount;
        m_nextPush = std::min(m_nextPush, m_lastPush + kMinimumGap);
    }

    if (now < m_nextPush)
        return;

    m_lastPush = now;
    m_nextPush = now + (playerCount > 0 ? kBusyInterval : kIdleInterval);
    Queue(BuildStatus());
}

std::string PublicStatusService::BuildStatus() noexcept
{
    const auto* pServer = GameServer::Get();

    std::string header = "\"name\":";
    AppendJsonString(header, {pServer->GetInfo().name.c_str(), pServer->GetInfo().name.size()});
    header += ",\"address\":";
    AppendJsonString(header, sPublicAddress.value());
    header += fmt::format(",\"version\":\"{}.{}.{}\",\"protocol\":", BUILD_MAJOR, BUILD_MINOR, BUILD_PATCH);
    AppendJsonString(header, BUILD_PROTOCOL);
    header += fmt::format(",\"password\":{},\"max_players\":{},\"started_at\":\"{}\"", pServer->IsPasswordProtected() ? "true" : "false",
                          uMaxPlayerCount.value_as<uint32_t>(), m_startedAt);

    std::string players;
    size_t listed = 0;
    uint32_t playerCount = 0;
    TiltedPhoques::Set<uint32_t> present;

    m_world.GetPlayerManager().ForEach([&](Player* pPlayer) {
        ++playerCount;
        present.insert(pPlayer->GetId());
        if (listed >= kMaxListedPlayers)
            return;
        ++listed;

        if (!players.empty())
            players += ',';

        // The character's name, as the client sent it at connect (TransportService::OnConnected). Never the endpoint,
        // the Discord id or anything else that tells who the person is.
        const auto& username = pPlayer->GetUsername();
        players += "{\"id\":\"" + TokenOf(pPlayer->GetId()) + "\",\"name\":";
        AppendJsonString(players, std::string_view(username.c_str(), username.size()).substr(0, kMaxNameLength));

        // A position only outdoors in Tamriel: anywhere else the page lists the player without a dot on its map.
        const auto& cell = pPlayer->GetCellComponent();
        const auto character = pPlayer->GetCharacter();
        if (character && m_world.valid(*character) && cell.WorldSpaceId && IsTamriel(m_world, cell.WorldSpaceId))
        {
            if (const auto* pMovement = m_world.try_get<MovementComponent>(*character))
            {
                // Rotation.z is the game's heading in radians: 0 faces north, and it turns clockwise.
                const long heading = std::lround(glm::degrees(pMovement->Rotation.z));
                players += fmt::format(",\"worldspace\":\"Tamriel\",\"x\":{},\"y\":{},\"heading\":{}", std::lround(pMovement->Position.x),
                                       std::lround(pMovement->Position.y), ((heading % 360) + 360) % 360);
            }
        }
        players += '}';
    });

    for (auto itor = m_tokens.begin(); itor != m_tokens.end();)
    {
        if (present.count(itor->first) != 0)
            ++itor;
        else
            itor = m_tokens.erase(itor);
    }

    m_header = header;

    return fmt::format("{{\"online\":true,{},\"player_count\":{},\"server_time\":\"{}\",\"players\":[{}]}}", header, playerCount,
                       IsoTime(std::chrono::system_clock::now()), players);
}

std::string PublicStatusService::BuildOfflineStatus() const noexcept
{
    return fmt::format("{{\"online\":false,{},\"player_count\":0,\"server_time\":\"{}\",\"players\":[]}}", m_header, IsoTime(std::chrono::system_clock::now()));
}

const std::string& PublicStatusService::TokenOf(uint32_t aPlayerId) noexcept
{
    auto itor = m_tokens.find(aPlayerId);
    if (itor == m_tokens.end())
        itor = m_tokens.emplace(aPlayerId, fmt::format("{:08x}", static_cast<uint32_t>(m_random()))).first;
    return itor->second;
}

void PublicStatusService::Queue(std::string aBody) noexcept
{
    {
        std::scoped_lock lock(m_mutex);
        // A status still waiting is replaced: only the newest is worth sending.
        m_pending = std::move(aBody);
    }
    m_wake.notify_one();
}

void PublicStatusService::Run() noexcept
{
    std::unique_lock lock(m_mutex);
    while (true)
    {
        m_wake.wait(lock, [this] { return m_pending.has_value() || m_stop; });
        if (!m_pending)
            return;

        const std::string body = std::move(*m_pending);
        m_pending.reset();
        // Once m_stop is set, what is pending is the shutdown's "online": false.
        const bool last = m_stop;

        lock.unlock();
        Post(body, last ? kLastTimeout : kTimeout);
        lock.lock();

        if (last)
            return;
    }
}

bool PublicStatusService::Post(const std::string& acBody, std::chrono::seconds aTimeout) noexcept
{
    httplib::Client client(m_origin);
    client.set_connection_timeout(aTimeout);
    client.set_read_timeout(aTimeout);
    client.set_write_timeout(aTimeout);
    // The certificate is checked, unlike ServerListService's announce: this request carries the key.

    const httplib::Headers headers{{"Authorization", "Bearer " + m_key}};
    const auto response = client.Post(m_path, headers, acBody, "application/json");

    if (response && response->status >= 200 && response->status < 300)
    {
        if (!m_confirmed)
            spdlog::info("Public status: the hub took this server's status");
        else if (m_failing)
            spdlog::info("Public status: the hub is reachable again");
        m_confirmed = true;
        m_failing = false;
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!m_failing || now - m_lastFailureLog >= kFailureLogInterval)
    {
        if (!response)
            spdlog::warn("Public status: could not reach the hub ({}); trying again at the next push", httplib::to_string(response.error()));
        else if (response->status == 401)
            spdlog::error("Public status: the hub refused the key (401); check {} on this machine against the hub's SERVER_KEY", kKeyVariable);
        else
            spdlog::warn("Public status: the hub answered {} ({}); trying again at the next push", response->status, response->body.substr(0, 200));
        m_lastFailureLog = now;
    }
    m_failing = true;
    return false;
}
