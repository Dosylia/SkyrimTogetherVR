#include <BranchInfo.h>
#include "CrashHandler.h"
#include <DbgHelp.h>
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <strsafe.h>

using time_point = std::chrono::system_clock::time_point;

std::string SerializeTimePoint(const time_point& time, const std::string& format)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = *std::gmtime(&tt); // GMT (UTC)
    // std::tm tm = *std::localtime(&tt); //Locale time-zone, usually UTC by default.
    std::stringstream ss;
    ss << std::put_time(&tm, format.c_str());
    return ss.str();
}

// Names a code address as "module+offset", or "<alloc base>+offset" for memory without a module.
// The game image is mapped by our own PE loader into private memory, so it has no module name.
static void DescribeCodeAddress(void* apAddress, char* apOut, size_t aOutSize)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(apAddress, &mbi, sizeof(mbi)))
    {
        _snprintf_s(apOut, aOutSize, _TRUNCATE, "<unqueryable>");
        return;
    }

    char name[MAX_PATH];
    name[0] = '\0';

    HMODULE hModule = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(apAddress), &hModule) &&
        hModule && GetModuleFileNameA(hModule, name, sizeof(name)))
    {
        const char* pBase = strrchr(name, '\\');
        _snprintf_s(apOut, aOutSize, _TRUNCATE, "%s+0x%llx", pBase ? pBase + 1 : name,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(apAddress) -
                                                    reinterpret_cast<uintptr_t>(hModule)));
        return;
    }

    // The custom-loaded game image, or generated code (trampolines, hook thunks).
    _snprintf_s(apOut, aOutSize, _TRUNCATE, "<alloc 0x%llx>+0x%llx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mbi.AllocationBase)),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(apAddress) -
                                                reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
}

static bool IsExecutableAddress(void* apAddress)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(apAddress, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;

    const DWORD prot = mbi.Protect;
    if (prot & (PAGE_GUARD | PAGE_NOACCESS))
        return false;

    return (prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Logs registers and a raw stack scan, because minidumps of this process are mostly unusable.
// A real stack walk needs unwind info, which the custom-loaded game image doesn't have. Scanning
// the stack for pointers into executable memory recovers the return-address chain instead
// (with some false positives from stale slots).
static void LogCrashContext(PEXCEPTION_POINTERS pExceptionInfo)
{
    const auto* pRecord = pExceptionInfo->ExceptionRecord;
    const auto* pContext = pExceptionInfo->ContextRecord;
    char desc[MAX_PATH + 64];

    if (pRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2)
    {
        const ULONG_PTR kind = pRecord->ExceptionInformation[0];
        spdlog::error("  faulting access: {} at 0x{:x}",
                      kind == 0 ? "read from" : (kind == 1 ? "write to" : "execute at"),
                      static_cast<uint64_t>(pRecord->ExceptionInformation[1]));
    }

    DescribeCodeAddress(pRecord->ExceptionAddress, desc, sizeof(desc));
    spdlog::error("  rip 0x{:x}  ({})", static_cast<uint64_t>(pContext->Rip), desc);

    spdlog::error("  rax 0x{:016x}  rcx 0x{:016x}  rdx 0x{:016x}  rbx 0x{:016x}",
                  static_cast<uint64_t>(pContext->Rax), static_cast<uint64_t>(pContext->Rcx),
                  static_cast<uint64_t>(pContext->Rdx), static_cast<uint64_t>(pContext->Rbx));
    spdlog::error("  rsp 0x{:016x}  rbp 0x{:016x}  rsi 0x{:016x}  rdi 0x{:016x}",
                  static_cast<uint64_t>(pContext->Rsp), static_cast<uint64_t>(pContext->Rbp),
                  static_cast<uint64_t>(pContext->Rsi), static_cast<uint64_t>(pContext->Rdi));
    spdlog::error("  r8  0x{:016x}  r9  0x{:016x}  r10 0x{:016x}  r11 0x{:016x}",
                  static_cast<uint64_t>(pContext->R8), static_cast<uint64_t>(pContext->R9),
                  static_cast<uint64_t>(pContext->R10), static_cast<uint64_t>(pContext->R11));
    spdlog::error("  r12 0x{:016x}  r13 0x{:016x}  r14 0x{:016x}  r15 0x{:016x}",
                  static_cast<uint64_t>(pContext->R12), static_cast<uint64_t>(pContext->R13),
                  static_cast<uint64_t>(pContext->R14), static_cast<uint64_t>(pContext->R15));

    // Stay inside the thread's stack: the guard page below StackLimit would fault.
    const auto* pTib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    auto* pCursor = reinterpret_cast<uintptr_t*>(pContext->Rsp & ~static_cast<uintptr_t>(7));
    const auto* pStackTop = reinterpret_cast<const uintptr_t*>(pTib->StackBase);
    const auto* pStackLimit = reinterpret_cast<const uintptr_t*>(pTib->StackLimit);

    if (pCursor < pStackLimit || pCursor >= pStackTop)
    {
        spdlog::error("  stack scan skipped: rsp 0x{:x} outside thread stack [0x{:x}, 0x{:x})",
                      static_cast<uint64_t>(pContext->Rsp), reinterpret_cast<uint64_t>(pStackLimit),
                      reinterpret_cast<uint64_t>(pStackTop));
        return;
    }

    spdlog::error("  stack scan (rsp 0x{:x} .. 0x{:x}), executable addresses only:",
                  reinterpret_cast<uint64_t>(pCursor), reinterpret_cast<uint64_t>(pStackTop));

    constexpr size_t kMaxSlots = 4096; // 32KB of stack
    constexpr size_t kMaxHits = 48;

    size_t hits = 0;
    for (size_t slot = 0; slot < kMaxSlots && pCursor < pStackTop && hits < kMaxHits; ++slot, ++pCursor)
    {
        auto* pCandidate = reinterpret_cast<void*>(*pCursor);
        if (!IsExecutableAddress(pCandidate))
            continue;

        DescribeCodeAddress(pCandidate, desc, sizeof(desc));
        spdlog::error("    [rsp+0x{:04x}] 0x{:012x}  {}", slot * sizeof(uintptr_t),
                      reinterpret_cast<uint64_t>(pCandidate), desc);
        ++hits;
    }

    if (!hits)
        spdlog::error("    (no executable addresses found - stack likely corrupted)");
}

namespace
{
//! A stand-in for the actor the crime alarm walks into and finds missing. Every field reads as zero, and every
//! virtual call on it returns zero instead of jumping through a null vtable, so a walk that stumbles onto it
//! finishes quietly instead of ending the session. Built once, on first use.
void* NullActorDecoy() noexcept
{
    static void* s_pDecoy = []() -> void*
    {
        auto* pStub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!pStub)
            return nullptr;
        pStub[0] = 0x31; // xor eax, eax
        pStub[1] = 0xC0;
        pStub[2] = 0xC3; // ret            (the caller cleans up, so this is safe whatever the arguments were)
        DWORD previous = 0;
        if (!VirtualProtect(pStub, 0x1000, PAGE_EXECUTE_READ, &previous))
            return nullptr;

        static void* s_vtable[512];
        for (auto& entry : s_vtable)
            entry = pStub;

        static uint8_t s_object[0x1000]{};
        *reinterpret_cast<void**>(s_object) = s_vtable;
        return s_object;
    }();
    return s_pDecoy;
}
} // namespace

namespace CrashGuard
{
static thread_local int s_depth = 0;
void Enter() noexcept
{
    ++s_depth;
}
void Leave() noexcept
{
    if (s_depth > 0)
        --s_depth;
}
bool Inside() noexcept
{
    return s_depth > 0;
}
} // namespace CrashGuard

LONG WINAPI VectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo)
{
    // A fault inside a guarded section is caught and recovered further up this thread's stack. Reporting it would
    // cost a second of coredump and burn the one report kept for a fault that really is fatal.
    if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && CrashGuard::Inside())
        return EXCEPTION_CONTINUE_SEARCH;

    // The crime alarm (Actor::StealAlarm) walks every actor that might have witnessed an offence, and one slot in
    // that walk is empty: the game reads the missing actor's flags at +0xE8 through a null pointer. Four sessions
    // died this way on 2026-09-20 and 21, both players, always this instruction, always a read of 0xE8 from a null
    // RCX, and always inside code a hook has relocated, which has no unwind data, so no __try of ours can ever
    // catch it and the fault arrives here instead. Hand the read a stand-in that is all zeroes and let the walk
    // finish. What put the empty slot in the list is still unknown; this only stops it ending the session.
    {
        const auto& record = *pExceptionInfo->ExceptionRecord;
        static std::atomic<uint32_t> s_recovered{0};
        constexpr uint32_t cMaxRecoveries = 64; // a genuine loop still surfaces instead of being papered over
        if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2 &&
            record.ExceptionInformation[0] == 0 /* a read */ && record.ExceptionInformation[1] == 0xE8 /* of null + 0xE8 */ &&
            pExceptionInfo->ContextRecord->Rcx == 0)
        {
            const uint32_t count = ++s_recovered;
            void* pDecoy = count <= cMaxRecoveries ? NullActorDecoy() : nullptr;
            if (pDecoy)
            {
                if (count <= 5)
                    spdlog::warn("Recovered from the crime alarm's missing actor (read of null+0xE8 at {:#x}, occurrence {}). The session continues; the crime may not have been reported.",
                                 static_cast<uint64_t>(pExceptionInfo->ContextRecord->Rip), count);
                pExceptionInfo->ContextRecord->Rcx = reinterpret_cast<DWORD64>(pDecoy);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }

    // One gate per exception type: a recovered access violation must not use up the report for a
    // later, fatal /GS failure.
    static int alreadyCrashedAV = 0;
    static int alreadyCrashedGS = 0;
    auto retval = EXCEPTION_CONTINUE_SEARCH;

    // Serialize
    static std::mutex singleThreaded;
    const std::lock_guard lock{singleThreaded};

    const auto exceptionCode = pExceptionInfo->ExceptionRecord->ExceptionCode;
    const bool isNewAV = exceptionCode == EXCEPTION_ACCESS_VIOLATION && alreadyCrashedAV++ == 0;
    const bool isNewGS = exceptionCode == STATUS_STACK_BUFFER_OVERRUN && alreadyCrashedGS++ == 0;

    // Access violations, and /GS stack cookie failures (STATUS_STACK_BUFFER_OVERRUN), which used to
    // leave no trace at all.
    if (isNewAV || isNewGS)
    {
        spdlog::critical (__FUNCTION__ ": crash occurred!"); 
        
        spdlog::error(__FUNCTION__ ": exception code is {:x}, at address {}, flags {:x} ",
                      pExceptionInfo->ExceptionRecord->ExceptionCode,
                      pExceptionInfo->ExceptionRecord->ExceptionAddress,
                      pExceptionInfo->ExceptionRecord->ExceptionFlags);

        LogCrashContext(pExceptionInfo);

#if (IS_MASTER)
        volatile static bool bMiniDump = false;
#else
        volatile static bool bMiniDump = true;
#endif
        if (bMiniDump)
        {
            HANDLE hDumpFile = INVALID_HANDLE_VALUE;
            BOOL dumpWritten = FALSE;
            DWORD dumpError = 0;
            try
            {
                MINIDUMP_EXCEPTION_INFORMATION M;
                char dumpPath[MAX_PATH];

                M.ThreadId = GetCurrentThreadId();
                M.ExceptionPointers = pExceptionInfo;
                M.ClientPointers = 0;

                std::ostringstream oss;
                oss << "crash_" << SerializeTimePoint(std::chrono::system_clock::now(), "UTC_%Y-%m-%d_%H-%M-%S")
                    << ".dmp";

                GetModuleFileNameA(NULL, dumpPath, sizeof(dumpPath));
                std::filesystem::path modulePath(dumpPath);
                auto subPath = modulePath.parent_path();

                CrashHandler::RemovePreviousDump(subPath);

                subPath /= oss.str();

                hDumpFile = CreateFileA(subPath.string().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, NULL);
                if (hDumpFile == INVALID_HANDLE_VALUE)
                    dumpError = GetLastError();

                // baseline settings from https://stackoverflow.com/a/63123214/5273909
                // Larger dump types fail on this process because of the 1GB+ game image buffer (full
                // memory: ~7GB and minutes to write; indirect memory: truncated dumps). DataSegs is ~1GB
                // but holds the decrypted game code, the only copy that can be disassembled.
                auto dumpSettings = MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithThreadInfo;

                if (hDumpFile != INVALID_HANDLE_VALUE)
                {
                    dumpWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile,
                                                    (MINIDUMP_TYPE)dumpSettings, (pExceptionInfo) ? &M : NULL, NULL,
                                                    NULL);
                    if (!dumpWritten)
                        dumpError = GetLastError();
                }
            }
            catch (...) // Mini-dump is best effort only.
            {
            }

            // CreateFileA fails with INVALID_HANDLE_VALUE, not NULL: the old check always claimed success.
            if (hDumpFile == INVALID_HANDLE_VALUE)
                spdlog::critical(__FUNCTION__ ": coredump file could not be created (error {}).", dumpError);
            else
            {
                CloseHandle(hDumpFile);
                if (dumpWritten)
                    spdlog::critical(__FUNCTION__ ": coredump created -> flush logs.");
                else
                    spdlog::critical(__FUNCTION__ ": MiniDumpWriteDump failed (error {:#x}) -> rely on the logged "
                                     "crash context above.",
                                     dumpError);
            }
        }

        // Something in STR breaks top-level unhandled exception filters.
        // The Win API for them is pretty clunky (non-atomic, not chainable), 
        // but they can do some important things. If someone actually set one
        // they probably meant it; make sure it actually runs.
        // This will make more CrashLogger mods work with STR.

        // Get the current unhandled exception filter. If it has changed
        // from when STR started up, invoke it here.
        LPTOP_LEVEL_EXCEPTION_FILTER pCurrentUnhandledExceptionFilter = SetUnhandledExceptionFilter(CrashHandler::GetOriginalUnhandledExceptionFilter());
        SetUnhandledExceptionFilter(pCurrentUnhandledExceptionFilter);
        if (pCurrentUnhandledExceptionFilter != CrashHandler::GetOriginalUnhandledExceptionFilter())
        {
            spdlog::critical(__FUNCTION__ ": UnhandledExceptionFilter() workaround triggered.");

            singleThreaded.unlock();        // Might reenter, but is safe at this point.
            if ((*pCurrentUnhandledExceptionFilter)(pExceptionInfo) == EXCEPTION_CONTINUE_EXECUTION)
                retval = EXCEPTION_CONTINUE_EXECUTION;
            singleThreaded.lock();
        }

        spdlog::shutdown();
    }
    return retval;
}

LPTOP_LEVEL_EXCEPTION_FILTER CrashHandler::m_pUnhandled;
CrashHandler::CrashHandler()
{
    // Record the original (or as close as we can get) top-level unhandled exception handler.
    // We grab this so we can see if it is changed, presumably by a mod or even graphics drivers.
    // Something in STR breaks unhandled exception handling, so we'll fake it if necessary.
    // This is the only way to get the current setting, but the race is small.
    m_pUnhandled = SetUnhandledExceptionFilter(NULL);
    SetUnhandledExceptionFilter(m_pUnhandled);

    m_handler = AddVectoredExceptionHandler(1, &VectoredExceptionHandler);
}

CrashHandler::~CrashHandler()
{
}

void CrashHandler::RemovePreviousDump(std::filesystem::path path)
{
    // This is the game folder (the launcher spoofs the module path), so only delete our own dumps.
    for (auto& entry : std::filesystem::directory_iterator(path))
    {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file() && name.rfind("crash_UTC_", 0) == 0 && entry.path().extension() == ".dmp")
        {
            DeleteFileA(entry.path().string().c_str());
        }
    }
}
