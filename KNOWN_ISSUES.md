# Skyrim Together VR: port notes and known issues

What is different, missing or fragile in the Skyrim VR (1.4.15) port, and why. Related files:
- `VR_TODO.md`: the roadmap.
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
| The **VR Address Library file that matches Community Shaders**: Emma's combined file (0.275.0 with every 0.158.0 id at its old address; section on it below) | Crash a second into the first load, before connecting, in shadow rendering: `SkyrimVR.exe+0x1354e5a`, which the crash logger names `100997+0x13A`. A plain newer library moves id 100997 to 0x1354D20 and the installed Community Shaders 0.8.7 patches there. Seen, twice on 2026-10-06 (19:41, 19:43), after installing Engine Fixes 7.10. | Not by the mod: give everyone the same library file. |

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
- **Lookup order:** the official VR Address Library CSV, with `Code/client/VRAddressOverrides.h` laid over it.
  Our table wins where the two disagree, and the log says so at start-up (`Address id N differs`). It was the other
  way round until 2026-10-01: library 0.275.0 defines id 35269, which this client uses as an (AE) number for the
  dialogue-option function, so a newer library would have put that hook on an unrelated function.
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
- **A hook whose address did not resolve is skipped, and named in the log** ("Hook X not
  installed: its game address did not resolve"). The check is `AddHookIfResolved` in
  `Code/client/TiltedOnlinePCH.h`, which `TP_HOOK` and `TP_HOOK_IMMEDIATE` go through. It
  matters because MinHook crashes on a null target. Passing it null also had a second trap:
  `MH_DisableHook(nullptr)` disables *every* hook, including the launcher's stubs.
  - This check used to be patched into `Libraries/TiltedReverse` itself (commit `d3ac591`,
    plus an uncommitted destructor guard). That commit was never pushed anywhere: the submodule
    remote is upstream TiltedPhoques. So the repo's submodule pointer stays on upstream
    `55ee3f29`, and nothing goes in the submodule.
  - VR skips three hooks on every start (2026-10-04), none of which loses anything:
    - `HookMainLoop` and `HookVMDestructor` (ids 36564 and 40412): empty placeholders.
    - `HookRegisterPapyrusFunction` (id 104788): already installed from the VM's vtable
      instead (`BSScript.cpp`).

    Without the check, any one of them crashes the game at start.

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
- **Dropped items** are kept by the server (`DroppedItemService`, `dropped_items.bin`) with where they lie, and moves
  of them are relayed while they happen. Two known gaps: an item this save already has, which somebody else moved more
  than 200 units while this player was offline, is not recognised on reconnecting and is placed a second time; and
  an item's turn is read from its reference, which may lag behind its 3D while it is carried.

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
- **Intermittent crash in the script engine on a freed temporary form -- a game/modlist crash, not ours.**
  Fingerprint: top frame `SkyrimVR.exe+0x93CE17` (`SkyrimScript::HandlePolicy`, turning a script handle into an
  object), `RDI = 0x3D`, `R15 = SkyrimVM*`, an access violation *executing* a heap-like address -- very often exactly
  `0x2001D342D0`, with `RCX` a `BSDismemberSkinInstance*`: the form was freed, its memory reused by a skin instance, and
  a virtual call reads past that object's vtable. The low word of `RSI` is the temporary form's id.
  - **Emma's own Crash Logger folder has it ten times since 2024-12-21**, same top frame every time, most of them
    before this mod existed. 2026-07-25 is byte-identical to Seen's crash of 2026-09-30 (address and `RCX` type), and
    Skyrim Together first ran the game on 2026-09-12.
  - 2026-09-30, co-op: Seen at 18:30:06 (form `FF00111E`, never touched by our code), then Emma at 18:30:46, same
    address (form `FF00109C` -- the temporary base of a levelled Fox the game had removed two minutes earlier; our
    levelled-NPC code had found it already matching and swapped nothing). `RecentDeletes` cleared our copy deletion
    in both. Emma's dump: `overwrite\Root\crash_UTC_2026-09-30_16-30-47.dmp`.
  - **Same day, 19:47:30, the same failure in our code:** `CharacterService::RunSpawnUpdates` cast what
    `TESForm::GetById` returned for a cached copy, and it was freed memory (vtable `0x3b33e809f967790a`, form type 166).
    Two unrelated callers -- the game's script engine and our spawn update -- both handed a freed temporary form by an
    id lookup, hours apart. What they share is the lookup, and EngineFixesVR's `FormCaching = true` sits on it ("caching
    recently used forms", `mods\Engine Fixes VR\skse\plugins\EngineFixesVR.ini`, with `TreeLODReferenceCaching`
    depending on it). The test: both players set both to false and play as usual; these crashes should stop. Our
    side is guarded regardless (`IsLiveGameObject`).
  - Suspects, unproven: EngineFixesVR form caching (a stale id-to-form cache entry would hand out exactly this
    pointer), PapyrusTweaks (loaded on both machines). Whether co-op makes it more frequent is open: Emma's came 0.44 s
    after spawning a batch of the other player's actors.

- **Crash within a second of travelling away from copies this client made -- open.** `SkyrimVR.exe+0x3AC1A8`
  (`FOCollisionListener`, a virtual call on freed memory), `+0x2EEF6A` (`HasKeyword` on a freed object) or
  `VCRUNTIME140.dll+0x504F` (a cast on the value 1), 0.5 to 0.9 s after arriving through a loading screen. In each the
  physics is handling a contact between something at the destination and a body of a creature skeleton (`NPC COM
  [COM ]` under `skeleton.nif`) whose owner is no longer an actor; in the one dump read, the freed actor's last
  position was where the copies had stood, seventeen cells away. Emma's Crash Logger has `+0x3AC1A8` once before:
  2026-09-26 18:34, in Apocrypha, an arm bone.
  - What the crashing trips share: three or more copies this client had made (`Actor::Create`) left behind, which the
    game destroys during the load -- by the time the client notices, they are gone from the game already (`CopyGone`
    lines). Three Seeker copies next to the game's own Seekers: five crashes in five (`live-copy-abandon` before the
    fix below). Eight bear copies and the bot's character, nothing else: crashed on the first trip that left them
    (`live-spawn-burst`, 2026-10-03 02:25). One or two copies left behind (a Seeker, a bear, the bot's character, in
    either owner's hands): clean in about fifteen trips. So it is the number of copies, not the kind, and not who
    runs them: `live-copies-left-3` crashes in a fresh session, `live-copies-left-2` does not.
  - **PLANCK is part of it.** The same `live-copies-left-3` run with PLANCK switched off passed (2026-10-03 03:00;
    `modlist.txt` backed up as `modlist.txt.bak-20261003-planck` and restored). PLANCK gives creature bones physics
    bodies (`activeragdoll.dll`); what the crash meets is a body of a creature skeleton whose owner is gone.
  - Seen again 2026-10-06 14:33 at a new address, `SkyrimVR.exe+0xCBFD24` (a read at 0x1A8 through a null pointer,
    `NiNode::Destroy`'s neighbourhood on the stack), on the trip of the travel tests, a Loading Menu just queued. The
    other player's sword body (new that day) had been out of the world for six minutes; the same tests in the next run
    passed.
  - Tried from our side, none of it a fix: disposing of copies this client runs when their cell unloads (it never ran:
    after a teleport the game has already destroyed them when the client notices); taking our own creatures back
    instead of copying them (right for its own reasons, see the next entry; it only removed the copies from one
    test); disabling every copy of ours the moment a loading screen is queued (the game then crashed during the
    load, in `BSExtraDataList::GetExtraData` on garbage). PLANCK's own settings only exclude by race.
  - The first explanation, a creature and its copy side by side, was wrong: the identity fix below removed the copies
    from `live-copy-abandon`, and that is all it did for the crash.
- **A creature this game made came back twice -- fixed 2026-10-03.** A temporary actor (placed by console, a quest
  spawn, a random encounter) has no id two games agree on. When its maker walked away the server gave it to the other
  player; when the maker came back the server sent it as "a character with no reference of its own", the maker's
  client made a copy of it next to the game's own actor, and also announced its own actor as a new character. Three
  Seekers placed, six standing. Now the client remembers which server character its own actor became
  (`s_handedAway` in `CharacterService.cpp`), does not announce it again when it is back in view, and takes it back
  as the same actor when the server sends it; the record ends when the server removes the character or the
  connection ends. Not handled: the server's message arriving before the game has the actor loaded again (never
  seen; the log line is `a copy of it stands here too; not handled`).

- **The game grows with every loading screen, in Emma's modlist -- found 2026-10-03, not ours.** Four round trips
  between Mistwatch and Windhelm's docks by console (`cow Tamriel 34 8` and back) took the game from 8.6 to 10.4 GB
  of private memory and from 236 to 287 threads; connected or not made no difference (8.6 to 10.3 GB over three
  trips connected). The threads are DynDOLOD's: `DynDOLOD.DLL` (DynDOLOD DLL NG Alpha-32; MO2 lists Alpha-33) starts
  twelve per load and never ends them -- 102 to 150 over two round trips, while every other module's count stayed
  the same (counted from each thread's entry frame with `cdbX64 -pv`, the non-invasive attach). What the memory is
  was not found. After enough loads the game either freezes or crashes:
  - Frozen at 12:07 (suite, thirteenth script, its first trip away): every thread waiting, the main thread in
    `MemoryManager::Allocate` (VR id 66859, entered through our `HookFormAllocate`) -> the game's low-memory path
    (35201) -> `TES::PurgeBufferedCells` (13159) -> a wait; a worker in the same allocator and the same path, on a
    `BSReadWriteLock` (66977). The game ran out of its own heap and its clean-up deadlocked. 13.4 GB private, 376
    threads, 5.6 GB of commit left on the machine. EngineFixesVR's `MemoryManager` (which replaces that allocator)
    is `false` in the modlist.
  - Crashed at 12:32 (the next run, eighth script, a trip away): in an AMD driver thread (`amdxx64.dll`), a call to
    0x7FFC00000028; 11.5 GB private, 265 threads. First time among 106 crash reports. Again at 15:48, on the first
    trip of a fresh game (`cow Tamriel 33 -9`), so not only the growth: the same frame (`amdxx64.dll+0x1280B0`), a
    call to 0x300000000. The driver files date from 2026-05-29; both crashes were in the rig (no headset, build
    22ae2af). Not seen in play.
  - Also frozen, the same way as far as could be seen: loading Emma's Autosave2 of 2026-10-03 09:32:50 in a fresh
    game, with this build and the one before. Not looked into further.
  The rig restarts the game every six scripts since (`run-live.ps1 -Batch`). In play, a long session with many
  loading screens is where this would show; the modlist is Emma's (see VR_TODO.md, "Needs Emma").
  To name a frame of our own exe in a debugger, use the linker map (`build/.../SkyrimTogetherVR.map`): the exe has
  no debug directory, so cdb finds no pdb. Game frames show as `game_seg+X`, which is `SkyrimVR.exe+(X+0x101000)`.

## 7. Debugging
- **Who may connect to whom is decided by the protocol id, not the version (2026-10-02).** `BUILD_PROTOCOL` is a
  digest of the contents of `Code/encoding` (every message, struct and opcode), computed by `modules/version.lua`
  from git's own hash of each file, so line endings and commit state do not matter; Linux and Windows give the same
  id. The client and the bot send it in `AuthenticationRequest.Protocol`, and the server refuses a different one as
  "wrong version", naming both ids. The version string is still sent and logged (`is on another build ... with the
  same protocol; let in`). `STBot.exe --version` prints both; the server prints `Protocol <id>:` at start.
  A build still has to be whole (`xmake build`, never one target): the server's DLL and its runner check each other.
- **`Tools/VR/check-linux-build.ps1`** builds the server on Debian 12 with GCC through the repo's Dockerfile, runs the
  unit tests there, and compares the protocol id with the Windows build. It starts Docker Desktop if needed and stops
  it again. MSVC lets through things GCC refuses: a local `constexpr` used in a lambda without capturing it, a header
  that uses `std::optional` without including it. Run it before telling anyone to build on Linux.
- **The real game can be run and tested with nobody in the headset (first done 2026-10-01).** Three parts:
  SteamVR's `null` driver with two virtual controllers (the SkyrimVR Devkit's source, built by Emma with
  `build.bat`; Valve's stock null driver has no controllers and `skyrimvrtools.dll` crashes six seconds after a
  load without them), switched on by a `driver_null` block in `steamvr.vrsettings` and removed again afterwards;
  DevBench (`http://127.0.0.1:8921`) to load a save, run console commands, read positions and quit (`qqq`); and a bot
  as the other player. Launch: start SteamVR, then `ModOrganizer.exe "moshortcut://:PLAY Skyrim Together VR"`; the
  client connects by itself five seconds after the load. Valve's original driver is kept beside the built one as
  `driver_null.dll.valve`.
  All of that is `Tools/VR/headless.ps1 up` and `down` now (`down` closes SteamVR by force and removes the setting).
  `Tools/VR/live-check.py <script>` then runs a bot from `Code/bot/scripts/live-*.txt` and, at each `log CHECK ...`
  line, asks the game through DevBench (copy / npc `exists`, `3d`, `alive`, `dead`, `health N`; `player dropped N`;
  `game alive`); a `log DO console <command>` line runs a console command at that point. Two traps: checks are asked
  while the script carries on, so put a `wait` after each group; and never rebuild while a session is up -- the bot
  and the running server must be the same build.
  Since 2026-10-02: `Tools/VR/run-live.ps1` does all of it in one command (up, every `live-*.txt`, one table, down);
  `log SHOT <name> of copy|npc|drop|player` takes a screenshot from a free camera placed in front of that thing
  (`build/.../logs/shots`); `drop exists|mark|moved dx dy dz|gone`, `player alive` and `game inworld` are checks;
  `log DO load last` loads the last save and `log DO god on|off` sets god mode; `headless.ps1 up -NoConnect` runs the
  game with no server. The run switches combat AI off first. Lydia stands under water in Emma's save, so a script
  that teleports to her needs `DO god on` or the player drowns. A bot's copy wears only base-game items: the bot
  cannot carry a mod's armour.
  The bot can play back real movement: `capture other|npc|base <hex> <seconds>` records the stream the server relays
  for one character (positions, animation variables, actions, VR pose) and says what it held, action names included;
  `replay [me|npc] [times]` sends it back as the bot's own character or an NPC it owns; `npc captured` registers a
  creature of the kind just captured (the only way the bot can name a form outside Skyrim.esm). On the game's side,
  `log DO key w 6000` walks the player, `log DO combat on` lets a spawned creature come for them, and
  `CHECK copy|npc gait walking|sliding <s>` reads the copy's own animation speed while it travels.
  Also since 2026-10-02: `log DO server restart` stops and starts the server and waits for the game to go back in by
  itself (about 17 s), and `log DO sleep <s>` makes the driver wait (the bot loses the server too, so its own `wait`
  lines run out meanwhile); the bot command `stay` stops the bot following the player through load doors, so that
  somebody is left behind to be given the player's actors (`ownership accept` keeps them); `CHECK ids reused <name>`
  says whether the game gave an actor of that name a form id of a copy deleted earlier. The driver now tells the bot
  where the player stands (the bot's own search of the client log finds nothing after a busy script and it then
  waits at the world origin), and a script that reaches no check is reported as not run, not as passed.
  `Tools/VR/leaver-check.py` needs no game: a fresh server and three bots, for what a player who leaves without a
  character does to the others.
  Since 2026-10-03 the "away" scripts travel by cell, `cow Tamriel 34 8` and back with `cow Tamriel 34 -9`
  (Mistwatch's cell), not to Lydia: she follows the player back, the next "go to Lydia" is two paces, nothing
  unloads, and the test passes without having tested anything. `CHECK logged <regex>` passes when the client has
  written such a line since the script began, and `CHECK distinct <most> <regex with one group>` when it has logged
  at least one and at most that many different values (`Spawn Actor: (\w+), and NPC Seeker` counts Seekers). The
  client says what a temporary actor was when its removal was noticed (`CopyGone: ... removed as local|remote; the
  actor is ...; made by this client: yes|no`): after a teleport the game has already destroyed the copies this
  client made and its own temporary actors alike, unless they were another player's at that moment.
  Added later on 2026-10-03: `CHECK copy|npc has|equipped|unequipped|lefthand|righthand <hex>` (the game's
  `IsEquipped` does not count the left hand; use `lefthand`), `CHECK npc infaction|notinfaction <hex>`, `CHECK ref
  <hex> alive|dead` for a reference of the game's own, `CHECK absent <regex>`, and the bot's own `expect` lines now
  count in the table ("bot: all N of its own checks"). `log DO papyrus <Script> <Function> <self|copy|npc> [hex ...
  n:<number>]` calls a game function (the VR console refuses `<id>.kill`; `DO papyrus Actor Kill <id>` works),
  `log DO pickup own` picks up the player's newest drop, `log DO server restart after console <command>` runs a command
  while offline. Bot commands: `additem <hex> [count]` (the copy only holds what its owner has), `faction npc <hex>
  [rank]`, `stay`, and `ref:<hex>` names the actor the server sent for one of the other game's placed references. The
  driver now follows the copy actually bound to a server character (`New entity remotely managed`), not the first
  one a spawn made. In Emma's save the Mistwatch bandit `45A63` is already dead; use the bear `860F8` for kills.
  **The rig plays in Emma's own MO2 profile, and the game saves there.** On 2026-10-03 a test autosaved at 16:01
  (it sent the player through a door), that file was the newest save, and Emma's "Continue" that evening loaded it: a
  rollback to a test copy of her morning. The test save `STTest_Mistwatch` (made by console `save`, a name without a
  character id) also gave every save made from it the character id `00000000`, and once one of those was loaded the
  Load menu hid all her real saves (`C232C981`). Since then `headless.ps1` copies the saves folder aside before the
  game starts and puts it back exactly once the game has gone (`saves-before-rig`, with a `.complete` marker; what
  the test game wrote goes to `saves-made-by-rig\<time>`); a run that died before that is put right by the next
  `up` or `down`. The test save was moved out with the rollback saves (`saves\_moved-2026-10-03`); without it the rig
  loads her most recent save. A save made by console must never go into her folder again.
  Before that the rig loaded the most recent save, which after Emma's session of 2026-10-03 was inside Mistwatch's
  tower: every bot waited outside for a player who never came. Her Autosave2 of 09:32:50 hung the game a frame
  after loading, with this build and the one before; not looked into further.
  `CHECK ref <hex> near <x> <y> <z> <units>` reads a reference's position; the bot's `moveobject <hex> <x> <y> <z>
  held|rest` moves a world object as a hand would, and `waitfor objectmoves >= <n>` counts the moves it was sent.
  Also: `CHECK player in <worldspace>` (outdoors there, not in an interior), `CHECK player has <hex>`, papyrus
  arguments `f:<float>` and `b:true|false`, and the bot's `activate <hex> by other|me [door state]` (an activation in
  the name of the game's player or the bot's own; a door is replayed only in the state the sender saw, 3 = closed).
- **Emma's VR Address Library is a combined file (2026-10-01), not a released one.** DevBench needs ids only the
  current library has (0.275.0); the modlist was built on 0.158.0. The released 0.275.0 corrects two addresses
  (100997, 74491) and drops three (63607-63609), and the installed Community Shaders is built for the old value of
  100997 (`LightLimitFix.h:164`): with the corrected address its patch landed inside a live rendering function and
  the game crashed at the main menu (`SkyrimVR.exe+0x1354e5a`). So the file in
  `E:\FUS\mods\VR Address Library for SKSEVR` is 0.275.0 with every id from 0.158.0 kept at its old address
  (script: scratchpad `merge_addrlib.py`; the untouched 0.158.0 is beside it as `version-1-4-15-0.csv.0.158.0.bak`).
  Do not replace it with a plain newer release without updating Community Shaders to match.
  Seen hit it on 2026-10-06 (19:41 and 19:43, twice out of two loads): his crash logger put 100997 at 0x1354D20, a
  plain newer library, which came with his Engine Fixes 7.10; Emma's file has it at 0x90D400.
- **The clean-up on connect deleted the game's own temporary actors (fixed 2026-10-06).** On connect
  the client deleted every actor with an `FF` id, not only its own copies from an earlier connection. Emma's crash at
  19:38:42 on 2026-10-06: FF0011DD "Aspiring Mage" (a Skyrim.esm base, made by the game while her save loaded, 6 s
  before her first connection) was deleted with 5 handles on it, and 43 ms later a game worker thread read its
  process: `SkyrimVR.exe+0x683850`, `GetCurrentlyEquippedWeapon`, the deleted actor in RSI. Now only ids in
  `s_ownCopies` are deleted; a game-made temporary actor is taken like one made after connecting
  (`CharacterService::OnConnected`, "kept on connect"). `live-temp-reconnect` 7 of 7 (20:2x): a creature placed by
  console kept through two server restarts, the game alive and in the world.
- **Engine Fixes VR 7.x (alandtse's unified build, `EngineFixes.dll`): black view, fixed in the launcher
  (2026-10-05).** It is a different mod from the 1.2.6 in FUS (`EngineFixesVR.dll`, `EngineFixesVR.ini`): settings in
  `EngineFixes.toml`, user overrides in `EngineFixesCustom.toml`, and it **must** be preloaded by FUS's `d3dx9_42.dll`
  (Part 2 v1.26, already in "FUS Boot Files"; held back from the preloader, it logs "plugin did not preload, please
  install the preloader" and the game stops at SKSE's plugin loading).
  - **Cause:** two of its guards (`bCullingFreedObjectCrash`, `bSceneGraphDetachFreedCrash`) only accept game code at
    0x7FF0'0000'0000 and above (`EmitLoadedSlotGuard` in its `src/util.h`), where Windows puts the game when it
    starts the exe itself. This launcher maps the game at 0x1'4000'0000, so every game function looked freed: the
    culling guard skipped every object's `OnVisible`, and nothing was drawn (sound and menus working; a tester with
    7.9.0, then the rig: every pixel 0 with it on, the scene back with only it off, twice). The scene-graph guard
    skips child nodes on teardown the same way.
  - **Fix:** the launcher writes both as `false` into `EngineFixesCustom.toml` when it lets `EngineFixes.dll` load
    (`stubs/DllBlocklist.cpp`, `EnsureEngineFixes7Settings`), and the client logs once whether they are off ("Engine
    Fixes 7 is loaded, with the two guards ... off"). Rig, from no override file at all and every other default
    (memory manager on): the file written, the scene drawn, three runs out of three.
  - The same black view would hit SE/AE players of this launcher with Engine Fixes 7.x; the clean fix is upstream
    (the guard should accept the game image's own range). Tested only on VR, a few minutes per run.
  - `headless.ps1 up -Shortcut SKSE` (the game without the mod) reached the main menu by Engine Fixes' log, but the
    script's menu check never saw it; not followed up.

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
