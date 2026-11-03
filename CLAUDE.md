# Pokémon Stadium Android Port (fork of SS Anne)

## Goal
Play Pokémon Stadium (US v1.0) natively on Android, primarily on an AYN Thor (Snapdragon 8 Gen 2, dual screen), with full Transfer Pak support: read AND write back Game Boy saves so Pokémon and items can be stored and managed. GB Tower is NOT required.

Keep the Windows build working alongside Android. One codebase, two targets.

## Base project
- Fork of SS Anne / PokemonStadiumRecomp by mstan: https://github.com/mstan/PokemonStadiumRecomp
  - Built on N64Recomp, N64ModernRuntime, RT64 (renderer), SDL2.
  - Already has Linux build and a Vulkan path. Transfer Pak works on PC.
  - Upstream is no longer maintained (Aug 2026). Author says forks are welcome; he believes N64Recomp's architecture is structurally unsound. Expect some issues to be toolchain fights, not simple bugs.
  - Known open upstream issues (see ISSUES.md): #13 Free Battle rental roster detail view crash (reproducible), #10 residual announcer audio static.

## Ground rules
- GPL: keep the project open source, keep upstream licenses, credit SS Anne / mstan, pret, and any reference projects.
- Never commit or distribute ROMs, BIOS, or extracted game assets. User supplies their own Stadium ROM (MD5 ed1378bc12115f71209a77844965ba50) and their own GB ROMs and saves.
- Don't send AI-written PRs upstream to pret or others unless asked. Support questions stay in this project.
- All QoL / enhancements are options. Defaults play like the original.
- The unofficial 3DS Stadium port is derived from GPL code (SS Anne), so its source is GPL too. It is distributed as binaries only. If its source becomes available (the user may ask the author; GPL entitles binary recipients to it), reuse it freely with credit. Until then, use the findings below; don't sink time into disassembling his binary. Never use the ROMs bundled with it.

## Reference: unofficial "Pokémon Stadium for New 3DS" port (findings, Oct 2026)
Inspected from its readme, patch notes and text strings only (no ROMs touched). Author: @dave_schobert. Binary-only, v1.1.
- **Base:** SS Anne. Build paths are `PokemonStadiumRecomp/platform/3ds/...`, i.e. upstream recomp plus a new `platform/3ds` backend. AI-assisted (built from a "Codex" folder).
- **Proves the recomp runs on ARM** (3DS ARM11, 32-bit). New 3DS only: recomp code is ~62 MB resident; Old 3DS prototype ran ~8 fps.
- **Renderer:** replaced RT64 with a custom HLE F3DEX2 (GBI) translator to the 3DS PICA GPU; audio via NDSP. This sidesteps RT64's x86-only RSP simulator. Our Android plan keeps RT64, so we still need the ARM64 fix.
- **ROM streaming:** ROM read from SD in blocks with an LRU-style cache plus learned prefetch lists (rom-prefetch.bin, rom-scenes.bin). Not needed on Thor's RAM, but note it.
- **Fragments:** custom fragment registration logic, including a special case for fragment 239 ("model slot holds fragment id"). SS Anne's #13 rental crash is "fragment eviction", so this area is likely where a fix lives. Unconfirmed; investigate fragment/model-slot handling first when tackling #13.
- **GB Tower:** works, via a reimplementation of the gbTowerMain / gbTowerColorMain RSP microcode.
- **Transfer Pak write-back design (good model to follow):**
  - Finds 3DS Virtual Console saves by title ID; validates it's a Gen 1 Pokémon save before using it.
  - Writes changes to an SD copy and back into the VC save archive.
  - Checks the game's own save checksum before writing; if cart RAM fails, does NOT write and keeps the last good copy.
  - Keeps a backups folder; writes on a background save thread.
  - Requires the matching GB ROM alongside the save (faithful cartridge emulation).
  - Accepts non-English R/B/Y cartridges (USA Stadium only recognizes English titles by default; he patched that).
- **Other features he added:** EN/ES/FR language support from other-region ROMs, screen modes (4:3 / full / 3D), "Smooth" mode, GB screen movable to the bottom screen, session log + bug dump folder, local wireless 2-player (host sends its save to guest for the session).
- **Bugs he reports fixing** (worth checking in our build): freeze when a Pokémon evolves after a trade; Pokémon duplicated between two saves after a trade; sound cutting out / 0 fps in Gym Leader Castle after a corrupted sound command; Recover rainbow effect; some reflective surfaces.

## Milestones

### Phase 1: parity with the 3DS port, on Thor
1. Build and run the existing Windows version from the user's ROM. Confirm toolchain.
2. Android build. Use Goemon64Recomp-Android as the template: https://github.com/ogdanimal/Goemon64Recomp-Android
   - Main known hurdle: CMake forces `-march=nehalem` for RT64's RSP simulator (x86 SSE). Needs an ARM64 path (check how Goemon Android handled it; SSE-to-NEON shims like sse2neon may apply).
   - RT64 on Android Vulkan (Adreno 740).
3. Plays properly: battles, cups, menus. Controls mapped to Thor buttons.
4. Transfer Pak on Android with standard 32 KB Gen 1 .sav + matching GB ROM. Reads the save AND writes changes back to the .sav reliably (the thing the Mupen64Plus-based Android emulators fail at).

### Phase 1.5: bug fixes
- #13 rental roster crash (debug on PC first; repo has trace/diag build scripts). Fixed 2026-10-30 (runtime patch 0003).
- #10 announcer audio static. Fixed 2026-11-02 (runtime patch 0006): audio RSP tasks now run when submitted instead of later on the SP task thread, where game threads had already changed the memory they read.

### Phase 2: enhancements (all optional toggles)
- Thor second screen: live battle info on the bottom screen, read from game memory (active Pokémon's moves, type, PP, power, accuracy; opponent info). Removes the need to hold a button to read moves. Use pret's Stadium decomp to locate battle structs: https://github.com/pret/pokestadium
- Reference for Thor second-screen plumbing: ThorScreen Android plugin in PMD-faith (GPLv3): https://github.com/ch4rizard-x/PMD-faith
- UI scaling option for the battle menu / move list (2D overlay elements; watch for layout overlap).
- Options and QoL in the existing recomp-ui launcher rather than bolted on.

### Phase 3: Gen1Recomp save support
- Read Gen1Recomp save slots directly (Lua data files, saves/<version>/slotN.lua) by converting to a Gen 1 cartridge save in memory, paired with a vanilla Red/Blue/Yellow ROM for the cart header. Write changes back on exit.
- Gen1Recomp: https://github.com/bryanthaboi/gen1recomp . Check its license before reusing its .sav export logic. Pin to a known version; its format may change.
- Note: an older Gen1Recomp version (0.1.54) had exported .sav files load into glitch city on real hardware (issue #691). Always test with backups.

### Backlog (added 2026-10-30)
- Graphics options: widescreen at minimum (RT64 supports it; Stadium's 2D menus/HUD need edge anchoring). An option in the launcher's Settings; default stays 4:3.
  - Done (2026-11-02): Settings > Aspect ratio "Widescreen battles" (`aspect_ratio=widescreen`, `src/main/widescreen.cpp`). RT64 Expand only while the battle engine is loaded (`gFragments[0x33]`, fragment62 at 0x84300000; fragment ids are `((TEXT_START & 0x0FF00000) >> 20) - 0x10`, not the file number). Menus stay 4:3: in Expand they flicker at the sides, fades only cover the middle, and menu models tear.
  - Open: battle fades still cover only the 4:3 middle. Stretching full-screen orthographic draws in RT64 also stretched HUD panels and broke the fade's blend, so it was reverted. A real fix probably needs the fade's display list found and given RT64 extended-GBI rect alignment.
- Easy install for non-developers: a downloadable Windows release (exe + DLLs + gen1recomp\ support files) on the fork's Releases page. Players only pick their own ROM in the launcher, as with other recomps; nobody else should need to build. Ideally built by GitHub Actions on a version tag, but recompiling needs the ROM's code, and ROMs must never enter the repo or CI logs. Work out how; the fallback is building locally and uploading the zip.

### Later / maybe
- Stadium 2 (separate recomp of a different game; much of the platform work would carry over). Check state of any Stadium 2 decomp first. The 3DS port's author (@dave_schobert) said Stadium 2 is his next port; watch it for findings. The same GPL source rules apply if he ships binaries.
- Netplay. Large; only after everything else is solid. Intended design:
  - Each player's own cart appears as the other's controller 2 (P1 = host's cart, P2 = guest's cart on both machines).
  - On connect, exchange 32 KB GB saves only. Never send ROMs; each side supplies its own GB ROM and they compare a hash to confirm a match.
  - Only the local player's cart writes back to their save; the opponent's save is a session-only copy (the 3DS port's local wireless uses a similar host-sends-save idea).
  - Delay-based lockstep input sync should be enough: Stadium battles are turn-based and menu-driven, so rollback is not needed.
  - Main risk: determinism of the recomp (timing, threads, audio, RNG). Build a per-frame state hash / desync check early.
  - From the 3DS port's patch notes (v1.1, local wireless beta): host = P1, guest = P2. On pairing the host sends its Stadium save to the guest, which plays the session on that copy (its own save untouched). Both must have the same version, language and even screen setting (4:3/full/3D), so it's almost certainly input lockstep, and settings that change game memory must match. No Game Boy carts allowed in a session; we want carts, so that part is new ground. Takeaways: the guest uses a session copy of the host's Stadium (FlashRAM) save, and the handshake compares versions and memory-affecting settings (including our planned widescreen).
  - Determinism findings (2026-10-30): the RNG is libultra `guRandom` (fixed seed), but the game mixes the clock into randomness and logic: `(guRandom() + osGetCount()) / 10 % n` (fragment5, fragment16), `osGetCount() & 0xF/0xFF` (2C1C0, fragment17, fragment36), `osGetTime()` parity/modulo (3D140, 4CBC0), fragment35 animation timing. The runtime serves `osGetTime` from the host clock (`timer.cpp`), so netplay needs a deterministic clock derived from frame count.
  - **Determinism experiment (2026-10-30): positive.** With `RECOMP_DETERMINISTIC_CLOCK=1` (runtime patch 0002: per-guest-thread counters advancing a fixed step per `osGetCount`/`osGetTime` read), one live recording and two replays of the same input (`src/main/netplay_probe.*`, `tools/netplay_probe.ps1`) were compared over a ~3,400-frame session through menus, rental selection and battle turns. RDRAM dumps at frame 2,500 (mid-battle) had **zero differing heap bytes**. All 77 KB of differences were in fixed memory: the audio heap (`373A0.c` `D_800B32A0[0x493E0]`, 99%), other audio (`38BB0`, `45720`, `unk_bss_12/17`), profiler timings (`profiler.c`, `5580.c`) and small OS/thread state. So a desync check should hash the heap and game globals but exclude audio, profiler and OS state. Open risks: whether battle flow ever waits on audio (e.g. the announcer finishing), which could shift frames between machines (not seen here), and that two machines will differ more than two instances on one PC.
  - Two instances run fine side by side on one PC if each has its own folder (launcher.cfg, Stadium save, saves\, logs and the debug port all live next to the exe).

## Windows build (working as of 2026-10-30)
Toolchain: VS 2022 Build Tools (C++ workload + "C++ Clang tools for Windows"; CMake and Ninja come bundled), Python 3, Docker Desktop.
1. `setup.bat` (clones `n64recomp\`), with the v1.0 ROM copied to `baserom.z64` first.
2. `powershell -ExecutionPolicy Bypass -File tools\setup_libs.ps1`. It provisions the gitignored `lib\` at the v0.4.5-beta release refs (runtime `d2aed7e`, rt64 `b58c2f7`, engine `2b949c5` in both engine checkouts) and sets the disasm checkout to LF endings.
3. Build pret's ELF in Docker: `docker run --rm -v "%CD%:/src" -w /src/disasm debian:bookworm sh /src/tools/docker_disasm.sh`. The rebuilt ROM must match ed1378bc.
4. Build the generator: the `N64RecompCLI` target in `n64recomp\` (clang-cl + Ninja, Release), then run `n64recomp\build\N64Recomp.exe game.toml` from the repo root, which writes `generated\`. The RSP microcode in `rsp\` is committed, so RSPRecomp isn't needed.
5. Run `build_ssanne.bat`, which produces `build\PokemonStadiumRecomp.exe`.

Gotchas:
- The ares submodule inside N64Recomp points at an unpublished commit. Never use `--recursive` across it; fetch with `--no-recurse-submodules`.
- PSR `main` calls `ultramodern::input::apply_stick_deadzone`, which was never published in any runtime fork, so it's implemented locally in `src/main/main.cpp`.
- Runtime fixes live in `patches/N64ModernRuntime/*.patch` and `setup_libs.ps1` applies them. Current one: `thread_queue_remove` on a NULL queue crashed `osDestroyThread` when opening the Pokémon Lab's Game Cart Machine.

## Transfer Pak findings (Windows, 2026-10-30)
- An empty `pN_save` means no save at all: cart RAM is memory-only and lost at exit, even though the launcher shows "Default (runtime chooses)". Fix planned: default to `<rom basename>.sav` next to the ROM.
- `gbcart.cpp` `flush_save()` truncates and rewrites the whole 32 KB `.sav` on every 32-byte block write. There's no backup or checksum check yet (planned, following the 3DS port's design).
- Stadium requires the Red/Blue/Yellow save to have been made inside a Pokémon Center ("wasn't saved at a Pokémon Center"). This is real game behavior.
- A fresh Gen 1 save never formats PC box banks 2 and 3 (pokered only runs `EmptyAllSRAMBoxes` on the first box change), so Stadium reports a "Transfer Pak not set properly" style error. Formatting them fixed the main Stadium paths.
- GB Tower still rejects a save that's valid, saved in a Pokémon Center, with formatted boxes. Its I/O and RAM test pass; it reads all four RAM banks and then returns to the menu. The cause is unknown (recomp bug vs. the real game). Not required; parked.
- GB Tower investigation (2026-11-02), Red (USA, Europe) SGB 1 MB MBC3:
  - It's intermittent, not save corruption: a checksum-repaired save failed, then the same save booted, saved in-game and reloaded fine in one run, then failed again. Upstream v0.4.4+ has the same cart engine (open-bus fix in runtime `gbcart.cpp`).
  - GB Tower copies the save, then preloads ROM banks in a fixed order (1, 2, 19, 28, 4, 6, 16, 31, 30, 3, 26, ...) while the GB ucode starts. In the working run it loaded 42 banks and skipped 42 and 44. In every failing run it loaded all 44 (1-44), then either "The Transfer Pak is not set properly" (thread 0x8007F730 turns the cart off) or a hang at the Nintendo logo. Banks 42 and 44 are almost empty (data to 0x032F and 0x060E, then zeroes); 45-63 are all zero.
  - Not the cause: Red's checksums, SI DMA latency (`N64RECOMP_SI_DMA_US=300` still fails). Starting open bus at a whole bank (bank 44's tail read as zeroes) made it hang at the logo, so that change was reverted.
  - Fixed (runtime patch 0005): the preload is time-limited. With instant cart reads it reached banks 42-44 and broke; per-read logging slowed it enough to stop at 43, which is why logging "fixed" it. A 100 µs busy hold per cart-memory block read (no yield; `PSR_GBCART_HOLD_US`, 0 disables) stops the preload at 43, and Red booted 3/3. Slowing reads with `N64RECOMP_SI_DMA_US` didn't help because that wait lets other guest threads run. Why loading bank 44 breaks the emulator is still unknown; a real fix may be in how the bank cache or open-bus bytes are handled. Tools: `PSR_GBRING=1` writes the cart I/O ring to `gbcart_ring.txt` on quit (every block op with thread, bank, time); `PSR_GBSELFTEST=1` traces status and save-RAM traffic (slow enough to change the outcome).
- Pokémon Lab write-back works: moving a Pokémon from the cart to a Stadium box updates the `.sav` with a valid main checksum, and Red still loads and changes boxes. Stadium also sets `wCurrentBoxNum` bit 7 and writes `FF` over the bank 2/3 box checksums; Red tolerated that in testing.
- Debug aids: `PSR_GBSELFTEST=1` traces Transfer Pak status reads and SRAM reads/writes to stderr.

## Gen1Recomp saves (findings, 2026-10-30)
- Pinned to Gen1Recomp **v0.3.40** (the user's build). It's licensed GPLv3 with additional terms: credit "BOIS CLUB GAMES, LLC's Pokemon Gen 1 Recompilation Project" (https://github.com/bryanthaboi/gen1recomp) in the README and in any launcher/front end. Its launcher is proprietary; never reuse it.
- Slots live at `%APPDATA%\pokemon-love2d\saves\<game>\slotN.lua` (+ `.bak`), as plain Lua tables. The ROM cache is `%APPDATA%\pokemon-love2d\<game>\` (`data\generated\*.lua`, marker `rom-cache.complete`).
- Pokémon live in `party` and `boxes[1..12]` + `currentBox`. A top-level `box = {}` is a New Game leftover that nothing reads.
- We reuse Gen1Recomp's own codec rather than writing one: `tools/save_convert/convert.lua export` (slot → .sav) under LuaJIT, run from the checkout with the ROM cache on `LUA_PATH` (v0.3.40 ignores `RED_CACHE`). LuaJIT's file I/O can't open paths over 260 chars.
- `tools/gen1recomp/merge.lua` merges back: it decodes the before/after carts with Gen1Recomp's importer, copies only changed sections (party, boxes, currentBox, inventory, pcItems, pokedex), and keeps slot-only per-mon fields matched by fingerprint (species + DVs + OT ID, the same as the nuzlocke mod). Re-encoding an unchanged slot is byte-identical (output must be written in binary mode).
- Nuzlocke mod (2.9.72): stores per-mon fields (`nuzlockeId`, `nuzlockeDead`, encounter data). A dead Pokémon is removed from the party into `modData.nuzlocke.route_forgiveness_dead_archive`, so Stadium never sees it. Unverified: how the mod reacts when a living Pokémon leaves the slot because it was moved into Stadium storage.
- Launcher integration (`src/main/gen1recomp_bridge.cpp`): a `.lua` battery save is exported at Play into `saves\gen1recomp\pN.cart.sav` (+ `pN.before.sav`, `pN.state` with the slot's hash) and merged back on quit via `transfer_pak::shutdown()`. It won't merge while gen1recomp.exe runs or if the slot's hash changed (Stadium's cart goes to backups instead), and a session left by a crash is merged at the next start. LuaJIT and the converter ship in `gen1recomp\` next to the exe. Vanilla saves default to `<rom>.sav`, and every session backs up to `saves\backups\` (newest 10 per save).
- Pokémon that leave a cart (into Stadium storage, or traded away) have their slot entry kept in `saves\gen1recomp\departed\<playthroughId>.lua`, and get it back when a Pokémon with the same fingerprint returns. That way a Stadium-box round trip keeps the nuzlocke ID and history. A Pokémon new to a slot (a real trade) arrives without nuzlocke fields; the mod marks it origin `EDITED`, and it shows up in its recovery screen to confirm (no penalty found in the mod's code).
- Two players work: each port exports and merges its own slot (`p1.*`, `p2.*`), and the same slot can't go in two ports. Verified with a Pokémon Lab trade between two Gen1Recomp slots (P2 on keyboard): each slot lost its outgoing Pokémon (kept in its own run's departed store) and gained the incoming one without nuzlocke fields, so nuzlocke IDs from two runs never collide.
- The rby_mmo mod conflicts with the nuzlocke mod (split Route 1 areas, unrecorded deaths). The user dropped it; focus is nuzlocke + Stadium battles/trades.
## Testing workflow
- Claude builds and fixes on PC; the user installs APKs on the Thor and reports results/logs.
- Back up every save before testing write-back.
