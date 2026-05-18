# bh-app/

The host C++ application — wraps the recompiled BH code in a real
Windows app with rendering (RT64), audio (XAudio2), input (SDL2 +
XInput), and the F1 cheats overlay.

## Files

| File | Role |
|------|------|
| `CMakeLists.txt` | Build config — links bh-recomp, RT64, librecomp, all the Win32 libs |
| `main.cpp` | Entry point, command-line parsing, audio thread, XAudio2 voice callbacks, the per-frame tick that drives the cheats system |
| `src/bh_cheats.cpp` | **The big one** — every cheat, every override, the F1 dialog, the bookmark system, the weapon editor, the terrain editor backend, all the renderer / water / cull sliders. ~5000 lines of well-commented code |
| `src/bh_renderer.cpp` | RT64 integration — initializes the renderer, exposes the enhancement-config setters that the F1 sliders drive |
| `src/bh_net.cpp` | Multiplayer POC — UDP loopback between two instances |
| `src/bh_stubs.cpp` | Empty / forwarding implementations of BH's hand-coded MMIO functions (PI status, AI length, EEPROM serial) |
| `src/register_overlays.cpp` | Tells librecomp where BH's overlays live in memory so dynamic loading works |
| `include/bh_app.hpp` | Public headers used across the cpp files |
| `tools/terrain_editor.html` | **Standalone HTML editor** for the terrain JSON files exported by the in-game Options tab |
| `gen_recomp_extras.py` | Build-time helper — scans recomp output for symbol references and emits forward declarations |
| `hash_rom.cpp` | Verifies your BH ROM file matches the supported US version |

## Where to look for things

- **Want to understand a cheat?** Read its comment block in
  `src/bh_cheats.cpp`. Each cheat (built-in and custom) has 5-20 lines
  of inline documentation explaining what it does, what RAM it touches,
  and why.

- **Want to add a cheat?** Pattern: declare a function with signature
  `void custom_my_cheat(uint8_t* rdram)`, then register it in the
  `kCustomCheats[]` array near the top of `bh_cheats.cpp`. The F1 menu
  picks it up automatically.

- **Want to add a per-tick override?** Pattern: declare a function with
  signature `void apply_my_tick(uint8_t* rdram)`, then add a call to
  it in `main.cpp`'s `stub_poll_input`. Used for things that need
  re-application each frame (water level, far plane, cull bypass).

- **Want to add a sub-dialog tool?** Pattern: copy the structure of
  `open_bookmark_manager()` near the end of `bh_cheats.cpp`. It's a
  Win32 popup window with its own message handler.

## The "stub" terminology

A few functions are called "stubs" because they replace BH's
hardware-direct implementations:

- `func_8001BCE0_1C8E0` — BH's `osAiGetLength` (reads AI_LEN_REG).
  Stub forwards to librecomp's `osAiGetLength_recomp`.
- `func_8001F6E0_202E0` — BH's `__osPiGetStatus`. Returns 0 (PI never
  busy on PC).
- `func_8001D5A0_1E1A0` — BH's `osEepromLongRead`. Stub forwards to
  librecomp's `save_read`.

These are listed in `bh-recomp/bh.us.toml` under `ignored`.

## The overlay system

BH uses RAM overlays to swap level-specific code in and out:

- Base gameplay code (always loaded): `overlay_gameplay_outside`,
  `overlay_gameplay_inside`, `overlay_gameplay_frontend`
- Per-level code (one of these loaded at a time): `overlay_level_greece`,
  `_java`, `_america`, `_siberia`, `_comet`

`register_overlays.cpp` tells librecomp about all of them so the right
code gets paged in when BH calls `osCreateRegion`/`osLoadOverlay`.

This is also why "loading a save state from a different level crashes"
— the overlay paged in is the new level's, but the save's RAM points
at function addresses in the previous level's overlay code that's no
longer loaded.
