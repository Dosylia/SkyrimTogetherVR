#include <Games/Skyrim/Interface/IMenu.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Misc/BSFixedString.h>
#include <TiltedOnlinePCH.h>
#include "immersive_launcher/stubs/DllBlocklist.h"

#include <World.h>

static bool g_RequestUnpauseAll{false};

UI* UI::Get()
{
    POINTER_SKYRIMSE(UI*, s_instance, 400327, 514178);
    return *s_instance.Get();
}

bool UI::GetMenuOpen(const BSFixedString& acName) const
{
    if (acName.data == nullptr)
        return false;

    TP_THIS_FUNCTION(TMenuSystem_IsOpen, bool, const UI, const BSFixedString&);
    POINTER_SKYRIMSE(TMenuSystem_IsOpen, s_isMenuOpen, 82074, 82074);

    return TiltedPhoques::ThisCall(s_isMenuOpen.Get(), this, acName);
}

void UI::CloseAllMenus()
{
    TP_THIS_FUNCTION(TUI_CloseAll, void, const UI);
    POINTER_SKYRIMSE(TUI_CloseAll, s_CloseAll, 82088, 82088);

    TiltedPhoques::ThisCall(s_CloseAll.Get(), this);
}

BSFixedString* UI::LookupMenuNameByInstance(IMenu* apMenu)
{
    for (auto& it : menuMap)
    {
        if (it.value.spMenu == apMenu)
            return &it.key;
    }
    return nullptr;
}

IMenu* UI::FindMenuByName(const BSFixedString& acName)
{
    for (const auto& it : menuMap)
    {
        if (it.key == acName)
            return it.value.spMenu;
    }
    return nullptr;
}

void UI::DebugLogAllMenus()
{
    for (auto& e : menuStack)
    {
        spdlog::info("Menu {}", e->uiMenuFlags);
    }
}

static void UnfreezeMenu(IMenu* apEntry)
{
    if (apEntry->PausesGame())
    {
        apEntry->ClearFlag(IMenu::kPausesGame);
#ifdef SKYRIMVR
        // VR only shows its menu panel (PlayerCharacter's UINode) while a menu pauses the game or carries this
        // flag. MessageBoxMenu and Console lack it, so unpaused they opened invisibly in the headset
        // (TiltedEvolutionVR 5c30ec6).
        apEntry->SetFlag(IMenu::kUpdateUsesCursor);
#endif
    }

    if (apEntry->FreezesBackground())
        apEntry->ClearFlag(IMenu::kFreezeFrameBackground);

    if (apEntry->FreezesFramePause())
        apEntry->ClearFlag(IMenu::kFreezeFramePause);
}

// TEMPORARY (2026-09-20): a message box opens at the end of every save load and, when it opens before the connection,
// pauses the world for good. Nothing names it, so the strings reachable from the menu object are dumped: the box's
// data hangs off the menu, and its body text and button labels are plain C strings. Every read is checked against
// the page tables first, so a wrong guess prints nothing instead of crashing. Remove once the box is identified.
namespace
{
bool ReadableBytes(const void* apAddress, const size_t aBytes) noexcept
{
    if (!apAddress)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apAddress, &info, sizeof(info)))
        return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return reinterpret_cast<uintptr_t>(apAddress) + aBytes <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}

std::string PrintableTextAt(const char* apText) noexcept
{
    std::string text;
    for (size_t i = 0; i < 200; ++i)
    {
        if (!ReadableBytes(apText + i, 1))
            return {};
        const char c = apText[i];
        if (c == 0)
            break;
        if (c == '\n' || c == '\r' || c == '\t')
        {
            text += ' ';
            continue;
        }
        if (c < 0x20 || c > 0x7E)
            return {};
        text += c;
    }
    return text.size() >= 4 ? text : std::string{};
}

void CollectStrings(const void* apObject, const size_t aBytes, const int aDepth, std::string& aOut) noexcept
{
    if (!ReadableBytes(apObject, aBytes))
        return;
    for (size_t offset = 0; offset < aBytes; offset += sizeof(void*))
    {
        const char* pCandidate = *reinterpret_cast<const char* const*>(static_cast<const uint8_t*>(apObject) + offset);
        const std::string text = PrintableTextAt(pCandidate);
        if (!text.empty())
            aOut += fmt::format(" [+{:X}] \"{}\"", offset, text);
        else if (aDepth > 0)
            CollectStrings(pCandidate, 0x80, aDepth - 1, aOut);
        if (aOut.size() > 1500)
            return;
    }
}
} // namespace

void UI::CollectReadableStrings(const void* apObject, const size_t aBytes, const int aDepth, std::string& aOut) noexcept
{
    CollectStrings(apObject, aBytes, aDepth, aOut);
}

static constexpr const char* kAllowList[] = {
    "TweenMenu",     "MagicMenu",     "InventoryMenu",
#ifndef SKYRIMVR
    // On VR the skills and level up screen is black whenever it runs unpaused: it works in solo, where this hook is
    // inactive, and never while connected (2026-09-18). There it pauses the game like vanilla.
    "StatsMenu",
#endif
    // Unpaused, a message box used to be invisible in the headset; UnfreezeMenu now gives it the flag the VR
    // panel needs, so it no longer has to pause the game for every player.
    "MessageBoxMenu",
    "ContainerMenu", "FavoritesMenu", "Tutorial Menu", "Console"
    //"MapMenu", // MapMenu is disabled till we find a proper fix for first person.
    //"Journal Menu", // Journal menu, aka pause menu, is disabled until we find a fix for manual save crashing while unpaused.
};

static void* (*UI_AddToActiveQueue)(UI*, IMenu*, void*);

static void* UI_AddToActiveQueue_Hook(UI* apSelf, IMenu* apMenu, void* apFoundItem /*In reality a reference*/)
{
    // TEMPORARY (2026-09-20): every menu the game queues, connected or not, for the frozen-world timeline. The VR
    // HUD widgets (WS*) are queued every frame and are left out.
    if (apMenu)
    {
        const BSFixedString* pQueuedName = apSelf->LookupMenuNameByInstance(apMenu);
        if (pQueuedName && pQueuedName->AsAscii() && strncmp(pQueuedName->AsAscii(), "WS", 2) == 0)
            return UI_AddToActiveQueue(apSelf, apMenu, apFoundItem);
        const BSFixedString* pName = apSelf->LookupMenuNameByInstance(apMenu);
        spdlog::info("Menu queued: {}{} (connected {})", pName ? pName->AsAscii() : "?", apMenu->PausesGame() ? " [pauses]" : "", World::Get().GetTransport().IsConnected() ? "yes" : "no");
        if (pName && strcmp(pName->AsAscii(), "MessageBoxMenu") == 0)
        {
            std::string strings;
            CollectStrings(apMenu, 0xC0, 2, strings);
            spdlog::info("MessageBoxMenu strings:{}", strings.empty() ? " (none readable)" : strings.c_str());
        }
    }

    // if the menu is empty we let the real function handle it.
    if (!apMenu || !World::Get().GetTransport().IsConnected() || stubs::g_IsSoulsREActive)
        return UI_AddToActiveQueue(apSelf, apMenu, apFoundItem);

#if 0
        if (auto* pName = apSelf->LookupMenuNameByInstance(apEntry))
        {
            spdlog::info("Menu requested {}", pName->AsAscii());
        }
#endif

    // A menu opened out of a conversation is left to pause, whatever the list says.
    //
    // Trading with a follower opens ContainerMenu on top of a Dialogue Menu that is still up, and ContainerMenu is
    // on the list. Unfrozen, the trade panel draws but the dialogue underneath keeps the input: Emma could see
    // "LYDIA <-> QUEEN EMMA" and could not touch it until she closed the conversation (2026-09-26). Vanilla pauses
    // there, and pausing is what hands the focus over.
    //
    // Only while a conversation is actually open, so looting a chest or a corpse -- the reason this list exists --
    // still leaves everyone else moving.
    const bool cInConversation = apSelf->GetMenuOpen(BSFixedString("Dialogue Menu"));

    // NOTE(Force): could also compare by RTTI later on...
    for (const char* item : kAllowList)
    {
        if (auto* pMenu = apSelf->FindMenuByName(item))
        {
            if (pMenu == apMenu)
            {
                if (cInConversation)
                {
                    spdlog::info("Menu opened while connected: {} (left paused; a conversation is open and would keep the input)", item);
                    break;
                }

                spdlog::info("Menu opened while connected: {} (runs unpaused)", item);
                UnfreezeMenu(apMenu);
            }
        }
    }

    return UI_AddToActiveQueue(apSelf, apMenu, apFoundItem);
}

using TCallback = void(void*, const BSFixedString*, uint32_t, void*);
static TCallback* UIMessageQueue__AddMessage_Real;

// TEMPORARY (2026-09-20): a message box with nothing in it opened at the main menu and at the end of every save load
// and paused the world for good. Every message aimed at MessageBoxMenu is logged with its caller and its text, and a
// show request that carries no data at all is dropped: it cannot display anything, it can only hold the pause.
static void DescribeCaller(void* apAddress, char* apOut, const size_t aOutSize) noexcept
{
    HMODULE hModule = nullptr;
    char name[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(apAddress), &hModule) && hModule &&
        GetModuleFileNameA(hModule, name, sizeof(name)))
    {
        const char* pBase = strrchr(name, 92);
        _snprintf_s(apOut, aOutSize, _TRUNCATE, "%s+0x%llx", pBase ? pBase + 1 : name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(apAddress) - reinterpret_cast<uintptr_t>(hModule)));
        return;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(apAddress);
    if (address >= 0x140000000ull && address < 0x140000000ull + 0x8000000ull)
        _snprintf_s(apOut, aOutSize, _TRUNCATE, "SkyrimVR.exe+0x%llx", static_cast<unsigned long long>(address - 0x140000000ull));
    else
        _snprintf_s(apOut, aOutSize, _TRUNCATE, "<0x%llx>", static_cast<unsigned long long>(address));
}

static bool s_sendingEnemyMeter = false;
static std::chrono::steady_clock::time_point s_lastGameEnemyMeterTargetAt{};

void UIMessageQueue__AddMessage(void* a1, const BSFixedString* a2, UIMessage::UI_MESSAGE_TYPE a3, void* a4)
{
    if (a2 && a2->AsAscii() && strcmp(a2->AsAscii(), "MessageBoxMenu") == 0)
    {
        char caller[160];
        DescribeCaller(_ReturnAddress(), caller, sizeof(caller));

        std::string text;
        if (a4)
        {
            // MessageBoxData (CommonLibVR): BSString bodyText at +0x10, whose first field is the char pointer.
            const uint8_t* pBody = static_cast<const uint8_t*>(a4) + 0x10;
            if (ReadableBytes(pBody, 16))
                text = PrintableTextAt(*reinterpret_cast<const char* const*>(pBody));
        }

        // Only the one caller that opened the phantom at the main menu and at the end of every load (SkyrimVR 1.4.15
        // at +0x168507) is dropped. Other data-less shows are real: the level-up choice panel is one (from +0x8d8683,
        // the game fills it afterwards), and dropping it left the player stuck in the level-up menu (2026-09-20 07:58).
        // +0x168507 turned out to be a shared helper: it also queued a real box 35 s after a level-up, and dropping that
        // one crashed the game 92 ms later (2026-09-20 11:47). The phantom only ever appears while the main menu or a
        // loading screen is up, so the drop is limited to those moments.
        const uintptr_t callerOffset = reinterpret_cast<uintptr_t>(_ReturnAddress()) - 0x140000000ull;
        bool loadingOrMainMenu = false;
        if (UI* pUi = UI::Get())
        {
            for (uint32_t i = 0; i < pUi->menuStack.length; ++i)
            {
                IMenu* pMenu = pUi->menuStack[i];
                const BSFixedString* pName = pMenu ? pUi->LookupMenuNameByInstance(pMenu) : nullptr;
                if (pName && pName->AsAscii() && (strcmp(pName->AsAscii(), "Loading Menu") == 0 || strcmp(pName->AsAscii(), "Main Menu") == 0))
                    loadingOrMainMenu = true;
            }
        }
        const bool phantom = a3 == UIMessage::kShow && a4 == nullptr && callerOffset == 0x168507ull && loadingOrMainMenu;
        // TEMPORARY (2026-09-20): the direct caller sits in generic UI code (+0xf207f7 is in the UIMessageQueue region,
        // +0x168507 in a shared TES helper), so the frames above it are what name the feature that closes the level-up
        // box with a save load while connected, and the one that shows the box after a respawn. The data's first
        // words are printed too: a vtable pointer there identifies the message data or callback type.
        std::string frames;
        {
            void* stack[12] = {};
            const USHORT count = RtlCaptureStackBackTrace(1, 12, stack, nullptr);
            for (USHORT i = 0; i < count; ++i)
            {
                char frame[160];
                DescribeCaller(stack[i], frame, sizeof(frame));
                frames += fmt::format("{}{}", i == 0 ? "" : " < ", frame);
            }
        }
        std::string dataWords;
        if (a4 && ReadableBytes(a4, 32))
        {
            const uint64_t* pWords = static_cast<const uint64_t*>(a4);
            for (int i = 0; i < 4; ++i)
            {
                char word[160];
                DescribeCaller(reinterpret_cast<void*>(static_cast<uintptr_t>(pWords[i])), word, sizeof(word));
                dataWords += fmt::format("{}{}", i == 0 ? "" : ", ", word);
            }
        }
        spdlog::info("UI message for MessageBoxMenu: type {}, data {}, text '{}', from {}{}; stack [{}]; data words [{}]", static_cast<int>(a3), a4 ? "yes" : "no", text, caller,
                     phantom ? "; dropped, the empty box that paused the world at load" : "", frames, dataWords);
        if (phantom)
            return;
    }
    // TEMPORARY (2026-09-20): the friend's name above their head is to come from the game's own world-space enemy
    // meter (WSEnemyMeters, built into Skyrim VR), the one NPCs get. Which message tells it whom to show, and who
    // sends it when the player hits an enemy, is unknown, so every message aimed at it is logged with the data's
    // first words (a vtable there names the HUDData type) and the caller stack. HUD Menu messages other than the
    // per-frame update are logged too, since the meter may listen to those. At most 20 lines a second.
    // The game pointing its enemy meter at someone: HUDData type 0xB with a non-zero actor handle at +0x28 (read from
    // the game's own sends on 2026-09-20). Remembered so the friend's meter never steals a real enemy's.
    if (a2 && a2->AsAscii() && strcmp(a2->AsAscii(), "WSEnemyMeters") == 0 && a3 == UIMessage::kUpdate && a4 && !s_sendingEnemyMeter && ReadableBytes(a4, 0x30) &&
        *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(a4) + 0x10) == 0xB && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(a4) + 0x28) != 0)
        s_lastGameEnemyMeterTargetAt = std::chrono::steady_clock::now();

    UIMessageQueue__AddMessage_Real(a1, a2, a3, a4);
}

void UI::SetEnemyMeterTarget(uint32_t aHandle, uint16_t aLevel)
{
#ifdef SKYRIMVR
    // The message the game sends from its hit handling (SE 0x1408D5130+0x284, VR +0x902ed4): a HUDData from the
    // queue's factory, type 0xB at +0x10, the actor's level at +0x20, two flag bytes 1,1 at +0x22, the actor handle
    // at +0x28 (0 to clear), queued as an update for WSEnemyMeters. EnemyHealth::Update then shows the meter itself.
    POINTER_SKYRIMSE(void*, s_queue, 400445, 400445);
    TP_THIS_FUNCTION(TCreateUIMessageData, void*, void, const BSFixedString*);
    POINTER_SKYRIMSE(TCreateUIMessageData, s_createData, 80061, 80061);
    void* pQueue = s_queue.Get() ? *s_queue.Get() : nullptr;
    if (!pQueue || !s_createData.Get() || !UIMessageQueue__AddMessage_Real)
        return;

    // The meter menu has to actually be there. Opening the journal or the console tears the HUD's sub-menus down
    // and builds them again, and for a moment "WSEnemyMeters" is gone while this driver keeps posting updates to
    // it -- each one allocating a HUDData from the queue's factory for a menu that cannot receive it. Seen's
    // session on 2026-09-25 is the shape of that: journal and console open for 33 s, closed at 20:50:46 leaving
    // the menu list as [HUD Menu] alone, three enemy-meter updates sent into the gap, and six seconds later an
    // access violation on `lock xadd [rcx+8], eax` with a junk pointer -- a reference count being decremented on
    // something already gone.
    BSFixedString meterMenu("WSEnemyMeters");
    UI* pUI = UI::Get();
    if (!pUI)
        return;

    if (!pUI->GetMenuOpen(meterMenu))
    {
        // ...but refusing and saying nothing is why the friend's bar "needs to be initialised once" (2026-09-26).
        // The meter is not part of the HUD until something has shown it: Emma's session of that morning started at
        // 10:43:50 and WSEnemyMeters first appeared in the menu list at 10:47:58, four minutes later, and was
        // missing from 205 of 575 probes -- a third of the session with no bar over anyone's head, whatever this
        // driver decided.
        //
        // So ask for it. kShow with no data is how the game opens a menu, and it allocates nothing from the
        // factory, which is what made the update above dangerous. If the menu comes up the next tick, 250 ms
        // later, finds it open and posts the real target; if it does not, nothing has changed and the log says so.
        if (aHandle == 0)
            return;

        static std::chrono::steady_clock::time_point s_nextShow{};
        const auto now = std::chrono::steady_clock::now();
        if (now < s_nextShow)
            return;
        s_nextShow = now + std::chrono::seconds(2);

        spdlog::info("Enemy meter: WSEnemyMeters is not open, asking the UI to show it");
        s_sendingEnemyMeter = true;
        UIMessageQueue__AddMessage_Real(pQueue, &meterMenu, UIMessage::kShow, nullptr);
        s_sendingEnemyMeter = false;
        return;
    }

    BSFixedString dataName("HUDData");
    void* pData = TiltedPhoques::ThisCall(s_createData, pQueue, &dataName);
    if (!pData)
        return;
    auto* pBytes = static_cast<uint8_t*>(pData);
    *reinterpret_cast<uint32_t*>(pBytes + 0x10) = 0xB;
    *reinterpret_cast<uint16_t*>(pBytes + 0x20) = aLevel;
    *reinterpret_cast<uint16_t*>(pBytes + 0x22) = 0x0101;
    *reinterpret_cast<uint32_t*>(pBytes + 0x28) = aHandle;

    s_sendingEnemyMeter = true;
    UIMessageQueue__AddMessage_Real(pQueue, &meterMenu, UIMessage::kUpdate, pData);
    s_sendingEnemyMeter = false;
#endif
}

std::chrono::steady_clock::time_point UI::LastGameEnemyMeterTargetAt()
{
    return s_lastGameEnemyMeterTargetAt;
}

static TiltedPhoques::Initializer s_s(
    []()
    {
        // Offsets inside the patched functions differ between builds. The VR ones were checked in the VR code: the
        // call to UI::AddToActiveQueue, the intro movie branch, and the menu mode jne in FavoritesHandler::CanProcess.
#ifdef SKYRIMVR
        constexpr size_t cAddToActiveQueueCall = 0x70C;
        constexpr size_t cStartupMovieBranch = 0x96;
#else
        constexpr size_t cAddToActiveQueueCall = 0x682;
        constexpr size_t cStartupMovieBranch = 0xFE;
#endif

        // Menus that don't pause the game while connected (see kAllowList).
        VersionDbPtr<uint8_t> ProcessHook(82082);
        TiltedPhoques::SwapCall(ProcessHook.Get() + cAddToActiveQueueCall, UI_AddToActiveQueue, &UI_AddToActiveQueue_Hook);

        // Ignore startup movie
        // TODO: Move me later.
        VersionDbPtr<uint8_t> MainInit(36548);
        TiltedPhoques::Put<uint8_t>(MainInit.Get() + cStartupMovieBranch, 0xEB);

        // Credits to Skyrim Souls RE for this fix.
        // Allows the favorites menu to be numbered during connect.
        VersionDbPtr<uint8_t> FavoritesCanProcess(51538);
        TiltedPhoques::Put<uint16_t>(FavoritesCanProcess.Get() + 0x15, 0x9090);

        // Some experiments:
#ifdef SKYRIMVR
        {
            POINTER_SKYRIMSE(TCallback, s_addMessage, 13530, 13631);
            if (s_addMessage.Get())
            {
                UIMessageQueue__AddMessage_Real = s_addMessage.Get();
                TP_HOOK(&UIMessageQueue__AddMessage_Real, UIMessageQueue__AddMessage);
                spdlog::info("UI message queue hooked for the message box diagnostic");
            }
            else
                spdlog::warn("UI message queue: id 13631 not resolved, message box diagnostic off");
        }
#endif

        // This kills the loading spinner
        // TiltedPhoques::Put<uint8_t>(0x1405D51C1, 0xEB);
        // TiltedPhoques::Nop(0x1405D51A2, 5);

        // use 8 threads by default!
        // TiltedPhoques::Put<uint8_t>(0x141E45770, 8);
    });
