#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <TiltedCore/Platform.hpp>

#include <windows.h>
#include <intrin.h>
#include <cstdint>

// TiltedCore
#include <TiltedCore/StackAllocator.hpp>
#include <TiltedCore/ScratchAllocator.hpp>
#include <TiltedCore/Filesystem.hpp>
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Outcome.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Math.hpp>
#include <TiltedCore/TaskQueue.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Initializer.hpp>
#include <TiltedCore/Serialization.hpp>

// TiltedReverse
#include <AutoPtr.hpp>
#include <App.hpp>
#include <FunctionHook.hpp>
#include <Entry.hpp>
#include <Debug.hpp>
#include <ThisCall.hpp>

extern void* RipAllocateN(size_t blockLength);
#define REVERSE_ALLOC_STUB(x) RipAllocateN(x)
#include <JitAssembly.hpp>

#define SPDLOG_WCHAR_FILENAMES
#define SPDLOG_WCHAR_TO_UTF8_SUPPORT
#include <entt/entt.hpp>
#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>

#include <any>
#include <mutex>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <fstream>

// A hook goes in only when its game address resolved. TiltedReverse's FunctionHookManager::Add hands any target to
// MinHook, a null one included, and MinHook then reads code at address 0 and takes the game down at start -- which an
// address id missing from the VR table does. Each hook is checked here first: one that did not resolve is named in the
// log and skipped, and only what it backs does not work. (This check lived in a TiltedReverse commit that never left
// one machine; the submodule follows upstream, so it lives here, and nothing null reaches the hook manager at all.)
template <class T, class U> void AddHookIfResolved(T** appSystemFunction, U* apHookFunction, const bool aDelayed, const char* acpName) noexcept
{
    if (*appSystemFunction == nullptr)
    {
        spdlog::error("Hook {} not installed: its game address did not resolve", acpName);
        return;
    }

    TiltedPhoques::FunctionHookManager::GetInstance().Add(appSystemFunction, apHookFunction, aDelayed);
}

#undef TP_HOOK
#undef TP_HOOK_IMMEDIATE
#define TP_HOOK(systemFunction, hookFunction) AddHookIfResolved(systemFunction, hookFunction, true, #hookFunction)
#define TP_HOOK_IMMEDIATE(systemFunction, hookFunction) AddHookIfResolved(systemFunction, hookFunction, false, #hookFunction)

#include <BuildInfo.h>
#include <Games/Primitives.h>

using TiltedPhoques::Allocator;
using TiltedPhoques::App;
using TiltedPhoques::AutoPtr;
using TiltedPhoques::Buffer;
using TiltedPhoques::List;
using TiltedPhoques::Map;
using TiltedPhoques::Outcome;
using TiltedPhoques::ScopedAllocator;
using TiltedPhoques::ScratchAllocator;
using TiltedPhoques::Set;
using TiltedPhoques::SortedMap;
using TiltedPhoques::StackAllocator;
using TiltedPhoques::String;
using TiltedPhoques::ThisCall;
using TiltedPhoques::UniquePtr;
using TiltedPhoques::Vector;

using namespace std::chrono_literals;

#include "Components.h"

#include <Utils.h>
#include <RTTI.h>
