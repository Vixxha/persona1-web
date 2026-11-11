# Persona: Project Philemon (Persona 1 PSP, native Windows recompilation)

A static recompilation of *Shin Megami Tensei: Persona* (PSP, US, **ULUS10432**) built on the
PSPRecomp framework in this repository. It runs natively on Windows with a Direct3D 11 renderer.

> **This repository contains no game data.** No ISO, no EBOOT, no recompiled game code, no
> textures, no movies, no saves. You supply your own copy of the game, dumped from a disc you
> own, and the tools here turn it into a native program on your machine. The English-undub ISO
> was the one used for development; any ULUS10432 dump with the same EBOOT should work.

## What works

Sound, movies, 3D dungeons, battles, world map, saving and loading (several slots), hi-res 3D
(up to 8x) and hi-res 2D, PPSSPP-format texture packs, Xbox controller support, and an optional
EXP/yen multiplier for testing routes.

## You need

- Windows 10/11, Visual Studio 2022 (Desktop C++ workload), CMake 3.20+, Python 3
- Your own legally dumped **Persona 1 ISO**
- A tool to extract an ISO (7-Zip works) and a way to get a **decrypted EBOOT** (see step 1)
- Roughly 3 GB free disk space (the generated code is large) and a patient compiler

## Where things go

```
PSPRecomp\                       <- this repository
  persona1\
    EBOOT_DECRYPTED.ELF          <- YOU PUT THIS HERE (step 1)
    nid_resolve.py               <- NID name table (already included; see step 5)
    gen6\                        <- created by steps 6 (generated, never commit)
  Persona_Project_Philemon\      <- the play folder
    Persona1.iso                 <- YOUR dumped ISO goes here
    game\PERSONA\PSP_GAME\...    <- the same ISO, extracted, goes here
    mods\                        <- OPTIONAL texture pack goes here
    EBOOT_DECRYPTED.ELF, Persona1Native.exe, *.dll, nids_persona1.csv  <- copied in by step 9
    ms0\                         <- created automatically (saves)
```

`.gitignore` already excludes all of the above, so `git add .` will not pick up game data.

## Build it, step by step

Run everything from the repository root (`PSPRecomp\`).

1. **Decrypt the EBOOT.** Open your ISO (7-Zip) and copy `PSP_GAME\SYSDIR\EBOOT.BIN` out. It is
   encrypted; produce a decrypted ELF with any PSP EBOOT decryptor (PPSSPP can dump a decrypted
   EBOOT, or use a PRX/EBOOT decrypter of your choice). Save it as
   `persona1\EBOOT_DECRYPTED.ELF`.
2. **Build the framework tools.**
   ```
   cmake -S . -B out\framework -DPSPRECOMP_PROFILE=""
   cmake --build out\framework --config Release
   ```
   This gives you `psp_analyze.exe` and `psp_recomp.exe` under `out\framework\Release\`
   (the Persona 1 build also puts them in `out\persona1\Release\`).
3. **Analyze the executable.**
   ```
   out\framework\Release\psp_analyze.exe persona1\EBOOT_DECRYPTED.ELF persona1\report.json
   ```
   This writes `persona1\report.json` and the `persona1\report_*.csv` files next to it.
4. **Make the function list.**
   ```
   python persona1\make_functions_csv.py
   ```
   Writes `persona1\functions_fixed3.csv`.
5. **Name the imports.**
   ```
   python persona1\make_nid_csv.py
   ```
   Writes `persona1\nids_persona1.csv`. It reads `persona1\nid_resolve.py` (a PSP NID-to-name
   table originally shared as a Ghidra script gist by SiD3W4y; check that gist's terms if you
   redistribute it separately).
6. **Recompile the game code and split it into compilable pieces.**
   ```
   out\framework\Release\psp_recomp.exe persona1\EBOOT_DECRYPTED.ELF persona1\functions_fixed3.csv persona1\gen6\manifest.cpp
   python persona1\split2.py persona1\gen6\manifest.cpp persona1\gen6\split
   ```
   (Create `persona1\gen6` first if it does not exist.) This produces about 350 MB of C++.
   It is derived from the game's code: **keep it on your machine, never commit it.**
7. **Create the profile.**
   ```
   python persona1\setup_profile.py
   ```
   Copies the host sources and the generated code into `profiles\persona1\`.
8. **Build the game.**
   ```
   cmake -S . -B out\persona1 -DPSPRECOMP_PROFILE=persona1
   cmake --build out\persona1 --config Release
   ```
   The first build takes a long time. The executable ends up at
   `out\persona1\profiles\persona1\Release\Persona1Native.exe`.
9. **Assemble the play folder** `Persona_Project_Philemon\`:
   - copy `Persona1Native.exe` and the `*.dll` files from that Release folder into it
   - copy `persona1\EBOOT_DECRYPTED.ELF` and `persona1\nids_persona1.csv` into it
   - copy your ISO in as `Persona1.iso`
   - extract your ISO so that `game\PERSONA\PSP_GAME\USRDIR\...` exists
   - (optional) put a texture pack in `mods\`

   Then double-click `Project_Philemon.bat`.

After changing host code later: `python persona1\setup_profile.py`, rebuild step 8, then run
`Update_Exe_After_Rebuild.bat` in the play folder.

## Texture packs (mods)

The game loads PPSSPP-format texture packs. Put the pack in `Persona_Project_Philemon\mods\`,
either directly (`mods\textures.ini` plus its image folders) or inside one sub-folder
(`mods\MyPack\textures.ini`). The pack needs a `textures.ini` with a `[hashes]` list that maps
texture hashes to PNG files; replacement images can be any size, so high-resolution packs
work. Texture packs are other people's work, so check their licence before redistributing them.

Environment variables: `PERSONA1_NOMODS=1` disables the pack, `PERSONA1_MODS=<folder>` uses a
different folder, `PERSONA1_MODSLOG=1` logs which textures were replaced and which were missed.

## Controls

| Input | Action |
| --- | --- |
| Arrows / Z X A S / Q W / Space / Enter | D-pad / cross circle square triangle / L R / Select / Start |
| Xbox controller | D-pad or left stick, A B X Y, LB RB, View = Select, Menu = Start |
| **R3** (right stick click) | EXP and yen rewards: off, 2x, 4x (a popup shows the setting) |
| **LT** | 3D internal resolution (same as F7) |
| **RT** | Fullscreen (same as F9) |
| F3 / F4 | Previous / next save slot |
| F6 | HD 2D mode (smooth, sharp, off) |
| F7 | 3D internal resolution (1x to 8x) |
| F8 | Upscale filter |
| F9 / Alt+Enter | Fullscreen |
| F12 | Screenshot (native 480x272) |

Saves are written to `Persona_Project_Philemon\ms0\PSP\SAVEDATA\` in the original PSP format.

## Debugging

Use `Persona1_Debug.bat` to write `log.txt`. Useful switches: `PERSONA1_REWARDSLOG=1`,
`PERSONA1_MODSLOG=1`, `PERSONA1_PROF=1`, `PERSONA1_SCALE=<1-8>`, `PERSONA1_NOWINDOW=1`.

## Legal

You must own the game, and everything the tools process must come from your own dump. This
project is not affiliated with or endorsed by Atlus or Sega. Persona and Shin Megami Tensei are
their trademarks. PSPRecomp is by jessicanataliagta; see `LICENSE`.
