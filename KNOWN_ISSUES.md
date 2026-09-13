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

## Friend-supplied RTTI anchors verified against CommonLibVR-NG (2026-09-13)

A list of SE addresses + suggested VR RTTI/vtable anchors for the parked
ids was cross-checked directly against CommonLibVR-NG's `Offsets_RTTI.h`/
`Offsets_VTABLE.h` and source. Results:

- **`32887`** (`IAnimationGraphManagerHolder::SetGraphVariableFloat`) -
  already correctly fixed (`-> 32143`), confirmed byte-for-byte against
  CommonLibVR-NG's own `RELOCATION_ID(32143, 32887)` for this exact
  function. No further action.
- **RTTI anchors confirmed genuinely correct** (computed VR offset +
  `0x140000000` matches the given anchor exactly):
  - `IAnimationGraphManagerHolder` (covers `32883`/`32887`): `0x141ee1038`
  - `PlayerCamera` (covers `50790`/`50796`): `0x141f45ae0`
  - `BSTaskletManager` (covers `69554`): `0x141f60270`
- **Important catch found via this check:** CommonLibVR-NG's own
  `PlayerCamera::ForceFirstPerson`/`ForceThirdPerson` explicitly refuse to
  run on VR (`if (REL::Module::IsVR()) return false;`) **even though the id
  resolves to a real VR address**. An address resolving is not proof it's
  safe to call - VR's camera/view handling doesn't map onto a first/third
  person force-switch the same way. Matched that judgment call: both
  functions are now VR no-ops (`Code/client/Games/Skyrim/Camera/PlayerCamera.cpp`)
  regardless of whether the id resolves. (Currently unused by any caller in
  this codebase, so this was a dormant risk, not an active bug.)
- **Still requiring real Ghidra work, confirmed absent from CommonLibVR-NG
  entirely** (not just "unmapped id" - the class/function isn't modeled
  there at all): `32883` (`InternalRevertAnimationGraphManager`), the
  specific `BSTaskletManager` getter behind `69554`, `32803`
  (`AnimationExperiments.cpp`'s unnamed helper), `51538`
  (`FavoritesCanProcess`). (`392578` was also listed here - **wrong, now
  resolved**, see "RTTI audit" below.)
  The RTTI/vtable anchors above are legitimate starting points for that
  work (find the vtable, find constructor cross-references to it in
  Ghidra, then identify the target member function by its call site), not
  shortcuts around it.

## Parked / unresolved address-library IDs

These VR ids could not be resolved by any method tried this session
(official VR Address Library CSV, CommonLibVR-NG RTTI table, or Ghidra
call-graph matching). Their hooks are skipped via a null-guard in
`FunctionHookManager::Add` (`Libraries/TiltedReverse/Code/reverse/src/FunctionHook.cpp`)
rather than crashing - meaning whatever feature depends on them silently
doesn't work on VR, with no error shown.

- Function ids: `32803`, `32883`, `32887`, `50790`, `50796`, `51538`, `69554`
- ~~RTTI id: `392578`~~ resolved to `0x1edb368`, see "RTTI audit" below.

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

## Neighbour interpolation: 27 more ids resolved, 142 left (`VR_POINTERS_TODO.md`)

For AE ids with no `se_ae.csv` entry, the SE id can be inferred when the nearest known
AE→SE neighbours below and above share the same id delta. It is then confirmed by comparing
function sizes (AE 1.6.318 addresses vs SE 1.5.97 addresses) for the id and its ±2
neighbours. Leave-one-out validation on known pairs: **1949/1958 correct** with ≥3/5 sizes
matching, 377/384 otherwise. Applied to `VRAddressOverrides.h` when fingerprint ≥3/5, the SE
id has a VR address, and VR function size equals SE size: **26 ids**, plus **32883**
(InternalRevertAnimationGraphManager → SE 32139 → VR `0x500890`) by hand. Entries are marked
`neighbour interpolation` / `function-size fingerprint`. They are **not yet dump-checked**.
Hooks re-enabled by this include the SaveLoad form-id read/write, EquipManager equip/unequip,
DropObject, UpdateDetectionState, CheckForNewPackage and the VM update. `69165`
(`BSFixedString::Set`) moved to `0xc6dc90`, which is the value the old crosswalk had wrongly
given to `SetCompleted`. So the crosswalk contains real addresses filed under the wrong ids.

Note for anyone matching by hand: a friend's lookups for 32883/32887 used the AE id as an SE
id (SE id 32883 is a different function), which is the same mistake the crosswalk made.

The remaining 142, with AE addresses, candidates and search windows, are in
**`VR_POINTERS_TODO.md`**, generated for hand-matching in Ghidra.

## Early EngineFixesVR crash when MO2's VFS isn't ready (2026-09-13, probably environmental)

A run died 2 seconds after launch in `EngineFixesVR!std::operator<<`, before any
`tp_client.log` output. Symbolized stack (Engine Fixes' PDB is in the cdb symbol
cache): `REL::Module::Module` → `"Failed to get handle to module!"` → its logger,
which isn't initialized yet → null stream → AV. Engine Fixes VR (2024 CommonLib)
calls `GetModuleHandleA("SkyrimVR.exe")` in a static initializer. Our launcher
normally makes that succeed (`TP_GetModuleHandleA/W` + `LdrGetDllHandleEx` in
`stubs/FileMapping.cpp`), but here the DLL was loaded via usvfs' `LoadLibraryExW`
hook almost at process start, before those hooks were in place.

Other signs that MO2's virtual filesystem wasn't working properly in that process:
ReShade (`dxgi.dll`) and Engine Fixes both failed to write their logs, and the crash
dump was written to the real tools folder instead of `overwrite\Root`. It hadn't
happened in five earlier runs with the same launcher code. The game was started 6
seconds after MO2 had been force-closed (for a redeploy) and restarted. **If it
recurs:** fully close MO2, wait for it to finish loading after restart, then launch.
If it still happens with MO2 settled, it's a real ordering bug in the launcher:
Engine Fixes getting loaded before `FileMapping` hooks are installed.

## The crosswalk is wrong for functions: measured, partly replaced (2026-09-13)

**Measured accuracy.** For the 69 function/global ids where the correct VR address
is independently known (CommonLibVR-NG `RELOCATION_ID(se, ae)` + official VR CSV),
the crosswalk (`VRAddressOverrides.h`) was **right 0 times out of 69**. Its RTTI
entries are fine (2805/2806). Treat every crosswalk *function* entry as wrong until
it has been checked.

**Better source: `alandtse/vr_address_tools`** (clone at `C:\dev\vr_address_tools`,
updated 2026-09-11). Chain: AE id → SE id via `se_ae.csv` → VR address via the
official VR CSV, falling back to `database.csv` (13296/13298 agree with the official
CSV) and then `addrlib.csv` (96.6% agree). Validated against **790 known answers:
315/315 correct** wherever it produced a result (the rest had no `se_ae.csv` entry).
Updating `C:\dev\CommonLibVR-NG` (505 commits behind) only added 6 relevant pairs.

**Applied to `VRAddressOverrides.h`:**
- **24 entries corrected** (marked `// was 0x...: AE -> SE -> VR ...`). Every
  new address was checked in a crash dump: 23 are clean function starts (`cc`
  padding + prologue, and `getFormById` already carries another mod's detour),
  and `410506` (NiCamera NiRTTI) points at the string `"NiCamera"`. All 24 old
  values were different, consistent with 0/69. This includes `24991`
  (`TESQuest::SetCompleted`): **the earlier note calling it "verified via
  Ghidra" was wrong**. The crosswalk value `0xc6dc90` sits among thread/system
  code, and the real one is `0x37fc30`. It also includes `19742`, the
  `SpawnActorInWorld` hook behind the latest crash.
- **50 entries removed** (marked `// removed: unverified hook target`): AE ids
  with no mapping that feed only a `TP_HOOK`. They now resolve to null, so
  `FunctionHookManager::Add` skips the hook instead of detouring unrelated game
  code. **12 of these are also called directly** by multiplayer-sync wrappers
  (`RealPickUpObject`, `RealDropObject`, `RealCharacterConstructor`,
  `RealSetWaypoint`/`RealRemoveWaypoint`, `RealLockChange`, `RealSimulateTime`,
  `RealPerformAction`, `RealInitiateMountPackage`, `RealSpeakSoundFunction`, and
  the `38903` unequip wrappers in `EquipManager.cpp`). If one of those paths runs,
  it will crash at address 0, which is at least an obvious crash rather than a
  silent call into wrong code. `37577` is also used by the debug `CombatView`.

**Still unresolved:** ~100 non-hook ids (direct calls/globals) with no mapping
still point at crosswalk values, plus the direct `VersionDbPtr` plumbing
(`36544` WinMain, `68261`/`69066`/`57704` BSThread, UI/renderer). Those were
deliberately left alone because startup depends on some of them. Mass-nulling
them broke startup once before.

## Hook audit (2026-09-13): 74 hooks still target untranslated AE ids on VR

After SKSE loaded, the next crash was `HookShowSubtitle` → `Cast<Actor>` on an object
whose "vtable" was a heap address. The hook target, id `52626`, was never translated
(`POINTER_SKYRIMSE(..., 52626, 52626)`). The crosswalk sent it to `0x93b1e0`, a small
lookup helper, so the "speaker" argument was never a speaker. `52627` (HideSubtitle)
went to a bare `ret 0`. Both are disabled on VR in `SubtitleManager.cpp`.

This is the general problem, not a one-off. Of 266 `POINTER_SKYRIMSE` uses, **183
have a VR id that is not in the official VR CSV, and every one of them is an
untranslated AE id** (`sse == vr`). These resolve only through the unaudited
crosswalk, or to null. **74 of them feed a `TP_HOOK`**. These are the dangerous
ones: they detour game code without us calling anything, and wrong ones fail far
from the hook, as in the `Cast<>` crash. Per file:

| File | Hooks on untranslated ids |
| --- | --- |
| `Games/Skyrim/Actor.cpp` | 15 |
| `Games/Skyrim/EquipManager.cpp` | 9 |
| `Games/Skyrim/TESObjectREFR.cpp` | 8 |
| `Games/Skyrim/PlayerCharacter.cpp` | 6 |
| `Games/References.cpp` | 5 |
| `Games/Skyrim/Magic/MagicTarget.cpp`, `Games/Misc/BSScript.cpp` | 4 each |
| `SkyrimVM64.cpp`, `Games/SaveLoad.cpp`, `Games/Animation.cpp` | 3 each |
| `ActorMagicCaster.cpp`, `Interface/UI.cpp` | 2 each |
| `TimeManager`, `Sky`, `LoadingScreen`, `TESNPC`, `SummonCreatureEffect`, `InvisibilityEffect`, `CombatController`, `SubtitleManager` (now off), `MenuTopicManager`, **`Memory.cpp`** | 1 each |

`Memory.cpp` was checked first, and it **was wrong**. The allocate/free hooks
(`66859`/`66861`) are correctly translated, but the game-heap global `400188` was
`0x1c395b0` in the crosswalk. VR's `MemoryManager::GetSingleton` (VR id `11045`)
and the game's own free path both use `0x141f81900`. So `Memory::Allocate`/`Free`
passed the allocator an unrelated object as its heap on every client allocation,
a likely source of the corrupted objects seen in several crashes. Fixed to
`0x1f81900`, which also fits the table's neighbours (`400186 → 0x1f81860`,
`400187 → 0x1f81880`). This count covers only `POINTER_SKYRIMSE`. Direct
`VersionDbPtr<>(id)` uses (e.g. `BSThread.cpp`) come on top.

Per hook, in order of preference: find a `RELOCATION_ID(se, ae)` for that AE id in
CommonLibVR-NG (grep the AE id, the second argument); otherwise interpolate from
neighbours in the same class and confirm by disassembling the target in a crash
dump; otherwise disable the hook on VR with a comment. **Do not mass-disable**. That
was tried once and broke startup (see `BSThread`).

## RTTI audit (2026-09-13): all 2806 RTTI ids checked, and `Cast<>` was broken on VR

**RTTI TypeDescriptor ids** (`Code/client/Games/Skyrim/RTTI.cpp`, AE-numbered
`392xxx`) resolve on VR through the crosswalk table. All 2806 were compared with
CommonLibVR-NG's `Offsets_RTTI.h` (`VariantID(se, ae, vr)`, 7917 entries):
**2805 match exactly, 0 mismatch, 0 collide with an id in the VR CSV.** The RTTI
part of the crosswalk can be trusted. The *function* part cannot (see below).

The one unresolved id was **`392578` (`TESSoundFile`)**. The earlier note calling it
"not a function / an RTTI type descriptor at `0x141b592b8`" was wrong. That SE
address is an `UNWIND_INFO` record (checked in Ghidra), almost certainly from
looking up the AE id in an SE table, which is the same mistake as the crosswalk.
Resolved to **`0x1edb368`**:
- a Ghidra string search for `.?AVTESSoundFile@@` in VR puts the name at
  `0x141edb378`, and the descriptor starts `0x10` before it;
- it sits exactly between CommonLibVR-NG's neighbours: `TESObjectARMO` at
  `0x1edb340` (descriptor is 0x28 bytes, ending at `0x1edb368`) and `TESAIForm` at
  `0x1edb390` (`TESSoundFile`'s descriptor is 0x28 bytes, ending there);
- in dump memory, all three descriptors share the `type_info` vftable
  `0x1419102d0`.

**`internal::DynamicCast` was wrong, which broke every `Cast<>` on VR.** It used AE id
`109689`, which the crosswalk mapped to `0x13cf190`. On VR that address is an SEH
unwind funclet (`lea rcx, <global>; jmp <destructor>`). So every `Cast<>` ran a
destructor on a static object and returned garbage. That is a plausible source of
the "random" corruption seen so far. CommonLibVR-NG has `RELOCATION_ID(102238,
109689)`. VR id `102238` is in the official CSV at `0x138baba`, an import thunk
whose IAT slot holds `VCRUNTIME140!__RTDynamicCast` (verified in the dump). Fixed,
and the bad crosswalk entry was removed.

**Lesson:** the crosswalk's function entries can land on real code that is simply
the wrong code, and nothing crashes at the call site. For any id that matters,
check the target in a dump (`u <addr>`) rather than trusting that it resolves.

## SKSE VR was never loaded -> 279 plugins overflowed VR's 255-slot plugin array (the `TESFile::OpenTES` crash)

**Root cause of the recurring `0x14018a6a6` crash, found 2026-09-13.**

The stack scan showed the caller: a game loop at `0x14017f220` that does, for
`i < [TESDataHandler+0xD70]`, `OpenTES([TESDataHandler+0xD78 + i*8])`. In the dump,
`loadedModCount` (`+0xD70`) was **`0x117` = 279**, but on VR `loadedMods` is a fixed
`TESFile*[0xFF]` (see CommonLibVR-NG `TESDataHandler.h`, `// D78 this should be
avoided if SkyrimVRESL is available`). Entries past 255 read whatever follows the
array, hence the non-canonical `this` (`0x00179a16'1c0b4550`).

FUS (RO DAH profile: 286 enabled plugins) depends on **Skyrim VR ESL Support**
(`skyrimvresl.dll`), which replaces that array with an expanded file collection.
It is an SKSE plugin, and **our launcher never loaded SKSE VR**: `LoadScriptExender()`
only accepted `skse64*.dll` with a `StartSKSE` export and build >= 20100. SKSE VR
is `sksevr_1_4_15.dll`, v2.0.12, with **no exports at all**. The module list in the
crash dump confirmed it: no `sksevr_*`, no `skyrimvresl` (only `EngineFixesVR`, which
comes in through its own preloader). This is also why FUS works fine through
`sksevr_loader.exe` but not through us.

**Fix (`Code/client/ScriptExtender.cpp`, VR only):** match the `sksevr` prefix and
just `LoadLibraryW` it. SKSE VR's `DllMain` runs its initializer on
`DLL_PROCESS_ATTACH` (verified by disassembly: entry -> `dllmain_dispatch` ->
`DllMain` -> the routine logging `"SKSEVR runtime: initialize"` / `"reloc mgr
imagebase = %016I64X"`). It relocates against `GetModuleHandle(NULL)`, which is our
launcher image hosting the game at `0x140000000`.

**Still to verify on the first run with this fix:**
- `Documents\My Games\Skyrim VR\SKSE\sksevr.log` exists, the imagebase is
  `0000000140000000`, and `skyrimvresl.dll` is listed as loaded.
- SKSE is loaded from `BeginMain()` (the game's WinMain), later than
  `sksevr_loader` would inject it. Anything SKSE needs to hook before WinMain may
  be missed.
- SKSE's hooks and SKSE plugins (HIGGS, VRIK, PLANCK, ...) now share the process
  with our own hooks, including the unverified crosswalk ones. New conflicts are
  expected.

## Minidumps: read the logged crash context first

> **Correction (2026-09-13):** with `MiniDumpNormal | MiniDumpWithDataSegs |
> MiniDumpWithThreadInfo` the dump is still ~1GB (`WithDataSegs` includes our
> exe's data segment, which *is* the `game_seg` buffer), but it **is valid**, and
> it is currently the **only way to disassemble the game**: the on-disk
> `SkyrimVR.exe` is SteamStub-encrypted, so `cdb -z SkyrimVR.exe` shows garbage.
> Open the dump instead, and `u`/`dq` the game addresses directly. Keep
> `WithDataSegs`. The older notes below about the *previous* flags still stand.

The custom PE loader reserves a 1GB+ buffer for the game image, and that single
fact defeats every meaningful `MiniDumpWriteDump` setting:

| Setting | Result |
| --- | --- |
| `MiniDumpWithFullMemory` | ~7GB, ~3 minutes. Looks like a hang; was killed mid-write in practice, leaving corrupt files. |
| `MiniDumpWithIndirectlyReferencedMemory` | Chases pointers into the same buffer. ~1GB dumps that `cdb` rejects as truncated (`Memory range data only partially present in dump`, `Win32 error 0n1392`), and sometimes a **0-byte** file. |

Both of the 2026-09-13 dumps for the `TESFile::OpenTES` crash were unusable this
way (0 bytes, and 353MB-but-truncated). 22 such dumps had accumulated to **21GB**
in `<MO2 instance>\overwrite\Root\`.

Two things were changed in `Code/client/CrashHandler.cpp` as a result:

1. **Dump flags reduced** to `MiniDumpNormal | MiniDumpWithDataSegs |
   MiniDumpWithThreadInfo`. `MiniDumpNormal` still includes thread contexts and
   stacks (all a call stack needs), and the dump now actually completes.
2. **`LogCrashContext()` added** - the crash context goes straight into
   `tp_client.log` and does not depend on the dump succeeding at all. It logs:
   - the faulting access (read/write/execute + target address);
   - all integer registers (a garbage `this` in `rcx` is recognisable on sight);
   - a **raw stack scan**: every qword between `rsp` and the thread's
     `NT_TIB::StackBase` that points into committed *executable* memory, with
     each hit attributed to `module+offset` or `<alloc base>+offset`.

   The stack scan exists because a proper stack walk is impossible here:
   `RtlVirtualUnwind`/`StackWalk64` need `.pdata` unwind info, and **the custom
   PE loader maps the game image without any**, so a real walk stops dead at the
   first game frame. Scanning recovers the return-address chain (with some
   false positives from stale slots - order and plausibility make the real chain
   readable). Attribution also distinguishes our exe from the custom-loaded game
   image from generated code (`CodeGenerator` trampolines, MinHook thunks), which
   is exactly the distinction needed to tell whether one of our hooks is in the
   chain.

### `"coredump created"` in older logs is a lie

`CreateFileA` reports failure as `INVALID_HANDLE_VALUE` (-1), not `NULL`, but the
handler tested `if (!hDumpFile)`. That test took the *success* branch for both
outcomes, so the log said `coredump created -> flush logs.` even when no file was
ever opened. This sent several debugging sessions hunting for dumps that did not
exist. Now fixed, and `MiniDumpWriteDump`'s return value plus `GetLastError()`
are both checked and logged.

## `ModManager::GetCellFromCoordinates` id 13718 is untranslated and probably null on VR

`Code/client/Games/ModManager.cpp` has `POINTER_SKYRIMSE(TModManager, getCell,
13718, 13718)` - i.e. the AE id was left in place for VR. Its two nearest
CommonLibVR-NG neighbours in `TESDataHandler` both shift by a consistent **-98**:

- `GetExtCellDataFromFileByEditorID`: `RELOCATION_ID(13618, 13716)`
- `CreateReferenceAtLocation`/`SpawnNewREFR`: `RELOCATION_ID(13625, 13723)`

so AE 13718 interpolates to **VR 13620**. Not applied, because neither 13618 nor
13620 is present in `version-1-4-15-0.csv` at all - the VR Address Library is
*sparse* (community-generated; many ids simply absent), so this id most likely
resolves to null on VR and `ThisCall` on it would fault at address 0. Only
reached on runtime cell lookups, not during load, so it is not the current
blocker - but it is a live landmine.

Note the wider implication: **a missing CSV entry is a different failure mode
from a wrong id**, and the sparse VR CSV means some correctly-translated ids
still won't resolve. `VersionDbPtr::GetPtr()` already logs unresolved ids via
`OutputDebugStringA` - watch DebugView for those.

## Verified correct against CommonLibVR-NG (no change needed)

- `ModManager::Get()` - `POINTER_SKYRIMSE(ModManager*, modManager, 400269, 514141)`
  matches `RELOCATION_ID(514141, 400269)` for `TESDataHandler::GetSingleton`.
- `SpawnNewREFR` - `(13723, 13625)` matches `RELOCATION_ID(13625, 13723)`.
- **The crashing address `0x14018a6a6` is genuinely `TESFile::OpenTES+0xf6`.**
  Confirmed two independent ways: `version-1-4-15-0.csv` puts id 13855 at
  offset `0x18a5b0` with the next entry at `0x18abf0` (so `+0xf6` is well inside
  that function), and CommonLibVR-NG's `TESFile::OpenTES` uses
  `RELOCATION_ID(13855, 13931)` whose *first* argument is the SE/VR id. This is
  not a symbol-misattribution artefact.

## Debugging methodology that worked this session

For the next crash:

0. **Read `logs\tp_client.log` first.** It is the fastest signal by far: it shows
   how far startup got, the exception code and address, and (since
   `LogCrashContext()` was added) registers and a stack scan. Only go to a dump
   if the log is somehow absent - see the minidump section above for why dumps
   are the *less* reliable source for this process.
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
