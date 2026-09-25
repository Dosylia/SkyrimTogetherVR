#include <TiltedOnlinePCH.h>

#include <Games/Memory.h>
#include <Games/References.h>

#include <TiltedCore/MimallocAllocator.hpp>
#include <mimalloc.h>

#pragma optimize("", off)

struct GameHeap
{
    static GameHeap* Get()
    {
        POINTER_SKYRIMSE(GameHeap, s_gameHeap, 400188, 400188);

        return s_gameHeap.Get();
    }
};

TP_THIS_FUNCTION(TFormAllocate, void*, GameHeap, size_t aSize, size_t aAlignment, bool aAligned);
TP_THIS_FUNCTION(TFormFree, void, GameHeap, void* apPtr, bool aAligned);

TFormAllocate* RealFormAllocate = nullptr;
TFormFree* RealFormFree = nullptr;

static TiltedPhoques::MimallocAllocator s_allocator;

void* TP_MAKE_THISCALL(HookFormAllocate, GameHeap, size_t aSize, size_t aAlignment, bool aAligned)
{
    switch (aSize)
    {
    case sizeof(Actor): aSize = sizeof(ExActor); break;
    case sizeof(PlayerCharacter): aSize = sizeof(ExPlayerCharacter); break;
    default: break;
    }

    auto* pPointer = TiltedPhoques::ThisCall(RealFormAllocate, apThis, aSize, aAlignment, aAligned);

    if (!pPointer)
        return nullptr;

    ActorExtension* pExtension = nullptr;

    switch (aSize)
    {
    case sizeof(ExActor): pExtension = static_cast<ActorExtension*>(static_cast<ExActor*>(pPointer)); break;
    case sizeof(ExPlayerCharacter): pExtension = static_cast<ActorExtension*>(static_cast<ExPlayerCharacter*>(pPointer)); break;
    default: break;
    }

    if (pExtension)
    {
        new (pExtension) ActorExtension;
    }

    return pPointer;
}

void* Memory::Allocate(const size_t aSize) noexcept
{
    return TiltedPhoques::ThisCall(HookFormAllocate, GameHeap::Get(), aSize, 0, false);
}

void Memory::Free(void* apData) noexcept
{
    TiltedPhoques::ThisCall(RealFormFree, GameHeap::Get(), apData, false);
}

static bool IsFormAllocateReplacedByEF(TFormAllocate** appOutEngineFixesAlloc) noexcept
{
    POINTER_SKYRIMSE(TFormAllocate, s_formAllocate, 68115, 66859);
    TFormAllocate* pFormAllocate = s_formAllocate.Get();

    auto opcodeBytes = reinterpret_cast<uint16_t*>(*pFormAllocate);
    uint8_t shift = 0;

    if (*opcodeBytes == 0x25FF) // 'jmp' opcode 'FF 25' and the 4-byte displacement bytes come before the virtual address we're after
        shift = 6;
    else if (*opcodeBytes == 0xB848) // 'mov' opcode '48 B8' comes before the virtual address we're after
        shift = 2;

    auto possibleEfAllocAddress = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(*pFormAllocate) + shift);

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)possibleEfAllocAddress, &mbi, sizeof(mbi)) != 0)
    {
        if (mbi.AllocationBase == GetModuleHandleW(L"EngineFixes.dll"))
        {
            *appOutEngineFixesAlloc = reinterpret_cast<TFormAllocate*>(possibleEfAllocAddress);
            return true;
        }
    }
    return false;
}

static void RehookFormAllocate(TFormAllocate* apEngineFixesAllocate) noexcept
{
    RealFormAllocate = apEngineFixesAllocate;
    TP_HOOK_IMMEDIATE(&RealFormAllocate, HookFormAllocate);
}

// Set when the game calls exit (hooked in its import table below). The game's exit handlers and plugin DLL detach
// then free static data, including a plugin string that isn't a heap block, and mimalloc crashed on it every time
// the game was closed (a 100 MB crash dump each quit). Memory released while the process exits doesn't need to go
// back to the heap. An atexit handler was not enough: the game registers its own exit handlers later, so they ran
// first.
static std::atomic<bool> s_processExiting{false};

bool IsProcessExiting() noexcept
{
    return s_processExiting.load(std::memory_order_relaxed);
}

// The real CRT entries, so a pointer that is not ours can be given back to whoever really owns it.
//
// These hooks sit on the game's import table and send its allocations to mimalloc, but the game is not the only
// thing in this process and mimalloc has not always been here. A block allocated before these hooks went in, or by
// a DLL carrying its own copy of the CRT, is still freed through the game's imports -- and mimalloc, handed a
// pointer it never allocated, walks it as though it were one of its own.
//
// Seen's crash of 2026-09-25 21:26 is that, during play rather than at exit: `_mi_free_delayed_block` reached
// through `Hook_aligned_free`, dereferencing 0x7ffa2f938d28 -- an address in the region where **DLLs are mapped**,
// not where any heap lives. The existing exit-time guard below was written for the same failure ("a plugin string
// that isn't a heap block"); it was simply never true that this only happens while quitting.
using TRealFree = void(__cdecl*)(void*);
using TRealMsize = size_t(__cdecl*)(void*);
static TRealFree s_realFree = nullptr;
static TRealFree s_realAlignedFree = nullptr;
static TRealMsize s_realMsize = nullptr;

static void ResolveRealHeapEntries() noexcept
{
    HMODULE crt = GetModuleHandleA("ucrtbase.dll");
    if (!crt)
        crt = GetModuleHandleA("api-ms-win-crt-heap-l1-1-0.dll");
    if (!crt)
        return;

    s_realFree = reinterpret_cast<TRealFree>(GetProcAddress(crt, "free"));
    s_realAlignedFree = reinterpret_cast<TRealFree>(GetProcAddress(crt, "_aligned_free"));
    s_realMsize = reinterpret_cast<TRealMsize>(GetProcAddress(crt, "_msize"));
}

//! Whether mimalloc allocated this, and so whether mimalloc may be asked about it.
static inline bool IsOurs(const void* apData) noexcept
{
    return apData == nullptr || mi_is_in_heap_region(apData);
}

size_t Hook_msize(void* apData)
{
    if (!IsOurs(apData))
        return s_realMsize ? s_realMsize(apData) : 0;

    return mi_malloc_size(apData);
}

void Hookfree(void* apData)
{
    if (s_processExiting.load(std::memory_order_relaxed))
        return;

    if (!IsOurs(apData))
    {
        if (s_realFree)
            s_realFree(apData);
        return;
    }

    mi_free(apData);
}

void* Hookcalloc(size_t aCount, size_t aSize)
{
    return mi_calloc(aCount, aSize);
}

void* Hookmalloc(size_t aSize)
{
    return mi_malloc(aSize);
}

void Hook_aligned_free(void* apData)
{
    if (s_processExiting.load(std::memory_order_relaxed))
        return;

    if (!IsOurs(apData))
    {
        if (s_realAlignedFree)
            s_realAlignedFree(apData);
        return;
    }

    mi_free(apData);
}

void* Hook_aligned_malloc(size_t aSize, size_t aAlignment)
{
    return mi_malloc_aligned(aSize, aAlignment);
}

static TiltedPhoques::Initializer s_memoryHooks(
    []()
    {
        POINTER_SKYRIMSE(TFormAllocate, s_formAllocate, 68115, 66859);

        POINTER_SKYRIMSE(TFormFree, s_formFree, 68117, 66861);

        RealFormAllocate = s_formAllocate.Get();
        RealFormFree = s_formFree.Get();

        using T_msize = decltype(&Hook_msize);
        using Tfree = decltype(&Hookfree);
        using Tcalloc = decltype(&Hookcalloc);
        using Tmalloc = decltype(&Hookmalloc);
        using T_aligned_malloc = decltype(&Hook_aligned_malloc);
        using T_aligned_free = decltype(&Hook_aligned_free);
        T_msize Real_msize = nullptr;
        Tfree Realfree = nullptr;
        Tcalloc Realcalloc = nullptr;
        Tmalloc Realmalloc = nullptr;
        T_aligned_malloc Real_aligned_malloc = nullptr;
        T_aligned_free Real_aligned_free = nullptr;

        // Before the hooks, so a foreign pointer arriving at the very first free already has somewhere to go.
        ResolveRealHeapEntries();

        const char* cModuleName = "api-ms-win-crt-heap-l1-1-0.dll";

        TP_HOOK_IAT(_msize, cModuleName);
        TP_HOOK_IAT(free, cModuleName);
        TP_HOOK_IAT(calloc, cModuleName);
        TP_HOOK_IAT(malloc, cModuleName);
        TP_HOOK_IAT(_aligned_malloc, cModuleName);
        TP_HOOK_IAT(_aligned_free, cModuleName);

        TP_HOOK(&RealFormAllocate, HookFormAllocate);
    });

using T_initterm_e = decltype(&_initterm_e);
T_initterm_e Real_initterm_e = nullptr;

// If EngineFixes loaded, and it changed our FormAllocate hook, 
// reset it. Our hook works just fine chaining to theirs.
int __cdecl Hook_initterm_e(_PIFV* apFirst, _PIFV* apLast)
{
    // We want to run last, so pre-chain.
    auto retval = Real_initterm_e(apFirst, apLast); 
    
    // Check if EngineFixes messed with STR's modified alloc hook; if it did, treat EF as truth and rehook
    TFormAllocate* pEngineFixesAllocate = nullptr;
    if (GetModuleHandleW(L"EngineFixes.dll") && IsFormAllocateReplacedByEF(&pEngineFixesAllocate))
        RehookFormAllocate(pEngineFixesAllocate);

    return retval;
}

using Texit = void(__cdecl*)(int);
using T_exit = void(__cdecl*)(int);
using T_cexit = void(__cdecl*)();
Texit Realexit = nullptr;
T_exit Real_exit = nullptr;
T_cexit Real_cexit = nullptr;

void __cdecl Hookexit(int aCode)
{
    s_processExiting = true;
    Realexit(aCode);
}

void __cdecl Hook_exit(int aCode)
{
    s_processExiting = true;
    Real_exit(aCode);
}

void __cdecl Hook_cexit()
{
    s_processExiting = true;
    Real_cexit();
}

void HookFormAllocateSentinelInit()
{
    TP_HOOK_IAT(_initterm_e, "api-ms-win-crt-runtime-l1-1-0.dll");

    // The game quits through these (see s_processExiting).
    TP_HOOK_IAT(exit, "api-ms-win-crt-runtime-l1-1-0.dll");
    TP_HOOK_IAT(_exit, "api-ms-win-crt-runtime-l1-1-0.dll");
    TP_HOOK_IAT(_cexit, "api-ms-win-crt-runtime-l1-1-0.dll");
}
#pragma optimize("", on)
