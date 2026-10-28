# Star Wars Battlefront (2004) 1.3.7 – LAN play over Tailscale

This lets you play the 2004 Star Wars Battlefront's **LAN multiplayer** with friends over the internet, using [Tailscale](https://tailscale.com). It uses a drop-in `dinput8.dll` and changes no game files.

On a real LAN, the game finds servers by broadcasting, and broadcasts don't cross Tailscale. The proxy turns server discovery into direct, point-to-point packets. It also works around two problems in the game itself: it refuses Tailscale's `100.x` addresses, and its join code has a socket bug.

**Tested:** GOG build 1.3.7.4, with a dedicated server in a Windows 10 VM and a remote Windows 11 client.
**Steam 1.3.7:** untested. The address-alias and discovery parts don't depend on the exact build. The join-bug workaround checks the code bytes of the exe and switches itself off on any build it doesn't recognise. The log tells you which case applies.

## Quick start

1. Install Tailscale on the **server machine** and on every **client**, all on the same tailnet. If the server runs in a VM, install Tailscale inside the VM.
2. Get `dinput8.dll` from the [Releases](../../releases) page, or build it yourself (see below). Copy it into `GameData\` on every machine.
3. **Server:**
   - Copy `examples/server/BattlefrontDirectIp.ini` into `GameData\`.
   - Edit `GAMEDIR` in `examples/server/BF1 dedicated.bat` and run it.
   - `/lan` in that batch file is required.
4. **Each client:**
   - Copy `examples/client/BattlefrontDirectIp.ini` into `GameData\`.
   - Set `Server=` to the server's Tailscale IPv4 address (run `tailscale ip -4` on the server).
5. Start the game on the client and open the LAN server browser. A server at `100.A.B.C` is listed as `10.A.B.C`. This is expected; the proxy translates between the two.

If the server doesn't appear, allow the game's ports on the **server**. Windows often treats the Tailscale adapter as a Public network. In an admin PowerShell:

```powershell
New-NetFirewallRule -DisplayName "Battlefront UDP (Tailscale)" -Direction Inbound -Protocol UDP -LocalPort 3657,3658 -Action Allow -Profile Any
```

## Dedicated-server options

`examples/server/BF1 dedicated.bat` sets these switches:

| Switch | Effect |
|---|---|
| `/lan` | LAN mode. Without it the server uses GOG Galaxy and is invisible to LAN clients. |
| `/playercount 1` | The match starts with 1 player. The default is 2. |
| `/bots 32` | 32 AI bots. The default is 16. |
| `/netplayers 32` | The game's LAN bandwidth profile. **This fixes bot rubberbanding.** A command-line server otherwise runs without it. |
| `/sideselect` | Players choose and switch sides. Without it, teams are auto-assigned. |

[FINDINGS.md](FINDINGS.md) lists more switches, such as `/tps`, `/throttle` and `/playerlimit`, with details.

## Troubleshooting

Each launch writes `GameData\BattlefrontDirectIp.log`. A healthy client log contains:

```
mode=client server=100.x.y.z
hooks: connect=1 recvfrom=1 send=1 sendto=1
proxy active
connect socket=... broadcast:3657 redirected to 100.x.y.z:3657
recvfrom socket=... discovery reply (41 bytes) from 100.x.y.z:...
game socket fix available: ...
game UDP 3658 socket was closed during join; reopened as socket=...
```

A healthy server log shows `discovery probe from 100.x...` followed by `discovery reply ... unicast to ...`.

| Symptom | Likely cause |
|---|---|
| No probes in the server log | Firewall on the server, or `/lan` missing |
| Client crashes when the server appears (`0x80000003`) | The proxy is missing or inactive on the client |
| Join times out after ~60 s with `sendto socket=0 ... error=10038` | The game-socket workaround is unavailable for this exe build (see the `game socket fix` log line) |
| Bots rubberband | `/netplayers 32` missing on the server |

## Build

This needs Visual Studio 2022 Build Tools with the C++ workload. Run `build.bat`; the output is `src\dinput8.dll` (32-bit).

## How it works

[FINDINGS.md](FINDINGS.md) has the full reverse-engineering notes:
- the game's sockets and discovery protocol
- the RFC 1918 assertion
- the join socket race
- ordinal imports and ASLR pitfalls
- a plan for porting this to Battlefront II (2005)

`ghidra_scripts/` contains the headless Ghidra helpers used for the analysis.

## Disclaimer

This is an unofficial fan project, not affiliated with Lucasfilm, Disney, Pandemic, Aspyr, GOG or Valve. It modifies the running game process in memory to change its networking behaviour. Use it at your own risk, and only with games you own.
