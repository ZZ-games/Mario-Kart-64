# Mario Kart 64 Recompiled

A static recompilation of Mario Kart 64 (USA) for the browser. Every function in the
game's MIPS machine code is translated to C and compiled to WebAssembly; the N64's
operating system, graphics chip (RSP/RDP, F3DEX) and controllers are reimplemented.

## Branches
- **main** – the web page and all source (recompiler, runtime). No game data. This is what
  GitHub Pages serves.
- **game-data** – the ROM and the recompiled game (`mk64.wasm`). Private; never served by Pages.

## Playing
- **iPad / any browser:** open the Pages site. The first time on a device, paste a read-only
  GitHub key (or pick `mk64.z64` + `mk64.wasm` from Files). The files are saved on that
  device and the game opens straight away after that. Add it to the Home Screen for full screen.
- **Windows PC:** put `index.html`, `main.js`, `worker.js`, `gl.js`, `sw.js`, the files from
  `local/`, plus `mk64.z64` and `mk64.wasm` in one folder and run `play.bat`.

Controls: arrows steer, X = A, C = B, Space/Shift = R, Z = Z, Enter = Start; touch controls
on tablets; gamepads supported.

## Status
Boots, intro, title, menus and 3D racing render. Not done: sound, full-race testing,
save persistence.

## Building (needs the mk64 decomp checkout in `decomp/` and the ROM in `rom/baserom.us.z64`)
    python3 tools/symmap.py && python3 tools/align.py && python3 tools/callvote.py && python3 tools/osalign.py
    python3 tools/recomp_mips.py
    OPT=-O2 sh tools/build.sh      # -> build/mk64.wasm
