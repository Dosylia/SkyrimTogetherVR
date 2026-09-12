# Known Issues / VR Port Technical Debt

## SkyrimTogether.esp header version must be 1.70 for VR (not 1.71)

**Symptom:** VR reaches the main menu, shows the logo, but **no menu text
ever appears**, then the process dies ~30s later. Plain FUS (same 423
plugins, without this mod) works fine.

**Cause:** `SkyrimTogether.esp` ships with a TES4 `HEDR` version float of
**1.71** (AE-era). Skyrim VR is based on 1.4.15 and only accepts **1.70**.
The engine rejects the plugin with the internal error *"File
SkyrimTogether.esp is a higher version than Skyrim.esm"*, data loading
never completes, the localization STRINGS never load (hence a menu with no
text), and the loader thread then crashes inside `TESFile::OpenTES` with a
garbage `TESFile*`.

**How it was found:** the recurring, perfectly reproducible crash at
`game_seg+0x896a6` was reverse-mapped to address-library id `13855` =
`TESFile::OpenTES` (via the VR Address Library CSV + CommonLibVR-NG), its
prologue confirmed unpatched and matching `OpenTES(this, OpenMode, bool)`,
and the error string was then read directly out of the crashing thread's
stack in the dump.

**Fix:** patch 4 bytes at file offset `0x1E` of the plugin - the `HEDR`
version float - from `48 e1 da 3f` (1.71f) to `9a 99 d9 3f` (1.70f):

```
printf '\x9a\x99\xd9\x3f' | dd of=SkyrimTogether.esp bs=1 seek=30 conv=notrunc
```

1.70 is accepted by SE/AE **and** VR, so this is safe for both builds.

**Applied to the deployed copy** (`<MO2>/mods/Skyrim Together VR/`, original
kept as `SkyrimTogether.esp.bak-1.71`). **NOT yet applied to the repo copy**
at `GameFiles/Skyrim/SkyrimTogether.esp`, which is still 1.71 - redeploying
from the repo will reintroduce the bug until that copy is patched too.

## ROOT CAUSE: AE vs SE address-library id numbering (read this first)

SkyrimTogether targets Skyrim SE **1.6.x (Anniversary Edition)**, whose
Address Library uses **AE-era id numbering**. The VR Address Library
(`version-1-4-15-0.csv`) uses the **older SE numbering**. These are
different id spaces: the same function has a different id in each.

Every `POINTER_SKYRIMSE(type, name, sseId, vrId)` in this codebase carries
an **AE id** as `sseId`. Only **9 of 257** ids used in the client happen to
be valid as-is for VR.

The earlier automated SE->VR crosswalk pass made this much worse rather
than better: it resolved each id through an **SE-1.5.97 id->offset table**
while feeding it **AE ids**. Looking up an AE id in an SE table returns the
offset of a completely unrelated function, so `VRAddressOverrides.h` is
largely a table of confidently-wrong addresses. When one of those is used
with `TP_HOOK`, MinHook writes a detour **into the middle of an unrelated
game function**, silently corrupting it. That is the mechanism behind the
"game runs but behaves insanely" class of bugs here - e.g. a crash traced
to `TESFile::OpenTES` (Skyrim's plugin/ESM reader), which is why menus
showed a logo but never any text: menu strings load through that path.

**How to get a correct VR id:** CommonLibVR-NG encodes the mapping as
`RELOCATION_ID(seID, aeID)` - first arg SE, second AE. Take the codebase's
AE id, find its `RELOCATION_ID` pair in `C:\dev\CommonLibVR-NG`, and use
the **SE id**; then confirm that id exists in the VR Address Library CSV
(installed as an MO2 mod, `mods/VR Address Library for SKSEVR/SKSE/Plugins/`).
Both hand-derived fixes made before this rule was understood (`14298->14108`
for `EventDispatcherManager::Get`, and the camera/animation "parked" ids)
fall straight out of it.

**Applied:** 71 ids translated AE->SE and verified present in the VR
library. Ids previously believed permanently unresolvable (`32887`, `50790`,
`50796`, ...) were never unresolvable - just AE-numbered.

**Still outstanding:** ~177 ids have no `RELOCATION_ID` pair in
CommonLibVR-NG and remain pointed at crosswalk garbage. Any of those that
are *hooked* are still corrupting game code. Do not guess at these: the id
deltas are not a formula (they range from ~90 to ~6800 and even change id
space entirely for globals, e.g. `400269 -> 514141`). The safe interim
treatment is to set their `vrId` to `0` so `VersionDbPtr` returns null and
the null-guards skip them - features silently off, but no corruption.
Exception: `24991` (`TESQuest::SetCompleted`) was hand-verified via Ghidra
call-graph matching and its override entry should be preserved.

Tracking file for the SE→VR port. Anything here is either a deliberate
temporary workaround (functionality turned off to stop a crash, not fixed)
or a known gap that hasn't caused a visible problem yet but might.

Last updated: 2026-09-12

## Temporary workarounds (functionality disabled, not fixed)

### `Projectile::LaunchData` fields not synced on VR
**File:** `Code/client/Games/Skyrim/Projectiles/Projectile.cpp` (`ReadLaunchDataFormIds`)

On VR, projectile-launch events no longer carry `ProjectileBaseID`,
`ShooterID`, `WeaponID`, `AmmoID`, `ParentCellID`, or `SpellID` - the code
that reads these went in behind `#ifndef SKYRIMVR` and is a no-op on VR.

**Why:** `Projectile::LaunchData` (see the struct in `Projectile.h`) was
reverse-engineered years ago against SE and reused as-is for VR, never
verified. On VR, `arData.pProjectileBase` (and presumably the other fields)
read back as non-null garbage - the *same* garbage value every single crash
(`0x0000003400000033`), which points to a genuine, consistent struct-layout
shift on VR rather than random uninitialized memory. A `__try/__except`
guard around the reads reliably stopped the resulting access violation, but
consistently left the *next* function epilogue's `/GS` stack cookie
corrupted, crashing with an unrecoverable `STATUS_STACK_BUFFER_OVERRUN`
right after - SEH recovery interacts badly with this heavily-inlined,
custom-loaded hook context, so catching the exception isn't a viable fix.

**Real fix:** reverse-engineer VR's actual `LaunchData` layout (Ghidra,
comparing the VR build's `TESObjectWEAP::Fire`/`Projectile::Launch` call
site against SE's) and correct the struct offsets in `Projectile.h`.
Basic projectile sync (position, does it exist) still works - only this
metadata is missing.

## Parked / unresolved address-library IDs

These VR ids could not be resolved by any method tried this session
(official VR Address Library CSV, CommonLibVR-NG RTTI table, or Ghidra
call-graph matching). Their hooks are skipped via a null-guard in
`FunctionHookManager::Add` (`Libraries/TiltedReverse/Code/reverse/src/FunctionHook.cpp`)
rather than crashing - meaning whatever feature depends on them silently
doesn't work on VR, with no error shown.

- Function ids: `32803`, `32883`, `32887`, `50790`, `50796`, `51538`, `69554`
- RTTI id: `392578`

If you need one of these features working, the next step is Ghidra
call-graph matching against the two binaries (see the session history for
the anchor-matching approach used to resolve the ~3038 ids that *did* get
found), or checking if a newer CommonLibVR-NG revision has since added it.

**Update:** five of these (`32887`, `32883`, `32803`, `50790`, `50796`) were
found calling their resolved function pointer directly via
`TiltedPhoques::ThisCall(...)` with no null check at all - bypassing
`FunctionHookManager`'s guard entirely, since that only protects hooks
installed through it, not direct calls. Any of these being invoked with the
id unresolved would call through a null function pointer and crash. Guarded
with an explicit `if (!x.Get()) return ...;` in
`IAnimationGraphManagerHolder.cpp`, `PlayerCamera.cpp`
(`ForceFirstPerson`/`ForceThirdPerson`), and `AnimationExperiments.cpp`
(`ActorMediator::RePerformComplexAction`). Found via a live Sysinternals
DebugView capture showing `VersionDbPtr: unresolved id 0` being logged
during a session where the game got stuck at the main menu (see the
"stuck at menu, no text" investigation below) - worth checking DebugView
output first if a similar silent-failure symptom shows up again, rather
than auditing files one by one.

## Unaudited crosswalk table - likely more landmines

**File:** `Code/client/VRAddressOverrides.h`

This table (~3000 entries) was built two ways:
1. An automated SE→VR binary-diff crosswalk (`sse_vr.csv`) - proven **0%
   accurate for RTTI/data symbols** by comparing against CommonLibVR-NG, and
   kept **only** for plain function-hook targets, explicitly flagged as
   "plausible, unverified" at the time.
2. CommonLibVR-NG's own verified RTTI table - high confidence.

Two concrete examples of (1) being wrong were found and fixed this session:
- id `17201` (used for `TESObjectREFR::GetByHandle`) didn't exist in the
  official VR Address Library at all - it pointed at some unrelated
  function, and calling it with the wrong assumed signature corrupted the
  stack on every single call. Replaced with `12204` (verified against both
  the official CSV and CommonLibVR-NG's `LookupReferenceByHandle`).
- id `14298` (used for `EventDispatcherManager::Get()` in
  `Code/client/Games/Skyrim/Events/EventDispacther.cpp`) turned out to be
  this symbol's **AE id, not its SE/VR id** - confirmed via
  CommonLibVR-NG's `ScriptEventSourceHolder::GetSingleton()`, which uses
  `RELOCATION_ID(14108, 14298)` (SE id first, AE id second). VR shares
  old-gen SE's numbering, not AE's. Replaced with `14108` (confirmed in the
  official VR CSV). This crashed inside real game code because it jumped
  into an unrelated function.

**Still unverified and suspected (not yet confirmed broken):** the other
three ids in the same `EventDispacther.cpp` file -
`details::InternalRegisterSink` (54425), `InternalUnRegisterSink` (54522),
and `InternalPushEvent` (19364) - also don't exist in the official VR CSV
and are only present in `VRAddressOverrides.h`'s crosswalk table. They
haven't been observed crashing yet, but given the exact same pattern just
found twice in this one file, they're a prime suspect for the next crash
in this area (quest/discovery/activate event registration). Worth checking
first if something in that space breaks next, rather than starting from
scratch. Note CommonLibVR-NG doesn't call into these at all for its own
event-sink handling - it manipulates `BSTEventSource<T>`'s raw struct
members (`sinks`/`pendingRegisters`/`pendingUnregisters`/spinlock, see
`RE/B/BSTEvent.h`) directly in C++ instead of resolving a game function, so
there's no id to cross-reference for these three even if we wanted one -
verifying them means checking the CSV directly or reimplementing the
direct-struct-manipulation approach instead.

**The concern:** there is no reason to believe `17201` was the *only* bad
entry from the crosswalk pass - it's just the one that happened to get
exercised and crash loudly. Others may be silently wrong (pointing at a
function that doesn't crash but does the wrong thing, or one that's never
been called yet). There is currently no systematic audit of this table.

**If you hit a bug that doesn't make sense** (wrong behavior, not a crash,
in code that uses a `POINTER_SKYRIMSE(...)` or raw `VersionDbPtr` with an
id you don't recognize): check whether that id actually exists in the real
VR Address Library CSV before assuming the bug is elsewhere. That CSV is
installed as an MO2 mod, not in this repo:
`<your MO2 instance>/mods/VR Address Library for SKSEVR/SKSE/Plugins/version-1-4-15-0.csv`
(id,offset - hex offset, look up by the decimal id). Cross-reference against
`C:\dev\CommonLibVR-NG` (or wherever it's cloned) if the CSV doesn't have it
or you want a second source.

## Renderer/UI raw byte-patches disabled on VR entirely

**Files:** `Code/client/Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp`,
`Code/client/Games/Skyrim/Interface/UI.cpp`

Symptom that led here: game reached the main menu in VR (huge milestone -
no more startup crashes), but got stuck with the logo visible and no menu
text/UI ever rendering, no crash.

A systematic check found that **every single raw byte-patch id in the
codebase** (`77226`, `68781`, `77246`, `68617`, `68276`, `14774`, `68261`,
`69066`, `57704`, `53112`, `52510`, `52518`, `34452`, `82082`, `36548`) is
**missing from the official VR Address Library CSV** - all of them only
exist in the unverified crosswalk table. Unlike a missing/null id (which
the null-guards from earlier in this session catch safely), these all
resolve to *some* non-null address via the crosswalk table - just possibly
the wrong one, silently patching or hooking unrelated code with no
guaranteed crash. That's a plausible explanation for "stuck, no crash."

Disabled on VR for now (`#ifndef SKYRIMVR` around the whole thing):
- `BSGraphicsRenderer.cpp`: the window-style patch, the DirectInput
  exclusivity patch, and - most importantly - the `TP_HOOK_IMMEDIATE` on
  `Renderer_Init` itself (id `77226`), which also wires up the mod's own
  overlay/imgui rendering via `OnDeviceCreation`. This is the top suspect:
  if it hooks the wrong function, our hook body writes through pointers
  that aren't really `RendererInitOSData*`/`ApplicationWindowProperties*`,
  which is silent corruption, not a guaranteed crash.
- `UI.cpp`: the `UI_AddToActiveQueue` `SwapCall` (id `82082`, only affects
  behavior once connected to multiplayer) and the "skip startup movie"
  byte patch (id `36548`, cosmetic).

**Consequence:** disabling `Renderer_Init`'s hook means `BSGraphics::GetMainWindow()`
(`g_RenderWindow`) is now always null on VR, since it was only ever set
inside that hook. Added null-guards at the four call sites that dereferenced
it unconditionally (`BSInputDeviceManager.cpp`, `DebugService.cpp` x2 via
`OnUpdate`'s early-out, `ComponentView.cpp` transitively via the same
early-out) so this doesn't just trade one crash for another. The mod's
overlay/imgui HUD will not initialize on VR until `77226` (and ideally the
whole list above) is verified against the real VR binary and given correct,
VR-specific ids.

**If the menu-stuck symptom persists after this fix**, the next suspects in
priority order are `BSInputDeviceManager.cpp` (`68617`, input polling -
already null-guarded but still hooks a potentially-wrong function) and
`BSGraphicsRenderer.cpp`'s DirectInput patch (`68781`) - both touch input,
which VR headsets/controllers depend on heavily.

## Unverified but not yet known to be broken

These were educated-guess implementations from earlier in the VR port and
have not been confirmed correct against real VR disassembly:

- `Code/client/Games/Skyrim/NetImmerse/NiTransform.h` - `NiMatrix3ToQuat()`,
  marked `TODOVR` for matrix convention.
- `Code/client/Systems/AnimationSystem.cpp` - raw VR node data offsets
  (`0x580` HMD, `0x490` left wand, `0x4F8` right wand) used to capture
  head/hand transforms for pose sync. These are typical/plausible offsets,
  not confirmed against the actual VR `PlayerCharacter` layout.

## Debugging methodology that worked this session

For the next crash:

1. **Get a real dump.** The game's own crash handler
   (`Code/client/CrashHandler.cpp`) only dumps on `EXCEPTION_ACCESS_VIOLATION`
   or `STATUS_STACK_BUFFER_OVERRUN`, and only the *first* one per process, and
   `/GS` stack-cookie failures (`STATUS_STACK_BUFFER_OVERRUN`, code
   `0xc0000409`, raised via `__fastfail`) bypass vectored exception handlers
   entirely - our own handler cannot catch them no matter what. For those,
   rely on Windows' **LocalDumps** feature instead (already configured on
   this dev machine: `HKLM\SOFTWARE\Microsoft\Windows\Windows Error
   Reporting\LocalDumps\SkyrimTogetherVR.exe` → dumps land in
   `C:\CrashDumps`). If setting this up fresh, it needs an elevated
   (admin) PowerShell.
2. **Analyze with `cdb.exe`, not the `WinDbgX.exe` GUI.** The modern WinDbg
   GUI shell can hang indefinitely for reasons that were never fully
   pinned down (possibly symbol-path related). `cdb.exe` (ships alongside
   it, `Microsoft.WinDbg` package under
   `%LOCALAPPDATA%\Microsoft\WindowsApps\cdbX64.exe`) works reliably
   headless:
   ```
   cdb.exe -z <dump.dmp> -y <path to build\windows\x64\release, for the pdb> \
     -c ".ecxr; .lines; kb 20; u @rip-40 L20; dps @rsp L60; q"
   ```
3. **Don't trust the symbol name at the crash address at face value.**
   Release/LTCG builds fold and misattribute static/inlined functions to
   whatever public symbol happens to be nearest in the binary - several
   "crashes in spdlog::warn" this session were actually inlined
   `VersionDb`/`VersionDbPtr` lookups. Disassemble a window around the
   crash and read the actual instructions/call targets rather than trusting
   the label.
4. **Rebuild just the one target** to iterate fast:
   `xmake build SkyrimImmersiveLauncherVR` (a few seconds once cached).
5. **Redeploy:** ModOrganizer.exe holds the deployed exe open (fails with
   "user-mapped section" or plain permission-denied errors on copy) - close
   it first, copy `SkyrimTogetherVR.exe`/`.pdb` from
   `build\windows\x64\release\` to
   `<MO2 instance>\tools\Skyrim Together VR\`, then relaunch MO2.
