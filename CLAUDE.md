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
- #13 rental roster crash (debug on PC first; repo has trace/diag build scripts).
- #10 announcer audio static.

### Phase 2: enhancements (all optional toggles)
- Thor second screen: live battle info on the bottom screen, read from game memory (active Pokémon's moves, type, PP, power, accuracy; opponent info). Removes the need to hold a button to read moves. Use pret's Stadium decomp to locate battle structs: https://github.com/pret/pokestadium
- Reference for Thor second-screen plumbing: ThorScreen Android plugin in PMD-faith (GPLv3): https://github.com/ch4rizard-x/PMD-faith
- UI scaling option for the battle menu / move list (2D overlay elements; watch for layout overlap).
- Options and QoL in the existing recomp-ui launcher rather than bolted on.

### Phase 3: Gen1Recomp save support
- Read Gen1Recomp save slots directly (Lua data files, saves/<version>/slotN.lua) by converting to a Gen 1 cartridge save in memory, paired with a vanilla Red/Blue/Yellow ROM for the cart header. Write changes back on exit.
- Gen1Recomp: https://github.com/bryanthaboi/gen1recomp . Check its license before reusing its .sav export logic. Pin to a known version; its format may change.
- Note: an older Gen1Recomp version (0.1.54) had exported .sav files load into glitch city on real hardware (issue #691). Always test with backups.

### Later / maybe
- Stadium 2 (separate recomp of a different game; much of the platform work would carry over). Check state of any Stadium 2 decomp first.
- Netplay. Large; only after everything else is solid. Intended design:
  - Each player's own cart appears as the other's controller 2 (P1 = host's cart, P2 = guest's cart on both machines).
  - On connect, exchange 32 KB GB saves only. Never send ROMs; each side supplies its own GB ROM and they compare a hash to confirm a match.
  - Only the local player's cart writes back to their save; the opponent's save is a session-only copy (the 3DS port's local wireless uses a similar host-sends-save idea).
  - Delay-based lockstep input sync should be enough: Stadium battles are turn-based and menu-driven, so rollback is not needed.
  - Main risk: determinism of the recomp (timing, threads, audio, RNG). Build a per-frame state hash / desync check early.

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
## Testing workflow
- Claude builds and fixes on PC; the user installs APKs on the Thor and reports results/logs.
- Back up every save before testing write-back.
