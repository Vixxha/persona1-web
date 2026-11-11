PERSONA: PROJECT PHILEMON  -  play folder
=========================================

This folder is where the game RUNS. It holds none of the game itself; you supply
your own copy of Persona 1 (PSP, US, ULUS10432) that you dumped from a disc you own.

What goes where
---------------
  Persona1Native.exe + the *.dll next to it   built by you (persona1\README.md, step 8), copied here
  EBOOT_DECRYPTED.ELF                         your decrypted EBOOT.BIN (persona1\README.md, step 1)
  nids_persona1.csv                           made by step 5, copy it here (names show up in logs)
  Persona1.iso                                YOUR dumped ISO of Persona 1 (the game reads raw sectors from it)
  game\PERSONA\                               the same ISO extracted, so that
                                              game\PERSONA\PSP_GAME\USRDIR\... exists
  mods\                                       OPTIONAL PPSSPP-style texture pack (see below)
  ms0\                                        created automatically: your save slots

Texture packs (mods)
--------------------
Put a PPSSPP texture pack in the  mods\  folder. Both layouts work:
    mods\textures.ini
    mods\<PackName>\textures.ini
The pack must use the PPSSPP format (a [hashes] list in textures.ini pointing at PNG
files). Packs are other people's work: check their terms before you redistribute one.
Set PERSONA1_NOMODS=1 to turn the pack off, PERSONA1_MODS=<folder> to point elsewhere.

Starting the game
-----------------
Double-click Project_Philemon.bat (it checks that everything above is present).
Persona1_Debug.bat does the same and writes log.txt next to it.
Update_Exe_After_Rebuild.bat copies a freshly built exe in (only works while this folder
sits inside the repo folder).

Controls
--------
Keyboard: arrows, Z = X(cross), X = O(circle), A = square, S = triangle, Q/W = L/R,
          Space = Select, Enter = Start
Xbox pad: D-pad/left stick, A/B/X/Y, LB/RB = L/R, View = Select, Menu = Start
  Right stick click (R3)  EXP and yen rewards: off -> 2x -> 4x (for testing routes)
  Left trigger (LT)       3D internal resolution (same as F7)
  Right trigger (RT)      fullscreen (same as F9)
F3/F4 save slot, F6 HD 2D mode, F7 3D resolution, F8 upscale filter,
F9 / Alt+Enter fullscreen, F12 screenshot (480x272).
