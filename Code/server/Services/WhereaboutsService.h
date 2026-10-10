#pragma once

struct World;
struct UpdateEvent;

/**
 * @brief Every two seconds, tells every player where every player is (NotifyPlayerWhereabouts), for the markers on
 * the map and the compass. Their games already know who is in their party.
 */
struct WhereaboutsService
{
    WhereaboutsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~WhereaboutsService() noexcept = default;

    TP_NOCOPYMOVE(WhereaboutsService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;

private:
    World& m_world;
    std::chrono::steady_clock::time_point m_nextSend{};

    entt::scoped_connection m_updateConnection;
};
