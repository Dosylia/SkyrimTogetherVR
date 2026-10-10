#pragma once

#include <condition_variable>
#include <random>
#include <thread>

struct World;
struct UpdateEvent;

/**
 * @brief Pushes this server's status (name, player count, character names and where they are) to the urSovngarde hub,
 * which the website's public server page reads. Only with LiveServices:bPublicStatus=true in STServer.ini, so a
 * friend's private server never publishes anything.
 *
 * The status is built on the game thread and sent from a thread of its own: a slow or unreachable hub never holds up
 * the game. A push that fails is dropped; the next one carries the newer status anyway.
 */
struct PublicStatusService
{
    PublicStatusService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~PublicStatusService() noexcept;

    TP_NOCOPYMOVE(PublicStatusService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;

private:
    [[nodiscard]] std::string BuildStatus() noexcept;
    [[nodiscard]] std::string BuildOfflineStatus() const noexcept;
    [[nodiscard]] const std::string& TokenOf(uint32_t aPlayerId) noexcept;
    void Queue(std::string aBody) noexcept;
    void Run() noexcept;
    bool Post(const std::string& acBody, std::chrono::seconds aTimeout) noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;

    bool m_enabled{false};
    std::string m_key;
    std::string m_origin;
    std::string m_path;
    std::string m_startedAt;
    // The fields that do not change between pushes, as last sent, for the "online": false push at shutdown: by then
    // GameServer::Get() is already null. Game thread only.
    std::string m_header;
    std::chrono::steady_clock::time_point m_lastPush{};
    std::chrono::steady_clock::time_point m_nextPush{};
    uint32_t m_lastPlayerCount{0};
    // A random token per connection, so the page can move a marker instead of redrawing it. Not the player's id, which
    // counts up and so tells how many people joined before.
    TiltedPhoques::Map<uint32_t, std::string> m_tokens;
    std::mt19937 m_random;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<std::string> m_pending;
    bool m_stop{false};

    // Read and written only by the sending thread.
    bool m_confirmed{false};
    bool m_failing{false};
    std::chrono::steady_clock::time_point m_lastFailureLog{};
};
