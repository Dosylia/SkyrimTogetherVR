# Checklist for a build

The minimum that proves a build before it goes to the others, and what a release adds. Nothing here needs a
headset except the last step of a session. Rig and game steps only when nobody is playing.

## Every build

1. **Build the four together:** `xmake build SkyrimImmersiveLauncherVR`, `SkyrimServerRunner`, `TPProcess` and
   `STBot`. `deploy-client.ps1` refuses a client whose server or bot has another version.
2. **The test for what changed:** the bot pair or script that covers it (`run-bot-tests.ps1 -Pair <name>` or
   `-Script <file>`; a new script goes in `Code/bot/scripts`, the runner copies it in). A change the bot cannot see
   (bodies, hands, menus): one live check with the game (`live-check.py <script>`, game up through
   `headless.ps1`), not a long rig session.
3. **The whole suite** (`run-all-pairs.ps1`, about 20 minutes) only after several substantial changes.
4. **Deploy:** `deploy-client.ps1` (sets the old exe aside, re-points MO2), and the server's two files into the
   server folder when the server changed.
5. **It starts:** the next start reaches a loaded save; the log's first lines say
   `Skyrim Together client, build <the new version>`.

## A release

1. A clean, committed tree (`make-release.ps1` refuses "dirty"), then `make-release.ps1`.
2. `scan-release.ps1` (VirusTotal; key in `%USERPROFILE%\.str-vt-key`); a flag is reported before publishing.
3. The same three zips on GitHub and Nexus, with the checksums and links `scan-release.ps1` wrote.
4. If the network messages changed (`Code/encoding`), say so: everyone updates, the server too.

## After a session together

`collect-logs.bat` on each side, then read for: crash logs (ask whether it really crashed, and when: a crash while
quitting is the quit crash), `EpochMiss`, `ActionFlood`, `Defender's rule`, `Weather:` and whatever the change
being watched logs.
