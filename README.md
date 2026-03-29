## SkyrimTogether VR

A fork of Tilted Online (Skyrim Together Reborn) with the goal of adding Skyrim VR support.
This is an active work-in-progress by two full-stack developers learning C++ and reverse engineering along the way. Nothing is playable yet.

## What this is

Skyrim Together Reborn is an open-source co-op mod for Skyrim Special Edition. This fork attempts to port it to Skyrim VR, which uses a different executable with different memory addresses and VR-specific engine classes.

## What is done

VR build target (SkyrimTogetherClientVR) compiles clean
Dual-ID macro system in place — every address hook has an SSE slot and a VR slot
VR address library loader — reads version-1-4-15-0.csv from SKSE plugins instead of the SSE binary format
VR launcher target (SkyrimImmersiveLauncherVR) wired up
9 of 257 engine addresses mapped to their VR equivalents

## What is missing

248 engine addresses still need VR mapping via reverse engineering (Ghidra)
VR-specific engine class hierarchy differences not yet handled
No VR input or controller support
Not installable or testable yet

## Building

See the original build guide. To build the VR client:
bashxmake build SkyrimTogetherClientVR
Original project
All core multiplayer logic is from Skyrim Together Reborn by the TiltedPhoques team.

## License

[![GNU GPLv3 Image](https://www.gnu.org/graphics/gplv3-127x51.png)](http://www.gnu.org/licenses/gpl-3.0.en.html)

Tilted Online is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
