#include <Services/VoiceService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Messages/NotifyVoiceData.h>
#include <Messages/VoiceDataRequest.h>
#include <Messages/VoiceStateRequest.h>

namespace
{
// A little past the 30 m where the listener's game fades a voice out (1 m is about 70 game units), so a voice fades
// rather than stops when its speaker walks away.
constexpr float kHearingRange = 2400.f;
constexpr uint32_t kMaxPacketsPerSecond = 60;
} // namespace

VoiceService::VoiceService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_voiceStateConnection(aDispatcher.sink<PacketEvent<VoiceStateRequest>>().connect<&VoiceService::OnVoiceState>(this))
    , m_voiceDataConnection(aDispatcher.sink<PacketEvent<VoiceDataRequest>>().connect<&VoiceService::OnVoiceData>(this))
{
}

void VoiceService::OnVoiceState(const PacketEvent<VoiceStateRequest>& acMessage) const noexcept
{
    acMessage.pPlayer->SetVoiceEnabled(acMessage.Packet.Enabled);
    spdlog::info("Voice: {} turned voice {}", acMessage.pPlayer->GetUsername().c_str(), acMessage.Packet.Enabled ? "on" : "off");
}

void VoiceService::OnVoiceData(const PacketEvent<VoiceDataRequest>& acMessage) noexcept
{
    Player* pSpeaker = acMessage.pPlayer;
    const VoiceDataRequest& packet = acMessage.Packet;
    if (!pSpeaker->IsVoiceEnabled() || packet.Data.empty())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    Budget& budget = m_budgets[pSpeaker->GetId()];
    if (cNow - budget.Since >= std::chrono::seconds(1))
    {
        budget.Since = cNow;
        budget.Packets = 0;
    }
    if (++budget.Packets > kMaxPacketsPerSecond)
        return;

    NotifyVoiceData notify{};
    notify.PlayerId = pSpeaker->GetId();
    notify.Sequence = packet.Sequence;
    notify.Data = packet.Data;

    for (Player* pListener : m_world.GetPlayerManager())
    {
        if (pListener != pSpeaker && pListener->IsVoiceEnabled() && CanHear(*pListener, *pSpeaker))
            pListener->SendUnreliable(notify);
    }
}

bool VoiceService::CanHear(const Player& acListener, const Player& acSpeaker) const noexcept
{
    // The party channel: heard at any distance.
    const auto& speakerParty = acSpeaker.GetParty().JoinedPartyId;
    if (speakerParty.has_value() && acListener.GetParty().JoinedPartyId == speakerParty)
        return true;

    // Proximity: the same area, then the distance between the two characters when both are known.
    if (!acSpeaker.GetCellComponent().IsInRange(acListener.GetCellComponent(), false))
        return false;

    const auto cSpeakerCharacter = acSpeaker.GetCharacter();
    const auto cListenerCharacter = acListener.GetCharacter();
    if (!cSpeakerCharacter || !cListenerCharacter)
        return false;

    const auto* pSpeakerMovement = m_world.try_get<MovementComponent>(*cSpeakerCharacter);
    const auto* pListenerMovement = m_world.try_get<MovementComponent>(*cListenerCharacter);
    if (!pSpeakerMovement || !pListenerMovement)
        return false;

    return glm::distance(pSpeakerMovement->Position, pListenerMovement->Position) <= kHearingRange;
}
