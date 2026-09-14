#include <TiltedOnlinePCH.h>

#ifdef SKYRIMVR

#include <Services/VRConnectService.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>

#include <Games/TES.h>
#include <PlayerCharacter.h>
#include <ScriptExtender.h>
#include <Utils.h>
#include <World.h>

#include <fstream>

namespace
{
constexpr auto kAutoConnectDelay = 5s;    // after a save is loaded, so the cell has settled
constexpr auto kAttemptTimeout = 30s;     // an attempt that never reports back is abandoned
constexpr std::array<std::chrono::seconds, 5> kRetryDelays{5s, 10s, 20s, 30s, 60s};

// Spaces and Notepad's UTF-8 byte order mark make the address unresolvable.
void Trim(std::string& aText)
{
    if (aText.rfind("\xEF\xBB\xBF", 0) == 0)
        aText.erase(0, 3);

    const auto first = aText.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        aText.clear();
        return;
    }
    aText.erase(aText.find_last_not_of(" \t\r\n") + 1);
    aText.erase(0, first);
}
} // namespace

bool VRConnectService::LoadConfig(Config& aConfig) noexcept
{
    char localAppData[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", localAppData, sizeof(localAppData)))
        return false;

    std::ifstream file(std::string(localAppData) + "\\SkyrimTogetherVR\\connect.txt");
    std::string address;
    if (!file || !std::getline(file, address))
        return false;

    Trim(address);
    if (address.empty())
        return false;

    std::string password;
    if (std::getline(file, password))
        Trim(password);

    aConfig.Address = address.c_str();
    aConfig.Password = password.c_str();
    return true;
}

VRConnectService::VRConnectService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&VRConnectService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&VRConnectService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&VRConnectService::OnDisconnected>(this);
    m_connectionErrorConnection = aDispatcher.sink<ConnectionErrorEvent>().connect<&VRConnectService::OnConnectionError>(this);
}

void VRConnectService::Toggle() noexcept
{
    if (m_state == State::kOnline || m_state == State::kConnecting)
    {
        // Set before closing: Close() reports the disconnection synchronously, and it must not schedule a retry.
        m_state = State::kIdle;
        m_transport.Close();
        Utils::ShowHudMessage("Skyrim Together: disconnected (F6 to connect again)");
        return;
    }

    m_failedAttempts = 0;
    StartAttempt();
}

bool VRConnectService::CheckInstall() noexcept
{
    // Problems that otherwise only show up as a refused connection or broken mods, reported in the headset once a
    // save is loaded. Only a setting the server refuses stops the automatic connection.
    bool ok = true;

    if (!IsScriptExtenderLoaded())
    {
        spdlog::error("VRConnectService: SKSE VR is not loaded");
        Utils::ShowHudMessage("Skyrim Together: SKSE VR is not loaded, check the SKSE VR install");
    }

    auto* pGrids = INISettingCollection::Get()->GetSetting("uGridsToLoad:General");
    if (pGrids && pGrids->data != 5)
    {
        spdlog::error("VRConnectService: uGridsToLoad is {}, the server requires 5", pGrids->data);
        Utils::ShowHudMessage("Skyrim Together: set uGridsToLoad=5 in SkyrimPrefs.ini, the server refuses other values");
        ok = false;
    }

    return ok;
}

bool VRConnectService::IsInGame() noexcept
{
    // Connecting reads the player's cell, which only exists once a save is loaded.
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer && pPlayer->parentCell && pPlayer->GetNiNode();
}

void VRConnectService::StartAttempt() noexcept
{
    if (!IsInGame())
    {
        Utils::ShowHudMessage("Skyrim Together: load a save before connecting");
        m_state = State::kIdle;
        return;
    }

    Config config;
    if (!LoadConfig(config))
    {
        spdlog::warn("VRConnectService: no server in %LOCALAPPDATA%\\SkyrimTogetherVR\\connect.txt");
        Utils::ShowHudMessage("Skyrim Together: no server set, write it in %LOCALAPPDATA%\\SkyrimTogetherVR\\connect.txt");
        m_state = State::kIdle;
        return;
    }

    spdlog::info("VRConnectService: connecting to {} (attempt {})", config.Address.c_str(), m_failedAttempts + 1);
    Utils::ShowHudMessage(TiltedPhoques::String("Skyrim Together: connecting to ") + config.Address);

    m_state = State::kConnecting;
    m_attemptStarted = std::chrono::steady_clock::now();
    m_transport.SetServerPassword(config.Password.c_str());
    m_transport.Connect(std::string(config.Address.c_str()));
}

void VRConnectService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();

    if (!IsInGame())
    {
        m_inGameSince = {};
        return;
    }
    if (m_inGameSince == std::chrono::steady_clock::time_point{})
        m_inGameSince = now;

    switch (m_state)
    {
    case State::kIdle:
        if (m_autoConnectDone || now - m_inGameSince < kAutoConnectDelay)
            return;

        m_autoConnectDone = true;
        if (!CheckInstall())
            return;
        {
            Config config;
            if (!LoadConfig(config))
            {
                Utils::ShowHudMessage("Skyrim Together: no server set, run setup-connect.bat in the Skyrim Together VR folder");
                return;
            }
        }
        StartAttempt();
        break;

    case State::kConnecting:
        if (now - m_attemptStarted > kAttemptTimeout)
        {
            spdlog::warn("VRConnectService: connection attempt timed out");
            m_transport.Close(); // reports a disconnection, which schedules the retry
            if (m_state == State::kConnecting)
                OnDisconnected({});
        }
        break;

    case State::kWaiting:
        if (now >= m_nextAttempt)
            StartAttempt();
        break;

    case State::kOnline: break;
    }
}

void VRConnectService::OnConnected(const ConnectedEvent&) noexcept
{
    m_state = State::kOnline;
    m_failedAttempts = 0;
    m_autoConnectDone = true;
}

void VRConnectService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // Idle: disconnected on purpose. Waiting: a retry is already scheduled (a cancelled attempt reports late).
    if (m_state == State::kIdle || m_state == State::kWaiting)
        return;

    const bool wasOnline = m_state == State::kOnline;
    const auto delay = kRetryDelays[std::min<size_t>(m_failedAttempts, kRetryDelays.size() - 1)];
    ++m_failedAttempts;

    m_state = State::kWaiting;
    m_nextAttempt = std::chrono::steady_clock::now() + delay;

    const auto message = fmt::format("Skyrim Together: {}, trying again in {} s", wasOnline ? "connection lost" : "server not reachable", delay.count());
    spdlog::info("VRConnectService: {}", message);
    Utils::ShowHudMessage(message.c_str());
}

void VRConnectService::OnConnectionError(const ConnectionErrorEvent&) noexcept
{
    // A refusal arrives while connecting, before the connected event (the reason is shown by OverlayService). The
    // same request would be refused again, so stop until the player presses F6. Errors raised once online (the
    // uGridsToLoad check) don't end the session and must not turn the reconnection off.
    if (m_state != State::kConnecting)
        return;

    spdlog::info("VRConnectService: connection refused by the server, not retrying");
    m_state = State::kIdle;
}

#endif
