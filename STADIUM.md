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

- Rewrite the fork's GitHub README/description around its focus: Windows and Android (AYN Thor) builds, online play (netplay), and dual-screen support; keep upstream credits. User wants this eventually (2026-11-02).

### Releases (fork pre-releases, `netplay-testNN`)
- Build so the exe embeds the release commit (`build_ssanne.bat` reconfigures; check `PSR_GIT_COMMIT` in `build\build.ninja` equals `HEAD`), package with `_package_release.ps1 -Version fork-netplay-testNN`, publish with `gh release create --target <full sha> --prerelease`, and confirm GitHub's asset digest matches.
- GitHub's release list order is not by date (it listed test9 before test10). Builds through test 10 offer whatever GitHub lists first; from `65d913f` on, the launcher picks the newest `published_at`. Tests 6-9 were renamed to `netplay-test06`-`09` for that reason. After publishing, run the previous release's exe from `dist\` for a few seconds and check its `[update]` stderr line offers the new build.
- Not yet released: `65d913f` (update order fix); bundle with the next fix.

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

## Android build (first boot on the AYN Thor 2026-11-16)
Branch `build/android`. Toolchain under `%LOCALAPPDATA%\Android` (no PATH changes): `jdk-17` (Temurin), `Sdk` (cmdline-tools, NDK 27.1.12297006, CMake 3.22.1, platforms;android-34, build-tools 34.0.0, platform-tools), `gradle-8.0`. Versions follow Goemon64Recomp-Android (GPLv3), which runs the same renderer on the Thor.
1. A finished Windows build first: the Android build runs its host `build\file_to_c.exe`.
2. `powershell -ExecutionPolicy Bypass -File tools\android_build.ps1 -Install` configures `build-android\` with the NDK (Ninja from VS Build Tools), builds `libPokemonStadiumRecomp.so` (~22 min clean, 1,003 generated files at `-g0`), stages it with libSDL2 and libc++_shared into `build-android\apk-libs`, and packages `android\` with Gradle (AGP 8.1.2). `-Native` / `-Package` run one half.
3. The app looks for `baserom.z64` in its folder, `/sdcard/Android/data/io.github.bearfeet0511.stadiumrecomp/files/` (push it with adb). `stderr.log` there has the game log; `adb logcat -b crash` has native crashes.

What Android changes:
- RT64 patch `0003-android-arm64-build`: Goemon's Android changes adapted to our older RT64 (no plume) plus Windows-host bits: dxc.exe for shaders, host `file_to_c` (`-DRT64_FILE_TO_C`), no nfd/zstd tools/texture tools, no `-Werror`, SDL2 2.32.8 fetched, R8G8B8A8 swap chain, Adreno compute-shader inline pass with the NDK's spirv-opt.
- Top-level CMake: a shared library instead of an exe, no `-march=nehalem` (the runtime's RSP code uses sse2neon), PIC + 16 KB page alignment. `post_mortem.cpp`/`debug_server.cpp` (Win32) are replaced by `android_diag_stubs.cpp`, and the recomp-ui launcher by `android_launcher_stub.cpp` (boots straight in, P1 = first gamepad, Vulkan, no SSAA/MSAA).
- `main.cpp`: the window needs `SDL_WINDOW_VULKAN` (otherwise SDL takes the surface for OpenGL); `exe_dir()` is the app's external storage folder; the process `chdir`s there, sets `HOME` there (RT64 writes `$HOME/.rt64`), and sends stderr to `stderr.log`.
- adb was unreliable through the PC's front-panel USB hub ("offline"); a rear motherboard port works.

Android status (2026-11-16, Thor): boots, runs smoothly, sound works, gamepad input works. Transfer Pak works with a cart in the app folder (`carts/`, paths relative to the app folder in `launcher.cfg`; push files with adb, `chmod 666` so the app can write them). Open:
- Music sounds quieter relative to effects and announcer than on PC (unconfirmed; compare the same scene on PC).
- GB Tower boots Red but loads in ~49 s (PC: a few seconds). Cause: `send_external_message_after` starts a thread per SI DMA (two per Transfer Pak block, ~22k blocks), ~1 ms each on Bionic. Tried and reverted: (1) sending sub-ms messages immediately: fast, but GB Tower's time-limited preload reached bank 44 and failed ("Transfer Pak not set properly"); (2) one long-lived sender thread: low frame rate during the load, then the same error. The preload depends on the loader blocking per read so other guest threads run; a fix must keep that (e.g. a timed wait inside the guest scheduler) and be checked against the bank-44 failure.
- GB Tower's picture is off-centre (shifted left/down) with a rainbow line at the top, on Android only; normal screens are centred. GB Tower uses its own VI mode.
- The debug APK doubled from 371 to 739 MB between builds (unexplained).

## Battle memory (for the second-screen panel, found 2026-11-16)
Found by dumping RDRAM during a Free Battle with Game Pak Pokemon (`tools\rdram_dump.py`, via the debug server's `rdram_peek`) and diffing turns. US v1.0 addresses:
- `D_800AE540` (passed to the battle engine, fragment62) holds both sides. Player block header at `0x800AE540` (trainer name), then 6 records at `0x800AE561`, stride `0x54`. Opponent block `0x21C` later: header at `0x800AE75C` (name "COM"), records at `0x800AE77D`. Records are in the cart/roster order, not pick order.
- Record layout (Gen 1 party struct with shifts): `+0` species (internal index; 0 for some rentals), `+1` HP (u16 BE), `+3` level (box), `+4` status (Gen 1 bits: sleep 0-2, PSN 3, BRN 4, FRZ 5, PAR 6), `+5/+6` types, `+8..+0B` move IDs, `+0x1F..+0x22` PP (top 2 bits are PP Ups), `+0x23` level, `+0x24` fainted flag (0/1), `+0x25` max HP (u16 BE), `+0x2F` nickname/species name (ASCII). HP and the fainted flag update when a Pokemon faints; check whether they update every hit.
- Active Pokemon: pointer at `0x800FCB18` -> battle copy (heap). `+0x0B` Pokedex number, `+0x0C` current HP (u16 BE, live), `+0x1C/+0x1D` types, `+0x1F..+0x22` moves, `+0x26` level, `+0x28` max HP, `+0x32..+0x35` PP. Match it to a record by moves + level. A second copy at `ptr + 0x340` is likely the opponent's (unverified).
- Move names: NUL-separated ASCII list in move-ID order in the ROM (US v1.0 at `0x795A98`, starting "POUND"). Read them from the user's ROM at runtime; don't copy game text into the repo.
- Picks: the records are in pick order, and `0x800AE546` holds how many are in the battle (3 or 6).
- Battle menu (from pret's decomp, `fragment62_2F74E0.c`; works, 2026-11-21): `D_84390010[2]` at fragment62 base + `0x90010` points to each side's battle state. Its menu block at `+0x654` has mode `+0x04` (0 top menu, 1 Fight, 2 Pokemon, 3 Run), step `+0x10` (0 waiting at the top menu, 1 in a submenu, 3 chosen) and the side's controller `+0xC8` (`gControllers` = `0x800A7320`, Player 1 first). C-buttons pick a move only at mode 1 / step 1. Tapping a move presses A at the top menu, waits for that state, then presses the C-button. (The earlier guess, a pointer at base + `0x90068`, flips between the two sides' states.) The battle engine is decompiled, so read pret's C before diffing RAM.
- Switching (same file, func_8430E8E8 / func_8430E72C): B at the top menu gives mode 2 / step 2 (a forced switch after a faint goes straight there). Slot buttons in pick order: with <4 in battle C-up/C-right/C-down; with more C-up, C-left, B, C-right, C-down, A. Count: side state `+0x720` -> `unk_08[0]` `+0x002`. The panel's team cards send out on a second tap ("Tap again to switch").
- Pre-battle pick screen: fragment64 (fragment id 0x38), the only fragment loaded there. Buttons (fragment64.c func_848027F0): B, C-left, C-up, A, C-down, C-right pick slots 1-6; L undoes; R checks. The team records at `0x800AE561` are in slot order on this screen. The panel shows a tile per slot plus L Undo. Button icons use the N64 colours (A blue, B green, C yellow).
- Controls (from the game's screen): moves are C-up / C-right / C-down / C-left for move slots 1-4; L cancels. The button that sends out each Pokemon is still unknown.

## Android to-do (2026-11-16, end of session)
- Widescreen battles (rt64 patch 0005, 2026-11-21). In Expand, RT64 keeps a rect at 4:3 unless it covers the whole width (or is drawn from a framebuffer copy), and keeps every orthographic draw at 4:3. Fixed, checked on the Thor:
  - Portraits out of their boxes: they're drawn from a framebuffer copy, which RT64 treated as full-screen. Now only copies at least half the screen wide are.
  - Zig-zag wipe: 24 px tall bands (on the 24-row grid) that grow from one edge or slide past the other, then a 20x15 grid of 16x16 black rects. Both now go to widescreen coordinates.
  - White fade into battle: a see-through orthographic quad (<= 2 triangles, full scissor). Its viewport is widened by the aspect ratio so it fills the width.
  - Rects spanning >= 90% of the width count as full-screen (a fill ends 1 px short: inclusive N64 coordinates).
  - Not the cause: widescreen switching on too late. The fade out to white happens on the 4:3 team-confirm screen, which stays 4:3 on purpose. Switching on at 'FIGT' didn't help, and many battles never push 'FIGT'.
- Widescreen, still open:
  - Trainer name boxes (24 px tall) match the band rule while they slide in, so they start at the wide screen's edge and then jump. Requiring untextured rects for the band rule broke other rects (reverted).
  - Surf's blue screen tint only covers 4:3 (probably another full-screen draw type; log it).
  - Debug: `PSR_RT64_RECTLOG=1` (psr_env.txt) logs each distinct rect and triangle draw on the 320-wide screen; `PSR_WIDESCREEN_TRACE=1` writes widescreen.log (fragments loaded, 'FIGT'). Fast frame capture on the Thor: `screencap -d <display id>` to raw files (~90 ms each, PNG ~1 s), then contact sheets on the PC (no ffmpeg installed).
- Idle bottom screen (done 2026-11-21): black with the Stadium logo at half width, ~35% alpha (user: like Cocoon's single-screen logo). The logo is read from the user's ROM at runtime: kids_club_game_ui (ROM 0x675FA0-0x696350, PERS-SZP = header + Yay0), offset 0x4F120, 100x56 RGBA32 (fragment2.c func_878021B4 draws D_304F120). The title screen art is file 0 of the backgrounds archive (ROM 0x3CB130; files are PRESJPEG = 16-byte header + a plain JPEG), if ever wanted.
- Panel text: one size throughout, HP and PP on a second line, long names end in "…" (2026-11-21). More tweaks may follow.

## Performance findings (Thor, 2026-11-21)
- Battle frame drops (pans, send-outs, moves) were the GPU: ~24 ms per battle frame at 3x, 30-50 ms on heavy effects. Cause: rt64 patch 0001 (and the fork's rect/alpha-blend rule before it) sent every rect, alpha-blended and 3D draw to the ubershader to avoid conservative raster, so specialised shaders were built but never used. Patch 0004 gives each specialised shader a non-conservative pipeline: intro GPU time 18 -> 5.4 ms, GPU at its lowest clock (401 MHz) instead of 550 MHz / 79% busy. "Way way better" in battle. Check on PC too (same patch), and watch for the rainbow lines / fade seam that 0001 fixed.
- The game draws 30 frames per second (30 display lists/s; screen updates are 60/s). update_screen counts VI swaps, not game frames.
- RT64 "render to RAM" is required: without it menu images turn to rainbow noise and battle portraits / Oddish's face break. It costs CPU time (sync copy every frame) but wasn't the GPU problem.
- Not the cause: HDR colour buffers (RT64 picks 16-bit float on Adreno; forcing standard changed nothing), texture filtering, the bottom-screen panel, shader compilation.
- Tools: `[frame-stats]` every 5 s in stderr.log (game frames, CPU time per display list, GPU time per frame from the workload queue's execute/wait, shader count); `[rt64-config]` at start; `psr_env.txt` in the Android app folder sets PSR_* overrides (`PSR_RT64_RES_MULT`, `PSR_RT64_HDR`, `PSR_RT64_FILTERING`, `PSR_RT64_RENDER_TO_RAM`); a `no_panel` file turns the bottom screen off; GPU clock in `/sys/class/kgsl/kgsl-3d0/gpuclk` and `gpu_busy_percentage`; simpleperf from the NDK for CPU profiles.
- 60 fps (experimental, 2026-11-21): `launcher.cfg` `framerate=60` (both platforms; no launcher control yet, the Windows launcher keeps the key when saving) sets RT64's refresh rate to Manual 60, so it draws in-between frames while the game logic stays at 30. "It looks so good" on the Thor; GPU ~3-5 ms per frame. Known glitches: brief one-frame errors on fast motion (Dratini's body splits when it moves quickly). Fixing them means tagging the affected draws (matrix IDs via RT64's extended GBI) so RT64 matches them between frames. `PSR_RT64_FPS=<n>` overrides for testing.
- CRT filter: wanted (user, 2026-11-21). RT64 has none; it would be a post-process shader in the present step, as an option.
- Texture packs: wanted (user, 2026-11-21). RT64 supports replacement textures; needs a way to pick a pack on Android and a launcher setting on Windows.
- The Windows build hasn't been rebuilt from `build/android`; check it still builds and runs before merging.
- Earlier open items: GB Tower load time and off-centre picture, music quieter than PC, Transfer Pak saves back to PokeDaisy, a touch launcher/ROM picker.
- Android launcher (next, 2026-11-21): settings (graphics, widescreen, 60 fps), Stadium ROM and per-player GB cart/save pickers, dual-screen friendly.
- In-app updates without losing saves: check GitHub Releases like the PC launcher, download the APK and hand it to Android's installer (user taps Update). Saves live in the app folder, which survives updates but is deleted on uninstall, so: (1) sign releases with ONE permanent release key (the debug key changes per build machine, and a signature mismatch forces an uninstall); never lose it; (2) also back saves up outside the app folder (e.g. Documents/StadiumRecomp).
- Public post on the AYN Thor subreddit once the launcher works (user's idea): beta label, features + known issues list, players supply their own ROMs/saves (never link ROMs), source public with credits (SS Anne/mstan, pret, N64Recomp, RT64, Goemon64Recomp-Android, Gen1Recomp terms).

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
