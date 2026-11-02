# Netplay — design (2026-10-30)

Input lockstep over TCP, host = player 1, guest = player 2. Feasibility was
measured first (see CLAUDE.md, "Determinism experiment"): with the
deterministic clock, game state stays byte-identical across runs given the
same inputs; only audio, profiler and OS memory differ.

## Connection
- The host forwards one TCP port (default 7311) on their router; the guest
  connects to the host's public IP. No relay server.
- Started with `PSR_NETPLAY=host:<port>` or `PSR_NETPLAY=join:<address>:<port>`
  (launcher UI later). `PSR_NETPLAY_DELAY=<reads>` sets the input delay.
- TCP_NODELAY. Messages are `{u8 type, u8[3] pad, u32 length}` + payload.

## Handshake (HELLO, both directions; refuse on any mismatch)
- Magic + protocol version, git commit of the build, input delay.
- Hash of the Stadium save (`saves/pokestadium.us.1.0.bin`). Slice 1 requires
  equal saves; slice 2 has the host send its save and the guest play on a
  session copy that is never written back (as the 3DS port does).
- Slice 1: no Transfer Pak carts on either side (also as the 3DS port).
- Netplay forces `RECOMP_DETERMINISTIC_CLOCK=1` on both sides.

## Lockstep
- The game reads controllers on its game thread at fixed points in its
  logic; read number N of a controller is the same moment on both machines.
- Each side owns one port (host 0, guest 1) and feeds it from its own local
  controller (whatever its launcher assigns to player 1). At local read N the
  input is queued for read N + delay and sent. The first `delay` reads are
  neutral.
- Reading the other side's port at read N waits until its input for N has
  arrived. Ports 2 and 3 are absent on both sides; ports 0 and 1 are present.

## Desync detection
- Every 120 reads of port 0, both sides hash the game heap
  (0x80100000..0x80800000; audio, profiler and OS state live below it) and
  exchange the hash. Three mismatches in a row stop the session with a
  message. A single mismatch can be a hash taken mid-write by another thread.

## Status
- Slice 1 (connection, handshake, lockstep, desync check) works over
  localhost (2026-10-30): a 2-player rental battle, 4,850 controller reads,
  stayed in sync; the desync check never fired. Test with
  `tools\netplay_probe.ps1 netplay` (host on gamepad, guest on keyboard; the
  host records both players' input to `a\netplay-input.bin`).
- Slice 2a (protocol 2): the host sends its Stadium save after the
  handshake, and the guest plays on it from `saves\netplay-session\` via the
  runtime's `RECOMP_SAVE_DIR` override (runtime patch 0004), never touching
  its own save. Saves no longer have to match. A goodbye message lets the
  other side say "the other player left". The host shows a "Hosting on port"
  message while it waits.
- Release zip (`_package_release.ps1`) includes `Netplay - Host.bat`,
  `Netplay - Join.bat` and `NETPLAY.txt` from `tools\release\`.

## Later slices
1. Stadium save transfer (host → guest session copy).
2. Cart exchange: P1 = host's cart, P2 = guest's cart on both machines. Swap
   32 KB saves only and compare GB ROM hashes; never send ROMs. Each side
   writes back only its own cart (the Gen1Recomp bridge already merges per
   port); the other cart is a session copy.
3. Launcher UI: Host / Join, address, port, delay, status.
4. Disconnect handling and a clear "desynced" screen.
