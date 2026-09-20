# Hosting a Skyrim Together VR server

The host installs the game side like everyone else (see the other README), plus this.

1. Keep the `Server` folder anywhere on the PC that will host. It does not need the game.
2. Run `host-server.bat` before playing and leave the window open. It prints the port: UDP 10578.
3. Let the others reach it, one of these:
   - Forward UDP 10578 in your router to this PC and give the others your public address.
   - Or put everyone on the same virtual LAN (Radmin VPN, ZeroTier, Tailscale) and give them that address.
4. Optional: a password. Open `Server\config\STServer.ini`, set `sPassword=` to something, and tell the
   others; they type it in `setup-connect.bat`.
5. You connect to your own server with `127.0.0.1:10578`.

Everyone must run the same build as the server, or the server refuses them at connect.

Updating: when a new build says the server changed, close the server, replace the three files
`SkyrimTogetherServer.exe`, `SkyrimTogetherServer.exe.manifest` and `STServer.dll`, start it again.
The others replace their client exe and pdb.

The Linux build of the server exists for people who host on a Linux box; ask for it.
