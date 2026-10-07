#pragma once

// A crash dump small enough to send with a crash report (the urSovngarde launcher takes it when the player agrees):
// every thread's stack and registers, the module list, and whatever memory the caller's callback adds (the
// crash handler adds the objects the crashing registers point at). Never the game's data segments, which are what
// make the full dump about 1 GB.
//
// Header-only so that the tests can write one of their own process.

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace SmallDump
{
// Older small dumps beyond this many are deleted: a report takes the newest two.
constexpr size_t cKeep = 3;

inline bool IsSmallDumpName(const std::wstring& acName) noexcept
{
    return acName.rfind(L"crash_UTC_", 0) == 0 && acName.size() > 10 && acName.ends_with(L".small.dmp");
}

// The launcher sends small dumps under 4 MB; above this a dump is written again with fewer stacks.
constexpr uint64_t cTarget = 3500ull * 1024;

namespace detail
{
// Wraps the caller's callback: the second, smaller pass keeps every thread and its registers but only the crashing
// thread's stack (the game runs a hundred threads or more; this test process a handful). Clearing a thread's
// ThreadWriteStack flag does not keep its stack out (tested 2026-10-07: same size); removing the stack's range
// does. So each other thread's stack range is noted as dbghelp reports the thread, and handed back when it asks
// for memory to remove, which it does after the threads.
struct Pass
{
    PMINIDUMP_CALLBACK_INFORMATION Inner;
    DWORD CrashingThread;
    bool OneStack;
    struct Range
    {
        ULONG64 Base;
        ULONG Size;
    } Stacks[512]{};
    size_t StackCount = 0;
    size_t NextRemoved = 0;
};

inline BOOL CALLBACK Callback(PVOID apParam, const PMINIDUMP_CALLBACK_INPUT apInput, PMINIDUMP_CALLBACK_OUTPUT apOutput) noexcept
{
    auto* pass = static_cast<Pass*>(apParam);
    if (pass->OneStack && apInput && apOutput)
    {
        if (apInput->CallbackType == ThreadCallback && apInput->Thread.ThreadId != pass->CrashingThread &&
            pass->StackCount < std::size(pass->Stacks))
        {
            const ULONG64 low = (std::min)(apInput->Thread.StackBase, apInput->Thread.StackEnd);
            const ULONG64 high = (std::max)(apInput->Thread.StackBase, apInput->Thread.StackEnd);
            if (high > low && high - low < 0x10000000)
                pass->Stacks[pass->StackCount++] = {low, static_cast<ULONG>(high - low)};
            return TRUE;
        }
        if (apInput->CallbackType == RemoveMemoryCallback)
        {
            if (pass->NextRemoved >= pass->StackCount)
                return FALSE; // nothing more to remove
            apOutput->MemoryBase = pass->Stacks[pass->NextRemoved].Base;
            apOutput->MemorySize = pass->Stacks[pass->NextRemoved].Size;
            ++pass->NextRemoved;
            return TRUE;
        }
    }
    if (pass->Inner && pass->Inner->CallbackRoutine)
        return pass->Inner->CallbackRoutine(pass->Inner->CallbackParam, apInput, apOutput);
    return TRUE;
}

inline uint64_t WriteOnce(const std::filesystem::path& acPath, PEXCEPTION_POINTERS apInfo, Pass& aPass, DWORD& aError) noexcept
{
    aError = 0;
    HANDLE file = CreateFileW(acPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        aError = GetLastError();
        return 0;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = GetCurrentThreadId();
    exception.ExceptionPointers = apInfo;
    exception.ClientPointers = FALSE;

    MINIDUMP_CALLBACK_INFORMATION callback{};
    callback.CallbackRoutine = &Callback;
    callback.CallbackParam = &aPass;

    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, apInfo ? &exception : nullptr,
                                           nullptr, &callback);
    if (!written)
        aError = GetLastError();

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    if (!written)
    {
        DeleteFileW(acPath.c_str());
        return 0;
    }
    return static_cast<uint64_t>(size.QuadPart);
}
} // namespace detail

// Writes `acPath` and returns its size in bytes, or 0 with `aError` set. When the whole dump comes out above
// `aTarget`, it is written again with only the crashing thread's stack. `apCallback` adds memory (its
// MemoryCallback); `apRestart` runs before each pass, so that it can hand out its memory again.
inline uint64_t Write(const std::filesystem::path& acPath, PEXCEPTION_POINTERS apInfo, PMINIDUMP_CALLBACK_INFORMATION apCallback,
                      DWORD& aError, void (*apRestart)() noexcept = nullptr, uint64_t aTarget = cTarget) noexcept
{
    detail::Pass pass{apCallback, GetCurrentThreadId(), false};
    if (apRestart)
        apRestart();
    const uint64_t size = detail::WriteOnce(acPath, apInfo, pass, aError);
    if (size == 0 || size <= aTarget)
        return size;
    pass.OneStack = true;
    if (apRestart)
        apRestart();
    return detail::WriteOnce(acPath, apInfo, pass, aError);
}

// Keeps the newest `cKeep` small dumps in `acFolder` and deletes the others (never any other file).
inline void Prune(const std::filesystem::path& acFolder) noexcept
{
    std::error_code ec;
    std::vector<std::filesystem::path> dumps;
    for (const auto& entry : std::filesystem::directory_iterator(acFolder, ec))
    {
        if (entry.is_regular_file(ec) && IsSmallDumpName(entry.path().filename().wstring()))
            dumps.push_back(entry.path());
    }
    // The UTC stamp in the name sorts by time.
    std::sort(dumps.begin(), dumps.end());
    while (dumps.size() > cKeep)
    {
        std::filesystem::remove(dumps.front(), ec);
        dumps.erase(dumps.begin());
    }
}
} // namespace SmallDump
