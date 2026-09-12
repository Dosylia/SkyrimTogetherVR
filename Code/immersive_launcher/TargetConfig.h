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
// exeLoadSz (buffer for the custom PE loader) is left the same 1GB allocation as
// SE - VR's exe is ~60MB in memory, well under either value, so no change needed.
static constexpr TargetConfig CurrentTarget{
    L"SkyrimTogetherVR.dll",
    L"Skyrim VR",
    611670, 0x40000000, 35530960};
#define TARGET_NAME L"SkyrimVR"
#define TARGET_NAME_A "SkyrimVR"
#define PRODUCT_NAME L"Skyrim Together VR"
#define SHORT_NAME L"Skyrim VR"
#endif

// clang-format on
