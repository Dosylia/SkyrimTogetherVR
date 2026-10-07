#include <RecentDeletes.h>
#include <BranchInfo.h>
#include "CrashHandler.h"
#include <DbgHelp.h>
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <strsafe.h>
#include <Games/Memory.h>
#include <SmallDump.h>

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

// Capture the objects the crashing registers point at, and nothing else.
//
// On 2026-09-27 Seen's dump answered the big question -- not one frame of our code anywhere on the 583-slot
// stack, the whole fault is inside the game's image -- and then stopped dead at the next one. The fault is a
// virtual call through a null slot, and the object it was called on (Rcx = 0x9d48e610) was **not in the dump**:
// MiniDumpNormal keeps stacks and data segments, not the heap. Without that object there is no way to see
// whether it had been freed, which is the whole question.
//
// Full memory is 7 GB and minutes to write, and indirect-memory produced truncated dumps (see below), so
// neither is an option. This is the narrow version: a few kilobytes around each pointer-looking register, plus
// the vtable each one points to. A handful of small ranges, added only at crash time.
namespace
{
struct ExtraRange
{
    ULONG64 Base;
    ULONG Size;
};

ExtraRange g_extraRanges[24]{};
size_t g_extraCount = 0;
size_t g_extraNext = 0;

bool RangeIsReadable(ULONG64 aAddress, SIZE_T aSize) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(aAddress), &info, sizeof(info)))
        return false;
    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) || (info.Protect & PAGE_NOACCESS))
        return false;
    return aAddress + aSize <= reinterpret_cast<ULONG64>(info.BaseAddress) + info.RegionSize;
}

void AddExtraRange(ULONG64 aAddress, ULONG aSize) noexcept
{
    if (g_extraCount >= std::size(g_extraRanges) || aAddress < 0x10000)
        return;
    // Page-align downwards so the whole object is caught whatever the pointer pointed into.
    const ULONG64 base = aAddress & ~static_cast<ULONG64>(0xFFF);
    for (size_t i = 0; i < g_extraCount; ++i)
        if (g_extraRanges[i].Base == base)
            return;
    if (!RangeIsReadable(base, aSize))
        return;
    g_extraRanges[g_extraCount++] = {base, aSize};
}

void CollectCrashObjects(PEXCEPTION_POINTERS apInfo) noexcept
{
    g_extraCount = 0;
    g_extraNext = 0;
    if (!apInfo || !apInfo->ContextRecord)
        return;

    const CONTEXT& c = *apInfo->ContextRecord;
    const ULONG64 candidates[] = {c.Rcx, c.Rdx, c.Rbx, c.Rsi, c.Rdi, c.Rbp, c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15};

    for (const ULONG64 value : candidates)
    {
        AddExtraRange(value, 0x1000);

        // And whatever its first qword points at -- for an object that is its vtable, which is the thing worth
        // reading: a freed object has a null or foreign one.
        if (!RangeIsReadable(value, sizeof(ULONG64)))
            continue;
        ULONG64 first = 0;
        std::memcpy(&first, reinterpret_cast<const void*>(value), sizeof(first));
        AddExtraRange(first, 0x1000);
    }
}

BOOL CALLBACK CrashDumpCallback(PVOID, const PMINIDUMP_CALLBACK_INPUT apInput, PMINIDUMP_CALLBACK_OUTPUT apOutput) noexcept
{
    if (!apInput || !apOutput)
        return FALSE;

    if (apInput->CallbackType == MemoryCallback)
    {
        if (g_extraNext >= g_extraCount)
        {
            apOutput->MemoryBase = 0;
            apOutput->MemorySize = 0;
            return FALSE; // done adding
        }
        apOutput->MemoryBase = g_extraRanges[g_extraNext].Base;
        apOutput->MemorySize = g_extraRanges[g_extraNext].Size;
        ++g_extraNext;
        return TRUE;
    }

    return TRUE;
}
} // namespace

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

        // Which object the faulting code was working on, against the ones we deleted. Rcx holds `this` for a
        // virtual call in the Microsoft x64 convention, which is what these crashes have been.
        if (pExceptionInfo && pExceptionInfo->ContextRecord)
        {
            const CONTEXT& cr = *pExceptionInfo->ContextRecord;
            const uint64_t regs[] = {cr.Rax, cr.Rcx, cr.Rdx, cr.Rbx, cr.Rsi, cr.Rdi, cr.Rbp,
                                     cr.R8,  cr.R9,  cr.R10, cr.R11, cr.R12, cr.R13, cr.R14, cr.R15};
            RecentDeletes::Report(regs, std::size(regs));
        }

        // The small dump first, in every build: it takes a moment, and it is the one a player can send with a
        // report (the launcher takes it from logs\ when they agree). Stacks, registers, modules and the objects the
        // crashing registers point at; not the ~1 GB of game data below.
        try
        {
            CollectCrashObjects(pExceptionInfo);
            MINIDUMP_CALLBACK_INFORMATION smallCallback{};
            smallCallback.CallbackRoutine = &CrashDumpCallback;
            const auto logs = TiltedPhoques::GetPath() / "logs";
            const auto name = "crash_" + SerializeTimePoint(std::chrono::system_clock::now(), "UTC_%Y-%m-%d_%H-%M-%S") + ".small.dmp";
            DWORD smallError = 0;
            // A second, smaller pass (when the first is too big to send) needs the crash objects handed out again.
            const uint64_t smallSize = SmallDump::Write(logs / name, pExceptionInfo, &smallCallback, smallError, []() noexcept { g_extraNext = 0; });
            if (smallSize)
            {
                spdlog::critical(__FUNCTION__ ": small crash dump {} ({} KB)", name, smallSize / 1024);
                SmallDump::Prune(logs);
            }
            else
                spdlog::critical(__FUNCTION__ ": small crash dump could not be written (error {:#x})", smallError);
        }
        catch (...) // best effort, like the full dump
        {
        }

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
                    CollectCrashObjects(pExceptionInfo);
                    MINIDUMP_CALLBACK_INFORMATION callback{};
                    callback.CallbackRoutine = &CrashDumpCallback;
                    callback.CallbackParam = nullptr;

                    dumpWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile,
                                                    (MINIDUMP_TYPE)dumpSettings, (pExceptionInfo) ? &M : NULL, NULL,
                                                    &callback);
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

namespace FreezeWatchdog
{
namespace
{
std::atomic<int64_t> s_lastBeatMs{0};
std::atomic<DWORD> s_updateThreadId{0};

int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The game's main thread: the oldest thread of the process (the launcher's, which runs the game's WinMain). In the
// freeze of 2026-10-03 12:07 it was waiting inside the game's allocator while every other thread waited too.
DWORD FindMainThreadId() noexcept
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return 0;
    DWORD oldestId = 0;
    ULONGLONG oldestTime = ~0ull;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(hSnapshot, &entry); ok; ok = Thread32Next(hSnapshot, &entry))
    {
        if (entry.th32OwnerProcessID != GetCurrentProcessId())
            continue;
        HANDLE hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!hThread)
            continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetThreadTimes(hThread, &created, &exited, &kernel, &user))
        {
            const ULONGLONG cTime = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            if (cTime < oldestTime)
            {
                oldestTime = cTime;
                oldestId = entry.th32ThreadID;
            }
        }
        CloseHandle(hThread);
    }
    CloseHandle(hSnapshot);
    return oldestId;
}

uint32_t CountThreads() noexcept
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return 0;
    uint32_t count = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(hSnapshot, &entry); ok; ok = Thread32Next(hSnapshot, &entry))
        if (entry.th32OwnerProcessID == GetCurrentProcessId())
            ++count;
    CloseHandle(hSnapshot);
    return count;
}

// Where a thread is, without touching anything it may hold: it is suspended only for as long as it takes to copy its
// registers and the top of its stack into a buffer that exists already. Nothing is allocated or logged meanwhile (the
// thread may hold the heap lock or the logger's), and the copy is made with ReadProcessMemory, which fails instead of
// faulting on a page that is not there.
void LogThread(const char* apWhat, const DWORD aThreadId) noexcept
{
    if (!aThreadId || aThreadId == GetCurrentThreadId())
        return;
    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, aThreadId);
    if (!hThread)
    {
        spdlog::error("FreezeWatchdog: could not open the {} (thread {})", apWhat, aThreadId);
        return;
    }

    static uintptr_t s_stack[4096]; // 32 KB, as much as the crash report scans
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    SIZE_T copied = 0;
    bool haveContext = false;
    if (SuspendThread(hThread) != static_cast<DWORD>(-1))
    {
        haveContext = GetThreadContext(hThread, &context) != FALSE;
        if (haveContext)
        {
            // Shrink the window until it reads: the stack ends somewhere above rsp.
            for (SIZE_T size = sizeof(s_stack); size >= 4096 && copied == 0; size /= 2)
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rsp & ~static_cast<DWORD64>(7)), s_stack, size, &copied))
                    copied = 0;
        }
        ResumeThread(hThread);
    }
    CloseHandle(hThread);

    if (!haveContext)
    {
        spdlog::error("FreezeWatchdog: could not read where the {} is (thread {})", apWhat, aThreadId);
        return;
    }

    char desc[MAX_PATH + 64];
    DescribeCodeAddress(reinterpret_cast<void*>(context.Rip), desc, sizeof(desc));
    spdlog::error("FreezeWatchdog: the {} (thread {}) is at rip 0x{:x} ({})", apWhat, aThreadId, static_cast<uint64_t>(context.Rip), desc);
    spdlog::error("  rax 0x{:016x}  rcx 0x{:016x}  rdx 0x{:016x}  rbx 0x{:016x}  rsp 0x{:016x}", static_cast<uint64_t>(context.Rax),
                  static_cast<uint64_t>(context.Rcx), static_cast<uint64_t>(context.Rdx), static_cast<uint64_t>(context.Rbx), static_cast<uint64_t>(context.Rsp));
    size_t hits = 0;
    for (size_t slot = 0; slot < copied / sizeof(uintptr_t) && hits < 40; ++slot)
    {
        auto* pCandidate = reinterpret_cast<void*>(s_stack[slot]);
        if (!IsExecutableAddress(pCandidate))
            continue;
        DescribeCodeAddress(pCandidate, desc, sizeof(desc));
        spdlog::error("    [rsp+0x{:04x}] 0x{:012x}  {}", slot * sizeof(uintptr_t), reinterpret_cast<uint64_t>(pCandidate), desc);
        ++hits;
    }
}

void LogHealth(const int64_t aSinceBeatMs) noexcept
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    const bool cHaveMemory = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE;
    spdlog::info("Health: private {} MB, working set {} MB, {} threads, last frame {} ms ago", cHaveMemory ? counters.PrivateUsage / (1024 * 1024) : 0,
                 cHaveMemory ? counters.WorkingSetSize / (1024 * 1024) : 0, CountThreads(), aSinceBeatMs);
}

DWORD WINAPI WatchdogMain(LPVOID) noexcept
{
    int64_t nextHealthMs = NowMs() + 30000;
    int64_t reportedForBeat = -1;
    int reportsForBeat = 0;
    for (;;)
    {
        Sleep(1000);
        if (IsProcessExiting())
            return 0;
        const int64_t cBeat = s_lastBeatMs.load(std::memory_order_relaxed);
        if (!cBeat)
            continue; // the game has not run a frame of ours yet
        const int64_t cNow = NowMs();
        const int64_t cSince = cNow - cBeat;

        if (cNow >= nextHealthMs)
        {
            nextHealthMs = cNow + 30000;
            LogHealth(cSince);
        }

        // Once at 25 s and once more at 2 minutes for the same stall. A long loading screen also gets here.
        if (cBeat != reportedForBeat)
        {
            reportedForBeat = cBeat;
            reportsForBeat = 0;
        }
        const int64_t cThreshold = reportsForBeat == 0 ? 25000 : 120000;
        if (cSince < cThreshold || reportsForBeat >= 2)
            continue;
        ++reportsForBeat;
        spdlog::error("FreezeWatchdog: no frame of ours for {} s (a long loading screen does this too); where the game is:", cSince / 1000);
        LogHealth(cSince);
        const DWORD cMain = FindMainThreadId();
        LogThread("game's main thread", cMain);
        const DWORD cUpdate = s_updateThreadId.load(std::memory_order_relaxed);
        if (cUpdate != cMain)
            LogThread("thread that last ran our update", cUpdate);
        spdlog::default_logger()->flush();
    }
}
} // namespace

void Beat() noexcept
{
    s_lastBeatMs.store(NowMs(), std::memory_order_relaxed);
    s_updateThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

void Start() noexcept
{
    if (HANDLE hWatchdog = CreateThread(nullptr, 0, &WatchdogMain, nullptr, 0, nullptr))
        CloseHandle(hWatchdog);
}
} // namespace FreezeWatchdog

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

    FreezeWatchdog::Start();
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
