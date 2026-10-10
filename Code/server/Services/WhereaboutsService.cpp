#include <Services/WhereaboutsService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>
#include <Events/UpdateEvent.h>

#include <Messages/NotifyPlayerWhereabouts.h>

namespace
{
constexpr auto kPeriod = std::chrono::seconds(2);
} // namespace

WhereaboutsService::WhereaboutsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&WhereaboutsService::OnUpdate>(this))
{
}

void WhereaboutsService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < m_nextSend)
        return;
    m_nextSend = cNow + kPeriod;

    auto& players = m_world.GetPlayerManager();
    if (players.Count() < 2)
        return;

    NotifyPlayerWhereabouts notify{};
    for (Player* pPlayer : players)
    {
        const auto cCharacter = pPlayer->GetCharacter();
        if (!cCharacter)
            continue;

        const auto* pCell = m_world.try_get<CellIdComponent>(*cCharacter);
        const auto* pMovement = m_world.try_get<MovementComponent>(*cCharacter);
        if (!pCell || !pMovement)
            continue;

        NotifyPlayerWhereabouts::Entry entry{};
        entry.PlayerId = pPlayer->GetId();
        entry.WorldSpaceId = pCell->WorldSpaceId;
        entry.CellId = pCell->Cell;
        entry.Position = pMovement->Position;
        entry.Place = pPlayer->GetPlace();
        notify.Players.push_back(std::move(entry));
    }

    if (notify.Players.size() < 2)
        return;

    // Unreliable: the next one is two seconds away, and a lost one is only a marker two seconds late.
    for (Player* pPlayer : players)
        pPlayer->SendUnreliable(notify);
}
