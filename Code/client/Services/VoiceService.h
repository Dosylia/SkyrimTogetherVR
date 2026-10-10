#pragma once

#include <deque>

struct World;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NotifyVoiceData;
struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

/**
 * @brief Voice chat, built on Steam's own voice functions (the game's steam_api64.dll): Steam opens the microphone,
 * detects speech and compresses it; we carry it and play it back placed in the world (VR_TODO, "Proximity voice
 * chat").
 *
 * Off until the player turns it on in the urSovngarde menu: no microphone is opened before that, and nothing is heard.
 * Everyone within about 30 m hears you, louder the closer they are; your party hears you at any distance, as a radio
 * when your body is not in their game. The settings menu's microphone test records five seconds and plays them back.
 */
struct VoiceService
{
    VoiceService(World& aWorld, entt::dispatcher& aDispatcher);
    ~VoiceService() noexcept = default;

    TP_NOCOPYMOVE(VoiceService);

    // From the urSovngarde menu; the menu remembers them and sends them again each time it loads.
    void StartMicrophoneTest() noexcept;
    void SetEnabled(bool aEnabled) noexcept;
    void SetVolume(float aVolume) noexcept;
    void SetPlayerMuted(uint32_t aPlayerId, bool aMuted) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnVoiceData(const NotifyVoiceData& acMessage) noexcept;

private:
    enum class TestState
    {
        kIdle,
        kRecording,
        kDraining,
        kPlaying,
    };

    struct Packet
    {
        uint32_t AtMs;
        std::vector<uint8_t> Bytes;
    };

    // One other player whose voice reaches us.
    struct Speaker
    {
        IXAudio2SourceVoice* pVoice{nullptr};
        // Buffers XAudio2 still reads, oldest first; dropped as it finishes them.
        std::deque<std::vector<int16_t>> Queued;
        size_t QueuedSamples{0};
        // Held back after a silence until there is enough to ride out the network's unevenness.
        std::vector<int16_t> Waiting;
        std::chrono::steady_clock::time_point WaitingSince{};
        std::chrono::steady_clock::time_point LastHeard{};
        uint32_t LastSequence{0};
        bool HasSequence{false};
    };

    // Microphone test
    void ReadVoice(std::chrono::steady_clock::time_point aNow) noexcept;
    void FinishRecording() noexcept;
    void SaveRecording() const noexcept;
    bool Play(const std::vector<int16_t>& acSamples, uint32_t aSampleRate) noexcept;

    // Live voice
    bool EnsureAudio() noexcept;
    void UpdateCapture() noexcept;
    void StopCapture() noexcept;
    void SendVoiceState() const noexcept;
    void UpdateSpeakers(std::chrono::steady_clock::time_point aNow) noexcept;
    void ReleasePlayed(Speaker& aSpeaker) noexcept;
    void Submit(Speaker& aSpeaker, std::vector<int16_t>&& aSamples) noexcept;
    void PlaceSpeaker(uint32_t aPlayerId, Speaker& aSpeaker) noexcept;
    void RemoveSpeakers() noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_voiceDataConnection;

    TestState m_testState{TestState::kIdle};
    std::chrono::steady_clock::time_point m_testStart{};
    std::chrono::steady_clock::time_point m_stateSince{};
    std::chrono::steady_clock::time_point m_lastSecondReport{};
    std::vector<Packet> m_packets;
    std::vector<uint8_t> m_readBuffer;
    uint32_t m_bytesThisSecond{0};
    uint32_t m_secondsWithVoice{0};
    int m_lastResult{-1};
    std::array<uint32_t, 7> m_resultCounts{};

    IXAudio2* m_pXAudio{nullptr};
    IXAudio2MasteringVoice* m_pMasteringVoice{nullptr};
    uint32_t m_outputChannels{2};
    bool m_audioFailed{false}; // XAudio2 could not start; not tried again, the log says why once
    IXAudio2SourceVoice* m_pSourceVoice{nullptr};
    std::vector<int16_t> m_playback;
    std::chrono::milliseconds m_playbackLength{0};

    bool m_enabled{false};
    float m_volume{1.f};
    bool m_connected{false};
    bool m_capturing{false};
    uint32_t m_sequence{0};
    uint32_t m_sampleRate{0};
    uint32_t m_sentThisMinute{0};
    uint32_t m_heardThisMinute{0};
    std::chrono::steady_clock::time_point m_lastMinuteReport{};
    std::unordered_map<uint32_t, Speaker> m_speakers;
    std::unordered_set<uint32_t> m_muted;
    std::vector<uint8_t> m_decodeBuffer;
};
