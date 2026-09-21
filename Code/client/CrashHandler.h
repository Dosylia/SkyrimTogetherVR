#pragma once

//! A stretch of game code known to fault, wrapped by an __except that recovers from it (see the crime alarm in
//! Actor.cpp). Inside one, an access violation is expected and handled, so the crash handler must not spend a
//! second writing a report and a coredump for it, nor use up its one-shot report for a fault that is not fatal.
namespace CrashGuard
{
void Enter() noexcept;
void Leave() noexcept;
[[nodiscard]] bool Inside() noexcept;
} // namespace CrashGuard

class CrashHandler
{
    PVOID m_handler;
    static LPTOP_LEVEL_EXCEPTION_FILTER m_pUnhandled; // For remembering "original" UnhandledExceptionFilter

  public:
    CrashHandler();
    ~CrashHandler();

    static void RemovePreviousDump(std::filesystem::path path);
    static inline LPTOP_LEVEL_EXCEPTION_FILTER GetOriginalUnhandledExceptionFilter()
    {
        return m_pUnhandled;
    }
};
