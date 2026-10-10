#pragma once

struct World;
struct UpdateEvent;
struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

/**
 * @brief Voice chat, built on Steam's own voice functions (the game's steam_api64.dll): Steam opens the microphone,
 * detects speech and compresses it; we carry it and play it back placed in the world.
 *
 * First step (VR_TODO, "Proximity voice chat", step 0): the microphone test of the settings menu, which records five
 * seconds and plays them back to the player. It decides whether voice is built this way or on Mumble.
 */
struct VoiceService
{
    VoiceService(World& aWorld, entt::dispatcher& aDispatcher);
    ~VoiceService() noexcept = default;

    TP_NOCOPYMOVE(VoiceService);

    // "Test my microphone" in the settings menu.
    void StartMicrophoneTest() noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;

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

    void ReadVoice(std::chrono::steady_clock::time_point aNow) noexcept;
    void FinishRecording() noexcept;
    void SaveRecording() const noexcept;
    bool Play(const std::vector<int16_t>& acSamples, uint32_t aSampleRate) noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;

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
    IXAudio2SourceVoice* m_pSourceVoice{nullptr};
    std::vector<int16_t> m_playback;
    std::chrono::milliseconds m_playbackLength{0};
};
