
#include <Games/Skyrim/BSSystem/BSThread.h>
#include <base/threading/ThreadUtils.h>

namespace
{
void (*BSThread_Initialize)(BSThread*, int, const char*){nullptr};

void Hook_BSThread_Initialize(BSThread* apThis, int aStackSize, const char* apName)
{
    BSThread_Initialize(apThis, aStackSize, apName);

    if (!apName && apThis->m_ThreadHandle)
        spdlog::warn("Unnamed thread started: {}", apThis->m_ThreadID);

    // this means the thread was successfully created
    if (apThis->m_ThreadHandle)
    {
        bool result = Base::SetThreadName(apThis->m_ThreadHandle, apName);
        if (!result)
        {
            spdlog::warn("Failed to set thread name for tid {} to {}", apThis->m_ThreadID, apName);
        }
    }
}

// bsthreadutils
// hook this in order to redirect the game to use the new naming apis (windows 10+)
void Hook_SetThreadName(uint32_t aThreadId, const char* apThreadName)
{
    // query thread handle
    if (auto hThread = ::OpenThread(THREAD_QUERY_INFORMATION, FALSE, aThreadId))
    {
        Base::SetThreadName(hThread, apThreadName);

        ::CloseHandle(hThread);
    }
    else
        spdlog::warn("Hook_SetThreadName(): Unable to query thread handle :(");
}

// experimental stuff..
#if 0
typedef struct
{
    DWORD dwType;     // Must be 0x1000.
    LPCSTR szName;    // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags;    // Reserved for future use, must be zero.
} THREADNAME_INFO;

void Hook_RaiseException(DWORD, DWORD, DWORD, const THREADNAME_INFO* lpArguments)
{
    base::SetCurrentThreadName(lpArguments->szName);
}

HANDLE WINAPI Hook_CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                                LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags,
                                LPDWORD lpThreadId)
{
    auto hThread =
        CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);

    if (hThread && lpThreadId)
    {
        auto name = fmt::format("BNetThread{}", *lpThreadId);
        base::SetThreadName(hThread, name.c_str());
    }

    return hThread;
}
#endif
} // namespace

static TiltedPhoques::Initializer s_BSThreadInit(
    []()
    {
        #ifndef SKYRIMVR
        const VersionDbPtr<uint8_t> threadInit(68261);
        BSThread_Initialize = static_cast<decltype(BSThread_Initialize)>(threadInit.GetPtr());
        // need to detour this for now :/
        TP_HOOK_IMMEDIATE(&BSThread_Initialize, &Hook_BSThread_Initialize);
        #else
        // Not hooked on VR. 68261 resolved through the crosswalk to 0xc80600,
        // which is a small lock/flag function (EnterCriticalSection on rdx+0x18),
        // not BSThread::Initialize. Detouring it made Hook_BSThread_Initialize
        // call Base::SetThreadName with a garbage "name" (0x768) while loading a
        // save. The hook only names threads for debugging, and the game names
        // them itself via BSThreadUtils::SetThreadName, so nothing is lost
        // until the real VR address is found.
        #endif

        #ifndef SKYRIMVR
        const VersionDbPtr<uint8_t> setThreadName(69066);
        #else
        const VersionDbPtr<uint8_t> setThreadName(69066);
        #endif
        // VR: 69066 now maps to 0xc6b170 (VRAddressOverrides.h). The old crosswalk
        // value 0xca3800 was a destructor; this Jump overwrote it. 0xc6b170 was
        // found by searching the decrypted game for "mov ecx, 406D1388h": it
        // builds THREADNAME_INFO{0x1000, name, threadId, 0} from (ecx, rdx) and
        // raises it, i.e. SetThreadName(uint32_t, const char*), and it is 0x40
        // bytes like AE 69066.
        if (setThreadName.Get())
            TiltedPhoques::Jump(setThreadName.Get(), &Hook_SetThreadName);

#if 0
    #ifndef SKYRIMVR
    const VersionDbPtr<uint8_t> createHavokThread(57704);
    #else
    const VersionDbPtr<uint8_t> createHavokThread(57704);
    #endif
    // relatively safe to do, since this is unlikely to ever change, as beth wont update havok
    TiltedPhoques::Nop(createHavokThread.Get() + 0x81, 6);

    TiltedPhoques::PutCall(createHavokThread.Get() + 0x81, &Hook_RaiseException);


    TiltedPhoques::Nop(0x1411F0FD4, 6);
    TiltedPhoques::PutCall(0x1411F0FD4, &Hook_CreateThread);
#endif
    });
