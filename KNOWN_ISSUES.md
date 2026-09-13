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

### Connected-mode actor calls routed through Papyrus natives on VR (2026-09-13)
**File:** `Code/client/Games/Skyrim/Actor.cpp`

The first connect to a server crashed in `PlayerService::OnServerSettingsReceived`
-> `Actor::SetPlayerRespawnMode` -> `SetNoBleedoutRecovery`. It called the
crosswalk value for untranslated AE id 38533 (`0x664750`) and faulted 9 bytes
in. On VR, `SetNoBleedoutRecovery` now calls the Papyrus native
`Actor.SetNoBleedoutRecovery`. `SetFactionRank` (37677, interpolated to
`0x600220`, never checked against a dump) now calls `Actor.SetFactionRank` the
same way. The native addresses come from the VM's registration table, so they
are correct. The natives may do slightly more (argument checks) than the
internal functions STR used.

**Real fix:** find the SE ids for AE 38533/37677 and use them directly.

### Audit: 73 unverified crosswalk overrides still used by live code (2026-09-13)
Three connected-mode crashes today came from bare `{ id, addr }` entries in `VRAddressOverrides.h`
(38533, 35503, 20460). A scan found 73 ids that code uses, that are not in the VR CSV, and whose
override is a bare crosswalk value. None matched an independent re-derivation, so treat all of
them as wrong.

New matcher (`scratchpad/align.py`): align the AE function-size sequence (±8 ids) against SE sizes
at offsets near the nearest known AE→SE deltas. Rule `>=10/16 matches and runner-up <=4` scored
**153/153** on known pairs with the target's own anchor hidden.

Applied (override value replaced, not yet dump-checked):

| AE | SE | VR | Function |
|---|---|---|---|
| 19075 | 18606 | `0x27a4c0` | TESObjectCELL GetCOCPlacementInfo |
| 19846 | 19418 | `0x2ad090` | TESObjectREFR::GetHandle |
| 37511 | 36511 | `0x5ee4f0` | PayGoldToContainer |
| 38899 | 37943 | `0x640f30` | EquipManager::UnEquipAll |
| 58377 | 57804 | `0xa2e820` | hkbBehaviorGraph HandleEvents |
| 58378 | 57805 | `0xa2ea40` | hkbBehaviorGraph sub |
| 68221 | 66964 | `0xc413f0` | CRC hash stub |

Fixed individually earlier: 20460 → SE 20026, 35503 → SE 34582.

Fixed from the 15:15 two-player crash dump (both checked in the dump):
- **35993 → SE 35100, VR `0x59ef30`:** `BGSLoadGameBuffer` ctor used by `BGSLoadFormBuffer`.
  The old `0x5ce330` crashed in `TESNPC::Deserialize` when the second player joined. The
  automatic matcher chose 35101 because AE 35992 has no SE twin, so anchor checks still matter.
- **37717 → SE 36707, VR `0x602f40`:** `SetPlayerTeammate`. The disassembly toggles boolBits
  `0x4000000` at `0xE0` and `0x80` at `0x1FC`.

Fixed from the 15:23 two-player crash (`rip 0` in `Actor::Create` from `OnCharacterSpawn`): the
Character ctors 40245/40246 had been removed as unverified hook targets, so `RealCharacterConstructor`
was null. Both are now resolved and checked in the dump:
- **40245 → SE 39171, VR `0x69bec0`:** `Character()`. Passes `dl=1` to the Actor ctor.
- **40246 → SE 39172, VR `0x69bfc0`:** `Character(uint8)`. Forwards `dl`; the 0x2B0 allocator calls it.

Both install the Character vtables starting at `0x1416d6de0`. The pass-through hooks on them are
live again. `s_characterDtor` (37175) is declared but never assigned or hooked.

Fixed from the 15:51 crash, where the remote player spawned and then `SetWeaponDrawn` crashed:
- **38979 → SE 38023, VR `0x645240`:** `ActorState::SetWeaponDrawn`. The disassembly writes the
  3-bit `weaponState` at `actorState2`+0xC, bit 5.
- **Animation sync, found proactively.** The PerformAction hook had been removed and the rest was
  crosswalk garbage:

  | AE | SE | VR | Function |
  |---|---|---|---|
  | 38949 | 37996 | `0x643f20` | `ActorMediator::PerformAction` (hook is live again) |
  | 38953 | 37999 | `0x644160` | `PerformComplexAction` |
  | 39004 | 38048 | `0x646160` | `ApplyAnimationVariables` |
  | 403566 | 517058 | `0x2febcb0` | Animation-variables global |
  | 403567 | 517059 | `0x2febcb8` | ActorMediator singleton |

  VR `PerformAction` calls the other two and loads `[0x142febcb0]`; its callers load `[0x142febcb8]`.

**Useful source: the repo's git history has SE 1.5.97 raw addresses** from before the AE move. Commit
`5da6679e` still has them in `Games/Animation.cpp`. Converting SE address → SE id (via
`se_ae_offsets.csv`) → VR (via addrlib or the CSV) needs no guessing. Mining older commits
should resolve many of the remaining unverified ids.

**Git-history mining applied (2026-09-13).** Script: `scratchpad/mine.py`. It reads every historical
`POINTER_SKYRIMSE(..., 0x14XXXXXXX)` line in `Code/client`, matches it to today's id by file and
variable, keeps addresses that are exact SE starts in `se_ae_offsets.csv`, then maps SE id → VR.
It reproduces every dump-verified result (35993, 38953, 39004, 58377/8, 68221, 403566/7).

Applied, not yet dump-checked:

| AE | SE | VR | Function |
|---|---|---|---|
| 12401 | 12274 | `0x1454a0` | `Lock::SetLock` (crashed the second player's `OnAssignObjectsResponse`) |
| 13718 | 13620 | `0x17c4e0` | GetCellFromCoordinates |
| 14529 | 14383 | `0x19f970` | GetItemCount |
| 14953 | 14775 | `0x1b0900` | TESTexture ctor |
| 19364 | 18949 | `0x28ccd0` | PushEvent |
| 24987 | 24468 | `0x37f980` | `TESQuest::SetStopped` |
| 25004 | 24482 | `0x3803d0` | `TESQuest::SetStage` |
| 27040 | 26454 | `0x3eada0` | CreateTints |
| 34401 | 33623 | `0x550540` | CastSpell |
| 54425 | 53604 | `0x976690` | RegisterSink |
| 70639 | 70273 | `0xcd5180` | NiCamera W2S |
| 70717 | 69335 | `0xcaef60` | CreateTexture |
| 76207 | 74481 | `0xd8a900` | GetObjectByName |
| 82074 | 79937 | `0xf1a3b0` | IsMenuOpen |
| 414675 | 527752 | `0x3423e20` | NiMaskedShader RTTI |

Not applied:
- `AnimationExperiments.cpp` entries, which look experimental.
- Ambiguous name matches: 35993's second hit, 57185.
- 36544 winMain and 37175 (unused dtor).
- Globals with no VR mapping.

No SE address in history, still open: ExtraDataList setters, 16113, 18518, 18563, 19075*, 19846*,
21600, combat 33235/33261/33285, 37511*, 37717*, 38533, 38899*, 38979*, 47196, 52627, 52847,
82088, globals 382393/382400/400312/404125/405282/406126/406160/414391. Starred ids were already
fixed another way.

**Removed hook targets are also called directly.** The second player crashed in
`OnAssignObjectsResponse` → `TESObjectREFR::LockChange` → `RealLockChange` (null, `rip 0`).
- 19512 → SE 19110 (`AddLockChange`, CommonLib `RELOCATION_ID(19110, 19512)`), VR `0x297310`.
  Its hook is live again.
- Null guards added (skip on VR) in `Actor::InitiateMountPackage` (37905), `Actor::PickUpObject`
  (37521), `Actor::SpeakSound` (37542), `PlayerCharacter::SetWaypoint` (40535) and
  `RemoveWaypoint` (40536). Their hook targets are still unresolved.
- Git history and size alignment agree for these still-disabled hooks: 14375→14257, 19708→19282,
  34140→33359, 36291→35402, 37313→36323, 40412→39341, 36564→35565. Enable after dump verification.
- Equip hooks 38928-38935 differ by one between the two methods, because AE inserted a function.
  They need a dump check. Direct equip calls already use CommonLib SE ids.

**Remote player shows up nearly naked** (seen 2026-09-13): only underwear and a torch.

Cause: on the receiver, `TESObjectREFR::AddOrRemoveItem` equips an item only if the `ExtraDataList`
built by `GetExtraDataFromItem` contains `Worn`/`WornLeft`. On VR, `SetWorn` (11612) and the other
setters called crosswalk garbage, so the flag was never set and nothing was equipped.

Fixed and **verified against live game code** (2026-09-13, `scratchpad/verify_extra.py`): each
setter references its `Extra*` vtable (`SetWorn` references both ExtraWorn and ExtraWornLeft;
`SetSoul` does it through a helper call). All 20 git-mined addresses are real function starts.

| AE | SE | VR | Setter | Source |
|---|---|---|---|---|
| 11612 | 11466 | `0x11e470` | SetWorn | constant −146 offset in this block |
| 11616 | 11470 | `0x11ea00` | SetHealth | same |
| 11619 | 11473 | `0x11ede0` | SetCharge | same |
| 11620 | 11474 | `0x11ef40` | SetSoul | same; name DB `BSExtraDataList::SetSoul` |
| 11822 | 11676 | `0x12a640` | SetPoison | same |
| 12060 | 11921 | `0x1372b0` | SetEnchantment | CommonLib |

The −146 offset is pinned by CommonLib AE 11598=SE 11452 and AE 11617=SE 11471 (`SetCount`).

**Mounting: left as is on purpose.** Two players end up on one horse and only the rider steers;
the user considers that minor. Candidate if needed: 37905 → SE 36881, VR `0x60e300`
(`InitiateMountPackage`, anchors AE 37904/37907 = SE 36880/36883). Its hook stays skipped and the
direct call stays null-guarded.

**Face tints:** `NiGeometry::effect` is now at VR 0x168 (CommonLibVR GEOMETRY_RUNTIME_DATA 0x160).
`FaceGenSystem::Update` is enabled on VR again; it hasn't run in-game yet.

**NPC kills by the non-owner didn't sync** (2026-09-13 test). `Actor::Kill`/`Respawn` used the virtuals
`KillImpl` (SE slot 0x10E) and `Resurrect` (SE 0xAB). The VR Actor vtable shift is only verified up
to `SetPosition`, so on VR these now call the functions directly:
- `Actor::KillImpl`: SE 36872, VR `0x60c340` (name DB).
- `Actor::Resurrect(bool, bool)`: SE 36331, VR `0x5dd850` (name DB).

The watcher now also dumps the VR Character vtable (`vtbl_character.bin`, 0x1416d6de0) so every
Actor virtual past 0xAB can be checked against known function addresses.

**Weather is not synced.** The client has Sky hooks but no weather service or messages. Low priority.

**Lydia spawn/remove loop in the second player's log:** both saves have the same follower, so both
clients claim her and ownership bounces. This is a save/gameplay conflict, not a VR port bug.
Dismiss the follower in one save.

**VR upper-body sync (head, spine, arms, hands) is implemented and untested in-game** (2026-09-13).
Files: `Code/client/Games/Skyrim/VRBodySync.h/.cpp`, `encoding/Structs/VRPose`, and server
`MovementComponent` / `CharacterService` relay. The protocol changed, so both clients and the
server must run matching builds.

Design:
- **Capture:** the local player's third-person skeleton, which VRIK drives. Store 12 bones (Spine1,
  Spine2, Neck, Head, both clavicles, upper arms, forearms and hands) as rotations relative to the
  root node.
- **Network:** send them in `VRPose` (32-bit smallest-three quaternions). The server must store
  and relay them; it currently drops `UpdatedVRPose`.
- **Apply:** slerp in `InterpolationSystem`. A vtable hook on `TESObjectREFR::UpdateAnimation`
  (slot 0x7D) sets each bone's local rotation from its parent's world transform, then recomputes
  world transforms under Spine1.
- **VR offsets:** children at node+0x138, parent 0x30, world 0x7C.

First two-player test (17:33): the sender captured (`capturing local VR pose`) and the receiver got
data and installed a vtable swap on Character slot 0x7D, but never applied the pose. Slot 0x7D is
`Actor::UpdateAnimation` (SE 36370, confirmed in live code), and the engine calls it directly, not
through the vtable. It is now a TP_HOOK detour on VR `0x5e1f10` (override added).

What to check in the log:
- `VRBodySync: capturing local VR pose` means the sender found the bones.
- `hooked UpdateAnimation` and `applying remote VR pose` mean the receiver applies them.

If the log shows the pose is applied but nothing moves in-game, the engine writes bone transforms
after `UpdateAnimation`; move the apply step to a later hook. This replaces the old capture
(f377c225), which used the yaw-only `UprightHmdNode` plus world-axis wand poses and had no consumer.

**Disabled on VR: `FaceGenSystem::Update` (remote-player face tints).** It needs 76207, 414675,
70717, 27040 and 14953, all unverified. Remote players keep the base NPC skin and tint colours.

**Still unresolved:** ExtraDataList setters 11612/11616/11619/11620/11822/12060, lock 12401,
13718, 14529, 14953, 15002/15006, 18518, 18563, 19364 PushEvent, 21600 IsFirstPerson (15/16 but
runner-up 5), quests 24987/25004, 27040 CreateTints, combat 32802/33235/33261/33285,
34401 CastSpell, 35993, 36544, 37147, 37175, 37717 SetPlayerTeammate, animation
38952/38953/38979/39002/39004, 47196, 52627, 52847, 54425 RegisterSink, 57185, 59310, 60079,
63362/63372/63591, 70639, 70717, 76207, 82074/82088. Globals (382393, 382400, 400312, 401100,
403566-403568, 403988, 404125, 405282, 406126, 406160, 414391, 414675) can't be size-matched.
Note that 400188 (`0x1f81900`) was confirmed correct via a crash register.

### Intermittent: script event sent to a freed temporary reference during cell attach (2026-09-13)
Crash seen once, about 35 seconds after loading a save, probably before connecting (no connection
lines in the log). The path was TES grid update -> TESObjectCELL attach -> `SkyrimVM::RelayEvent`
-> `HandlePolicy::HandleIsType(type 0x3D)` on handle `0xFFFFFF000FD7`. Temporary form
`0xFF000FD7` was still registered, but its vtable and virtual function pointers pointed into heap
memory, so it had been freed. The dump has no heap, so the form's identity is unknown.
EngineFixesVR `formcaching` (`hk_GetFormByID`) wraps the form lookup and is a candidate (stale
cache entry). No Skyrim Together hook is known to be involved.

Ruled out: the `HookFormAllocate` size match. A scan of all 6096 allocator call sites in the VR
image shows Character allocated at 0x2B0 (VR 0x3882a5 -> ctor 0x69bfc0) and PlayerCharacter at
0x12D8 (VR 0x5beca6). Both equal our `sizeof(Actor)`/`sizeof(PlayerCharacter)`, so ExActor
extensions get their extra space on VR.

If it repeats, take a dump that includes the heap to identify the form.

### FIXED: `TESForm::GetChangeFlags` crashed while syncing remote actors (2026-09-13)
**File:** `Code/client/Games/Forms.cpp`

About 50 seconds into a connected session, a remote Mudcrab spawn crashed at VR
`0x5b238d` (`rcx=0x800000000`). There were two bugs:
- **Wrong function:** id 35503 used the crosswalk value `0x5b2360`. AE 35503
  is really SE 34582 (`BGSSaveLoadChangesMap::GetChangeFlags`), VR
  `0x57f650`. Neighbour ids and function sizes match, and the dump
  disassembly matches the `(this, formId, ChangeFlags&) -> bool` signature.
- **Wrong offset:** the `BGSSaveLoadGame` singleton (SE 516851) keeps
  `saveLoadChanges` at `+0x500` on VR, not `+0x330`. VR
  `BGSSaveLoadGame::GetChange` reads `[rcx+500h]`. CommonLibVR-NG's `0x1fe`
  VR offset for `RUNTIME_DATA2` is misleading: the real alignment is `0x200`.

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

## Hook bisection results + PAPYRUS_FUNCTION table was empty on VR (2026-09-13)

**Bisect tool:** `FunctionHookManager::Add` numbers every hook and writes
`%LOCALAPPDATA%\SkyrimTogetherVR\hook_list.txt`. If `hook_bisect.txt` exists there, only listed
indices (`N`, `A-B`) and `keep 0xDETOUR` addresses are installed. Get `HookFormAllocate`'s
address from the PDB and always `keep` it (actor extensions depend on it). Lessons: enabling
**small subsets** produced new, unrelated crashes (hooks are coupled: save/load pairs, VM
update ↔ script binding). **Removing a small group from the full set** gives clean results.

**Results:**
- **Only `HookFormAllocate`:** the save loads and plays normally.
- **Everything except `SetPosition` (17) + `RotateX/Y/Z` (61–63):** no physics crash; the player
  loads in. **The physics use-after-free is in one of those four.** Still narrowing.

**Separate bugs found on the way:**
- `BSScript::Variable::Reset` (104296) had crosswalk `0x13bc0b1` (mid-function) → exception
  inside noexcept → `std::terminate`. Now VR `0x126f1c0` (AE 104297 ↔ SE 97509 both 0x290 → 104296
  = SE 97508). `IObjectHandlePolicy::Get` now asks the VM on VR instead of the unverified global 414391.
- **`HookRegisterPapyrusFunction` (104788) had no VR address**, so PapyrusService's name→native
  table was empty and **every `PAPYRUS_FUNCTION` was null** (crash: `HookActivate` →
  `GetOpenState` → call 0). Fixed on VR by hooking the VM vtable's `BindNativeMethod` (slot 0x18)
  from inside `HookBindEverythingToScript`, before the game registers its natives.

## SOLVED: physics use-after-free = Actor vtable has a SECOND VR-only slot (2026-09-13)

Bisect result: all hooks except `RotateX/Y/Z` still crashed, so **`HookSetPosition` is the
culprit**. For non-player actors it calls `apThis->SetPosition(pos, false)`, a virtual. From a
VR Character vtable dump (`VTABLE_Character` VR `0x16d6de0`):
- VR `0xA8` `0x5dad60` (player override `0x6f6020`) = DetachCharController (SE 0xA7, +1)
- VR `0xA9` `0x5dc110` = RemoveCharController (SE 0xA8, +1); it reads `[rcx+F0]` currentProcess
- **VR `0xAA` `0x5dc170` = VR-only, and it calls slot 0xA9**
- **VR `0xAB` `0x5dc380` = SetPosition(pos, bool)** (SE 0xA9, **+2**): it calls
  `TESObjectREFR::SetPosition` `0x2a8010`

With only the `TESObjectREFR` insertion, our `SetPosition` went to VR `0xAA`, which removed the
character controller of every NPC whose position was set → dangling `bhkCharRigidBodyController`
in the physics world. Fixed: `Actor.h` declares a VR-only `VR_Unk_AA()` before `SetPosition`.

**Not verified:** Actor virtuals declared after SetPosition that our code calls (`Resurrect`,
`PayFine`, `SetRefraction`, `AttachArrow`, `PutCreatedPackage`, `UpdateAlpha`, `MoveToHigh`...)
now assume exactly +2 on VR. Check any of them against the VR vtable before trusting them.
The bisect file has been disabled (`hook_bisect.txt.disabled`), so all hooks run again.

## (superseded) player physics controller use-after-free during save load (2026-09-13)

The current crash: Havok post-simulation callback (`bhkWorldM`/`ahkpWorld` `postSimCb`) → a
`bhkCharRigidBodyController` virtual call at `0x140e48638` on an object whose vtable slot
holds a heap pointer, i.e. freed memory. Crash Logger's stack walk ties it to the player
("Queen Emma", `PlayerWorldNode`). **Crash Logger VR then crashes in its own symbol code and
calls `TerminateProcess`**, which is why several runs died with no log or dump.

Ruled out so far:
- **Allocation sizes:** the VR player is 0x12D8 and Characters 0x2B0, confirmed by
  `mov edx, 12D8h` / `2B0h` before calls to MemoryManager::Allocate (VR `0xc3d0e0`).
  `HookFormAllocate` is committed before game main. Other 0x2B0 allocations are memory-heap
  objects at init (harmless).
- **Engine Fixes VR `MemoryManager`:** it's `false`.
- **UpdateReference3D hook signature:** the target only uses `rcx`.
- **The 27 interpolated addresses:** 17 have names in `1.5.97_comments.csv`, and all match our
  use (RemoveItem, GetContainer, ReadFormIdFromBuffer, DropObject,
  SkyrimVM::ProcessRegisteredUpdates, ...).

Found along the way: **before the VR vtable shift was added, `Actor::SetPosition` (SE slot 0xA9)
called VR slot 0xA9 = SE 0xA8 `RemoveCharController`** for every NPC position change
(`HookSetPosition`). It's fixed by the shift, but it shows how wrong slots silently break physics.

Next step: the watcher now has hardware breakpoints on VR `PlayerCharacter::DetachCharController`
(`0x1406f6020`, vtable slot 0xA8) and `RemoveCharController` (`0x1405dc110`, 0xA9, filtered to
the player), to log who frees the player's controller.

## Silent exits = CEF CHECK failures: the overlay doesn't exist on VR (2026-09-13)

After the layout port, the game started dying during save load with **no log, no dump, no
WER event**. A debugger watcher (`cdb -p`, breakpoints on `TerminateProcess` /
`RtlExitUserProcess`, second-chance handlers) caught it: `int3` + `ud2` inside `libcef.dll`,
Chromium's `IMMEDIATE_CRASH` for a failed `CHECK`. It was called from
`OverlayService::RunDebugDataUpdates` → `CefListValue::Create`. On VR `OverlayService::Create`
never runs (it's driven by the D3D11/renderer hooks that are disabled on VR), so
`m_pOverlay` is null and **CEF was never initialized**. Any CEF API call then kills the process.
Our vectored handler only reports AV/GS, so none of this was logged. The path became
reachable once the player singleton and layouts were right, and `OnUpdate` started running.

**Fix:** every `OverlayService` method that touches CEF now returns early when `m_pOverlay` is
null. `PartyService::OnPartyInfo`/`OnPartyInvite` still update party state and only skip the
UI part. `InputService::SetUIActive` checks `GetOverlayApp()`.

**Consequence:** Skyrim Together's UI (connect dialog, party, chat, debug data) is
completely absent on VR. Connecting to a server will need another route (console/command,
or a VR-native UI). Any new crash inside `libcef.dll` means another unguarded CEF call.

**Tooling note:** the watcher command file must handle `bpe` (continue on first chance). A
bare break leaves `cdb` at a prompt, it reads EOF, quits, and **kills the game**.

## Struct layouts: VR uses the SE layout, not AE (2026-09-13)

Crash in `DiscordService::OnLocationChangeEvent`: `[player+0xAD0]` treated as a BGSLocation.
Skyrim Together's game structs are AE layouts, and **Skyrim VR's are SE layouts plus VR
additions**:
- **`ExtraDataList`** got a `virtual ~ExtraDataList()` for AE in commit **553793fc**. That
  vtable shifts every later member of `TESObjectREFR`, `Actor` and `TESObjectCELL` (which embed
  it) by 8. It is now `#ifndef SKYRIMVR`. On VR: `TESObjectREFR` 0x98, `Actor` 0x2B0,
  `TESObjectCELL` 0x140. The Actor asserts use the pre-AE values from that commit, and the cell
  asserts match CommonLibVR-NG's SE/VR column.
- **`TESObjectREFR` vtable:** VR has an extra virtual at slot 0x82 (`AttachWeapon`), so
  every later virtual is one slot higher. A VR-only `VR_AttachWeapon()` is declared after
  `sub_81`.
- **`PlayerCharacter`** is far bigger on VR. New `#else` layout from CommonLibVR-NG: objectives
  B70, skills 10B0, location 11C8, difficulty 11F4, tints 1208/1220, size **0x12D8**. The SE
  equivalents match this file's pre-AE layout exactly.
- **`Projectile`** padded to absolute AE offsets. On VR, `fPower` is 0x188 (was 0x190, and it
  gets written).
- **Why it matters beyond field reads:** `HookFormAllocate` (`Memory.cpp`) enlarges an
  allocation to append `ActorExtension` only when the size equals `sizeof(Actor)` /
  `sizeof(PlayerCharacter)`, while `GetExtension()` casts every Character form regardless. With
  AE sizes, no VR actor got an extension, and every extension access read/wrote past the real
  object into the heap.

**Not yet audited:** other structs with AE-era hand-written offsets (`AIProcess`,
`ActorExtension` consumers, `TESNPC`, `BGSEncounterZone`, menus...), and virtual slot shifts
in `Actor`'s own vtable beyond the `TESObjectREFR` one (CommonLibVR-NG annotates
"SE/AE 0x93, VR 0x94" etc. — it looks like the same single shift, but that isn't verified).

## Globals/singletons from the crosswalk are wrong too (2026-09-13)

The first in-world crash: `DiscoveryService::VisitCell` → `TESObjectREFR::GetWorldSpace` with
`this = 0xffffffff007c9be0`. `PlayerCharacter::Get()` used AE id 401069 → crosswalk
`0x1c3cbd8`, and that address held exactly that garbage. Our code's global ids use an
older AE numbering than CommonLib (401069 vs CommonLib's 403521 for the player), so neither
CommonLibVR-NG ids nor `se_ae.csv` (functions only) map most of them.

**Fixed:**
- **Singletons, from CommonLibVR-NG names:** player 401069 → `0x2feb9f0` (VR CSV SE 517014);
  TES 400441 → `0x2feb6f8` (SE 516923).
- **INI/game settings, found in the dump:** search for the name string, then for a pointer to
  it. The Setting is `[vtable][value][name]`, so the value is 8 bytes before the name pointer.
  `bAlwaysActive:General` 380768 → `0x1eabf30` (value 1). **This one was WRITTEN every
  frame** by `TiltedOnlineApp::Update`, corrupting memory at the old address.
  `iDifficulty:GamePlay` 381472 → `0x1eaef68` (2). `fAIMinGreetingDistance` 370892 →
  `0x1e96c58` (85.0).

**Still on crosswalk values (read-only uses, unverified):** 400312 null handle; 403566/403567
ActorMediator; 404125 AI timer; 406126/406160 HUD world-to-cam matrix and viewport;
414391 script object handle policy; AnimationExperiments 401100/403568/403988; the
CombatView debug globals. Any of these can feed garbage into logic; resolve them by name
the same way when they surface.

## Third bad-address class: untranslated AE ids that exist in the VR CSV as SE ids (2026-09-13)

The save got as far as spawning actors, then crashed in `TESObjectREFR`'s
`HookAddInventoryItem` with item pointer `0x29`. Its id, 37525, was never translated,
but **37525 is also a valid SE id in the official VR CSV** (`0x62a180`, a different
function). So it resolved cleanly to the wrong function. None of the earlier audits
caught this, because they only looked at untranslated ids that were *missing* from the
CSV. There were 9 such ids:
- **Fixed by name** (`se_ae.csv` + `database.csv`): 13894 → SE 10878
  `BGSDefaultObjectManager::GetSingleton`; 19800 → 19373 `TESObjectREFR::Enable`;
  51093 → 50164 `Console::SetSelectedRef`; 52933 → 52050 `DebugNotification`.
- **Fixed from the dump:** 19789 `s_rotateZ` → SE 19362, VR `0x2a7f00` (override). It
  compares `[rcx+50h]`, while its siblings X/Y compare `+48h`/`+4Ch`.
- **Unknown, set to 0 on VR** (null-guarded): 37525 AddInventoryItem hook, 37527
  GetGoldAmount (returns 0), 33282 sortTargetSelectors, 37757 GetDetectionState (debug view).

Because the override table only applies to ids *not* in the CSV, these fixes change the
VR id in `POINTER_SKYRIMSE` itself (to the SE id, which uses the CSV's own correct entry),
or use a new SE-id override key.

## Save-load crash: BSThread hooks were on the wrong functions (2026-09-13)

Loading a save crashed in `Base::SetThreadName` with name = `0x768`, called from game
`0xc80633`. The BSThread hooks used crosswalk values, and both were wrong:
- `68261 → 0xc80600` (hooked as `BSThread::Initialize`) is a small lock/flag
  function. **The hook is now disabled on VR** (it only named threads).
- `69066 → 0xca3800` (Jump to `Hook_SetThreadName`) was a destructor, which the Jump
  overwrote. The real VR `BSThreadUtils::SetThreadName` is **`0xc6b170`**, found by
  searching the decrypted game (in a dump) for `b9 88 13 6d 40` (`mov ecx,406D1388h`).
  Two hits; this one builds `THREADNAME_INFO{0x1000, name, id, 0}` from `(ecx, rdx)`, is
  0x40 bytes like AE 69066, and `addrlib.csv` lists it as SE 67740. The other hit,
  `0xa122a8`, is probably Havok's.
- An earlier session concluded `Hook_SetThreadName` was "required by the custom loader".
  That was almost certainly the MinHook disable-all bug (mass-nulling ids disabled every
  hook). The game has been naming threads natively all along, because the Jump was
  elsewhere.

Audit of raw patches still using unverified ids on VR, now neutralised:
`Projectile.cpp` 34452 (a `Jump` at +0x374; its comment claimed it was neutralised but
both `#ifdef` branches used the id), `BSInputDeviceManager.cpp` 68617 (hook only forwards
rcx/xmm1, and does nothing on VR), `BSRandom.cpp` 68276/14774 (falls back to `aMin`).

`CrashHandler::RemovePreviousDump` used to delete any file whose path contained "crash".
Dumps now land in the **game folder** (spoofed `GetModuleFileName`), so it's restricted
to `crash_UTC_*.dmp`. Look for new dumps in `F:\...\SkyrimVR\`.

## Main menu with no text = data load blocked by a plugin popup (2026-09-13)

"Logo and smoke but no menu text" means the game is still loading. The menu text
appears after SKSE's `kDataLoaded`. A live, non-invasive attach (`cdb -pv -p <pid>
-c "~* kn 18; qd"`) showed the loading thread sitting in `MessageBoxW` opened by
**DynDOLOD.dll** from its `kDataLoaded` handler: `SKSE/Trampoline.cpp(54): failed to
create trampoline`. Popups are invisible in the headset, and CommonLib plugins
terminate the game when OK is clicked.

**Cause:** CommonLib's `Trampoline::create` needs free memory within ±2GB of the image
base (`0x140000000`). The VR launcher used SE's 1GB `exeLoadSz` for `game_seg`, so our
image ran to `0x18094a000`, and heap allocations filled what was left of the window by
`kDataLoaded`. **Fix:** VR `exeLoadSz` is now `0x05000000` (80MB; `SkyrimVR.exe`
SizeOfImage is `0x3959000`), so the image ends at `0x14594a000`. `ExeLoader::Load` now
rejects images larger than the buffer instead of silently skipping sections. Side
effect: crash dumps with `MiniDumpWithDataSegs` should shrink from ~1GB.

**The `exeLoadSz` change alone did not fix it** (the popup came back). By `kDataLoaded`,
heap allocations fill the ±2GB window regardless. **Second fix:
`Code/immersive_launcher/memory/NearImageReserve.cpp`** (VR only). At the start of `main()`
it reserves a 256MB pool just below the image (`0x130000000`–`0x140000000`), and hooks
`KernelBase!VirtualQuery` / `VirtualAlloc`. `VirtualQuery` reports the pool as `MEM_FREE`.
A `VirtualAlloc` with an explicit address inside the pool releases that piece, allocates,
and re-reserves the rest. Allocations with `lpAddress == NULL` can never land in the pool.
A standalone test (`cl` + the xmake MinHook 1.3.4 package) filled the address space with
250GB of reservations and then ran CommonLib's exact `do_create` search: 39/40 trampolines
came from the pool, all within 256MB of the image, and 200 more succeeded while another
thread churned 20k ordinary allocations. Served allocations are logged via
`OutputDebugString` (`NearImageReserve: served ...`), visible in DebugView.

**Debugging tip:** when the game "hangs" on a screen, attach non-invasively and look
for `MessageBox` frames before anything else. Also, the Crash Logger plugin writes
`Documents\My Games\Skyrim VR\SKSE\crash-*.log`.

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

## Early EngineFixesVR crash: the d3dx9_42 plugin preloader (2026-09-13)

**Not environmental after all**: it reproduced with MO2 settled. The raw stack showed
the chain: `launcher::StartUp` → game CRT → our `Hook_initterm_e` (`Memory.cpp`) → the
**"skse64 plugin preloader" `d3dx9_42.dll`** (FUS ships it) → `LoadLibraryA` →
ReShade/usvfs → `EngineFixesVR.dll` static init → `GetModuleHandleA("SkyrimVR.exe")`
fails → crash while logging. The preloader's log (`overwrite\Root\d3dx9_42.log`) shows it
had **never fired in a Skyrim Together run before 08:49**, the first run of the build that
removed 50 crosswalk hook entries. One of those bogus detours was presumably
(accidentally) keeping the preloader's `_initterm_e` path from firing. In every run that
reached the menu, EF was loaded later by SKSE VR and worked.

**Real root cause (found after the mitigation below failed to trigger):** a
`FunctionHookManager` bug that my hook-entry removals exposed. `FunctionHookManager::Add`
returns early for a null target, which destroys the temporary `FunctionHook`, whose
destructor called `MH_DisableHook(m_pSystemFunction)` with `nullptr`. **MinHook's
`MH_ALL_HOOKS` is `NULL`**, so every skipped hook disabled *all* hooks in the process,
including the launcher's `CoreStubsInit` stubs (`LdrLoadDll`, `GetModuleHandleA/W`,
`GetModuleFileName*`). Evidence: the crash stack went `LoadLibraryExW` → `ntdll!LdrLoadDll`
with no `TP_LdrLoadDll` frame, and launching became instant instead of ~2 minutes.
Removing 50 crosswalk entries created 50 null hook targets. Fixed in
`Libraries/TiltedReverse/Code/reverse/src/FunctionHook.cpp`: `Add` disarms the hook before
returning, and `~FunctionHook` also requires a non-null target. **Any earlier build that
skipped a null hook had the same silent problem.** The null-guard itself is from an earlier
session, so previously "parked" ids may have been disabling launcher stubs all along.

**Mitigation (VR only):** `TP_LdrLoadDll` (`stubs/FileMapping.cpp`) returns
`STATUS_DLL_NOT_FOUND` for `EngineFixesVR.dll` until `g_ScriptExtenderStarting` is set
(`ScriptExtender.cpp`, just before SKSE VR is loaded). So EF now always loads via SKSE, as
in the working runs. It deliberately doesn't use the blocklist's invalid-image-hash
status, which tells Windows not to retry. Other preloaded plugins
(`PrivateProfileRedirector`) are still preloaded.

### Earlier (wrong) hypothesis, kept for context: MO2's VFS not ready

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
