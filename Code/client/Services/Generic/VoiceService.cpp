#include <Services/VoiceService.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <PerfScope.h>
#include <Services/OverlayService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Components.h>
#include <PlayerCharacter.h>
#include <Forms/TESForm.h>
#ifdef SKYRIMVR
#include <Games/Skyrim/VRBodySync.h>
#endif

#include <Messages/NotifyVoiceData.h>
#include <Messages/VoiceDataRequest.h>
#include <Messages/VoiceStateRequest.h>
#include <Utils.h>
#include <World.h>

#include <TiltedCore/Filesystem.hpp>

#include <xaudio2.h>

#include <fstream>

namespace
{
// Steam's voice functions, called through the flat API of the game's own steam_api64.dll (2.89.45.4 on Emma's
// SkyrimVR, interface SteamUser018; checked 2026-10-10): the game has initialised Steam, so nothing is linked or
// shipped. The flat functions take the interface pointer first.
enum EVoiceResult : int
{
    kVoiceOK = 0,
    kVoiceNotInitialized = 1,
    kVoiceNotRecording = 2,
    kVoiceNoData = 3,
    kVoiceBufferTooSmall = 4,
    kVoiceDataCorrupted = 5,
    kVoiceRestricted = 6,
};

const char* VoiceResultName(int aResult) noexcept
{
    switch (aResult)
    {
    case kVoiceOK: return "ok";
    case kVoiceNotInitialized: return "Steam voice not initialised";
    case kVoiceNotRecording: return "not recording";
    case kVoiceNoData: return "no data";
    case kVoiceBufferTooSmall: return "buffer too small";
    case kVoiceDataCorrupted: return "data corrupted";
    case kVoiceRestricted: return "restricted (Steam account limited)";
    default: return "other";
    }
}

struct SteamVoice
{
    using TSteamClient = intptr_t (*)();
    using THandle = int32_t (*)();
    using TGetISteamUser = intptr_t (*)(intptr_t, int32_t, int32_t, const char*);
    using TRecording = void (*)(intptr_t);
    using TGetAvailableVoice = int (*)(intptr_t, uint32_t*, uint32_t*, uint32_t);
    using TGetVoice = int (*)(intptr_t, bool, void*, uint32_t, uint32_t*, bool, void*, uint32_t, uint32_t*, uint32_t);
    using TDecompressVoice = int (*)(intptr_t, const void*, uint32_t, void*, uint32_t, uint32_t*, uint32_t);
    using TOptimalSampleRate = uint32_t (*)(intptr_t);

    intptr_t User = 0;
    TRecording Start = nullptr;
    TRecording Stop = nullptr;
    TGetAvailableVoice GetAvailableVoice = nullptr;
    TGetVoice GetVoice = nullptr;
    TDecompressVoice Decompress = nullptr;
    TOptimalSampleRate OptimalSampleRate = nullptr;

    // Empty when it fails; the reason is logged once.
    static SteamVoice& Get() noexcept
    {
        static SteamVoice s_voice = Load();
        return s_voice;
    }

    bool IsReady() const noexcept { return User != 0; }

private:
    static SteamVoice Load() noexcept
    {
        SteamVoice voice;

        const HMODULE cModule = GetModuleHandleW(L"steam_api64.dll");
        if (!cModule)
        {
            spdlog::warn("Voice: steam_api64.dll is not loaded in the game; no voice");
            return voice;
        }

        const auto pSteamClient = reinterpret_cast<TSteamClient>(GetProcAddress(cModule, "SteamClient"));
        const auto pGetUser = reinterpret_cast<THandle>(GetProcAddress(cModule, "SteamAPI_GetHSteamUser"));
        const auto pGetPipe = reinterpret_cast<THandle>(GetProcAddress(cModule, "SteamAPI_GetHSteamPipe"));
        const auto pGetISteamUser = reinterpret_cast<TGetISteamUser>(GetProcAddress(cModule, "SteamAPI_ISteamClient_GetISteamUser"));
        const auto pStart = reinterpret_cast<TRecording>(GetProcAddress(cModule, "SteamAPI_ISteamUser_StartVoiceRecording"));
        const auto pStop = reinterpret_cast<TRecording>(GetProcAddress(cModule, "SteamAPI_ISteamUser_StopVoiceRecording"));
        const auto pAvailable = reinterpret_cast<TGetAvailableVoice>(GetProcAddress(cModule, "SteamAPI_ISteamUser_GetAvailableVoice"));
        const auto pGetVoice = reinterpret_cast<TGetVoice>(GetProcAddress(cModule, "SteamAPI_ISteamUser_GetVoice"));
        const auto pDecompress = reinterpret_cast<TDecompressVoice>(GetProcAddress(cModule, "SteamAPI_ISteamUser_DecompressVoice"));
        const auto pRate = reinterpret_cast<TOptimalSampleRate>(GetProcAddress(cModule, "SteamAPI_ISteamUser_GetVoiceOptimalSampleRate"));

        if (!pSteamClient || !pGetUser || !pGetPipe || !pGetISteamUser || !pStart || !pStop || !pAvailable || !pGetVoice || !pDecompress || !pRate)
        {
            spdlog::warn("Voice: a Steam voice function is missing from steam_api64.dll; no voice");
            return voice;
        }

        const intptr_t cClient = pSteamClient();
        const int32_t cUser = pGetUser();
        const int32_t cPipe = pGetPipe();
        if (!cClient || !cUser || !cPipe)
        {
            spdlog::warn("Voice: Steam is not initialised in the game (client {:X}, user {}, pipe {}); no voice", cClient, cUser, cPipe);
            return voice;
        }

        const intptr_t cSteamUser = pGetISteamUser(cClient, cUser, cPipe, "SteamUser018");
        if (!cSteamUser)
        {
            spdlog::warn("Voice: Steam refused the SteamUser018 interface; no voice");
            return voice;
        }

        voice.User = cSteamUser;
        voice.Start = pStart;
        voice.Stop = pStop;
        voice.GetAvailableVoice = pAvailable;
        voice.GetVoice = pGetVoice;
        voice.Decompress = pDecompress;
        voice.OptimalSampleRate = pRate;
        spdlog::info("Voice: Steam voice ready (SteamUser018), best sample rate {} Hz", pRate(cSteamUser));
        return voice;
    }
};

// The menu's chat box shows it while the menu is open, where the HUD's messages are hidden; the HUD shows it in game.
void TellPlayer(const std::string& acMessage) noexcept
{
    World::Get().GetOverlayService().SendSystemMessage(acMessage);
    Utils::ShowHudMessage(TiltedPhoques::String("urSovngarde: ") + acMessage.c_str());
}

constexpr auto kTestLength = std::chrono::seconds(5);
// Steam goes on giving the end of the speech for a moment after StopVoiceRecording.
constexpr auto kDrainLength = std::chrono::milliseconds(600);

// Live voice. Distances in game units, about 70 to the metre.
constexpr float kFullVolumeDistance = 350.f; // 5 m
constexpr float kSilentDistance = 2100.f;    // 30 m; the server stops sending a little further (34 m)
constexpr float kHeadHeight = 120.f;         // a standing character's mouth above their feet
// A party member heard from afar, or whose body is not in our game: centred and a little quieter, like a radio.
constexpr float kRadioGain = 0.6f;
// A voice the server says is near but whose body is not loaded here yet.
constexpr float kUnplacedGain = 0.5f;
// Held back after a silence before playing, to ride out the network's unevenness; played anyway after kJitterWait.
constexpr uint32_t kJitterMs = 80;
constexpr auto kJitterWait = std::chrono::milliseconds(150);
// More than this queued and we are falling behind: new pieces are dropped until it drains.
constexpr uint32_t kMaxQueuedMs = 400;
constexpr auto kSpeakerIdle = std::chrono::seconds(30);

float DistanceGain(float aDistance) noexcept
{
    if (aDistance <= kFullVolumeDistance)
        return 1.f;
    if (aDistance >= kSilentDistance)
        return 0.f;
    const float cFade = 1.f - (aDistance - kFullVolumeDistance) / (kSilentDistance - kFullVolumeDistance);
    return cFade * cFade;
}

Actor* FindPlayerActor(World& aWorld, uint32_t aPlayerId) noexcept
{
    auto view = aWorld.view<FormIdComponent, PlayerComponent>();
    for (const auto cEntity : view)
    {
        if (view.get<PlayerComponent>(cEntity).Id == aPlayerId)
            return Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id));
    }
    return nullptr;
}

// Where our ears are: the headset in VR, else the character's head facing its heading.
bool ListenerPose(glm::vec3& aPosition, glm::vec3& aForward, glm::vec3& aRight) noexcept
{
#ifdef SKYRIMVR
    if (VRBodySync::HeadsetPose(aPosition, aForward, aRight))
        return true;
#endif
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;
    const float cHeading = pPlayer->rotation.z;
    aPosition = glm::vec3(pPlayer->position.x, pPlayer->position.y, pPlayer->position.z + kHeadHeight);
    aForward = glm::vec3(std::sin(cHeading), std::cos(cHeading), 0.f);
    aRight = glm::vec3(std::cos(cHeading), -std::sin(cHeading), 0.f);
    return true;
}
} // namespace

VoiceService::VoiceService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&VoiceService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&VoiceService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&VoiceService::OnDisconnected>(this);
    m_voiceDataConnection = aDispatcher.sink<NotifyVoiceData>().connect<&VoiceService::OnVoiceData>(this);
}

void VoiceService::StartMicrophoneTest() noexcept
{
    if (m_testState != TestState::kIdle)
    {
        spdlog::info("Voice test: already running");
        TellPlayer("Microphone test already running, wait for it to end.");
        return;
    }

    auto& steam = SteamVoice::Get();
    if (!steam.IsReady())
    {
        TellPlayer("Voice is not available (Steam), see the log.");
        return;
    }

    m_packets.clear();
    m_readBuffer.resize(8 * 1024);
    m_bytesThisSecond = 0;
    m_secondsWithVoice = 0;
    m_lastResult = -1;
    m_resultCounts.fill(0);

    // The test reads Steam's voice itself; live voice takes the microphone back when it ends.
    m_capturing = false;
    steam.Start(steam.User);

    const auto cNow = std::chrono::steady_clock::now();
    m_testStart = cNow;
    m_stateSince = cNow;
    m_lastSecondReport = cNow;
    m_testState = TestState::kRecording;

    spdlog::info("Voice test: recording for {} s", kTestLength.count());
    TellPlayer("Microphone test: recording now, speak for five seconds.");
}

void VoiceService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_testState == TestState::kIdle && !m_capturing && m_speakers.empty() && !(m_enabled && m_connected))
        return;

    PerfScope perfScope("VoiceService::OnUpdate");

    const auto cNow = std::chrono::steady_clock::now();
    UpdateCapture();
    UpdateSpeakers(cNow);

    if (m_testState == TestState::kIdle)
        return;

    auto& steam = SteamVoice::Get();

    switch (m_testState)
    {
    case TestState::kRecording:
        ReadVoice(cNow);
        if (cNow - m_stateSince >= kTestLength)
        {
            steam.Stop(steam.User);
            TellPlayer("Microphone test: recording stopped.");
            m_stateSince = cNow;
            m_testState = TestState::kDraining;
        }
        break;
    case TestState::kDraining:
        ReadVoice(cNow);
        if (cNow - m_stateSince >= kDrainLength || m_lastResult == kVoiceNotRecording)
            FinishRecording();
        break;
    case TestState::kPlaying:
        if (cNow - m_stateSince >= m_playbackLength + std::chrono::milliseconds(300))
        {
            if (m_pSourceVoice)
            {
                m_pSourceVoice->DestroyVoice();
                m_pSourceVoice = nullptr;
            }
            m_playback.clear();
            m_testState = TestState::kIdle;
            spdlog::info("Voice test: playback done");
            TellPlayer("Microphone test: done.");
        }
        break;
    default: break;
    }
}

void VoiceService::ReadVoice(std::chrono::steady_clock::time_point aNow) noexcept
{
    auto& steam = SteamVoice::Get();

    // Steam keeps a little of what it recorded; reading every frame until it has nothing more keeps none waiting.
    for (int i = 0; i < 8; ++i)
    {
        uint32_t written = 0;
        if (i == 0 && aNow - m_lastSecondReport >= std::chrono::seconds(1))
        {
            uint32_t available = 0;
            const int cAvailable = steam.GetAvailableVoice(steam.User, &available, nullptr, 0);
            spdlog::info("Voice test: GetAvailableVoice says {} ({}), {} bytes waiting", VoiceResultName(cAvailable), cAvailable, available);
        }

        const int cResult = steam.GetVoice(steam.User, true, m_readBuffer.data(), static_cast<uint32_t>(m_readBuffer.size()), &written, false, nullptr, 0, nullptr, 0);

        if (cResult != m_lastResult && cResult != kVoiceOK && cResult != kVoiceNoData)
            spdlog::info("Voice test: GetVoice says {} ({})", VoiceResultName(cResult), cResult);
        m_lastResult = cResult;
        if (cResult >= 0 && cResult < static_cast<int>(m_resultCounts.size()))
            ++m_resultCounts[cResult];

        if (cResult == kVoiceBufferTooSmall)
        {
            m_readBuffer.resize(m_readBuffer.size() * 2);
            continue;
        }
        if (cResult != kVoiceOK || written == 0)
            break;

        Packet packet;
        packet.AtMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(aNow - m_testStart).count());
        packet.Bytes.assign(m_readBuffer.begin(), m_readBuffer.begin() + written);
        m_packets.push_back(std::move(packet));
        m_bytesThisSecond += written;
    }

    if (aNow - m_lastSecondReport >= std::chrono::seconds(1))
    {
        // What Steam answered this second: "no data" every time means the microphone it listens to gives it nothing.
        spdlog::info("Voice test: {} bytes of voice in the last second (GetVoice: {} ok, {} no data, {} not recording)", m_bytesThisSecond,
                     m_resultCounts[kVoiceOK], m_resultCounts[kVoiceNoData], m_resultCounts[kVoiceNotRecording]);
        m_resultCounts.fill(0);
        if (m_bytesThisSecond > 0)
            ++m_secondsWithVoice;
        m_bytesThisSecond = 0;
        m_lastSecondReport = aNow;
    }
}

void VoiceService::FinishRecording() noexcept
{
    auto& steam = SteamVoice::Get();

    size_t total = 0;
    for (const auto& packet : m_packets)
        total += packet.Bytes.size();

    spdlog::info("Voice test: recording done, {} packets, {} bytes, {} of the seconds with voice", m_packets.size(), total, m_secondsWithVoice);

    if (m_packets.empty())
    {
        m_testState = TestState::kIdle;
        TellPlayer("Microphone test: Steam heard nothing. Check the microphone in Steam's settings, Voice.");
        return;
    }

    SaveRecording();

    const uint32_t cRate = steam.OptimalSampleRate(steam.User);
    std::vector<int16_t> samples;
    std::vector<uint8_t> decoded(64 * 1024);
    int failures = 0;

    for (const auto& packet : m_packets)
    {
        uint32_t written = 0;
        int result = steam.Decompress(steam.User, packet.Bytes.data(), static_cast<uint32_t>(packet.Bytes.size()), decoded.data(), static_cast<uint32_t>(decoded.size()), &written, cRate);
        if (result == kVoiceBufferTooSmall && written > decoded.size())
        {
            decoded.resize(written);
            result = steam.Decompress(steam.User, packet.Bytes.data(), static_cast<uint32_t>(packet.Bytes.size()), decoded.data(), static_cast<uint32_t>(decoded.size()), &written, cRate);
        }
        if (result != kVoiceOK)
        {
            ++failures;
            continue;
        }

        const auto* pSamples = reinterpret_cast<const int16_t*>(decoded.data());
        samples.insert(samples.end(), pSamples, pSamples + written / sizeof(int16_t));
    }

    int16_t peak = 0;
    for (const int16_t cSample : samples)
        peak = std::max<int16_t>(peak, static_cast<int16_t>(std::min(32767, std::abs(static_cast<int>(cSample)))));

    spdlog::info("Voice test: decoded {:.2f} s at {} Hz, {} packets failed to decode, loudest sample {} of 32767", static_cast<double>(samples.size()) / cRate, cRate, failures, peak);

    if (samples.empty() || !Play(samples, cRate))
    {
        m_testState = TestState::kIdle;
        TellPlayer("Microphone test: your voice was recorded but could not be played back, see the log.");
        return;
    }

    m_stateSince = std::chrono::steady_clock::now();
    m_testState = TestState::kPlaying;
    TellPlayer("Microphone test: playing it back, this is what the others will hear.");
}

void VoiceService::SaveRecording() const noexcept
{
    // Kept for the rig, where nobody speaks: the bot replays it as a player's voice (VR_TODO, voice, step 5). Each
    // packet: its time from the start in ms and its size, both uint32, then Steam's compressed bytes.
    const auto cPath = TiltedPhoques::GetPath() / "logs" / "voice_test.bin";
    std::ofstream file(cPath, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        spdlog::warn("Voice test: could not write {}", cPath.string());
        return;
    }

    for (const auto& packet : m_packets)
    {
        const uint32_t cSize = static_cast<uint32_t>(packet.Bytes.size());
        file.write(reinterpret_cast<const char*>(&packet.AtMs), sizeof(packet.AtMs));
        file.write(reinterpret_cast<const char*>(&cSize), sizeof(cSize));
        file.write(reinterpret_cast<const char*>(packet.Bytes.data()), cSize);
    }

    spdlog::info("Voice test: recording saved to {}", cPath.string());
}

bool VoiceService::EnsureAudio() noexcept
{
    // XAudio2 2.9 is part of Windows 10 and 11; loaded by hand so the client needs no new import. Never released: the
    // engine lives as long as the game, and tearing it down while the game exits only risks a crash on the way out.
    if (m_audioFailed)
        return false;
    if (!m_pXAudio)
    {
        m_audioFailed = true; // until it has fully started
        const HMODULE cModule = LoadLibraryW(L"xaudio2_9.dll");
        using TXAudio2Create = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
        const auto pCreate = cModule ? reinterpret_cast<TXAudio2Create>(GetProcAddress(cModule, "XAudio2Create")) : nullptr;
        if (!pCreate)
        {
            spdlog::warn("Voice: xaudio2_9.dll or its XAudio2Create is missing");
            return false;
        }

        HRESULT result = pCreate(&m_pXAudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(result) || !m_pXAudio)
        {
            spdlog::warn("Voice: XAudio2Create failed ({:X})", static_cast<uint32_t>(result));
            m_pXAudio = nullptr;
            return false;
        }

        result = m_pXAudio->CreateMasteringVoice(&m_pMasteringVoice);
        if (FAILED(result))
        {
            spdlog::warn("Voice: no audio output for XAudio2 ({:X})", static_cast<uint32_t>(result));
            m_pXAudio->Release();
            m_pXAudio = nullptr;
            return false;
        }

        XAUDIO2_VOICE_DETAILS details{};
        m_pMasteringVoice->GetVoiceDetails(&details);
        m_outputChannels = std::max<uint32_t>(1, details.InputChannels);
        spdlog::info("Voice: audio out ready, {} channels at {} Hz", m_outputChannels, details.InputSampleRate);
        m_audioFailed = false;
    }
    return true;
}

bool VoiceService::Play(const std::vector<int16_t>& acSamples, uint32_t aSampleRate) noexcept
{
    if (!EnsureAudio())
        return false;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = aSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HRESULT result = m_pXAudio->CreateSourceVoice(&m_pSourceVoice, &format);
    if (FAILED(result))
    {
        spdlog::warn("Voice: CreateSourceVoice failed ({:X})", static_cast<uint32_t>(result));
        m_pSourceVoice = nullptr;
        return false;
    }

    // XAudio2 reads the samples while it plays, so they are kept until the test ends.
    m_playback = acSamples;

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(m_playback.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(m_playback.data());
    buffer.Flags = XAUDIO2_END_OF_STREAM;

    result = m_pSourceVoice->SubmitSourceBuffer(&buffer);
    if (SUCCEEDED(result))
        result = m_pSourceVoice->Start(0);
    if (FAILED(result))
    {
        spdlog::warn("Voice: playback failed to start ({:X})", static_cast<uint32_t>(result));
        m_pSourceVoice->DestroyVoice();
        m_pSourceVoice = nullptr;
        m_playback.clear();
        return false;
    }

    m_playbackLength = std::chrono::milliseconds(m_playback.size() * 1000 / aSampleRate);
    spdlog::info("Voice test: playing {} ms back", m_playbackLength.count());
    return true;
}

void VoiceService::SetEnabled(bool aEnabled) noexcept
{
    if (aEnabled == m_enabled)
        return;

    m_enabled = aEnabled;
    spdlog::info("Voice: turned {}", aEnabled ? "on" : "off");
    if (!aEnabled)
    {
        StopCapture();
        RemoveSpeakers();
    }
    SendVoiceState();
}

void VoiceService::SetVolume(float aVolume) noexcept
{
    m_volume = std::clamp(aVolume, 0.f, 2.f);
}

void VoiceService::SetPlayerMuted(uint32_t aPlayerId, bool aMuted) noexcept
{
    if (aMuted)
    {
        m_muted.insert(aPlayerId);
        if (const auto it = m_speakers.find(aPlayerId); it != m_speakers.end())
        {
            if (it->second.pVoice)
                it->second.pVoice->DestroyVoice();
            m_speakers.erase(it);
        }
    }
    else
        m_muted.erase(aPlayerId);
}

void VoiceService::OnConnected(const ConnectedEvent&) noexcept
{
    m_connected = true;
    m_sequence = 0;
    SendVoiceState();
}

void VoiceService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_connected = false;
    StopCapture();
    RemoveSpeakers();
}

void VoiceService::SendVoiceState() const noexcept
{
    if (!m_connected)
        return;

    VoiceStateRequest request{};
    request.Enabled = m_enabled;
    m_world.GetTransport().Send(request);
}

void VoiceService::UpdateCapture() noexcept
{
    auto& steam = SteamVoice::Get();
    if (!m_enabled || !m_connected || m_testState != TestState::kIdle || !steam.IsReady())
    {
        if (m_capturing)
            StopCapture();
        return;
    }

    if (!m_capturing)
    {
        steam.Start(steam.User);
        m_capturing = true;
        m_readBuffer.resize(std::max<size_t>(m_readBuffer.size(), 8 * 1024));
        spdlog::info("Voice: microphone on");
    }

    // Everything Steam has, every frame, so nothing waits: each GetVoice is one piece of a few hundred bytes.
    for (int i = 0; i < 8; ++i)
    {
        uint32_t written = 0;
        const int cResult = steam.GetVoice(steam.User, true, m_readBuffer.data(), static_cast<uint32_t>(m_readBuffer.size()), &written, false, nullptr, 0, nullptr, 0);
        if (cResult == kVoiceBufferTooSmall)
        {
            m_readBuffer.resize(m_readBuffer.size() * 2);
            continue;
        }
        if (cResult != kVoiceOK || written == 0 || written > VoiceDataRequest::kMaxBytes)
            break;

        VoiceDataRequest request{};
        request.Sequence = m_sequence++;
        request.Data.assign(m_readBuffer.begin(), m_readBuffer.begin() + written);
        m_world.GetTransport().SendUnreliable(request);
        ++m_sentThisMinute;
    }
}

void VoiceService::StopCapture() noexcept
{
    if (!m_capturing)
        return;

    auto& steam = SteamVoice::Get();
    if (steam.IsReady())
        steam.Stop(steam.User);
    m_capturing = false;
    spdlog::info("Voice: microphone off");
}

void VoiceService::OnVoiceData(const NotifyVoiceData& acMessage) noexcept
{
    if (!m_enabled || acMessage.Data.empty() || m_muted.contains(acMessage.PlayerId))
        return;

    auto& steam = SteamVoice::Get();
    if (!steam.IsReady() || !EnsureAudio())
        return;
    if (m_sampleRate == 0)
        m_sampleRate = steam.OptimalSampleRate(steam.User);

    Speaker& speaker = m_speakers[acMessage.PlayerId];
    // Unreliable pieces can arrive late: one older than the last played is dropped.
    if (speaker.HasSequence && static_cast<int32_t>(acMessage.Sequence - speaker.LastSequence) <= 0)
        return;
    speaker.LastSequence = acMessage.Sequence;
    speaker.HasSequence = true;

    const auto cNow = std::chrono::steady_clock::now();
    speaker.LastHeard = cNow;
    ++m_heardThisMinute;

    if (m_decodeBuffer.size() < 32 * 1024)
        m_decodeBuffer.resize(32 * 1024);
    uint32_t written = 0;
    int result = steam.Decompress(steam.User, acMessage.Data.data(), static_cast<uint32_t>(acMessage.Data.size()), m_decodeBuffer.data(), static_cast<uint32_t>(m_decodeBuffer.size()), &written, m_sampleRate);
    if (result == kVoiceBufferTooSmall && written > m_decodeBuffer.size())
    {
        m_decodeBuffer.resize(written);
        result = steam.Decompress(steam.User, acMessage.Data.data(), static_cast<uint32_t>(acMessage.Data.size()), m_decodeBuffer.data(), static_cast<uint32_t>(m_decodeBuffer.size()), &written, m_sampleRate);
    }
    if (result != kVoiceOK || written < sizeof(int16_t))
        return;

    const auto* pSamples = reinterpret_cast<const int16_t*>(m_decodeBuffer.data());
    std::vector<int16_t> samples(pSamples, pSamples + written / sizeof(int16_t));

    if (!speaker.pVoice)
    {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = m_sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        if (FAILED(m_pXAudio->CreateSourceVoice(&speaker.pVoice, &format)) || FAILED(speaker.pVoice->Start(0)))
        {
            spdlog::warn("Voice: could not open a voice for player {}", acMessage.PlayerId);
            if (speaker.pVoice)
                speaker.pVoice->DestroyVoice();
            m_speakers.erase(acMessage.PlayerId);
            return;
        }
        spdlog::info("Voice: hearing player {}", acMessage.PlayerId);
        PlaceSpeaker(acMessage.PlayerId, speaker);
    }

    ReleasePlayed(speaker);
    if (speaker.Queued.empty())
    {
        // Nothing playing: start again only once a little is gathered.
        if (speaker.Waiting.empty())
            speaker.WaitingSince = cNow;
        speaker.Waiting.insert(speaker.Waiting.end(), samples.begin(), samples.end());
        if (speaker.Waiting.size() >= m_sampleRate * kJitterMs / 1000)
            Submit(speaker, std::move(speaker.Waiting));
    }
    else if (speaker.QueuedSamples < m_sampleRate * kMaxQueuedMs / 1000)
        Submit(speaker, std::move(samples));
}

void VoiceService::ReleasePlayed(Speaker& aSpeaker) noexcept
{
    if (!aSpeaker.pVoice)
        return;

    XAUDIO2_VOICE_STATE state{};
    aSpeaker.pVoice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    // XAudio2 finishes buffers in the order they were given.
    while (aSpeaker.Queued.size() > state.BuffersQueued)
    {
        aSpeaker.QueuedSamples -= aSpeaker.Queued.front().size();
        aSpeaker.Queued.pop_front();
    }
}

void VoiceService::Submit(Speaker& aSpeaker, std::vector<int16_t>&& aSamples) noexcept
{
    if (!aSpeaker.pVoice || aSamples.empty() || aSpeaker.Queued.size() >= XAUDIO2_MAX_QUEUED_BUFFERS)
    {
        aSamples.clear();
        return;
    }

    // Moved, not copied: the samples XAudio2 reads stay where they are while the deque grows.
    auto& queued = aSpeaker.Queued.emplace_back(std::move(aSamples));
    aSamples.clear();

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(queued.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(queued.data());
    if (FAILED(aSpeaker.pVoice->SubmitSourceBuffer(&buffer)))
    {
        aSpeaker.Queued.pop_back();
        return;
    }
    aSpeaker.QueuedSamples += queued.size();
}

void VoiceService::UpdateSpeakers(std::chrono::steady_clock::time_point aNow) noexcept
{
    for (auto it = m_speakers.begin(); it != m_speakers.end();)
    {
        Speaker& speaker = it->second;
        ReleasePlayed(speaker);

        // The end of a short phrase never reaches kJitterMs on its own.
        if (speaker.Queued.empty() && !speaker.Waiting.empty() && aNow - speaker.WaitingSince >= kJitterWait)
            Submit(speaker, std::move(speaker.Waiting));

        if (speaker.Queued.empty() && speaker.Waiting.empty() && aNow - speaker.LastHeard >= kSpeakerIdle)
        {
            if (speaker.pVoice)
                speaker.pVoice->DestroyVoice();
            it = m_speakers.erase(it);
            continue;
        }

        PlaceSpeaker(it->first, speaker);
        ++it;
    }

    if (aNow - m_lastMinuteReport >= std::chrono::minutes(1))
    {
        if (m_sentThisMinute || m_heardThisMinute)
            spdlog::info("Voice: last minute, {} pieces sent, {} heard from {} players", m_sentThisMinute, m_heardThisMinute, m_speakers.size());
        m_sentThisMinute = 0;
        m_heardThisMinute = 0;
        m_lastMinuteReport = aNow;
    }
}

void VoiceService::PlaceSpeaker(uint32_t aPlayerId, Speaker& aSpeaker) noexcept
{
    if (!aSpeaker.pVoice)
        return;

    const auto& partyMembers = m_world.GetPartyService().GetPartyMembers();
    const bool cIsParty = std::find(partyMembers.begin(), partyMembers.end(), aPlayerId) != partyMembers.end();

    float gain = cIsParty ? kRadioGain : kUnplacedGain;
    float pan = 0.f; // -1 left, 1 right

    glm::vec3 listener{}, forward{}, right{};
    Actor* pActor = FindPlayerActor(m_world, aPlayerId);
    if (pActor && ListenerPose(listener, forward, right))
    {
        const glm::vec3 cMouth(pActor->position.x, pActor->position.y, pActor->position.z + kHeadHeight);
        const glm::vec3 cToSpeaker = cMouth - listener;
        const float cDistance = glm::length(cToSpeaker);
        gain = DistanceGain(cDistance);
        if (cDistance > 1.f)
            pan = std::clamp(glm::dot(cToSpeaker / cDistance, right), -1.f, 1.f);

        // A party member stays heard at any distance: as a radio, centred, once their own voice would be quieter.
        if (cIsParty && gain < kRadioGain)
        {
            pan *= gain / kRadioGain;
            gain = kRadioGain;
        }
    }

    // Equal power between left and right; channels past the first two get nothing.
    std::array<float, XAUDIO2_MAX_AUDIO_CHANNELS> matrix{};
    const uint32_t cChannels = std::min<uint32_t>(m_outputChannels, XAUDIO2_MAX_AUDIO_CHANNELS);
    if (cChannels == 1)
        matrix[0] = 1.f;
    else
    {
        const float cAngle = (pan + 1.f) * glm::pi<float>() / 4.f;
        matrix[0] = std::cos(cAngle);
        matrix[1] = std::sin(cAngle);
    }
    aSpeaker.pVoice->SetOutputMatrix(nullptr, 1, cChannels, matrix.data());
    aSpeaker.pVoice->SetVolume(gain * m_volume);
}

void VoiceService::RemoveSpeakers() noexcept
{
    for (auto& [id, speaker] : m_speakers)
    {
        if (speaker.pVoice)
            speaker.pVoice->DestroyVoice();
    }
    m_speakers.clear();
}
