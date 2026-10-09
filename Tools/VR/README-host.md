# Hosting an urSovngarde server

With the urSovngarde launcher, Host a game does all of this and gives you a code for your friends; with launcher
0.3.0 or newer on both sides it goes through our relay, so no port needs to be open. By hand, the host installs the
game side like everyone else (see the other README), plus this.

1. Keep the `Server` folder anywhere on the PC that will host. It does not need the game.
2. Run `host-server.bat` before playing and leave the window open. It prints the port: UDP 10578.
3. Let the others reach it, one of these:
   - Forward UDP 10578 in your router to this PC and give the others your public address.
   - Or put everyone on the same virtual LAN (Radmin VPN, ZeroTier, Tailscale) and give them that address.
4. Optional: a password. Open `Server\config\STServer.ini`, set `sPassword=` to something, and tell the
   others; they type it in `setup-connect.bat`.
5. You connect to your own server with `127.0.0.1:10578`.

Builds that exchange the same network messages connect to each other, whatever their version. When a release
changes them it says so; then everyone updates, the server included, or the server refuses them at connect and
tells both versions.

Updating: when a new build says the server changed, close the server window and drag the server update zip onto
`update.bat` in the mod's folder (it finds a `Server` or `urSovngarde Server` folder next to it; otherwise run
`update.bat` from a copy placed inside the server's folder). By hand: replace `urSovngardeServer.exe`
and `STServer.dll`, then start the server again. The others update their client.

The server program was `SkyrimTogetherServer.exe` before the rename; `host-server.bat` starts either.

A Linux build of the server is being fixed; ask if you need one.
