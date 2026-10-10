#pragma once

#include <Events/PacketEvent.h>

struct World;
struct VoiceDataRequest;
struct VoiceStateRequest;

/**
 * @brief Passes players' voices on (VR_TODO, "Proximity voice chat", step 2): each piece goes, unreliable, to the
 * players with voice on who are near enough to hear it, and to the speaker's party at any distance. Nothing is kept.
 */
struct VoiceService
{
    VoiceService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~VoiceService() noexcept = default;

    TP_NOCOPYMOVE(VoiceService);

protected:
    void OnVoiceState(const PacketEvent<VoiceStateRequest>& acMessage) const noexcept;
    void OnVoiceData(const PacketEvent<VoiceDataRequest>& acMessage) noexcept;

private:
    bool CanHear(const Player& acListener, const Player& acSpeaker) const noexcept;

    struct Budget
    {
        std::chrono::steady_clock::time_point Since{};
        uint32_t Packets{0};
    };

    World& m_world;
    // Per speaker: pieces this second. Steam gives about 17 a second while talking; a client sending far more is
    // broken or abusing the server, and the rest of its second is dropped.
    Map<uint32_t, Budget> m_budgets;

    entt::scoped_connection m_voiceStateConnection;
    entt::scoped_connection m_voiceDataConnection;
};
