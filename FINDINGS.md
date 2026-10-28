# Battlefront (2004) LAN over Tailscale: findings and how-to

Technical write-up of how LAN play for **Star Wars Battlefront (2004)** was made to work over Tailscale. It is also meant as a handoff for doing the same with **Star Wars Battlefront II (2005)**.
Everything below was verified on **Battlefront (2004), GOG build 1.3.7.4** on 2026-10-28, unless it is marked *unverified*.
BF2 uses the same Pandemic "Zero" engine and has a similar re-release, so much of it should transfer. All addresses, byte patterns and offsets are BF1-specific and must be found again for BF2.

## Goal and result

Goal: play LAN multiplayer between machines that are not on the same broadcast domain. The test setup was a dedicated server in a Windows 10 VM and a remote client, connected only by Tailscale. The constraints: no GOG Galaxy or GameSpy, and no changes to the game's files on disk.

Result: working. The solution is a `dinput8.dll` proxy placed in `GameData\`. It hooks Winsock via the executable's IAT at runtime, and the same DLL is used on both sides, configured by an ini file.

## Repository layout

| Path | What |
|---|---|
| `src/dinput8_proxy.cpp` | Proxy source, the complete implementation |
| `src/dinput8_proxy.def` | Exports `DirectInput8Create` only |
| `build.bat` | MSVC x86 build |
| `examples/` | Server and client ini files, and the dedicated-server batch file |
| `ghidra_scripts/` | Headless Ghidra helpers used for the analysis |

Working conventions that paid off:
- Work only in a **copy** of the game install, never in the original folder.
- Back up the exe before anything else, and keep a dated notes file.

## Build

MSVC Build Tools 2022, x86. The game is a 32-bit process. Run `build.bat`, or:

```powershell
$vc="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
$d="<repo>\src"
cmd /c "`"$vc`" >nul 2>nul && cd /d `"$d`" && cl /nologo /O2 /MT /EHsc /LD dinput8_proxy.cpp /Fo:`"$d\\`" /Fe:dinput8.dll /link /DEF:dinput8_proxy.def ws2_32.lib kernel32.lib"
```

Pass `/Fo` explicitly. Otherwise the `.obj` lands in the current directory, which could be the game's install folder.

## Ghidra headless

Ghidra 12.2 needs JDK 25+. Import and analyse the exe once, then run the scripts read-only with `-noanalysis`:

```powershell
$env:JAVA_HOME="<path to JDK 25>"
& "<ghidra>\support\analyzeHeadless.bat" <projdir> <name> -import <path>\Battlefront.exe
& "<ghidra>\support\analyzeHeadless.bat" <projdir> <name> -process Battlefront.exe -noanalysis -readOnly -scriptPath <repo>\ghidra_scripts -postScript DecompileAt.java 0x005aeb21
```

- `DecompileAt.java <addr>... [nocallers]` decompiles the function containing each address, plus its direct callers.
- `RefsTo.java <addr>...` lists every reference to a global or function and decompiles the referencing functions.

## Architecture of the game's LAN networking (BF1, verified)

- **Command line:** `/autonet dedicated|client`, `/lan`, and `/gamename <name>`. `/lan` sets a flag that **skips GOG Galaxy init**. Without it the dedicated server uses Galaxy online mode and **never opens UDP 3657**.
- **Sockets** (globals in `.data`):
  - `DAT_00940688`: UDP socket `connect()`ed to `255.255.255.255:3657`, with `SO_BROADCAST`. It sends discovery probes from the client, and replies from the server, via `send()`.
  - `DAT_0094068c`: UDP socket bound to `0.0.0.0:3657`, which receives discovery.
  - `DAT_00940690`: UDP socket bound to `0.0.0.0:3658`, used for all session and join traffic via `sendto` and `recvfrom`.
  - Creation: `FUN_005E8420` (3657 pair) and `FUN_005E85E0` (3658). Close: `FUN_005E8320` (3658, then sets the global to 0). Send dispatcher: `FUN_005E8F00`. Receive pump: `FUN_005E8AA0`.
- **Discovery protocol:**
  - Probe, 13 bytes: `00 ff ff 00 00 | 4-byte per-search nonce | 04 10 00 00`. Sent every ~2 s.
  - Reply, 41 bytes: starts `01 ff ff 00 00` and echoes the nonce. The server broadcasts it.
  - **The nonce varies.** A first matcher compared all 13 bytes from one capture and silently never matched.
- **Join handshake on 3658:** client → server 17 bytes (`02 ff ff ...`), server → client 49 bytes (`03 ff ff ...`), then the client sends 69-byte packets.
- **Advertised addresses:** `gethostname` + `gethostbyname`, filtered by `FUN_005B0F00`, which accepts **only RFC 1918** addresses.
- **Address assertion:** `FUN_005AEAF0`, the net-address constructor, calls `FUN_005B0F00` and executes **`int3`** when an address is not RFC 1918. The client crashes with exception `0x80000003` as soon as it sees a reply from a Tailscale `100.x` address.

## The five problems and their fixes

1. **The server didn't listen.** Fix: add `/lan` to the dedicated command line.
2. **Tailscale doesn't carry broadcast.** The client's probes to `255.255.255.255:3657` never leave the LAN.
   - Client hook: `connect()` / `sendto()` to `255.255.255.255:3657` are rewritten to `Server=` from the ini.
   - Server hook: `recvfrom()` records the source of each discovery probe for 15 s. A discovery reply going to broadcast is then sent by unicast to every recent prober: `connect()` to each, then `send()`, or `sendto()`.
3. **The game rejects non-RFC 1918 addresses** with the `int3` above. Fix: a bijective alias in the hooks, `100.A.B.C` ↔ `10.A.B.C`, which maps 100.64.0.0/10 onto 10.64.0.0/10.
   - `recvfrom` rewrites the source 100.x → 10.x before the game sees it.
   - `connect` / `sendto` rewrite the destination 10.64/10 → 100.x.
   - The game never sees a Tailscale address. A server at `100.A.B.C` shows up in the browser as `10.A.B.C`.
4. **Game bug, probably also the cause of intermittent ~60 s join timeouts on a real LAN.**
   - The client's server browser keeps stopping and restarting its search. Each stop (`FUN_005AF900`, `FUN_005AF970`, `FUN_005AF9F0`) calls `FUN_005E8320`, which closes the 3658 socket, zeroes `DAT_00940690`, and clears the "open" flag `DAT_008C19E4`.
   - The join's start, `FUN_005B23D0`, opens the socket only if that flag is 0. A stop that runs *after* the join's start leaves the socket at 0.
   - The client then sends every 69-byte join packet with `sendto(socket=0)`, gets `WSAENOTSOCK` (10038), and times out after ~60 s.
   - Fix (client mode only): in the `sendto` hook, when the socket is 0 and the game global is 0, call the game's own `FUN_005E85E0(0)` to reopen the socket. Write the returned port into `DAT_008C19E4` and retry on the new socket.
   - Guard this with a byte-signature check of `FUN_005E85E0`, so the fix is a no-op on any other build.
5. **The proxy was never active.** Two separate causes:
   - It initialised in `DirectInput8Create`. A `/norender` dedicated server **never calls `DirectInput8Create`**, and a client may create sockets before it does. Fix: initialise in `DllMain(DLL_PROCESS_ATTACH)`. The exe's IAT is already bound by then, and patching it there is fine.
   - `Battlefront.exe` imports **WS2_32 by ordinal**: connect=4, recvfrom=17, send=19, sendto=20. Name-based IAT matching found nothing. Fix: match IAT slots by comparing their value with `GetProcAddress(GetModuleHandle("ws2_32.dll"), "connect")` and so on.

## Pitfalls to remember

- **ASLR:** the exe has relocations and was observed loaded at `0x005F0000`, not `0x00400000`. Compute every game address as `GetModuleHandle(NULL) + (ghidraVA - 0x400000)`. Absolute operands inside code are relocated too: a `mov [DAT_00940690], eax` reads `a3 90 06 b3 00` at runtime. Signature checks must compare relocated operands, not file bytes. A first signature check failed for exactly this reason.
- **`ws2_32.dll` is a KnownDLL,** so a local `ws2_32.dll` proxy is ignored. `dinput8.dll` is the non-KnownDLL that the exe imports, with only `DirectInput8Create`. For BF2, check its import table for a suitable non-KnownDLL import: `dinput8`, `binkw32`, `eax`, `unicows`, or a `version.dll`-style candidate.
- **Log everything to a file next to the exe.** Each wrong assumption above was only found because the log showed it: hooks=0, imports by ordinal, no probes matched, `int3` right after the reply, sendto on socket 0, the relocated operand. The useful log lines were:
  - mode and hook counts
  - every redirected connect
  - probes and replies with addresses
  - up to 50 other datagrams from 100.64/10, with a hex prefix
  - translated sendtos with result and WSA error
  - the signature check with the module base and the bytes found
- **Windows crash events** give a quick crash location: `Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000}` gives the exception code and the faulting offset (offset + 0x400000 = Ghidra VA).
- **Firewall:** Windows may class the Tailscale adapter as Public. A blanket rule avoids that question: `New-NetFirewallRule -DisplayName "Battlefront UDP (Tailscale)" -Direction Inbound -Protocol UDP -LocalPort 3657,3658 -Action Allow -Profile Any`.
- **Tailscale topology:** Tailscale must run *on the machine or VM that runs the server*. With Tailscale only on a host acting as a subnet router, the VM cannot reply to the client's 100.x address.
- **pktmon** needs an elevated shell. A reference capture of a working real-LAN join was invaluable for working out the protocol.

## Deployment (BF1, working)

- **Both sides:** put `GameData\dinput8.dll` (SHA-256 `A55D9E0B3CEBB2F7C24B060F0998ADC1889C359E5FFBDC100FD67E370C951921`) and `GameData\BattlefrontDirectIp.ini` in place.
- **Server:**
  ```ini
  [BattlefrontDirectIp]
  Mode=server
  ```
  Launch with:
  ```
  battlefront.exe /win /norender /autonet dedicated /lan /resolution 320 240 /nosound /playercount 1 /bots 32 /netplayers 32 /sideselect /gamename MyLANServer tat2i 100 100
  ```
- **Client:**
  ```ini
  [BattlefrontDirectIp]
  Mode=client
  Server=<server's Tailscale IPv4, 100.x.y.z>
  ```
  Use the normal LAN browser.
- **Log:** `GameData\BattlefrontDirectIp.log`, rewritten on each launch and capped at 4 MB.

## Dedicated-server options (BF1, from parser `FUN_004B4190` and .data defaults)

| Switch | Global | Default | Meaning |
|---|---|---|---|
| `/playercount N` | `DAT_0083A5E8` | 2 | Players required before the match starts. The server returns early while `players < N`. |
| `/playerlimit N` | `DAT_008396E4` (+2 copies) | 50 | Maximum players. |
| `/bots N` | `DAT_0083A57C` | 16 | AI bot count. Sent over the network as 8 bits. The game itself sets 32 in one mode. |
| `/sideselect` | `DAT_0083A56F` := 0 | 1 | That global is the "auto-assign teams" flag. `/sideselect` turns auto-assign off, so players choose or switch sides. |
| `/skill N` | `DAT_0083A840` | – | Clamped to 1..3. |
| `/throttle N` | `DAT_00933A10` | 0 | Server upload budget in KB/s (`N<<10`). Takes priority over `/netplayers`. |
| `/netplayers N` | `DAT_00933A14` | 0 | Upload budget = N × 40 KB/s (`FUN_005B5BC0`). Menu hosting copies it from the connection-type table at `0x008396F4` = {3,6,9,12,19,32}, where LAN = 32. A command-line dedicated server probably leaves it at 0, so pass `/netplayers 32`. |
| `/tps N` | `DAT_00936DEC` | 0 → 60 | Tick rate, clamped 15..120 (`FUN_004706E0`). If below 15, the profile byte `+0x31B` is used, or 60. Applied by `FUN_005AF830` → `FUN_005AA080(rate,1)` and sent to clients in session info (`FUN_005C1C80`). |
| `/bandwidth` `/latency` `/loss` | `DAT_008C4A88`=1 | – | These look like the network **simulator** (debug fake lag and loss). Do not use them for tuning. |

**Verified in play (2026-10-28):** with 32 bots over Tailscale (30–60 ms), bots rubberbanded until `/netplayers 32` was added. After that it played smoothly. Always pass an explicit network budget on a command-line dedicated server.

For BF2, find the same parser and read the `.data` defaults in the same way. Translate VA to file offset through the section table, and treat a value as BSS if it lies beyond the section's raw size.

## Suggested plan for Battlefront II (2005)

1. Copy the install and back up the exe. Record the version and hash.
2. **Static survey:** strings (`/lan`, `/autonet`, `dedicated`, `/gamename`, GameSpy and Galaxy names), and the import table. Check whether WS2_32 is imported by ordinal or name, and which non-KnownDLL can host the proxy.
3. Import into Ghidra. Find the `socket` / `bind` / `connect` (broadcast) / `sendto` callers, the LAN command-line parser, and any RFC 1918 filter or `int3` assertion on addresses. Search for references to `FUN_*` that test `0x0a`, `0xac10`/`0xfff0`, or `0xc0a8`.
4. Get a **real-LAN baseline first**: does a plain LAN dedicated server plus client discover and join? Capture it with pktmon if possible.
   - *Unverified:* BF2 likely uses different ports and packet layouts. Find them from the capture and from the bind/connect code, not by assuming 3657/3658.
5. Port the proxy, changing the ini, log and hook names:
   - IAT hooks matched by address
   - init in DllMain
   - broadcast→unicast on both sides
   - the 100.x↔10.x alias
   - logging
6. Test with logs from both sides after every change. Check for the same socket-teardown race during join: it looks like shared engine code, but that is *unverified*.
