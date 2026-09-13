// Copyright (C) 2021 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.
#pragma once

#include <cstdint>
#include <limits>
#include <BuildInfo.h>

#define CLIENT_DLL 0

struct TargetConfig
{
    const wchar_t* dllClientName;
    const wchar_t* fullGameName;
    uint32_t steamAppId;
    uint32_t exeLoadSz;
    // Needs to be kept up to date.
    uint32_t exeDiskSz;
};

// clang-format off

#ifndef SKYRIMVR
static constexpr TargetConfig CurrentTarget{
    L"SkyrimTogether.dll",
    L"Skyrim Special Edition",
    489830, 0x40000000, 35410264};
#define TARGET_NAME L"SkyrimSE"
#define TARGET_NAME_A "SkyrimSE"
#define PRODUCT_NAME L"Skyrim Together"
#define SHORT_NAME L"Skyrim Special Edition"
#else
// VR: steamAppId/exeDiskSz confirmed against a real Skyrim VR install and Steam's
// own registry entry (HKLM\SOFTWARE\WOW6432Node\Bethesda Softworks\Skyrim VR).
// exeLoadSz (buffer for the custom PE loader, the game_seg array in our own image)
// is 80MB, not SE's 1GB. SkyrimVR.exe's SizeOfImage is 0x3959000 (~60MB), so 80MB
// fits it with margin. The 1GB value mattered: game_seg sits at 0x140000000, so a
// 1GB buffer pushed our image end to ~0x18094a000 and used over half of the ±2GB
// window that SKSE plugins need for their hook trampolines (CommonLib's
// Trampoline::create searches base ±2GB). With heaps filling the rest,
// DynDOLOD.dll's trampoline allocation failed at kDataLoaded ("failed to
// create trampoline"), blocking the main menu from finishing loading.
// ExeLoader::Load refuses images larger than this.
static constexpr TargetConfig CurrentTarget{
    L"SkyrimTogetherVR.dll",
    L"Skyrim VR",
    611670, 0x05000000, 35530960};
#define TARGET_NAME L"SkyrimVR"
#define TARGET_NAME_A "SkyrimVR"
#define PRODUCT_NAME L"Skyrim Together VR"
#define SHORT_NAME L"Skyrim VR"
#endif

// clang-format on
