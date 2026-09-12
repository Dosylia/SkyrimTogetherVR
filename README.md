## SkyrimTogether VR

A fork of Tilted Online (Skyrim Together Reborn) with the goal of adding Skyrim VR support.
This is an active work-in-progress by two full-stack developers learning C++ and reverse engineering along the way. Nothing is playable yet.

## What this is

Skyrim Together Reborn is an open-source co-op mod for Skyrim Special Edition. This fork attempts to port it to Skyrim VR, which uses a different executable with different memory addresses and VR-specific engine classes.

## What is done

VR build target (SkyrimTogetherClientVR) compiles clean
Dual-ID macro system in place — every address hook has an SSE slot and a VR slot
VR address library loader — reads version-1-4-15-0.csv from SKSE plugins instead of the SSE binary format, then fills any gaps from a supplemental table (VRAddressOverrides.h) derived by cross-referencing this project's SSE address IDs against a community SSE↔VR binary-diff table
VR launcher target (SkyrimImmersiveLauncherVR) wired up (still fails to link — pre-existing CEF runtime-library mismatch, not VR-specific)
Nearly all engine addresses mapped to their VR equivalents this way: 267 of 275 function/global hooks, and 2771 of 2818 RTTI type descriptors. The remaining ~55 (8 hooks + 47 RTTI types) aren't in the community binary-diff data and need real reverse engineering (Ghidra) to find
One real VR struct-layout difference found and fixed (PlayerControls::Data offset 0x20 -> 0x24)

## What is missing

~8 engine addresses and ~47 RTTI type descriptors still need VR mapping via reverse engineering (Ghidra) — the bulk of the address work is done, this is the long tail
Other VR-specific engine class hierarchy/struct-layout differences almost certainly exist beyond the one found so far (PlayerControls) and haven't been systematically audited
No VR input or controller support (headset pose, motion controllers, room-scale movement) — this is new code, not address porting, and is the biggest remaining piece of work before it's playable in VR
Not installable or testable yet — untested against the actual game; even with most addresses mapped, first launch may still crash on something undiscovered
Networking: the dedicated server (Code/server) is engine-agnostic and already works over UDP for SE; VR-specific player state (headset/hand poses) will need new sync fields once VR input exists, but this is additive, not a rewrite

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
