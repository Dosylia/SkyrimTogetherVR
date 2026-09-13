// Address space reserved near the game image for SKSE plugin trampolines (VR).
//
// Plugin trampolines must be within +-2GB of the game image. Under this launcher the game's heaps
// fill that window before late plugins (DynDOLOD at kDataLoaded) create theirs, which kills the game.
//
// A pool just below the image is reserved at startup, so ordinary allocations can't use it.
// VirtualQuery reports the pool as free, and a VirtualAlloc at an explicit address inside it releases
// that piece, allocates, and reserves the rest again. Only allocators asking for an address near the
// image, i.e. trampolines, ever get pool memory.

#include <Windows.h>
#include <MinHook.h>

#include <array>
#include <cstdint>
#include <cstdio>

#ifdef SKYRIMVR

namespace
{
constexpr uintptr_t kImageBase = 0x140000000;
constexpr uintptr_t kPoolSize = 0x10000000;   // 256MB of address space (reserved only, no commit)
constexpr uintptr_t kGranularity = 0x10000;   // allocation granularity on x64 Windows
constexpr uintptr_t kWindow = 0x7FFF0000;     // stay inside the +-2GB rel32 range

struct Piece
{
    uintptr_t base = 0;
    uintptr_t end = 0; // exclusive; base == end means unused
};

std::array<Piece, 256> s_pieces{};
SRWLOCK s_lock = SRWLOCK_INIT;

LPVOID(WINAPI* RealVirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD) = nullptr;
SIZE_T(WINAPI* RealVirtualQuery)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T) = nullptr;

uintptr_t RoundDown(uintptr_t aValue) { return aValue & ~(kGranularity - 1); }
uintptr_t RoundUp(uintptr_t aValue) { return RoundDown(aValue + kGranularity - 1); }

bool ReservePiece(uintptr_t aBase, uintptr_t aEnd)
{
    if (aEnd <= aBase)
        return true;

    for (auto& piece : s_pieces)
    {
        if (piece.base == piece.end)
        {
            // Bypass the hook: this runs inside TP_VirtualAlloc with s_lock held.
            const auto pAlloc = RealVirtualAlloc ? RealVirtualAlloc : &VirtualAlloc;
            if (!pAlloc(reinterpret_cast<LPVOID>(aBase), aEnd - aBase, MEM_RESERVE, PAGE_NOACCESS))
                return false;
            piece = {aBase, aEnd};
            return true;
        }
    }
    return false; // out of piece slots - the remainder just becomes ordinary free space
}

// Caller holds the lock.
Piece* FindPiece(uintptr_t aAddress)
{
    for (auto& piece : s_pieces)
        if (piece.base != piece.end && aAddress >= piece.base && aAddress < piece.end)
            return &piece;
    return nullptr;
}

SIZE_T WINAPI TP_VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength)
{
    const SIZE_T result = RealVirtualQuery(lpAddress, lpBuffer, dwLength);
    if (!result || !lpBuffer || dwLength < sizeof(MEMORY_BASIC_INFORMATION))
        return result;

    AcquireSRWLockShared(&s_lock);
    if (const Piece* pPiece = FindPiece(reinterpret_cast<uintptr_t>(lpAddress)))
    {
        // Present the whole reserved piece as one free region.
        lpBuffer->BaseAddress = reinterpret_cast<PVOID>(pPiece->base);
        lpBuffer->AllocationBase = nullptr;
        lpBuffer->AllocationProtect = 0;
        lpBuffer->RegionSize = pPiece->end - pPiece->base;
        lpBuffer->State = MEM_FREE;
        lpBuffer->Protect = PAGE_NOACCESS;
        lpBuffer->Type = 0;
    }
    ReleaseSRWLockShared(&s_lock);
    return result;
}

LPVOID WINAPI TP_VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    if (!lpAddress || !dwSize)
        return RealVirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);

    const uintptr_t start = RoundDown(reinterpret_cast<uintptr_t>(lpAddress));
    const uintptr_t end = RoundUp(reinterpret_cast<uintptr_t>(lpAddress) + dwSize);

    AcquireSRWLockExclusive(&s_lock);
    Piece* pPiece = FindPiece(start);
    if (!pPiece || end > pPiece->end || !(flAllocationType & MEM_RESERVE))
    {
        // Not a new allocation inside the pool (e.g. a commit into memory already handed out).
        ReleaseSRWLockExclusive(&s_lock);
        return RealVirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
    }

    const Piece piece = *pPiece;
    *pPiece = {};
    VirtualFree(reinterpret_cast<LPVOID>(piece.base), 0, MEM_RELEASE);
    // Another thread could grab this range before the allocation below. The window is tiny and normal
    // allocations are placed far lower, so this is accepted.

    const LPVOID result = RealVirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);

    // Re-reserve what is left. If the allocation failed, put the whole piece back.
    if (result)
    {
        ReservePiece(piece.base, start);
        ReservePiece(end, piece.end);
    }
    else
    {
        ReservePiece(piece.base, piece.end);
    }
    ReleaseSRWLockExclusive(&s_lock);

    char msg[160];
    sprintf_s(msg, "NearImageReserve: served VirtualAlloc(%p, 0x%zx) from pool -> %p\n", lpAddress, dwSize, result);
    OutputDebugStringA(msg);
    return result;
}
} // namespace

// Called from main() before anything else allocates much.
bool NearImageReserveInit()
{
    // Scan down from the image for a free run of kPoolSize that stays inside the rel32 window.
    constexpr uintptr_t kLowest = kImageBase - kWindow + kPoolSize;
    for (uintptr_t top = kImageBase; top >= kLowest; top -= kPoolSize)
    {
        if (ReservePiece(top - kPoolSize, top))
            return true;
    }
    return false;
}

// Called from CoreStubsInit() after MH_Initialize and before MH_EnableHook.
bool NearImageReserveInstallHooks()
{
    return MH_CreateHookApi(L"KernelBase.dll", "VirtualAlloc", &TP_VirtualAlloc, reinterpret_cast<LPVOID*>(&RealVirtualAlloc)) == MH_OK &&
           MH_CreateHookApi(L"KernelBase.dll", "VirtualQuery", &TP_VirtualQuery, reinterpret_cast<LPVOID*>(&RealVirtualQuery)) == MH_OK;
}

#endif
