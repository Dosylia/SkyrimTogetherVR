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

// Describes a code address: which allocation it lives in, and which file that
// allocation was mapped from (if any). The game is mapped by our own custom PE
// loader into a plain private buffer, so it has no module entry and
// GetModuleFileName/SymFromAddr can't name it - GetMappedFileNameA returns
// nothing for private memory either. Reporting the allocation base is still
// enough to tell "our exe" from "the custom-loaded game image" from "a real
// DLL", which is the distinction that matters when reading the scan below.
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

    // Named module (our exe, a DLL) - report module-relative offset, which is
    // what a .pdb lookup needs.
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

    // Unnamed executable memory: the custom-loaded game image, or generated
    // code (our own CodeGenerator trampolines / MinHook thunks both live here).
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

// The minidumps for this process are unusable in practice (see the dump-flag
// comment further down), so the crash context has to reach the log directly.
// Two parts:
//   * registers + the faulting access, so a bad pointer can be recognised
//     on sight (our recurring failures all involve a garbage `this` in rcx);
//   * a raw stack scan for executable addresses, standing in for a real stack
//     walk. RtlVirtualUnwind/StackWalk64 need .pdata unwind info, and the
//     custom PE loader maps the game without any, so a proper walk stops at
//     the first game frame. Scanning every qword between rsp and the thread's
//     stack base and keeping the ones that point into committed executable
//     memory recovers the return-address chain (plus some false positives from
//     stale slots - order and plausibility still make the real chain readable).
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

    // Thread stack bounds from the TIB - the scan must not walk off the end of
    // the stack (the guard page below StackLimit would fault, and past
    // StackBase isn't ours).
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

    constexpr size_t kMaxSlots = 4096; // 32KB of stack - deep enough for a load-time call chain
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

LONG WINAPI VectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo)
{
    // Separate one-shot gates per exception code: an access violation that a
    // downstream __except recovers from must not consume the single dump
    // opportunity a later, actually-fatal STATUS_STACK_BUFFER_OVERRUN needs -
    // that's exactly what was happening (AV dumped and recovered, the /GS
    // failure that followed it got silently skipped every time).
    static int alreadyCrashedAV = 0;
    static int alreadyCrashedGS = 0;
    auto retval = EXCEPTION_CONTINUE_SEARCH;

    // Serialize
    static std::mutex singleThreaded;
    const std::lock_guard lock{singleThreaded};

    const auto exceptionCode = pExceptionInfo->ExceptionRecord->ExceptionCode;
    const bool isNewAV = exceptionCode == EXCEPTION_ACCESS_VIOLATION && alreadyCrashedAV++ == 0;
    const bool isNewGS = exceptionCode == STATUS_STACK_BUFFER_OVERRUN && alreadyCrashedGS++ == 0;

    // Check for severe, not continuable and not software-originated exception.
    // Also catch STATUS_STACK_BUFFER_OVERRUN (/GS cookie failures, raised via
    // __fastfail) - these were previously invisible here (no dump, no WER
    // detail beyond a bare fault offset), making stack-corruption bugs
    // essentially undiagnosable.
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
                //
                // Keep this minimal. Every larger setting has been tried on this
                // process and failed, because of the 1GB+ buffer the custom PE
                // loader reserves for the game image:
                //   * MiniDumpWithFullMemory: ~7GB, ~3 minutes - indistinguishable
                //     from a hang, and killed mid-write in practice.
                //   * MiniDumpWithIndirectlyReferencedMemory: chases pointers into
                //     that same buffer, producing ~1GB dumps that cdb then rejects
                //     as truncated ("Memory range data only partially present"),
                //     and sometimes a 0-byte file. Unusable either way.
                // MiniDumpNormal always includes thread contexts and stacks, which
                // is what a call stack needs. MiniDumpWithDataSegs is NOT cheap
                // here (~1GB: our data segment is the game_seg buffer), but it
                // is valid and it holds the decrypted game code - the on-disk
                // SkyrimVR.exe is SteamStub-encrypted, so this dump is the only
                // place the game can be disassembled. Keep it. Anything the dump can't hold is
                // covered by LogCrashContext() above, which logs registers and a
                // stack scan and does not depend on the dump succeeding at all.
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

            // CreateFileA reports failure as INVALID_HANDLE_VALUE (-1), not NULL -
            // the old "if (!hDumpFile)" test therefore took the success branch for
            // both outcomes and logged "coredump created" even when no file was
            // ever opened. That claim sent several debugging sessions looking for
            // dumps that did not exist; report what actually happened instead.
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
    for (auto& entry : std::filesystem::directory_iterator(path))
    {
        if (entry.path().string().find("crash") != std::string::npos)
        {
            DeleteFileA(entry.path().string().c_str());
        }
    }
}
