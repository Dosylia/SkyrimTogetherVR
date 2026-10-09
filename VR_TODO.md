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
- [!] Join friends 3 -- no port opening at all, through our own relay (Emma, 2026-10-07, after the free options were
      compared: playit.gg needs its paid plan for a game not on its list; Radmin VPN, ZeroTier and Tailscale make
      every player install a program; only a relay of ours is free or near free and asks nothing of the players).
      **Design: a plain UDP relay, no change to the mod.** A small program of ours (Rust, `relay/` in the hub repo)
      on a rented server with a public address. Host: Host starts the server as today plus a host tunnel in the
      launcher, one outbound UDP flow to the relay (so no port to open), which registers a session; the invite code
      at the hub then names the relay session instead of an address. For each player the relay announces, the host
      tunnel opens one local socket and passes packets to and from the server on 127.0.0.1:<port>, so the server
      sees every player as a separate local connection. Player: Join with a code that names a relay session starts a
      join tunnel in the launcher listening on 127.0.0.1:<port>; the connect file points the game there. The game
      and the server never know. Facts checked (2026-10-07): the client connects with `ConnectByIPAddress` and the
      server listens with `CreateListenSocketIP` (TiltedConnect), so a local address works; the server uses a
      player's address only for its log lines and the player list's endpoint (`GameServer.cpp`
      HandleAuthenticationRequest, `SetEndpoint`), never to decide anything, so relayed players only show as
      127.0.0.1 there. GameNetworkingSockets encrypts every connection, so the relay passes bytes it cannot read.
      Wire: [session 8 bytes][player 2 bytes][the game's packet]; the game keeps its packets under about 1,300 bytes,
      so the header stays under any internet path's limit. Keepalives every 15 s hold the routers' mappings. Not an
      open proxy: it forwards only between a registered host and players holding its code, caps players and
      bandwidth per session, ends sessions the host stops renewing, limits sessions per address, and keeps no
      addresses in its logs (the website's privacy page to say so when it goes live). The tunnels live in the
      launcher, which already stays open while the game runs; closing it during a session asks first.
      **Steps**, each tested before the next: (1) measure a real session's traffic per player (packets and kB per
      second, at the server), which sizes the relay and says whether a free server is enough, half a day; (2) the
      relay and both tunnels on this PC: relay in WSL, Emma's server behind the host tunnel, the rig's bot client
      behind a join tunnel, real game traffic through it, about 4 to 5 days; (3) the hub's code record and the
      launcher's screens (Host says "through the relay", Join needs nothing new), 1 to 2 days; (4) the relay deployed
      on Emma and Seen's existing IONOS VPS, which runs Plesk and their websites (Emma, 2026-10-07): one program as a
      sandboxed systemd service, no Docker, one UDP port opened in the IONOS policy and Plesk's firewall, a `deploy`
      account with Emma's key that may only restart it; Seen's setup guide is the Claude Docs page "urSovngarde relay
      server setup", 1 day; (5) a session with Seen, each on his own connection. About 1.5 to 2 weeks in all.
      The P2P route this item first described (GameNetworkingSockets' ICE, present in the built library, with the hub
      as go-between) stays possible later to save the relay's hop for pairs whose routers allow it; it changes the
      mod's network code on both sides. Questions under "Needs Emma".
      Progress (2026-10-07, the plumbing, steps 2 and 3 in part): the relay is a std-only Rust program in the
      launcher's repo, `relay/` (not the hub's: the launcher shares its wire format, `relay/src/proto.rs`), 9 tests
      (routing between a host and its players only, refusals, lost answers, a host's changed address, leaving,
      silence, the byte allowance). The launcher's two tunnels, `src-tauri/src/relay/` (host: one local socket per
      player towards 127.0.0.1:<server port>; friend: a local port the connect file names), 3 tests through a real
      relay on this PC with a stand-in server and games (both ways, 1,300 bytes, two players seen as two, the
      session's end heard). The hub: `GET /relay` (its `RELAY` setting, empty today) and a relay session on codes,
      tested locally with `wrangler dev`, not deployed. Host and Join use it when the hub names a relay, directly
      otherwise. End to end with the relay program, the local hub, the launcher's code path and stand-ins: a packet
      through and back (`examples/relay-check.rs`). Left: the screens, the hub deployed, a Linux build of the relay
      for the VPS, then the measurement and a real game through it.
      Later the same day: Seen's VPS is ready (85.215.172.211, `relay.ursovngarde.com` points at it, unlimited
      traffic, UDP checked from both homes). The Linux build (static, 437 KB, built and tested in `rust:1-alpine`) is
      in `/opt/ursovngarde/relay`, copied through `deploy` with Emma's key (same checksum). Waiting on Seen, in his
      guide's step 8: stop the echo test that still holds udp/10610, add the sudoers rule (`deploy` is asked for a
      password), install `relay/ursovngarde-relay.service`. Then a test from Emma's PC through the real relay with
      the hub run locally, before the live hub's `RELAY` is set.
      Done: the service runs on the VPS (active, udp/10610). Through it over the internet, from Emma's PC, with the
      hub run locally naming `relay.ursovngarde.com:10610` and the launcher's own code path (`relay-check`): code
      registered with its session, joined, a packet to a stand-in server and back; 50 round trips all back, median
      50.0 ms (49.4 to 51.5). Host and friend were both on Emma's PC, so that is two trips to the VPS: about 25 ms
      each way from Emma's line, and a real friend adds their own trip to hers. Still open: ICMP is dropped by the
      server's firewall (ping unanswered, no effect on the relay), and the real game through it before the live
      hub's `RELAY` is set.
      Hardened (2026-10-08, from the launcher's review): a relay restart or a host's changed address no longer ends
      a game. The relay rebuilds a lost session for the host holding its token, the host tunnel takes its place back
      after 45 s of silence, a friend's tunnel joins again (test: a game goes on across a relay restart, 5 runs). A
      friend's launcher closed and opened again rebuilds the tunnel on the port the connect file names (test). A
      failed join through the relay is said (host gone, game full, relay unreachable) instead of leaving a connect
      file that points at nothing. This relay build runs on the VPS since 2026-10-08 19:40 (same checksum as built
      here; the previous one kept beside it as `ursovngarde-relay.previous`). Seen added the sudoers rule: `deploy`
      may run `systemctl restart ursovngarde-relay` and `systemctl status ursovngarde-relay` (exactly those, no
      options), so relay updates no longer need him.
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
- [!] **Join friends 3, our relay** (decided 2026-10-07: our own relay; plan in the urSovngarde section above).
      Where it runs is settled: the existing IONOS VPS with Plesk (Emma, 2026-10-07), set up by Seen from the guide;
      its traffic allowance against the measured traffic (step 1) is the one thing to watch. Two choices left:
      (1) Relay always (one path to test and to explain), or direct when the host's port is open and
      the relay otherwise (saves the extra hop for those hosts, two paths to test). (2) When a session with Seen
      could be, each on his own connection: needed once to measure (it can also be measured with the rig) and once
      at the end.
- [x] **Only a weapon or shield blocks** (Emma, 2026-10-09: "only a sword or shield should block"). A touch of his
      weapon counts when that hand holds a weapon (the game's hand object is a WEAP), the left hand a worn shield
      (read at most once a second: it walks the inventory), or something held with HIGGS's grab; a bare, spell or torch
      hand gets nothing: no buzz, sound, sparks, no ClashRequest, no defender's rule (`VRBodySync::HandParries`,
      `CharacterService::SendClashes`, said every 5 s as "no parry"). Correction to the note of that evening: Emma's
      right hand held Dawnbreaker, not a spell (`FEE34` is `DA09EncDawnbreaker`, read from Skyrim.esm); her blocks
      were real. Built, deployed (v1.9.0-dirty.5b57258); to see in a fight with a bare or spell hand.
- [x] **Hitting his sword still hurt him** (Emma, 2026-10-09). Dawnbreaker's strike enchantment (touch delivery,
      checked in Skyrim.esm) was sent as a spell cast at every strike, the ones dropped on his blade included, and
      his game replayed it from her copy at him: 13 local hits of 16 to 74 between 18:34:43 and 18:36:27, each
      0.2 to 0.8 s after a cast, none weighed by the defender's rule. Now a touch enchantment aimed at the other
      player's copy is not sent (`MagicService::OnSpellCastEvent`), and one aimed at this player is not replayed
      (`OnNotifySpellCast`, for a game on an older build). The strike itself still travels as the hit; the
      enchantment's own damage on a landed strike between players is not sent for now (her game never reported it
      as a spell hit). Also found: in his game her copy held no weapon body from 18:34:16 to 18:36 ("another weapon or
      none in the hand", then "the weapon is not out"), so his game saw none of her blade meeting his and his rule
      blocked nothing; her arrows (Orcish, Steel) were equipped and unequipped on the copy 35 times in two minutes.
      Not fixed: needs a look at why the copy's weapon was taken out of the world while she fought with it.
- [!] **How your equipped sword meets his.** A sword grabbed with HIGGS is stopped by his; an equipped one passes
      through (two keyframed bodies). (1) B&S-like: the equipped sword becomes a dynamic body held to the hand, what
      you see follows it, his blade stops yours -- the most work; (2) feedback only: buzz, sound, sparks and the
      defender's rule, no physical stop -- what exists now; (3) fight with grabbed swords. [Physics queue]
      Emma, 2026-10-08, after a fight with Seen: "if I meet my enemy sword I shouldn't be able to still go past it, or
      it defeats the purpose" -- that is (1). To confirm before it starts (the most work), and after the damage path
      below is right: a stop that lets blades through still has to drop the hit.
- [x] **7.9 GB copied at every start** (Emma's OK, 2026-10-08). MO2's `overwrite\Root` held 77 crash dumps (7.5 GB,
      18 Sep to 6 Oct, written by the game into its folder and captured by RootBuilder), a 388 MB unchanged copy of
      the v1.9.0 `Skyrim Together VR` folder and `urSovngarde Server` (both from the launcher's no-MO2 test install
      into the game folder); RootBuilder copied all of it into the game folder before each start (41 s of a 66 s start
      that evening). Done: the dumps moved to `E:\urSovngarde crash dumps (moved out of FUS 2026-10-08)`, the copy
      deleted, the server moved to `E:\FUS\tools\urSovngarde Server` (where an MO2 install puts it) with the
      launcher's `serverDir` pointing there; `overwrite\Root` keeps 1.6 MB of small files. Hosting from the new place
      is allowed by the existing "Skyrim Together Server (UDP 10578)" firewall rule. Still to see: the next start's
      RootBuilder time. Worth a launcher check later: dumps piling up in a RootBuilder modlist's overwrite.
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

- [ ] **Everything renamed urSovngarde** (Emma, 2026-10-09: "rename everything"). `SkyrimTogetherVR.exe` is
      `urSovngarde.exe` (`set_basename`, `Code/immersive_launcher/xmake.lua`), `SkyrimTogetherServer.exe` is
      `urSovngardeServer.exe` (Linux too: Dockerfile, linux.yml, release.yml), the release folders `urSovngarde` and
      `urSovngarde mod`, the SteamVR dashboard tab and the HUD lines say urSovngarde, PRODUCT_NAME too. Kept on
      purpose: `SkyrimTogether.esp` (saves name their plugins), `%LOCALAPPDATA%\SkyrimTogetherVR\connect.txt`, the log
      line "Skyrim Together client, build" (the launcher, collect-logs and the crash tools read it), the window class
      and internal code names (upstream merges). Moving over: `deploy-client.ps1` re-points the MO2 executable (MO2
      closed; open, it leaves a copy under the old name) and sets the old exe aside; `update.ps1` writes the new names
      and refreshes an old-named file only where one is (tested on an old and a new install); the release's update
      zip carries the build under both names for an older update.bat; the launcher (`install/mod.rs` Layout) reads both
      release layouts and both installs, re-points an MO2 entry in place and removes the old program. Found on the way:
      the game runs inside the mod's program, so no SkyrimVR.exe process exists while the mod plays, and the launcher
      never saw a game running; it now looks for both program names (`GAME_PROCESSES`). Built and deployed to Emma's
      setup (v1.9.0-1-g651691e9-dirty.bdb57ff): her MO2 entry runs urSovngarde.exe (ini copied aside), the old exe kept
      as `.old-20261009-210250`, the server in `E:\FUS\tools\urSovngarde Server` renamed and started once (5 s, fine).
      Launcher: 94 tests (new: a release from before, an install from before moved to the new names, the MO2 entry
      re-pointed once and not twice). Left: a launcher release before the mod release (launcher 0.3.2 finds the mod
      only by the old name), a session with the new names, the website's install page at the mod release (brief).
- [ ] **PvP damage that skips the defender's rule** (session of 2026-10-08, Emma hosting, both on v1.9.0). Sparks
      and the attacker's side of the rule work: 51 of Emma's swings that met Seen's blade were not sent, and Seen's
      game blocked 10 of the 42 that were. But none of Seen's 28 sword hits and 2 spell hits on Emma reached her
      rule (no "Defender's rule" line in her log), while she went down twice (20:30:27, 20:32:46). Her damage came from
      her own game instead: Seen's game logged her health falling every frame (every 28 ms) at 20:32:43, which is a
      concentration spell, Seen's Sparks (2DD2A, cast on her copy of him at 20:32:33, 35 and 48) hitting her inside
      her world, and a single 78-point loss at 20:30:26 where his game had counted 22. So hits a player's copy makes
      in the other game (its sword body, its spells) are applied there unweighed, and the sound is a body hit's.
      Her character was server id 0 (she started the server and joined first), which every path handles in the
      code as read. Diagnostic lines built and deployed (client and the hosted server, v1.9.0-dirty.383a24f): where
      a hit sent to this player goes when it is not held, this game's own hits from a player copy (summed per
      second), every spell interrupt sent and received, and on the server each player-on-player hit with who it went
      to. Next session (both on this build) says which path to close; no fix before that.
      Answered by the fight of 2026-10-09 18:34 (Seen hosting on port 9600, both clients on the diagnostic build,
      his server plain v1.9.0): with Emma's character at server id 1, all of Seen's hits on her reached her rule
      (11: 7 blocked, 4 landed), none of his copy's hits landed in her game, and nothing was counted twice. On his
      side, her 15 sent hits all landed (his game saw no blade meeting: her right hand held a spell) after her game
      had held back 26 on his blade, and his game applied 13 hits of her right-hand spell FEE34 itself (16 to 74
      each, 0.2 to 0.8 s after each cast); spells only do damage in the target's game, which is the one path they
      have, not a double. The "received; it is actor ... here" lines were each player's own damage reports to the
      other (the server names the sender as attacker), harmless. So the one difference with 2026-10-08 is id 0:
      which entity gets it is chance (here an NPC her game reported a moment before her character). Fix: the server
      keeps id 0 for an empty entity for its whole life (`Code/server/World.cpp`), built and deployed to Emma's tools
      folder and `E:\FUS\tools\urSovngarde Server` (v1.9.0-dirty.c9063a5, starts and runs). Whoever hosts needs this
      server. To see: a fight with Emma hosting. The exact line that mishandles 0 is still unknown.
- [ ] **Sparks that never stops** (same session): Seen's two-handed Sparks cast at 20:32:48 went on in Emma's game
      after he stopped and after her respawn, still hitting her; she could not equip a weapon for a while. The
      interrupt lines above say whether his stop was sent and whether her game applied it.
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
- [ ] **Antivirus false positives** (Nexus quarantined the first upload, 2026-10-09; VirusTotal: guessing engines
      such as Rising, Trapmine, MaxSecure). (1) The programs' identity: until now upstream's ("Together Team",
      "TogetherOnline", `launcher.exe`, 0.0.0.0), now urSovngarde's, with the version as four numbers from the root
      `xmake.lua` (v1.9.0-2 is 1.9.0.2) in the four `.rc` files. Built and deployed (v1.9.0-3-g22c24c5b): Explorer's
      Details and .NET read urSovngarde, 1.9.0.3 and the right file names for all four (.NET read nothing before;
      TPProcess and the two server files were copied by hand, old ones set aside as `.old-*`). Nothing reads our
      exe's version: the game's comes from
      SkyrimVR.exe, and plugins asking for the running program's path get SkyrimVR.exe once the game is loaded.
      (2) `Tools\VR\scan-release.ps1`: scans a release on VirusTotal before publishing (the three zips, the four
      programs and the eight helper scripts one by one) and writes the checksums, the links and where to report each
      flag. Tested against a stand-in VirusTotal (lookup, small and over-32 MB uploads, waiting, a flag, the report,
      the release layout); needs Emma's free key in `%USERPROFILE%\.str-vt-key` for real use. If a helper `.bat` is
      flagged (they start PowerShell with "-ExecutionPolicy Bypass"): move the helpers to a separate "manual tools"
      zip, the launcher covering connect, host and update (Emma's call: is the manual route still needed?).
      (3) Signing, Emma's decision: SignPath Foundation (free for open source; needs the release built by GitHub
      Actions, see "How releases are made", and the launcher's repo public) or a paid certificate in her name.

### Tooling

- [ ] The bot registers only one of two NPCs asked for in the same instant (`live-copies-left-2`).
- [ ] An equip of a weapon the copy already holds may leave its hand empty (`live-equip`); the creature-gait check
      is noisy; the other order of "a creature this game made comes back".
- [ ] TiltedEvolutionVR's two Havok crash guards (`29f99ed`) and ownership blacklists (`1660eb0`): read, not taken.
