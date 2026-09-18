# Skyrim Together VR: port notes and known issues

What is different, missing or fragile in the Skyrim VR (1.4.15) port, and why. Related files:
- `VR_TODO.md`: the roadmap.
- `VR_POINTERS_TODO.md`: game addresses still missing or unconfirmed on VR.
- `VR_MULTIPLAYER_GUIDE.md`: how to install and play.
- `github.com/cmpayc/TiltedEvolutionVR`: an independent VR port; its address table and fixes were used here.

Git history has the full investigation notes behind each item.

## 1. Install requirements that fail silently

| Requirement | What happens without it | Where it's handled |
| --- | --- | --- |
| `SkyrimTogether.esp` header version **1.70** (not 1.71) | The engine rejects the plugin: main menu with a logo but no text, then a crash in `TESFile::OpenTES`. | Patched in `GameFiles/Skyrim/SkyrimTogether.esp` (works on SE/AE too). |
| **SKSE VR** loaded by the launcher | No SKSE plugins, including Skyrim VR ESL Support. More than 255 plugins overflow VR's plugin array and crash in `OpenTES`. | `ScriptExtender.cpp`: loads `sksevr_*.dll`, whose `DllMain` starts it (no `StartSKSE` export). |
| EngineFixesVR loaded **by SKSE**, not by the `d3dx9_42` plugin preloader | Crash at startup, before any log. | `stubs/FileMapping.cpp` refuses the DLL until SKSE starts. |
| Free address space within ±2GB of the game for plugin trampolines | DynDOLOD shows an invisible "failed to create trampoline" popup at data load, and the game hangs or closes. | 80MB game buffer (`TargetConfig.h`), plus a reserved pool in `memory/NearImageReserve.cpp`. |
| `uGridsToLoad = 5` | The server refuses the connection. | Checked on connect. |

## 2b. Audit of 2026-09-18 (external, then re-checked here)

A friend ran an automated audit of every address this client resolves. Its own conclusion was that
only three comparisons had a strictly better alternative; the other sixteen "differences" were ties,
where a tied score is not evidence against the address in use. Re-checking each one here:

| Finding | Verdict | What was done |
| --- | --- | --- |
| `s_regenAttributes` VR id 36452 | **Confirmed wrong.** SE 36452 is `TESObjectREFR::GetSubmergeLevel`: VR 0x1405e9b60 loads `xmm1` and tests `r8` as a pointer, so it is `(this, float, TESObjectCELL*)`, while the hook declares `(this, int, float)`. SE 37513 (`Actor::RestoreActorValue`, VR 0x1406296b0) reads `edx` as an int and `xmm2` as the amount, and is what `5da6679e` called. | VR id changed to 37513. |
| `sub_14063CFB0` VR id 38952 | **Confirmed wrong.** AE 38952 is also `PerformIdleAction`'s id, so the override for that id handed this pointer 0x140644070 — the outer dispatcher, which opens with the same `[rdx+0x58]` flag test that `ActorMediator::RePerformIdleAction` reimplements, so the call at the bottom of that function re-entered the whole action. The name carries its SE address: 0x14063cfb0 is SE 38047, VR 0x140646020. | VR id changed to 38047. |
| `dispelAllSpells` alternative | **Not accepted**, as the audit itself said: the alternative scored about 53 and nothing else supports it. | Left alone. |
| `s_release` VR id 67847 | **Left alone.** The audit's own second hypothesis ranks the current address first, and `se_ae.csv` pairs it at confidence 3. The SE-era literal points 0x1140 lower, at a sibling. | Left alone. |
| Five RTTI overrides with offset 0 | **Real.** `{394235, 396753, 396833, 396837, 400180}` resolve to the image base rather than null, so those five type checks silently never match. Not a crash: 0x140000000 is readable. | Documented, not changed. |
| CSV metadata row `13291,0.158.0` | **Real.** `stoull` stopped at the dot and produced offset 0, so id 13291 mapped to the image base. Nothing looks that id up, but any malformed row would have done the same silently. | `LoadCSV` now rejects a row unless both fields parse whole. |

Two checks worth keeping, both run over every `POINTER_SKYRIMSE` this repo has ever declared:

- **No AE id anywhere collides with a real SE id in the VR library.** Where nobody converted an id
  (77 of them), the AE number simply is not in the library, so it falls to an override or stays
  unresolved rather than silently resolving to the wrong function.
- **Ten of the eleven overrides that the SE-era code can corroborate agree with it exactly.** The
  one that did not was `sub_14063CFB0` above.

A caution the audit did not raise: the AE id for `s_regenAttributes` (37448, AE 0x140607080) does
not pair with SE 37513 in any table, and bracketing it between its paired neighbours puts it
somewhere else entirely. Either upstream's AE id is wrong for this hook, or the pair table is too
sparse there to bracket across. The VR side was decided on the disassembled signature, which is the
stronger evidence; the AE path was left untouched.

## 2. How game addresses are resolved on VR

- **Ids in this code are AE (1.6.x) ids; VR uses SE numbering.** The same number is a different function.
  `POINTER_SKYRIMSE(type, name, aeId, vrId)` takes a separate VR id, which should be the SE id.
- **Lookup order:** the official VR Address Library CSV, then `Code/client/VRAddressOverrides.h`
  (only for ids the CSV lacks).
- **Three failure modes, and only one of them crashes where it happens:**
  1. No address: `Get()` is null. Hooks are skipped, and direct calls must be null-guarded.
  2. Wrong address from the old SE/VR binary-diff "crosswalk": it was wrong for **0 of 69** checked
     functions (right for RTTI). Hooks then corrupt unrelated game code, and crashes show up far
     away.
  3. An untranslated AE id that is also a valid SE id in the CSV: it resolves cleanly to the wrong
     function.
- **Finding the right id, most to least reliable:**
  1. CommonLibVR-NG `RELOCATION_ID(se, ae)`.
  2. `vr_address_tools` `se_ae.csv`.
  3. SE 1.5.97 addresses in this repo's git history (commit `5da6679e`).
  4. Neighbouring ids with matching function sizes.

  Then check the target in the disassembly.
- **Fixed in `Libraries/TiltedReverse` (`FunctionHook.cpp`):** skipping a hook with a null target
  used to call `MH_DisableHook(nullptr)`, which disables *every* hook, including the launcher's
  stubs.

## 3. Struct layouts and virtual tables

VR uses the **SE layout plus VR additions**, not the AE layout the code was written for. All
entries below are under `#ifdef SKYRIMVR` with `static_assert`s.

- `ExtraDataList` has no vtable on VR (AE only), so `TESObjectREFR` is 0x98, `Actor` 0x2B0 and
  `TESObjectCELL` 0x140. `HookFormAllocate` only adds `ActorExtension` when the size matches
  exactly, so wrong sizes corrupt every actor.
- `PlayerCharacter` is 0x12D8 (offsets from CommonLibVR-NG).
- `Projectile::fPower` is at 0x188.
- `PlayerControls::Data` is at +0x24.
- `NiGeometry` shader property is at 0x168.
- `BGSSaveLoadGame` change map is at +0x500.
- `NiAVObject` is 0x138 on VR (SE 0x110), so `VRBodySync` reads nodes by raw offsets.
- **Virtual tables:**
  - `TESObjectREFR` has one extra VR virtual at slot 0x82.
  - `Actor` has a second one before `SetPosition`, so `Actor` virtuals from there on are two slots
    higher. Every named virtual the client calls was checked against a VR vtable dump.
  - `MoveToHigh/Low/...` are declared one slot early; this is harmless because nothing calls them.
- **Unverified assumption:** `Actor::IsDragon` reads the `TESRace` keywords at +0x78/+0x80 (SE layout).

## 4. Features that are off or work differently on VR

| Feature | State on VR | Why |
| --- | --- | --- |
| Skyrim Together UI (connect dialog, chat, party) | **SteamVR dashboard tab** "Skyrim Together" (`Systems/VRDashboard.cpp`). F6 and `connect.txt` still work, and status also shows as HUD messages. | No game window to draw into. CEF renders offscreen into a texture on our own D3D11 device; the laser pointer and SteamVR keyboard are forwarded to the page. |
| Menu patches (menus don't pause the game while connected, favorites numbering, intro movie skip) | **On** since 2026-09-14 | Addresses from TiltedEvolutionVR, patch points checked in the VR code. Message boxes still pause: unpaused, they are invisible in the headset. |
| Skills / level up menu unpaused | **On** since 2026-09-16, untested | Unpaused, the menu sets `kFreezeFrameBackground` and waits for a freeze frame VR never renders, which is why it was black. SE 51638 (`StatsMenu::ProcessMessage`, VR 0x8ec3e0) writes that flag at +0xBB6 (`or dword ptr [rsi+0x1C], 0x20`; AE has it at +0xA10), and the four bytes are NOP-ed after a byte check. The other three AE patches have no VR address: "menu not appearing" (+0x84E), "keep the menu updated" (+0x1040), and the controls fix (AE 52518), so in-menu input may not respond. |
| Projectile null-handle patch | **On** since 2026-09-16 | SE 33672 is in the VR address library (0x554980). The VR frame differs, so the patch point is +0x397 (`mov rbx, [rsp+0x58]`) with a 0x158 frame, checked in the VR code. |
| Renderer and input byte patches | **Off** | Addresses unverified; a wrong patch silently corrupts code. |
| Naked-NPC re-equip workaround | **Off** | `GetArmorInSlot` doesn't exist on VR. |
| Projectile metadata (spell, weapon, ammo, cell) | **Not sent** | `Projectile::LaunchData` layout is wrong on VR. A remote shooter's projectile is launched and then deleted, because VR's `LaunchSpell` doesn't null-check. |
| First-person checks and camera switching | Always third person, no switching | VR has no first-person graph (same as CommonLibVR). |
| `Actor::Kill`, `Respawn` | Called by address | Chosen before the vtable was fully checked; works. |
| Mount sync | **On** since 2026-09-15 | `InitiateMountPackage` (SE 36881, VR 0x60e300) checked in the VR code: the rider takes ownership of the horse, and the other client mounts the remote rider on it. |

## 5. VR-specific sync added in this port

- **VR body sync** (`VRBodySync.cpp`, `VRPose`):
  - 12 upper-body bone rotations relative to the root, read from the skeleton VRIK drives.
  - Sent with movement (32-bit quaternions) and relayed by the server.
  - Applied once per frame at the renderer's frame end (`StopTimer` call in SE 75461, +0x15), on one
    thread. It used to be applied from the animation job threads (hook on SE 36372), and the renderer read
    half-written bones: the body flickered, and the face and spell beams stayed on the animation pose.
    Approach and offsets from TiltedEvolutionVR's measurements, implemented here separately.
  - Each posed bone is turned about its own position and everything below it is carried: child nodes
    (weapon, shield, magic node) and the skeleton's flattened bone array (`BSFlattenedBoneTree` +0x158,
    0x80 per bone), where fingers and facial bones exist without a node.
  - Cached bone pointers are checked every frame against what they pointed at when resolved (a freed
    node's first bytes change), and the skeleton is resolved again when they differ.
  - Not posed: dead, dying or bleeding out actors (the ragdoll owns them), and actors outside a 50 degree
    view cone from the headset (`PlayerCharacter` +0x570, checked by name), because posing a culled actor
    tears its skin.
  - Bones are found along the skeleton chain, because physics armour carries duplicate hand nodes.
- **Send rate:** the local player 30 Hz, other actors 10 Hz. The VR pose plays back 100 ms late,
  movement 300 ms.
- **Equipment snapshot:**
  - Once a second the player's worn items and hand spells are compared and sent if changed.
  - The receiver makes the remote hands match.
  - Single equip events were lost or out of date on VR.
  - **Protocol change:** the server and all clients must match.
- **Actor removal grace period:** an NPC this client owns that loses its 3D stays known for 5 s. Before this, a
  circling dragon was destroyed and recreated by the server 73 times in one session. Copies of actors owned by
  another player are still removed at once: with the grace period they ignored the server's respawn after a
  load door, and a remote player stayed invisible. A spawn request for an entity whose actor is gone now
  re-creates it.
- **Dragon detection** also checks the race keyword `ActorTypeDragon`, so modded dragons get the wide
  range from their first spawn.
- **Death sync:** the server sends a death to every player, not only players in range (a missed death left a
  living body forever). A remote NPC is allowed to play its death animation. While dying, a remote body falls
  with its own ragdoll; once dead it is moved to the owner's corpse position when more than 64 units away.
- **Remote AI suppression** (`Actor::Process` hook) skips every remote actor except corpses. Letting remote
  players run their update was tried against the interior head glitch and made both players flicker constantly.
- **Addresses from TiltedEvolutionVR:** about 60 VR addresses (remote NPC AI suppression, item sync, dialogue,
  subtitles, waypoints, beast form, time skip and more) come from that fork's table, cross-checked against
  SE/AE function sizes and this repo's git history. Its Papyrus VM vtable layout (two extra VR virtuals) is
  also applied.
- **`FadeOutGame` takes seven arguments on VR** (an extra flag and a ref-counted "fade done" callback, as the
  game's own callers show). Passing five made the game store stack garbage as the callback and crash when
  the fade-in after a respawn finished.
- **Respawn** finishes once started. On VR the player left bleedout early, which left the screen black.
- **Stutter report:** `Perf spike: ...` in the log when a frame is over 25 ms, naming the slowest mod
  section.

## 6. Open bugs

- **Many sync hooks were enabled at once** (see section 5) and are untested in play. If a crash
  points at one of them, its address is the first suspect.
- **VR menu shows an empty tab.** The page paints at 1600x900, runs its scripts (CEF console log), and SteamVR
  accepts the frame. Two causes are fixed, untested: the pixel upload was never flushed on our own D3D11
  device (it never presents, so SteamVR could read a blank shared texture), and the UI is mostly transparent
  (it's made to sit over the game), so it is now blended over an opaque panel. The upload log line gives the
  share of the page that has content.
- **Crash while quitting the game** (every quit wrote a ~100 MB dump): mimalloc freeing a pointer into a
  plugin's image (po3's ENB light plugin) during exit. Frees are now skipped once exit has started
  (`Memory.cpp`); untested.
- **Remote player's bare face out of the helmet, and the body flickering**, seen only in one interior
  (Gallows Rock, cell `15273`). Outside it looks right, and yesterday's build (`8670d4c4`) shows
  the same thing there, so it isn't a regression. Ruled out by logs: the face, hair and helmet are all skinned
  to the real skeleton head bone, and nothing fights the actor's position. Cause unknown.
- **Players with different modlists** are missing each other's NPCs (`Failed to retrieve Actor X,
  possibly missing mod`), so those NPCs can't sync.
- **Shared follower** in both saves (Lydia): both clients claim her and ownership bounces. Dismiss
  her in one save.
- **Remote dragons:** the client grid check still passes `IsDragon = false` for entities without a
  local actor.
- **Intermittent crash:** a script event sent to a freed temporary form during cell attach, seen once.
  EngineFixesVR form caching is a suspect. A heap dump is needed if it repeats.
- **Unconfirmed addresses** (`VR_POINTERS_TODO.md` section 5): suspect these first when a crash lands
  near one.

## 7. Debugging

- **Read `logs/tp_client.log` first.** On a crash it contains the faulting access, the registers and a
  raw stack scan. A real stack walk is impossible, because the custom-loaded game image has no
  unwind info.
- **Crash dumps** (`crash_UTC_*.dmp` in the game folder) are about 1GB, but they are the only place
  the game code can be disassembled: `SkyrimVR.exe` on disk is SteamStub-encrypted. Use `cdb`, not
  the WinDbg GUI:
  `cdb -z <dump> -c ".ecxr; kn 30; u @rip-40 L20; q"`
- **Symbol names near a crash are unreliable** (LTCG folding). Read the instructions.
- **The game hangs on a screen:** attach non-invasively (`cdb -pv -p <pid> -c "~* kn 18; qd"`) and
  look for a `MessageBox`. Popups are invisible in the headset.
- **The game closes silently with no log:** usually a CEF `CHECK` (an overlay call before the overlay exists) or
  another plugin calling `TerminateProcess`. Crash Logger VR writes to
  `Documents\My Games\Skyrim VR\SKSE\`.
- Logs from before 2026-09-13 say "coredump created" even when no dump was written.
- **Don't keep a debugger attached during play sessions.** Thread creation in DynDOLOD and similar
  plugins makes it stutter.
