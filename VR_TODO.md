# Skyrim Together VR: TODO

What is current, and nothing else. The full story -- session reports, investigations, evidence for every fix, the
Physics and PvP queues as they were worked -- is in `VR_HISTORY.md` (everything this file held up to 2026-10-07, word
for word); `KNOWN_ISSUES.md` says what is known to be broken and why. Section names in brackets point into the history.

`[ ]` open, `[!]` waiting on Emma, `[x]` built and tested in the rig but not yet seen in a real session. An item the
players confirm is deleted, not ticked; git keeps it.

**Aims, in order** (2026-09-25): no crashes; the game in sync for both players; VRIK and HIGGS interactions seen by
everyone; dead bodies in sync; NPCs never below the floor; weapon hits land where the weapon is. Since 2026-10-06:
sword fights that feel like Blade & Sorcery -- the other player's sword and body are physics objects in your game, and
the defender's screen decides whether a hit was blocked.

## urSovngarde: launcher, reports and crash triage (2026-10-07)

The project around the mod is called **urSovngarde**. Its launcher and its online side (reports, invite codes, the
crash triage) live in their own private repos, `C:\dev\urSovngarde-launcher` and `C:\dev\urSovngarde-hub`, each with
its `TODO.md`. What stays here is the mod's own part:

- [x] Small crash dumps (2026-10-07, deployed as v1.8.1-121-gc9f9b92f-dirty.6df6e9f): on a crash the handler first
      writes `logs\crash_UTC_<time>.small.dmp` in every build (stacks, registers, modules, the objects the crashing
      registers point at; not the game's data segments), keeps the newest three, then the full dump as before. Above
      3.5 MB it is written again with every thread's registers but only the crashing thread's stack (clearing a
      thread's stack flag does nothing, removing its range works). `Code/client/SmallDump.h`; TPTests
      `[smalldump]`: a real minidump of the test process, 44 KB; with eight busy threads 616 KB, the second pass
      75 KB; pruning keeps the newest three and nothing else; all 41 test cases pass. The launcher's report takes
      the newest two under 4 MB from `logs\`, masked in place. Not seen yet: one written by a real game crash (the
      next crash's log says "small crash dump ... (N KB)"). Was: the game's crash handler writes a few-MB minidump
      (call chain, registers, memory near the crash -- never a full 1 GB dump), for the launcher to send with the logs
      when the player agrees.
- [!] Join friends 3 -- no port opening at all: direct connections through both routers with GameNetworkingSockets'
      P2P (TiltedConnect is built on it, v1.4.1), the hub as the go-between. Changes the mod's network code on both
      sides; its real test needs two PCs on two internet connections (Emma and Seen). Facts (2026-10-07): the
      xmake package builds GameNetworkingSockets with its own ICE (`ice` config on by default, not overridden; ICE
      code present in the built `gamenetworkingsockets.lib`), so P2P needs no new library. What it needs: the server
      listening for P2P besides its UDP port, the client connecting with custom signaling, the hub passing the
      signaling messages between the two (a handful per connection; Workers KV's free plan allows 1,000 writes a
      day), public STUN servers (free) to learn each side's address, and for the pairs whose routers both refuse
      (often quoted as one in five or fewer) a TURN relay, which is a paid service, or no relay and those players
      use Tailscale. Question under "Needs Emma".
- [!] Release packaging for VR. Done 2026-10-07: `make-release.ps1` no longer takes files local to Emma's PC (a
      `.map`, a stray `.zip`, found by the launcher's install test in the 2026-10-05 zip), and `-ListOnly` shows the
      client folder's selection on any tree (29 entries from her tools folder, none of those). Open: `release.yml`
      publishes a GitHub release on any `vX.Y.Z` tag push, built by `windows-playable-build.yml` in upstream's SE
      layout (a `SkyrimTogetherReborn` folder): a tag pushed today would publish a zip the launcher cannot install,
      and its update notice would announce it. Question under "Needs Emma". Was: release packaging for VR (the
      workflows still name upstream's SE files).

## Work queue (2026-10-07)

Emma's answers of 2026-10-07: a player's follower always belongs to that player's game; a dragon flying in the other
game is never taken over; keep at the crash on travelling away with PLANCK (option 3). Worked top to bottom by the loop:
`[x]` with its evidence in one line, `[!]` with the question or action under "Needs Emma", `[-]` with why. Details of
each item's past are in `VR_HISTORY.md` under the section named in brackets.

- [x] **A corpse lies where its owner's lies** (2026-10-07, `live-corpse-place` 06:35 and 06:38, 5 of 5 each): the
      dead body's ragdoll -- what is drawn -- is moved, every rigid body by one offset under the world lock, as PLANCK
      warps a ragdoll (`VRBodySync::PlaceCorpse`, from InterpolationSystem). "ragdoll lay 1068 units from where its
      owner's corpse is; moved there (19 bodies)", a second later "now lies 14 units" (1147 then 23 in the second run);
      the camera at the owner's position shows the bear (empty ground on 2026-10-04). Was: **A corpse lies where its owner's lies.** Nothing moves a dead body's ragdoll on the receiving side: `ForcePosition`,
      `MoveTo`, papyrus `SetPosition` and the console move the reference, not the ragdoll the body is drawn from, so the
      two screens show it in different places ("CorpseDiag"), and a dragged body falls back to its old spot for the
      watcher when the dragging stops. Find how a ragdoll is moved (PLANCK's source moves them: its scratchpad clone),
      then `live-corpse-place` red to green. ["Dead bodies lie in different places", "Goal 4"]
- [x] **A player's follower belongs to that player's game** (2026-10-07, bot pair `follower`, green twice 07:06): a
      claim now says when the actor is the claimant's own follower (`RequestOwnershipClaim::Follower`, set by the client
      from `IsPlayerTeammate`); the server grants it to a party member who does not lead (refused before: "the player
      is not the party leader", 06:59) and remembers whose follower it is, refusing every other player's leader claim
      and leader assignment while that player is connected ("Rejected party leader claim from player 1 for actor 2:
      it is another player's follower"). Owed: `live-follower` in the rig (the client side), with the next session.
      Was: **A player's follower belongs to that player's game**, whoever else is near or whatever loading screen is
      between (Lydia, 2026-10-03 15:09: 92 hand-overs in 15 s). Server ownership rule; a rig test with a follower.
      ["Who runs a follower", "Shared follower"]
- [ ] **A dragon flying in the other game is never taken over** (the party leader's claim skips it until it lands or
      dies; the Mistwatch dragon fell dead, 2026-10-03 09:06). ["Dragons taken over in flight"]
- [ ] **The crash on travelling away with PLANCK and our copies left behind** (`+0x3AC1A8`, `+0xCBFD24`;
      `live-copies-left-2`). How PLANCK keeps track of the actors it ragdolls, then our copies leave in a way it notices.
      [KNOWN_ISSUES, "Crash within a second of travelling away"]
- [x] **The weapon's visible blade** (2026-10-07, built with the corpse fix): `kWorldBoundOffset` is 0xE4; nothing
      reads it on a live path any more (`ReadWorldBound` has no caller), only the shape report's line. Was: `kWorldBoundOffset` 0xB0 is `previousWorld`; the world bound is at 0xE4 on VR
      (measured 2026-10-06). Batch into another item's build.
- [ ] **The arrow nocked on the other player's bow**: cause known, not built. ["Nocked arrow not shown on the other
      player's bow"] Batch into another item's build.

## Needs Emma

### Decisions

- [!] **How releases are made.** (1) GitHub builds them when you push a tag: `release.yml` rewritten to make
      `make-release.ps1`'s layout (`Skyrim Together VR`, `Server`, `Skyrim Together mod`) from the build and the UI;
      (2) `make-release.ps1` on your PC, uploaded by hand, and `release.yml` switched off until then. Until one is
      chosen, do not push a `vX.Y.Z` tag: it would publish upstream's SE layout.
- [!] **Join friends 3, a relay or not.** Direct connections without opening a port (urSovngarde section above) work
      for most pairs of routers; for the rest a TURN relay passes the traffic, which costs money each month. (1) No
      relay: those pairs are told to use Tailscale, as today. (2) A relay later, once players hit it. Either way the
      work ends with a test session with Seen, each on his own connection; when could that be?
- [!] **How your equipped sword meets his.** A sword grabbed with HIGGS is stopped by his; an equipped one passes
      through (two keyframed bodies). (1) B&S-like: the equipped sword becomes a dynamic body held to the hand, what
      you see follows it, his blade stops yours -- the most work; (2) feedback only: buzz, sound, sparks and the
      defender's rule, no physical stop -- what exists now; (3) fight with grabbed swords. [Physics queue]
- [!] **A stiffer PLANCK?** His ragdoll follows his pose 8-14 units loose because PLANCK's ragdolls follow softly
      (your `activeragdoll.ini` is at the defaults: `positionGain = 0.05`, `hierarchyGain = 0.6`, `poweredTau = 0.8`).
      Stiffer means his body (what your sword hits) is tighter, and every NPC reacts more stiffly to hits. May I try
      e.g. `positionGain = 0.2` in the rig?
- [!] **Handling a living NPC the other game runs.** Today your grab or shove is undone at once and he sees nothing;
      only damage gets through. (1) your game takes it over while your hand holds it (seen by both, AI switches games
      mid-fight); (2) your pull is sent to his game, which applies it (smoother AI, a delay, much new work); (3) leave it.
- [!] **Grabbing or pushing the other player.** Nothing reaches him today; his real hands cannot be moved. A buzz on
      the grabbed hand, a stagger when shoved, or nothing?
- [!] **Long sessions grow** (~0.4 GB and 12 threads per loading screen, DynDOLOD DLL NG's threads; it ends in a
      freeze or a driver crash). Try DynDOLOD DLL NG Alpha-33, and/or `MemoryManager = true` in EngineFixesVR.ini --
      both machines the same. Which?
- [!] **A message when the other player goes down** ("X is down"), and maybe revive by activating them? (asked
      2026-10-01)
- [!] **Playback delay:** after your next session, the "InterpDiag players" lines say how much of the 225 ms delay
      the network really needs; it could then follow the connection (lower on a good link). Yes or no, once measured.

### Actions

- [!] **Update DevBench** in MO2 (Nexus 181326): its newer releases drive the VR controllers by script, so the rig
      can swing a sword and test PLANCK hits, the sword-hit fix, blocking and swing lag without you. Test tool only.
- [!] **Next session, one drag each way:** Seen drags a body your game owns for a few seconds, then you drag one of
      his. Both logs say whether it changed hands ("asking for it", "Transferred ownership ... for party leader
      claim") and whether its bones arrived ("posing body").
- [!] **Next fight:** does hitting his sword still hurt him; do blocks on your screen hold; does damage feel late
      (another player's hits wait about a third of a second to be judged)? Logs: "PvP: ... not sent", "Defender's rule".
- [!] **Two minutes in the headset:** connected, let yourself be killed, then load a save while down or within ten
      seconds of getting up. Does it crash? With nobody in the headset it does every time (`skyrimvrtools.dll`).
- [!] **Seen's mods match yours:** his newer VR Address Library crashed him on loading (Community Shaders patches the
      wrong spot); your combined `version-1-4-15-0.csv` fixes it (KNOWN_ISSUES section 1).

## Built, waiting for a real session

- [x] A hit on his sword is not a hit on him: PLANCK's hit point (+0x6BC) on a spot his weapon body was just touched
      is not sent (22 of Emma's 56 hits in the first fight were on his sword).
- [x] Clashes: buzz on the hand that met it (both players), the game's blade-block sound and sparks at the point; a
      clash is a weapon starting to be touched at 10 units a second or more; slower contact still blocks.
- [x] Defender's rule: another player's hit waits until your screen shows it (tick + 225 ms + 100) and is dropped if
      your weapon or hand met his weapon from 300 ms before it to 100 ms after it was seen. Rig: edges exact.
- [x] Dragging: a body you drag that the other game owns is asked for (watched every 100 ms, as the owner does; was
      every frame, too fast for a hand) and now granted for temporary bodies too; the receiver poses a dragged body
      where its owner has it, not where it lay before.
- [x] The clean-up on connect keeps the game's own temporary actors (deleting one crashed Emma's game, 2026-10-06).
- [x] "InterpDiag players": the other player's delay alone, every 10 s.
- [x] Dying while connected no longer ends at the main menu: level-up, which went the same way, is confirmed fixed
      (2026-09-30); confirm on the next death while connected.

## Open work

### Crashes and stability

- [ ] Crash on travelling away with PLANCK and our copies left behind -- in the work queue.
- [ ] Load after death crashes in the rig (`skyrimvrtools.dll+0x71B5`, `PlayerCharacter::UpdateAnimation`) -- the
      headset test above says whether it is real.
- [ ] Script-engine crash on freed temporary forms (`+0x93CE17`, the game's): Emma's EngineFixesVR has
      `FormCaching = false` since 2026-09-30; whether it helps is not known yet.
- [ ] Emma's freeze one frame after loading into the Ragged Flagon (2026-10-03 21:06:48), no report; the AI thread on
      an actor with no cell (19:52); Elbios on opening the wrist menu; Seen on the load after death. Debug plans in
      the history ["2. Crashes with no cause yet"].
- [ ] Quit crash in Enchantment Art Extender (Seen, 2026-10-04 16:26:56).
- [ ] Audit every overlay call made before the VR menu exists (a hard crash).

### Sync: NPCs, followers, the world

- [ ] Followers and dragons in flight -- in the work queue.
- [ ] Maven fought by Seen was invisible for Emma; the blacksmith dead on Seen's side only; Brynjolf (essential)
      spinning on the floor for Seen after being "killed" (2026-10-04).
- [ ] Lydia did not fight back while Emma was down; enemies ignored Seen while he was invisible to her (2026-10-03).
- [ ] The `Unequip` loop: one actor sends the same unequip again and again (the Bandit Leader "ignoring us", 92 a
      second); the queue is capped, the cause is not found.
- [ ] A skill book Emma touched could not be read by Seen.
- [ ] Quest NPCs out of sync (Bastianus Axius); quest dialogue heard twice; message boxes shown to both players.
- [ ] Weather and time on VR: only a party member gets the leader's weather.
- [ ] Mounts: untested.
- [ ] A downed follower lying sideways and running.
- [ ] Sliding: creatures with no animation-variable descriptor slide whatever else is fixed; Dinya Balu sliding on
      Emma's screen only. ["Sliding: the descriptor lookup"]
- [ ] Different animals on each screen: the owner's base form travels now; not confirmed.
- [ ] NPCs below the floor for one player: never measured where `ForcePosition` puts them. ["Goal 5"]
- [ ] A distant dragon vanishing; the range check for a remote dragon with no copy here.
- [ ] Vanishing at the Whiterun gate: logging in place, untested.
- [ ] The paused player goes silent; grey distant hills (LOD).
- [ ] Ownership: "actor for ownership transfer not found" warnings; a newer ownership number accepted by the six
      receiving handlers instead of exact equality; removals for a viewer who walked away (the asymmetric range path).
- [ ] An NPC's health after a revive is not re-sent; a respawn does not restore stored health (`OnRequestRespawn`);
      `NotifyRespawn` is sent from inside the inventory lookup (`PlayerService`).
- [ ] Connecting right after loading a save sometimes times out (the attempt, not a running connection); the spawn
      burst on joining (30-40 actors in one millisecond) has never been timed.
- [ ] Upstream's per-dungeon respawn positions (needs cell editor ids, which VR may not keep).

### Dead bodies

- [ ] Nothing we do moves a corpse's ragdoll, so bodies lie in different places on the two screens ("CorpseDiag");
      `ForcePosition`, `MoveTo` and the console all move the reference, not the ragdoll. Next: what moves a ragdoll.
- [ ] The bot sending a humanoid body's bones, to test the receiving side in the rig.

### PvP and physics

- [ ] Less delay for hands and weapons: from the next real session's "InterpDiag players".
- [ ] The weapon's visible blade: `kWorldBoundOffset` is 0xB0 (`previousWorld`); the world bound is at 0xE4 on VR.
- [ ] The other player's hands sit slightly off (palms together, a little low): structural, `VRPose`.
- [ ] Your own sword hitting where the blade is not (2026-09-24 screenshots). ["4. Position accuracy"]
- [ ] Nocked arrow on the other player's bow: cause known, not built. ["VRIK and HIGGS interactions"]
- [ ] Objects already in the world moved by hand (a cup on a table) are not seen moving; dropped items are.
- [ ] A floating item whose carrier disconnects mid-carry.
- [ ] Item and body drift fixes from TiltedEvolutionVR (`aaf5d83`), if drift shows up.

### The other player's body

- [ ] Legs when moving: a walk or run on the copy with smooth locomotion.
- [ ] Face "looks off" (FaceGen on VR).
- [ ] The invisible player (worse indoors): look at render state, not actor state. ["0a. The three open complaints"]
- [ ] The friend's health bar: our message is right, the widget refuses it; the enemy health bar on the other player.
- [ ] Copies spawned on the ground when the incoming height is bad; fade in instead of popping.

### Performance

- [ ] Spawn bursts on cell change; `RunNakedNPCBugChecks` and the equipment snapshot once a second; linear searches
      in hot paths. Measure first (a 60 s benchmark at one spot, four setups).
- [ ] Both games slowed over a session (threads and memory) -- the DynDOLOD decision above.

### Shareable

- [ ] An in-headset menu (the Skyrim Together UI as a SteamVR dashboard tab, large text, a small overlay for chat).
- [ ] Start-up checks with plain messages in the headset: address library, fewer than 255 plugins, the plugin
      header, and the address library disagreeing with our table.
- [ ] Install script, a guide for other modlists, a licence check before sharing builds (GPL-3), VR server
      defaults (difficulty, PvP, time scale), a test checklist per build.

### Tooling

- [ ] The bot registers only one of two NPCs asked for in the same instant (`live-copies-left-2`).
- [ ] An equip of a weapon the copy already holds may leave its hand empty (`live-equip`); the creature-gait check
      is noisy; the other order of "a creature this game made comes back".
- [ ] TiltedEvolutionVR's two Havok crash guards (`29f99ed`) and ownership blacklists (`1660eb0`): read, not taken.
