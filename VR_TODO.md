# Skyrim Together VR: TODO

What is current, and nothing else. The full story -- session reports, investigations, evidence for every fix, the
Physics and PvP queues as they were worked -- is in `VR_HISTORY.md` (everything this file held up to 2026-10-07, word
for word); `KNOWN_ISSUES.md` says what is known to be broken and why. Section names in brackets point into the history.

`[ ]` open, `[!]` waiting on a test or action, `[x]` built and tested in the rig but not yet seen in a real session. An item the
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
      mod's network code on both sides. Questions under "Waiting on a test or action".
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
      and its update notice would announce it. Question under "Waiting on a test or action". Was: release packaging for VR (the
      workflows still name upstream's SE files).

## Now (Emma, 2026-10-10 morning): the blade and body upgrade, all of it

"Need to work on the blade and body upgrade" and "Need all of them": the loop works these first and does not stop
to ask which to start. In this order, each in steps, each step tested in the rig (DevBench 1.27 drives the VR
controllers: `input vrTrackedSet`, one frame = HMD and both controllers, a 3x4 `matrix` pose each, buttons and axes):
1. The blade stop (Decisions, "How your equipped sword meets his: (1)").
2. Handling a living NPC the other game runs: send the pull (Decisions).
3. Grabbing the other player: a buzz on the grabbed hand (Decisions).
4. Party members on the map and compass (Later).
5. Mounts: the rider's copy put on the horse directly (Sync).
6. The skill book that cannot be taken after the other player touched it (Sync).

Next big work (Emma, 2026-10-10 afternoon, for the public server: "Need plan for this and map pointers of players
completed, this is next big work"): party members on the map (item 4, plan completed under Later) and proximity
voice chat, our own first ("I would rather a test run with our own system and if it fails, then fall back to
mumble"; plan under Later, "Proximity voice chat").

- [ ] **The next build, after Emma's session of 2026-10-10** (handed over by the public-page session, which builds
      nothing itself). On disk, unbuilt: `AuthenticationRequest.HideFromPublicPage` and the new `PlayerPlaceRequest`
      (protocol change), the client's place updates (PlayerService, TransportService) and the server's
      `PublicStatusService` (off by default), and two new crash lines in `Code/client/CrashHandler.cpp` for the
      launcher's reports ("VectoredExceptionHandler: in <module>+0x<offset>, version x" and "... stack ...", from the
      new helpers above `LogCrashContext`; crash path only). Also, for Seen's public server: `Code/tests/xmake.lua` keeps
      `smalldump.cpp` and dbghelp to Windows (the Linux CI has failed at Build on every run since c5d3236e, which added
      them; with the change, a Debian 12 Docker build of the tree on 2026-10-10 built every Linux target, TPTests passed
      all 40 cases and the server started), and `ServerListService::Announce` no longer contacts upstream's list when
      `bAnnounceServer` is off (a 403 from it stops the server). Build all five together (client, launcher, server, runner, bot); a
      compile error in those files goes to that session (tiltedevolution-09). Deploy, then tell Emma: her friends need
      the new client from the tools folder before joining a server from this build. The blade stop is in the same tree,
      unproven: prove it in the rig first, or keep it from turning anything, before this build reaches the tools folder.

## Work queue (2026-10-07)

Emma's answers of 2026-10-07: a player's follower always belongs to that player's game; a dragon flying in the other
game is never taken over; keep at the crash on travelling away with PLANCK (option 3). Worked top to bottom by the loop:
`[x]` with its evidence in one line, `[!]` with the question or action under "Waiting on a test or action", `[-]` with why. Details of
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
- [x] **A dragon flying in the other game is never taken over** (2026-10-09, bot pair `dragonfly` green on
      v1.9.0-3-g22c24c5b, 6 of 6 checks): the server refuses a claim on a dragon whose owner's movement says it flies
      (`CharacterService.cpp`, "it is a dragon flying in its owner's game"; the client sends `Movement::Flying` from
      `actorState.IsFlying()`, `AnimationSystem.cpp`); the leader's claims for 8 s of flight got nothing, and 5 s after
      "landed" the dragon was the leader's. Owed: a real dragon in a session (the client's flying flag is untested in
      game). Was: the party leader's claim skips it until it lands or dies; the Mistwatch dragon fell dead,
      2026-10-03 09:06. ["Dragons taken over in flight"]
- [ ] **The crash on travelling away with PLANCK and our copies left behind** (2026-10-10: `live-copies-left-3`, which crashed on 2026-10-03, passes on today's build with PLANCK 0.6.6 twice and with 0.8.1; watch real sessions for `+0x3AC1A8` / `+0xCBFD24`) (`+0x3AC1A8`, `+0xCBFD24`;
      `live-copies-left-2`). How PLANCK keeps track of the actors it ragdolls, then our copies leave in a way it notices.
      [KNOWN_ISSUES, "Crash within a second of travelling away"] (2026-10-10, PLANCK's source read,
      github.com/adamhynek/activeragdoll) It keeps raw `Actor*` in a dozen tables (`g_activeActors`, bump, shove,
      grab, collision tables in `main.cpp`); an actor destroyed without passing its clean-up leaves a dangling one,
      which fits a contact with "a body whose owner is no longer an actor". Emma has **PLANCK 0.6.6** (2024-11-08);
      it is at **0.8.1** (2026-07-29), 99 commits later, among them "RemoveRagdoll on cell detach instead of
      detachhavok on process change" (2025-01-09, the clean-up moved to when a cell is detached, the moment our
      left-behind copies go), "add lock around active actors set", "Remove actors from the world if their AI is not
      active", "Refactor ragdoll add/remove", "fix crash", "Make things safer". 0.8.1 needs HIGGS 1.6 or newer; she
      has 1.10. The test is ready: `live-copies-left-3` crashes on 0.6.6 in a fresh session. "PLANCK 0.8.1 in the
      rig" under "Waiting on a test or action".
- [x] **The weapon's visible blade** (2026-10-07, built with the corpse fix): `kWorldBoundOffset` is 0xE4; nothing
      reads it on a live path any more (`ReadWorldBound` has no caller), only the shape report's line. Was: `kWorldBoundOffset` 0xB0 is `previousWorld`; the world bound is at 0xE4 on VR
      (measured 2026-10-06). Batch into another item's build.
- [ ] **The arrow nocked on the other player's bow** (measured 2026-10-10 in Emma's 11:40 session, Seen: "dosylia
      loaded an arrow in the bow but seenfront does not see it"). A full VR draw goes 8, 9 (bow draw), 10 (arrow
      attached), the arrow flies while the state is 10 (11:40:53.463, "Projectile launch: shooter 14"), then 0; it never
      reaches 11, 12 or 13. So the state does move: send 9 and 10 to the other game and set them on the copy, then
      check the arrow shows on its string. Was blocked on that measurement. Cause known: the arrow on the string is an animation attachment
      driven by the attack state (`ActorState::AttackState()`, 9 to 13), which nothing syncs; whether VR archery moves
      that state through a real draw is unknown (2026-09-27 saw only 8, 9, 10, 0 within a second: a bow raised, no
      arrow shot). The log line is in the build (`VRArchery: local attack state`, `CharacterService.cpp`). Then: sync
      the state if it moves; attach the arrow by hand on the receiving side if it never leaves 0 (bigger). ["Nocked
      arrow not shown on the other player's bow"]

## Waiting on a test or action

### Decisions

- [x] **Players on the map: decided** (Emma, 2026-10-10). (1) The quest is "Fellow travellers". (2) Everyone gets a
      marker, "a bit different depending if on party or not": party members as quest markers on the map and the
      compass, other players as a world-map icon only, never on the compass. (3) A party member whose body is loaded
      in your game: marker hidden. (4) The text is the name and the place ("Seen, near Whiterun").
- [x] **Proximity voice: decided** (Emma, 2026-10-10). (1) Everyone near you hears you (about 30 m). (2) Off until
      each player turns it on. (3) Yes to a party channel heard at any distance, besides proximity.

- [x] **How releases are made: both ways** (Emma, 2026-10-10: "Both options should work"). (1) GitHub builds them
      when a `vX.Y.Z` tag is pushed: `release.yml` rewritten to make `make-release.ps1`'s layout (`urSovngarde`,
      `Server`, `urSovngarde mod`, the licence files) from the build and the UI; (2) `make-release.ps1` on her PC stays
      as it is. Done 2026-10-10: `release.yml` builds the VR client, server and menu process, gathers the client's
      run-time files with `xmake install` (`SkyrimTogetherClientVR`, `TPProcess`), builds the menu with pnpm and runs
      `make-release.ps1 -ClientFolder distrib/bin`; a tag on main publishes the three zips, a run by hand only builds
      them. Tested here: the same gathering on this PC gave the same 29 entries as a release from Emma's folder; the
      YAML parses (PyYAML). Not yet run on GitHub: "Try the release workflow" under Actions. The Linux server is out
      of it until it builds.
- [!] **Join friends 3, our relay: direct first** (Emma, 2026-10-10; 2026-10-10 night: not started here, the
      launcher's `relay/*`, `join/mod.rs`, `join/hub.rs` and `session.rs` are mid-change in the other launcher session,
      uncommitted; the decision is written in the launcher's TODO for that session, or for here once it lands). Connect directly when the host's port is open,
      through the relay otherwise (two paths to test). The relay runs on the IONOS VPS; its traffic allowance against
      the measured traffic is the one thing to watch. A session with Seen on his own connection is still wanted once
      to measure and once at the end. No UPnP (Emma, 2026-10-10: the relay covers closed ports).
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
      2026-10-10: a likely cause found and fixed: an equip of the weapon a copy already holds empties its hand
      (`live-equip-twice`, Tooling). Seen's log of that fight has her copy equipping items it already held
      (`0:12FCD` three times with no unequip between, `0:4F912` twice with one); whether those were hand weapons was
      not looked up. To watch in the next fight: "equip skipped" lines in the watcher's log, and whether her blade
      still meets his.
- [ ] **How your equipped sword meets his: (1), the physical stop** (Emma, 2026-10-10: "Yes, start it"). A sword grabbed with HIGGS is stopped by his; an equipped one passes
      through (two keyframed bodies). (1) B&S-like: the equipped sword becomes a dynamic body held to the hand, what
      you see follows it, his blade stops yours -- the most work; (2) feedback only: buzz, sound, sparks and the
      defender's rule, no physical stop -- what exists now; (3) fight with grabbed swords. [Physics queue]
      Emma, 2026-10-08, after a fight with Seen: "if I meet my enemy sword I shouldn't be able to still go past it, or
      it defeats the purpose" -- that is (1). To confirm before it starts (the most work), and after the damage path
      below is right: a stop that lets blades through still has to drop the hit.
      Progress, 2026-10-10 morning:
      - [x] Harness: `live-blade` swings the player's EQUIPPED sword by script (`DO swing right through copy <ms>
            [past <m>] [hold <ms>]`, DevBench vrTrackedSet, aimed at the copy's blade centre) and the bot's new
            `infront <units>` puts the copy at the player's height (it stood 156 units inside a hillside before). The
            swing reaches his blade: "Clash: our right hand ... met the right weapon of FF001245" (08:16, 08:37).
      - [ ] The stop, written (not proven): `VRBodySync` "BladeStop" at the frame end. Blades are segments (the
            weapon node's +Y, as far as its bound reaches; measured 53.8 for his iron sword, 58-69 for ours). A
            crossing is the sign change of ours against the plane through our grip and his blade. While crossed,
            the drawn weapon is turned about the grip to rest on his, and let go when the hand comes back, slides off
            either end, reaches his blade or would turn more than 60 degrees. The stopped grip is sent
            (`DrawnGripRotation`), and a hit along that blade is dropped (`IsBladeStoppedAt`, Actor.cpp). Rig 08:37:
            the bodies touched (Clash) but no "BladeStop: ... met" line, so the crossing test never fired. Next: a
            temporary geometry line (both segments, side, t and v) during the swing, then fix and photograph
            `live-blade-held`. That build (069af59) is set aside in the tools folder as
            `urSovngarde.*.bladestop-20261010-0832`. Emma plays on the tested 05:33 build (da73f2f).
- [x] **7.9 GB copied at every start** (Emma's OK, 2026-10-08). MO2's `overwrite\Root` held 77 crash dumps (7.5 GB,
      18 Sep to 6 Oct, written by the game into its folder and captured by RootBuilder), a 388 MB unchanged copy of
      the v1.9.0 `Skyrim Together VR` folder and `urSovngarde Server` (both from the launcher's no-MO2 test install
      into the game folder); RootBuilder copied all of it into the game folder before each start (41 s of a 66 s start
      that evening). Done: the dumps moved to `E:\urSovngarde crash dumps (moved out of FUS 2026-10-08)`, the copy
      deleted, the server moved to `E:\FUS\tools\urSovngarde Server` (where an MO2 install puts it) with the
      launcher's `serverDir` pointing there; `overwrite\Root` keeps 1.6 MB of small files. Hosting from the new place
      is allowed by the existing "Skyrim Together Server (UDP 10578)" firewall rule. Still to see: the next start's
      RootBuilder time. Worth a launcher check later: dumps piling up in a RootBuilder modlist's overwrite.
- [x] **A stiffer PLANCK, tried in the rig** (2026-10-10): no gain, her setting stays. `live-hands`, the
      "VRRagdoll: ... ragdoll hands" lines (8 each run, both hands): at `positionGain = 0.05` (hers) left 18.5-45.2,
      right 13.0-41.6, median 18.7 units from the drawn hands; at 0.2 left 20.4-45.0, right 5.3-42.9, median 20.6.
      Drawn hands unchanged ("hands within 80": median 65.5 and 65.4). Her `activeragdoll.ini` changed for that run
      only and restored byte for byte (`activeragdoll.ini.bak-20261010-gain`). The gap is not PLANCK's position gain.
      Was: (Emma, 2026-10-10: "Try it in the rig"). His ragdoll follows his pose
      8-14 units loose because PLANCK's ragdolls follow softly (`activeragdoll.ini` at the defaults: `positionGain =
      0.05`, `hierarchyGain = 0.6`, `poweredTau = 0.8`). Measure e.g. `positionGain = 0.2` in the rig, revert, and
      give her the numbers to decide (stiffer also means every NPC reacts more stiffly to hits).
- [ ] **Handling a living NPC the other game runs: (2), send the pull** (Emma, 2026-10-10). Today a grab or shove is
      undone at once and the other player sees nothing; now the pull is sent to the game that runs the NPC, which
      applies it (smoother AI, some delay, much new work).
- [ ] **Grabbing the other player: a buzz on the grabbed hand** (Emma, 2026-10-10; no stagger). His real hands
      cannot be moved; his controller on the hand held vibrates. (2026-10-10, read so far) The way is the clash's:
      a `GrabRequest` from the grabber's game, passed on by the server as `NotifyGrab` to the one grabbed, whose game
      pulses that hand (`VRHaptics::Pulse`, as `FeelClash`); new messages, so a protocol change (everyone updates).
      Not a clash: a clash also plays a sound and sparks, and the parry rule only reads local contacts (`ClashEvent`),
      so a grab cannot open a parry. Detection: the mod has no HIGGS interface, but `HiggsBodyHolding(body)` finds the
      HIGGS hand holding a dynamic body; the copy's ragdoll bodies are keyframed ("motion type 2" in the VRRagdoll
      lines), and whether HIGGS can hold one at all is unknown ("can be bent a little by your hand"). Next: the rig,
      DevBench 1.27's `vrTrackedSet` puts the right controller on the copy's hand and presses grip, a diagnostic
      says whether any of the copy's bodies is held. Two things found for that step: `HiggsBodyHolding` reads Havok's
      constraint lists and is only safe inside the physics step (its own note), so the check needs a hook there, not
      the frame end where the ragdoll hands are measured; and `vrTrackedSet` frames carry poses in OpenVR tracking
      space (room), so placing a hand on the copy means standing the bot's copy at a known offset in front of the
      player (DevBench README; `input` schema: frames of `tMs`, `originCode`, `hmd`, `left`, `right`, each controller
      with pose, `packetNumber`, `pressed`, `touched`, five axes).
- [x] **Long sessions grow: DynDOLOD DLL NG, newer version tried** (2026-10-10, rig, game alone, four round trips
      `cow Tamriel 34 8` and back, 45 s each). Alpha-32 (installed): threads 222, 247, 271, 296 and private memory
      9.7, 10.1, 10.6, 11.2 GB at each return. **Alpha-43** (Emma's download, 2026-09-09, newer than the Alpha-33
      asked for): threads 201, 193, 192, 193 (no leak) and 10.0, 10.3, 10.7, 10.7 GB (half the growth, flat on the
      last trip). Modlist switched for that run only and restored byte for byte (`modlist.txt.bak-20261010-
      dyndolod43`); Alpha-43 kept as `E:\FUS\DynDOLOD DLL NG Alpha-43 (tested 2026-10-10, ready to install)`.
      Distant LOD not looked at. To do: both of you install it (replace the files of the mod `DynDOLOD DLL NG`).
      Was: try DynDOLOD DLL NG Alpha-33 (Emma, 2026-10-10) (~0.4 GB and 12 threads per loading
      screen, DynDOLOD DLL NG's threads; it ends in a freeze or a driver crash). Measure threads and memory over
      loading screens in the rig against Alpha-32; both machines the same afterwards.
- [x] **A message when the other player goes down ("X is down")**, message only, no revive (Emma, 2026-10-10).
      Done: when a party member's reported health goes from above zero to zero, the HUD says "urSovngarde: <name> is
      down" and the log "Party: <name> is down" (`OverlayService::OnNotifyPlayerHealthUpdate`; names kept from
      "is online", forgotten on leave and disconnect). Tested in the rig with `live-down` and a new bot command,
      `partyhealth`: one line per time down, 3 of 3. The other HUD lines that still said "Skyrim Together:"
      (connected, connection failed, online, left, party) now say "urSovngarde:". Deployed
      v1.9.0-4-gfa8d5355-dirty.76a82e0.
- [x] **PLANCK 0.8.1 in the rig** (Emma, 2026-10-10: yes). Done 2026-10-10, v1.9.0-4-gfa8d5355-dirty.91ff85f: on her
      PLANCK 0.6.6, `live-copies-left-3` no longer crashes (two fresh sessions, 5 of 5 each: alive after the trip away
      and back, five "CopyGone"), so the test can no longer tell the versions apart; on 0.8.1 ("PLANCK v0.8.1.0",
      "Got higgs interface!") it passes the same, 5 of 5. Modlist switched for that run only and restored byte for
      byte (`modlist.txt.bak-20261010-planck081`); 0.8.1 kept outside MO2 as `E:\FUS\PLANCK 0.8.1 (tested
      2026-10-10, ready to install)`. Updating is safe as far as the rig shows; whether to is her call (it changes how
      grabbing and hits feel).
- [x] **The server's defaults in a release** (Emma, 2026-10-10): PvP off, difficulty 2 (Adept), name "urSovngarde
      Server"; the rest as the code's defaults (time scale 20, death system on, XP shared in the party, party made and
      joined automatically, no gold lost on death, mod check off, 8 players). Done: `Tools/VR/STServer.default.ini`,
      copied by `make-release.ps1` instead of the build folder's test file. Tested: a scratch server started with it
      ("started on port 10578"), kept the values and added back a setting taken out of the file (the server rewrites
      the file at start, comments included, so it has none); the stand-in release's server zip carries PvP false,
      difficulty 2, the name, empty passwords.
- [!] **Playback delay:** after your next session, the "InterpDiag players" lines say how much of the 225 ms delay
      the network really needs; it could then follow the connection (lower on a good link). Yes or no, once measured.

### Actions

- [x] **Update DevBench** (2026-10-10): 1.27.0 from its author's GitHub release, in `E:\FUS\mods\DevBench` (meta.ini
      1.27.0); 1.22.0 kept as `E:\FUS\DevBench 1.22.0 (set aside 2026-10-10)`. The rig game started, loaded and
      connected with it; it now drives `vrTrackedSet` (headset and both controllers: poses and buttons). Was: its newer releases drive the VR controllers by script, so the rig
      can swing a sword and test PLANCK hits, the sword-hit fix, blocking and swing lag without you. Test tool only.
- [!] **Try the release workflow once** (2026-10-10): after you commit and push, GitHub, Actions, "Create GitHub
      Release", Run workflow, on main. It builds the three zips as an artifact and publishes nothing; if it goes
      green, the next `vX.Y.Z` tag on main publishes a release by itself. If it goes red, I read its log.
- [!] **Install DynDOLOD Alpha-43** on both PCs (tested 2026-10-10, ready as `E:\FUS\DynDOLOD DLL NG Alpha-43 (tested
      2026-10-10, ready to install)`): replace the files of the mod `DynDOLOD DLL NG` with it, or install it as a mod
      and untick Alpha-32. Seen downloads 'DynDOLOD DLL NG and Scripts 3.00 Alpha-43' from Nexus (97720).
- [!] **Next session, one drag each way:** Seen drags a body your game owns for a few seconds, then you drag one of
      his. Both logs say whether it changed hands ("asking for it", "Transferred ownership ... for party leader
      claim") and whether its bones arrived ("posing body").
- [!] **Next fight:** does hitting his sword still hurt him; do blocks on your screen hold; does damage feel late
      (another player's hits wait about a third of a second to be judged)? Logs: "PvP: ... not sent", "Defender's rule".
- [!] **Two minutes in the headset:** connected, let yourself be killed, then load a save while down or within ten
      seconds of getting up. Does it crash? With nobody in the headset it does every time (`skyrimvrtools.dll`).
- [!] **Seen's mods match yours:** his newer VR Address Library crashed him on loading (Community Shaders patches the
      wrong spot); your combined `version-1-4-15-0.csv` fixes it (KNOWN_ISSUES section 1).
- [x] **Seen's health bar in your fights** (8 and 9 Oct). Emma, 2026-10-09: "Health bar is fully fixed".
- [x] **Next session, a few arrows:** done 2026-10-10. A VR draw goes 8, 9, 10 and the arrow flies at 10; it never
      reaches 11 to 13. The nocked-arrow item is open again with this.

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
- [-] Quit crash in Enchantment Art Extender (Seen, 2026-10-04 16:26:56). (2026-10-09) Not actionable: the
      signature of the quit crash (`EnchantmentEffectExtender.dll` reading the `UI` singleton after it is gone,
      VR_HISTORY "Crash when quitting"), 15 s after Emma's own crash ended that session; the quit guard
      (`IsProcessExiting()`, 2026-09-19) is in. A crash while quitting loses nothing. Reopen if this signature shows up
      while someone is still playing.
- [x] Audit every overlay call made before the VR menu exists (a hard crash) (2026-10-09): done on 2026-10-07
      (VR_HISTORY: every `OverlayService`, `PartyService`, `VRDashboard` and `OverlayClient` path checks first; the
      `InputService` window hook fixed); re-checked: the only overlay line added since (`git diff f8f219aa HEAD`) is the
      SteamVR tab's name in `VRDashboard`, an OpenVR call, not the menu's browser.

### Sync: NPCs, followers, the world

- [x] Followers and dragons in flight -- in the work queue: both done there (bot pairs `follower` 2026-10-07,
      `dragonfly` 2026-10-09).
- [ ] Maven fought by Seen was invisible for Emma; the blacksmith dead on Seen's side only; Brynjolf (essential)
      spinning on the floor for Seen after being "killed" (2026-10-04).
- [ ] Lydia did not fight back while Emma was down; enemies ignored Seen while he was invisible to her (2026-10-03).
- [x] The `Unequip` loop: one actor sends the same unequip again and again (the Bandit Leader "ignoring us", 92 a
      second); the queue is capped, the cause is not found. 2026-10-09: no longer sent: Seen's logs of 10-08 and
      10-09 hold about 700 `ActionFlood` lines for action `46BAF` (Unequip / WeapUnEquip, 30 to 36 a second, actors
      such as 64814-64816, 529CD, D4FDA), every one "refused by the game here and not sent". Left: whether the AI's
      retrying is ours. (2026-10-10, rig) Not ours: at the rig's spot actor 107CD0 retries action 132AF about 60 times
      a second, all refused, 9 `ActionFlood` lines in 90 s with the mod loaded and not connected, and the same 9 in
      90 s connected. The game's AI does it whatever the network does; nothing of it is sent.
- [ ] A skill book Emma touched could not be read by Seen. (Emma, 2026-10-10: "once I had interacted with the item,
      it was like it being in a corrupted state, he couldn't grab it, stash it or read it. I got same issue
      reversed.") So: an item one player has touched becomes unusable for the other, both ways. Reproduce in the rig
      with the bot taking and dropping a book (`live-world-object` does a plate), then find what state stays on it. Not reproduced: in the rig a plate held and let go of can
      still be taken (`live-world-object`, VR_HISTORY); Seen's game had the book still on the table and held it while
      she moved it, each hold released 2-3 s later. "Ask Seen about the skill book" under Actions.
- [ ] Quest NPCs out of sync (Bastianus Axius); quest dialogue heard twice; message boxes shown to both players.
- [ ] Weather and time on VR: only a party member gets the leader's weather. (2026-10-09) That is upstream's rule
      (`WeatherService`, server and client); the open part is whether a VR member applies it. Addresses checked: the
      Sky functions match CommonLibVR-NG (`ForceWeather` 25696, `ResetWeather` 25695, `SetWeather` 25694, `Get` 13789)
      and the two ids given as AE are mapped by `VRAddressOverrides.h` (25684, 25697). A rig test needs the bot to
      lead and the game to join after it (the game connects first in the live harness, so it always leads): not worth
      it. Instead both logs now say it: "Weather: ours is now X, sent to the party as its leader" and "Weather: the
      party leader's weather X applied, the sky now has Y" (`WeatherService.cpp`). Built and deployed
      (v1.9.0-4-gfa8d5355-dirty.94246eb); read both logs after the next session with both players in a party.
- [ ] Mounts: untested. (Emma, 2026-10-10: "Need all of them": fix it) (2026-10-10, rig, `live-mount` and a new bot command `mount`) **The other player does not see
      a rider mount.** The chain runs: the rider's `MountRequest` passes the server ("Mount: rider 15 mounts 16; told the
      players in range") and the game gives its copy of the rider the mount package, which the game accepts ("Mount:
      copy ... told to ride copy ... of mount ...; the game accepted it"), but the copy never gets on: not after 12 s,
      not after 32 s (screenshot: the horse standing over the copy). Second approach, the copy left unpinned from its
      owner's position for 15 s so the package could walk it to the horse: still not on (reverted; it would leave a
      copy frozen for 15 s at every mount). So the package does not run on a copy, most likely because copies do not
      run their own AI packages. Kept: the "Mount:" log lines on both sides. Next, if wanted: put the copy on the
      horse directly (the game's own mount state, without a package) and get it off when the owner's `bIsRiding`
      (synced) goes false; "Mounts" under "Waiting on a test or action".
- [ ] A downed follower lying sideways and running.
- [x] Sliding: creatures with no animation-variable descriptor slide whatever else is fixed; Dinya Balu sliding on
      Emma's screen only. ["Sliding: the descriptor lookup"] (2026-10-10, from the logs) Gone as far as the logs see:
      since 2026-10-07, in Emma's log and Seen's bundles, `SlideDiag` counts 54 of 34,216 remote moves with frozen
      animation variables (0.2%; it was 100% on 2026-09-27); the reads still skipped (`AnimVarDiag`, 8,017 in 203
      lines) are mostly references with no animation graph, 90 of them the local player's. Reopen on a sliding seen
      in play, with the creature's name.
- [x] Different animals on each screen: the owner's base form travels now; not confirmed. Confirmed in the rig since
      (`live-levelled`, 2026-10-03: a goat claimed at a bear's spawn point is conformed to the owner's; VR_HISTORY), and
      in the sessions since 2026-10-07 the owners' picks are sent ("Captured leveled NPC pick", 100+ lines) and no game
      had to change an animal to match ("Applied leveled NPC pick": none). (2026-10-10)
- [x] NPCs below the floor for one player: never measured where `ForcePosition` puts them. ["Goal 5"] Measured since
      (VR_HISTORY, 2026-10-03): `SinkDiag` named 11 placements in all the logs, one actor each, never the same twice;
      nothing sinks repeatedly. The one big drop belongs to "Copies spawned on the ground" (The other player's body).
- [-] A distant dragon vanishing; the range check for a remote dragon with no copy here. (2026-10-10) The game's,
      not the sync: read with positions (VR_HISTORY, "Distant dragon vanishes"), the dragon hovered at the edge of
      the 5x5 loaded grid and both engines unloaded it there; no ownership rule can draw an actor the engine unloaded.
      The churn around it (hand-offs of an unloaded dragon) is cosmetic in the logs. Reopen on a dragon vanishing
      close by.
- [x] Vanishing at the Whiterun gate: logging in place, untested. Tested since: `live-whiterun`, 2026-10-03, 8 of 8
      (the copy goes when its player enters WhiterunWorld, another worldspace, and comes back when they leave;
      VR_HISTORY).
- [x] The paused player goes silent; grey distant hills (LOD). (2026-10-10, from the logs) A paused player keeps
      sending: 162 probes taken with the pause counter above 0 since 2026-10-06 (Emma's log, Seen's bundles), the
      longest "last move sent" 32 ms. Grey hills: the game's LOD streaming, not the sync (DynDOLOD's, see "Long
      sessions grow" for Alpha-43).
- [x] Ownership: "actor for ownership transfer not found" warnings; a newer ownership number accepted by the six
      receiving handlers instead of exact equality. (2026-10-09) Settled by the logs: the warning's text is no longer
      in the client, and none of the 11 log bundles in Downloads nor Emma's current log has it; `EpochMiss` (counting
      every update dropped for a wrong ownership number, since 2026-09-26) appears in none of them either, so exact
      equality has thrown nothing away and stays.
- [x] Removals for a viewer who walked away (Emma, 2026-10-10: try it in the rig; keep only if nothing breaks). Done
      and kept: the server now sends a removal for each character that was in range of where a player was and is not
      of where it is (`PlayerService::RemoveWhatLeftRange`, from both the exterior entry and the grid shift: the entry
      comes first and already moves the player, so a first try in the shift alone removed nothing). `rangeback`
      now also requires the removal: 12 of 12; all 13 bot pairs green; in the rig `live-copies-left-3`,
      `live-npc-away`, `live-whiterun` 5 of 5 each with removals arriving ("server removed player copy"), the game
      alive. Deployed v1.9.0-4-gfa8d5355-dirty.f2b75bb, client and server. Was (the asymmetric range path): `OnShiftGridCellRequest` sends what came
      into range and nothing for what left, so a viewer keeps copies of what it walked away from (`rangeback` pair,
      2026-09-30). Left undone on purpose: removals are entity churn, where crashes have been. Look at it together
      with the PLANCK crash on copies left behind (work queue).
- [x] An NPC's health after a revive is not re-sent; a respawn does not restore stored health (`OnRequestRespawn`);
      `NotifyRespawn` is sent from inside the inventory lookup (`PlayerService`). (2026-10-09) The revive: bot pair
      `revive` green, 8 of 8 (the watcher sees the NPC die, then alive at 100, live and rebuilt from the server's
      store). The player's respawn restores the stored health and the alive flag (`OnPlayerRespawnRequest`, since
      2026-09-25); `OnRequestRespawn` is the beast-form rebuild, not a death. `NotifyRespawn` now goes out after the
      inventory lookup, not inside it; relay pair green with its respawn round (6 of 6, "a respawned player must not
      be rebuilt at the health they died on"); deployed v1.9.0-4-gfa8d5355-dirty.94246eb, client and server.
- [x] (2026-10-10: not seen since: 62 connection attempts in Emma's log and Seen's bundles since 2026-10-07, none
      "timed out" (`VRConnectService`); the spawn burst was timed and fixed on 2026-10-03, `live-spawn-burst`.)
      Connecting right after loading a save sometimes times out (the attempt, not a running connection); the spawn
      burst on joining (30-40 actors in one millisecond) has never been timed.
- [ ] Upstream's per-dungeon respawn positions (needs cell editor ids, which VR may not keep). (2026-10-10) Already
      in our code (`CellRespawnOverrides`, upstream #875, used by `PlayerCharacter::RespawnPlayer`); the open part
      is only whether VR keeps the editor ids it looks cells up by. The respawn now logs "RespawnPlayer: in cell X
      'name', at its override position | its COC marker": an empty name after the next death means the overrides
      can never apply on VR. Built and deployed (v1.9.0-4-gfa8d5355-dirty.012d445).

### Dead bodies

- [x] Nothing we do moves a corpse's ragdoll, so bodies lie in different places on the two screens ("CorpseDiag");
      `ForcePosition`, `MoveTo` and the console all move the reference, not the ragdoll. Done in the work queue,
      "A corpse lies where its owner's lies" (`VRBodySync::PlaceCorpse`, `live-corpse-place` 5 of 5, 2026-10-07).
- [!] The bot sending a humanoid body's bones, to test the receiving side in the rig. (2026-10-10) The bot can
      already play a recorded stream, bones included, on a body it owns ("capture", then "replay npc", `Bot.cpp`), so
      the bones need not be made up: record the game dragging a human corpse (`live-body-grab-human` sets that up),
      then replay it on a second corpse of the bot's and check the game's "posing body". The drag needs a VR hand
      moved by script: waits for "Update DevBench" (Actions).

### PvP and physics

- [ ] Less delay for hands and weapons: from the next real session's "InterpDiag players".
- [x] The weapon's visible blade: `kWorldBoundOffset` is 0xB0 (`previousWorld`); the world bound is at 0xE4 on VR.
      Done in the work queue (0xE4, 2026-10-07).
- [ ] The other player's hands sit slightly off (palms together, a little low): structural, `VRPose`.
- [ ] Your own sword hitting where the blade is not (2026-09-24 screenshots). ["4. Position accuracy"]
- [ ] Nocked arrow on the other player's bow: measured 2026-10-10 (the state moves 8, 9, 10); see the work queue.
      ["VRIK and HIGGS interactions"]
- [x] Objects already in the world moved by hand (a cup on a table) are not seen moving; dropped items are. Done
      earlier (VR_HISTORY: movable objects within 400 units are watched and sent, 22ae2af; `live-world-object` green
      twice, the game's plate within 20 units of where the bot sent it). Line closed 2026-10-10.
- [x] A floating item whose carrier disconnects mid-carry. (2026-10-10, by the code; not seen live) Handled since the
      drop-move work of 2026-09-30: an item held still on the other side's word goes back to physics after 2 s
      without a word, whatever the reason (`DroppedItemService.cpp`, "stopped being moved over there without coming
      to rest; released it here"; the same for world objects, "WorldObject: ... handed back to physics here"). The
      line in a log is the proof when it happens; `live-dropmove` does not cover a carrier going silent.
- [-] Item and body drift fixes from TiltedEvolutionVR (`aaf5d83`), if drift shows up. (2026-10-10) No drift reported
      or logged since; corpses now lie where their owner's do (`PlaceCorpse`) and dropped items are moved for everyone.
      Reopen with a drift seen in play.

### The other player's body

- [ ] Legs when moving: a walk or run on the copy with smooth locomotion.
- [ ] Face "looks off" (FaceGen on VR).
- [ ] The invisible player (worse indoors): look at render state, not actor state. ["0a. The three open complaints"]
- [x] The friend's health bar: our message is right, the widget refuses it; the enemy health bar on the other player.
      (2026-10-09) Emma: "Health bar is fully fixed", seen in play; no change needed since the meter fixes of
      September (`UI::SetEnemyMeterTarget`, the menu shown when it is missing).
- [x] Copies spawned on the ground when the incoming height is bad; fade in instead of popping (Emma, 2026-10-10:
      "Fade in"). Done as "not drawn until placed": a player's copy has its 3D hidden (NiAVObject flags, +0x10C on VR)
      from the moment it arrives until the first movement update places it, 1.5 s at most (`Systems/SpawnReveal`).
      Rig, `live-reveal` (new): "shown 10 ms / 27 ms after its 3D arrived (placed from a movement update)", the
      screenshot shows the copy standing in place; `live-npc` 8 of 8. NPC copies are left out: standing still they
      send no movement, and hidden they all appeared 1.5 s late. Not a gradual fade: papyrus `Actor.SetAlpha` could do
      one, but a copy left at alpha 0 by a failed fade would be the invisible player again; the jump is gone, which
      was the complaint. (Corrected 2026-10-10: an earlier note said the fade function was not mapped on VR.) Deployed v1.9.0-4-gfa8d5355-dirty.4af5155.

### Performance

- [x] Spawn bursts on cell change; `RunNakedNPCBugChecks` and the equipment snapshot once a second; linear searches
      in hot paths. Measure first (a 60 s benchmark at one spot, four setups). (2026-10-10) Measured from what every
      log already writes ("Perf last 30 s", "Mod update took ..., slowest section"), Emma's log and Seen's bundles:
      since 2026-10-07 the mod's own work averages 0.22 ms a frame (148 windows of 30 s; 11 with one update over
      20 ms, 1 over 50 ms). The once-a-second checks never show as the slowest part. Spawn bursts were fixed on
      2026-10-03 (`live-spawn-burst`). The large `RunLocalUpdates` spikes (to 251 ms) are all in Seen's logs of
      10-06 and gone since 10-07. What is left is the next item.
- [x] Hitches from the in-game menu's update: `OverlayService::OnUpdate` is the slowest part of 70 slow frames since
      2026-10-07, 12 over 20 ms, up to 86 ms; then `CharacterService::RunRemoteUpdates` (6 over 20 ms, to 35 ms) and
      `RunRemotePlayerDiag`, a diagnostic at about 9 ms each of 14 times. (2026-10-10, from the lines before each)
      None of them is in play: the big overlay ones (63 to 158 ms) are once per game start, right after the loading
      screen, when the SteamVR dashboard tab is created ("VRDashboard: ... overlay created"); the small ones (7 to
      11 ms, once a second) only while that dashboard is open, the game paused behind it. The slow remote updates
      are the moment the other player's copy gets its 3D ("Applied 3D for actor") or a corpse is put in place. The
      diagnostic measures each copy once on first sight and then every 5 minutes, by design. If the start stutter
      ever matters: create the dashboard tab during the loading screen instead.
- [!] Both games slowed over a session (threads and memory) -- the DynDOLOD decision above. (2026-10-10) DynDOLOD DLL
      NG Alpha-43 ends the thread leak and halves the memory growth in the rig (Decisions, "Long sessions grow"):
      "Install DynDOLOD Alpha-43" under Actions.

### Shareable

- [ ] An in-headset menu (the Skyrim Together UI as a SteamVR dashboard tab, large text, a small overlay for chat).
- [-] Start-up checks with plain messages in the headset (Emma, 2026-10-10: the manual route stays as it is, no checks in the headset) (2026-10-09): the launcher already makes them before the
      game starts, in plain words (`urSovngarde-launcher/rules/setup.json`: game version, address library and its
      Community Shaders mismatch, SKSE, the Engine Fixes preloader, uGridsToLoad, the mod's plugin, its header
      version, the plugin count). In the headset they would only serve installs without the launcher: "Does the
      manual route stay?" under "Waiting on a test or action".
- [x] A licence check before sharing builds (GPL-3) (2026-10-09). Every zip of `make-release.ps1` now carries
      `LICENSE.txt` (the repo's notice), `GPL-3.0.txt` (the full text, from gnu.org, `Tools/VR/licences/`) and
      `SOURCE.txt` (the repository at the exact commit, and where the build steps are); the full zip also
      `THIRD-PARTY.txt` (CEF 141 with its BSD licence, the DirectX compilers, the Discord SDK). README.md's "Building"
      now holds the real steps (those of `windows.yml`). Found on the way: the next zip would have shipped
      `UI.before-logo` (a whole old menu), now skipped with every `.before-*`; `EarlyLoad.dll` came from Emma's folder
      instead of the build, now from the build. Tested on a stand-in repo: 166, 11.7 and 3.1 MB zips, all three with
      the licence files, no `before-` entry. Not settled here, upstream's choices kept as they are: the Discord SDK
      is proprietary and OpenSSL 1.1.1 (linked) has a GPL-incompatible licence.
- [x] Install script and a guide for other modlists (2026-10-09): the launcher installs; by hand,
      `README-mod-manager.md` (any MO2 or Wabbajack list), `README-vortex.md`, `README-manual.md`.
- [x] VR server defaults (difficulty, PvP, time scale): done 2026-10-10, "The server's defaults in a release" (Decisions).
- [x] A test checklist per build (2026-10-09): `Tools/VR/TEST-CHECKLIST.md`, every build (the four built together,
      the test for what changed, deploy, it starts), a release (clean tree, `make-release`, `scan-release`, the same
      zips on GitHub and Nexus) and what to read after a session.
- [!] **Antivirus false positives** (2026-10-10: (1) and (2) done, signing none for now; left: Emma's VirusTotal key)
      (Nexus quarantined the first upload, 2026-10-09; VirusTotal: guessing engines
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
      zip, the launcher covering connect, host and update (Emma, 2026-10-10: the manual route stays as it is).
      (3) Signing: none for now (Emma, 2026-10-10); VirusTotal scans and false-positive reports only. Her action:
      the free VirusTotal key in `%USERPROFILE%\.str-vt-key`.
- [~] **The website's public server page, live** (Emma, 2026-10-10: a push every 10 s while anyone is on, 60 s
      when empty; place names from the client; a player who opts out is left out of the list but counted in
      `player_count`). Asked by the website session (`urSovngarde-public-server-live.md`; its contract is
      `docs/public-server-status.md` in `~/www/stvr`). The hub is ready (its `PUBLIC_SERVER.md`).
      (1) Done 2026-10-10: the server pushes its status, `Code/server/Services/PublicStatusService.cpp`. Only with
      `LiveServices:bPublicStatus=true` (off by default); the key from the `URSOVNGARDE_SERVER_KEY` environment
      variable, never the ini; `sPublicAddress` is the address shown, `sPublicStatusUrl` the hub. It sends the name,
      address, version, protocol, password flag, player limit, start time and `player_count`, and per player a
      random token per connection, the character's name and, outdoors in Tamriel only, x, y and heading. Sent from
      its own thread; joins and leaves pushed at once; "online": false on `/quit`. Closing the window or Ctrl+C
      sends nothing (the runner's quit handler is disabled, `server_runner/main.cpp`): the hub and the page call it
      offline after 180 s. Tested against the hub on `wrangler dev` with a bot (join, walk east: heading 90, leave,
      clean stop), a wrong key, no hub, no key, and the real hub's certificate (401, as expected with no key there).
      (2) Written 2026-10-10, not built yet (Emma was about to play; the next five-target build carries it), one
      protocol change: `PlayerPlaceRequest` (client `PlayerService::RunPlaceUpdates`, once a second: the room's name
      indoors, the location's outdoors, the worldspace's otherwise, in the game's language; the server converts to
      UTF-8) and `AuthenticationRequest.HideFromPublicPage`, read from
      `HKCU\Software\TiltedPhoques\TiltedEvolution\Skyrim VR\HideFromPublicPage` (DWORD), which the launcher's
      checkbox "Hide me from the public server page" writes (asked of the launcher session). A hidden player is
      counted in `player_count` and never named. To check after the build: a place name from the real VR client.
      (3) Launcher, done 2026-10-10, uncommitted and not released: the checkbox (Friends panel, Join, under "or an
      address"; fr "Me cacher de la page du serveur public", es "Ocultarme de la página del servidor público", de "Mich
      auf der Seite des öffentlichen Servers verbergen") and `ursovngarde://join?address=<host>:<port>` (IPv4 or a
      dotted name and a port; anything else refused; always asks "Join this server?"). It ships with or after the mod
      build that reads the setting. Older launchers have no handler for the link. (4) Emma: `SERVER_KEY` into Cloudflare and a hub deploy (the hub's TODO.md), the key in
      `URSOVNGARDE_SERVER_KEY` on the public server's machine, `bPublicStatus=true` and `sPublicAddress` in its
      STServer.ini. (5) The map's calibration (Whiterun's gate, Windhelm's bridge) from the rig with DevBench.
      Not settled: parties on a public server. `IsPublicServer()` is `bAnnounceServer`, which also lists the server
      on upstream's list (a 403 there stops our server), so with it off every stranger lands in one party (XP and
      quest sync); `in_party_with` is left out until that is decided.

### Tooling

- [x] The bot registers only one of two NPCs asked for in the same instant (`live-copies-left-2`) (2026-10-09):
      the bot's, not the server's: one `m_npcCookie` for every "npc" request, so a second request overwrote it and the
      first answer was dropped. Now one cookie per pending request (`m_npcCookies`, `Bot.cpp`); new script
      `npc-twice.txt` green (two bears 10 ms apart, "NPC registered as actor 3" and "4", owned == 2, 4 of 4).
      `run-bot-tests.ps1` now copies the tracked scripts into the build like `run-all-pairs.ps1` (a new script was
      "missing" there). `live-copies-left-2` can now leave its two bears behind, for the PLANCK crash item.
- [x] An equip of a weapon the copy already holds may leave its hand empty (`live-equip`). (2026-10-10) Proven and
      fixed in the rig with a new script, `live-equip-twice` (the held sword equipped again, then twice at once):
      without a fix the copy's right hand was empty after the second and third equips while the game said the sword
      was equipped (2 of 8 failed); now an equip of a hand item the copy already holds in that hand is skipped
      ("already holds X in the right hand; equip skipped", `InventoryService::OnNotifyEquipmentChanges`): 8 of 8.
      (A first run blamed the double equip alone: its last check raced the script's unequip; a 3 s wait fixed the
      script.) Deployed v1.9.0-4-gfa8d5355-dirty.91ff85f, same logic as the build that passed.
- [ ] The creature-gait check is noisy; the other order of "a creature this game made comes back".
- [-] TiltedEvolutionVR's two Havok crash guards (`29f99ed`) and ownership blacklists (`1660eb0`): read, not taken.
      (2026-10-09, re-read from GitHub.) `29f99ed` guards two crashes, `+0AB1ABA` (ragdoll added with a bad
      constraint) and `+03AD7B1` (island listener on a reference without 3D): none of the 115 Crash Logger logs in
      `My Games\Skyrim VR\SKSE` nor the 4 in Downloads has either address; port them the day one does (they name the
      reference). `1660eb0`: nothing left to take. Our server already drops actor-value, max-value and death updates
      not from the current owner and epoch (`ActorValueService.cpp`, `ReportEpochDrop`), clients drop stale ones
      (`ReportEpochMiss`), there is no owner blacklist to persist, and a refused move (not owned: `OwnerView`) logs
      the id without touching the iterator.

### Later

- [ ] **A flat (non-VR) player with VR players** (asked on Reddit, 2026-10-09: "Is it possible for one person to be
      non VR?"; Emma: add it, for later). Possible in principle: the repo still builds the flat client
      (`SkyrimTogether.exe`, `build_client("SkyrimTogetherClient", false)`), `Code/encoding` has no VR-only message,
      so both builds share the protocol and the server, and a flat player arrives in VR through animations like any
      NPC. Missing: (1) the VR player on the flat screen: VR swings are not animations, and the hand, head and weapon
      poses we send (`VRPose`) are applied only in VR builds (`VRBodySync`, about 130 `SKYRIMVR` switches in the
      client); the flat side would pose the arms and the held weapon from them -- the bulk of the work. (2) The mod
      check (`GameServer.cpp`, `kModsMismatch`; `bEnableModCheck`, off by default) refuses differing plugins when on,
      and form ids must still mean the same thing on both sides: `SkyrimVR.esm` on one side, the Creation
      Club files on the other, VR-only plugins in a VR modlist; a rule for the expected differences and a light
      shared modlist. (3) Fights across the two: a flat block has to count in the defender's rule, which knows only
      VR blade contact. (4) The flat build untested since the fork, against the current Skyrim SE; testing needs an
      SE copy. Only once VR to VR is solid.
- [x] **Party members on the map and compass** (built and tested in the rig 2026-10-10 afternoon, waiting for a real
      session: Emma and Seen look at the compass and the map in the headset). What was built, against the plan below:
      the records come from `Tools/VR/player-markers.py` (Python, not xEdit: it writes the quest, the holding cell,
      7 party markers with their own activators and 32 map markers into `GameFiles/Skyrim/SkyrimTogether.esp` at
      fixed object ids 0x5000 to 0x505F); exact positions came now, not later: the protocol changed anyway for voice,
      so the server sends `NotifyPlayerWhereabouts` every 2 s (each player's world, cell, position and place name,
      `server/Services/WhereaboutsService`); `client/Services/PlayerMarkerService` moves the markers (MoveTo), names
      them natively (the activator's name for the objective text, ExtraMapMarker's for the icon; no SKSE natives),
      shows objective N through the game's Quest.SetObjectiveDisplayed, and puts a marker away in the holding cell
      while that player's body is in our game. Rig (`live-markers.txt`, 10 checks, 0 failed): forms found, quest
      started; bot near, no marker; five cells away its party marker follows it and objective 1 is displayed
      (IsObjectiveDisplayed true, marker at its x); leaving the party puts that away and a map marker follows instead;
      back near, the map marker is put away. Found on the way: an objective shown in the frame the quest is started
      is never displayed (the quest is still starting); it is now shown once the quest runs, and checked again every
      2 s. Not seen yet: the compass and the map themselves (the rig has no headset view of them), and whether a map
      marker near you also shows on the compass (it is put away while the body is loaded, which covers the loaded
      area). The map icon is type 11 (a standing stone), a guess for Emma to look at. Was: (Emma, 2026-10-10: "Start now"; she and Seen try invite, accept and teleport in the next session) (Emma, 2026-10-09, for a public server: "how hard would it be marked on
      the map? Some that look good without breaking immersion"). Upstream has no player markers: only the player
      list, parties with "teleport to" and the member's place in the party menu, and shared custom waypoints
      (`MapService`). Plan: Skyrim's own quest markers, so the VR map and compass need nothing: a "Fellow
      travellers" misc quest in `SkyrimTogether.esp` (name kept, saves fine) with up to 7 objectives, each aimed at
      an invisible persistent marker that the client moves to that member. A far player has no actor in our game (the
      server spawns characters only in range, `CharacterService.cpp` `IsInRange`), but every client already gets each
      player's world and cell (`NotifyPlayerCellChanged`): cell precision (about 58 m outdoors) needs no protocol
      change; in an interior the game itself points to the door. Immersion: party members only; tracking the quest in
      the journal shows or hides them; the objective hidden while their real body is in our area; text in the game's
      voice ("Seen travels near Whiterun"). Work: the records (Creation Kit or xEdit), the client code moving markers
      and showing objectives, VR addresses for the quest functions (the likely surprise), one VR session. Exact
      positions later would need a new message (everyone updates). Before it: try invite, accept and "teleport to" in
      VR once; never tried, our sessions were two players who knew where the other was.
      Plan, completed 2026-10-10 afternoon (checked against the code; no code written yet). The plugin's last records
      were added by hand in 2026-09; no generator for them in the repo.
      1. Records, in `GameFiles/Skyrim/SkyrimTogether.esp` (the client finds its forms by name, as
         `MagicService.cpp` does with `ModManager::Get()->GetByName("SkyrimTogether.esp")`), made by an xEdit script
         kept in the repo (`Tools/VR/party-markers.pas`), run in TES5VREdit (`E:\FUS\tools\SSEEdit 4.1.5f`; once
         from the command line if xEdit allows it, else one "Apply Script" click by Emma on a copy of the plugin):
         a misc quest "Fellow travellers", not start-enabled; 7 persistent markers (an activator with no model, so it
         can carry a name) in a small holding interior; 7 reference aliases Member1..7, each a forced reference to its
         marker, so the client never fills aliases; 7 objectives, objective N aimed at alias N, text `<Alias=MemberN>`
         (the marker's name). Form ids written down in the script's header.
      2. Moving the markers natively, not by papyrus: `TESObjectREFR::MoveTo(cell, position)` (`TESObjectREFR.cpp`,
         the call the teleport of an NPC already uses, `CharacterService::MoveActor`) takes any cell and its
         worldspace. Far member: their cell from `NotifyPlayerCellChanged` (world and cell, already sent to everyone):
         an exterior cell, the marker at its centre (grid x 4096 + 2048, the same for y; about 58 m of precision); an
         interior, the marker inside it and the game itself points at the way in. Near member (their copy loaded
         here): the marker follows the copy twice a second, so the compass is exact without touching aliases.
      3. The quest from papyrus natives through `PapyrusService` (the `PAPYRUS_FUNCTION` pattern, as
         `Quest.SetCurrentStageID` in `TESQuest.cpp`): Quest.Start on connect and Stop on disconnect,
         Quest.SetObjectiveDisplayed per member, and SKSE VR's ObjectReference.SetDisplayName for the member's name
         (fallback if the lookup fails: the name written natively on the marker). The only new risk: SKSE's natives
         not found by name; the first rig step checks it before anything else is built.
      4. `PartyMarkerService` (client, new): party members from `PartyService`, slot per member, once a second the
         markers moved and objectives shown or hidden; a member leaving the party or the server frees the slot.
         Nothing in the protocol.
      5. Tests: in the rig, the bot joins the party (`party` commands), then `teleport away`: a log line with the
         marker's cell and position, `IsObjectiveDisplayed` true, a SHOT of the map (DevBench `menu`); `teleport
         back`: the marker on the copy. Then one session with Seen: the compass and the map in the headset.
      6. Later, exact positions for far members: a server message with each party member's position every 2 s
         (protocol change); the public server's status push already sends x, y and heading.
      Decided by Emma (Decisions, "Players on the map: decided"), which adds to the steps above:
      - Other players (not in the party): a world-map icon each, not on the compass. Records: a pool of persistent
        map-marker references (32, a public server's size; one icon type unused by vanilla locations), named and
        moved like the party markers (step 2) and made visible on the map; the pool slot freed when the player
        leaves. To check in the rig first: a moved map marker redraws at its new place, and whether the game puts it
        on the compass when near (if so, it is hidden while that player's body is loaded, like a party member).
      - A party member whose body is loaded here: objective hidden (instead of the marker following the copy), shown
        again when the copy unloads.
      - Text "Seen, near Whiterun": the place is the name of the location of the member's cell, read in our game
        from the cell `NotifyPlayerCellChanged` gives (no protocol change); a cell with no named location gives the
        name alone.
- [ ] **Proximity voice chat** (Emma, 2026-10-10, for the public server; "I would rather a test run with our own
      system and if it fails, then fall back to mumble"; no extra install for players). Step 0 passed, so no Mumble:
      0. Feasibility, about an hour, before anything else: the game's `steam_api64.dll` (F:\SteamLibrary\steamapps\
         common\SkyrimVR, 2.89.45.4, interface `SteamUser018`) exports StartVoiceRecording, GetVoice,
         DecompressVoice and GetVoiceOptimalSampleRate (flat API with the interface pointer first, from
         `SteamClient()->GetISteamUser(SteamAPI_GetHSteamUser(), SteamAPI_GetHSteamPipe(), "SteamUser018")`). A
         client test build records, logs the bytes GetVoice returns each second, and plays your own voice back to you
         after a second (a loop through DecompressVoice and XAudio2). Passes if: speech gives data, silence gives
         none (Steam's own voice detection, so no push-to-talk, nothing on a keyboard), the playback is clear.
         Needs Emma for two minutes with the headset's microphone. Fails: Mumble (step 8).
         [x] Passed 2026-10-10 13:08, Emma: "heard my voice perfectly". Log: 0 bytes in the silent first second,
         then 751, 4648, 4638, 4506 bytes a second while talking; 64 packets, 16609 bytes, 0 failed to decode at
         24000 Hz; logs\voice_test.bin kept for the bot (step 5). Her first tries looked dead because the game hides
         HUD messages while a menu is open: the test now also writes each step ("recording now, speak", "recording
         stopped", "playing it back", "Steam heard nothing...") into the menu's chat box, and logs GetVoice's answers
         per second. That change is written, not yet built (she was playing). For step 3: decoding the whole test at
         once cost one 37 ms frame; live voice decodes a packet at a time.
      1. Capture: `VoiceService` (client) records while connected and voice is on, reads GetVoice every frame and
         sends what it returns. Steam's compressed voice is small: measured 4.5 to 4.7 KB a second while talking, none in silence.
      Steps 1 to 5 built 2026-10-10 afternoon: `VoiceDataRequest`, `VoiceStateRequest`, `NotifyVoiceData` (unreliable,
      a 16 KB send path on both sides instead of the 1 MB one); server `VoiceService` (heard within 2400 units, about
      34 m, or by the party at any distance; only between players with voice on; 60 pieces a second at most per
      speaker); client `VoiceService` (capture while on and connected, decode per piece, 80 ms gathered after a
      silence, 400 ms most queued, volume full to 5 m and silent at 30 m, left-right from the headset, a far party
      member as a centred radio at 0.6); menu: Voice on/off (off by default), voice volume, Mute per player in the
      player list, kept by name. Bot pair `voice` (run-all-pairs -Pairs voice): 40 pieces each of near in the party,
      far in the party, far out of the party, near out of the party: 118 of 120 heard, none from the far one out of
      the party: PASS. Not yet heard by a person: Emma turns Voice on and a bot speaks her recording (`speak
      <path to voice_test.bin>`), then one session with Seen.
      2. Transport: a new `VoiceData` request (sequence number, bytes) sent unreliable (`kUnreliable`, which
         TiltedConnect has and nothing of ours uses yet; a lost packet is a skipped 20 ms, never a delay), and
         `NotifyVoiceData` (player id, sequence, bytes) from the server only to the players within range of the
         speaker, so a voice never reaches anyone who could not hear it. Protocol change: everyone updates. Goes
         through the relay like everything else.
      3. Playback: per speaker, DecompressVoice to 16-bit PCM, about 80 ms of buffer to absorb network jitter, one
         XAudio2 voice each, placed with X3DAudio: the listener is our headset, the source the speaker copy's head.
         Full volume up to about 5 m, silent by about 30 m (to tune in play). Everyone in range hears you (Emma's
         choice), party or not. Party channel (Emma: yes): the server also forwards a speaker to their party members at
         any distance; a member whose body is not loaded here is played without position, a little quieter, like a
         radio; when the body is loaded, proximity takes over.
      4. Controls without a keyboard: in the urSovngarde menu on the VR dashboard: voice on or off, mute a player,
         volume. Off until each player turns it on (Emma's choice): no microphone is opened before that, and the
         choice is remembered. Mute is local and remembered.
      5. Tests: the rig has no voice, so the feasibility test also saves a few seconds of Emma's compressed voice to
         a file; the bot replays it as a player's voice (a new bot command), and the rig checks it arrives and is
         placed (log: volume and direction per speaker as the bot walks away). Then one session with Seen.
      6. Privacy: voice passes through the server and the relay and is never stored; the privacy page (hub
         `LAUNCHER.md` section 9 and the website) says so before it ships.
      7. Cost on a public server: the server forwards each speaker to those in range; 10 people talking near each
         other is about 30 KB a second per listener at most.
      8. Fallback, only if step 0 fails: Mumble positional audio, the client writing our position and head to
         MumbleLink each frame (no network work), the launcher installing Mumble and joining the server's channel;
         needs a Mumble server for the public server (a small VPS, UDP; Cloudflare cannot host it).
