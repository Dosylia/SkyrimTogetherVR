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

## Needs Emma

### Decisions

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
- [!] **Who runs a follower when you two are apart** (Lydia, 2026-10-03: the two games pulled her back and forth 92
      times over a loading screen). (1) a player's follower always belongs to that player's game -- recommended; (2) a
      loading screen does not count as leaving.
- [!] **Dragons taken over in flight** (the Mistwatch dragon fell and died, 2026-10-03). (1) never take over a dragon
      that is flying in the other game -- recommended; (2) keep the claim.
- [!] **PLANCK and the crash on travelling away** (`+0x3AC1A8`, `+0xCBFD24`; only with PLANCK on, when three or more
      of our copies are left behind). (1) live with it; (2) PLANCK off for co-op; (3) let me keep at it (next: how PLANCK
      tracks the actors it ragdolls).
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

- [ ] Crash on travelling away with PLANCK and our copies left behind -- decision above. [Crashes and stability]
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

- [ ] Followers and dragons in flight -- decisions above.
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
