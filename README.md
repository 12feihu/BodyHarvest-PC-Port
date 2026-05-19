# bh-recomp-toolkit

A PC recompilation of *Body Harvest* (Nintendo 64, 1998) with an extensive
in-game cheats menu, terrain editor, save-state and bookmark systems, and
ongoing research notes on the game's internals.

This is **not** a ROM redistribution. You supply your own legally-obtained
copy of the Body Harvest US ROM.


## What this is

In Short: A very messy and cluttered project. It's not meant to be "THE" PC port, It's meant to give others a starting point. "My port walked so yours can run"
It is accidentally the most accurate documentation due to trial and error. also yes it's 99% AI written. It started as a benchmark and only took 14 hours to make it fully playable. as of right now, has used a context of over 4 million tokens. the model I use has a 1 million maximum, so when I reach the limit, I have it put all important data into a file, then I flush its contexts and make it read the file/start again. 

Full:
A research + tooling project built on top of
[N64Recomp](https://github.com/N64Recomp/N64Recomp) +
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) +
[RT64](https://github.com/rt64/rt64). It statically recompiles the BH ROM
to native x86_64 and runs it as a Windows D3D12 application.

On top of that base, this project adds:

- **F1 in-game overlay** with three tabs:
  - **Cheats** — every built-in BH cheat (21 of them, including dev-only
    ones that aren't normally reachable) plus a couple dozen custom
    modder cheats (terrain editing, drain water, render-everything,
    spawn vehicles/aliens, save-state, position bookmarks, weapon
    editor, etc.)
  - **Enhancements** — aspect ratio, refresh rate, 2D upscale, fog
    distance slider, water level slider, entity render distance slider,
    far-plane slider
  - **Options** — multiplayer-POC toggle (loopback UDP between two
    instances), terrain export/import (JSON), entity export, save state
    to disk

- **Standalone terrain editor** (`bh-app/tools/terrain_editor.html`)
  - Loads the JSON exported by the in-game terrain dump
  - 256×256 heightmap rendering with 5 color palettes
  - Brush tools: paint / raise / lower / smooth / sample
  - PNG round-trip (export to PNG, edit in GIMP/Photoshop, import back)
  - Shield wall + portal overlays
  - Building / vehicle / alien / save-beacon overlays
  - Map cell grid (A1–H8) matching BH's in-game minimap
  - BH map-cell hover info, world coords, tile values

- **Multi-slot bookmarks with disk persistence**
  - 10 slots, each captures position + HP/fuel/ammo/weapons/items
  - Auto-saved to `bh_bookmarks.json`
  - Works while running, in vehicles, mid-combat (atomic field writes)

- **Weapon editor**
  - 18-weapon dropdown per inventory slot
  - Live ammo edits
  - Items bitmask editor

- **Research documentation** — see `docs/` and the extensive source
  comments in `bh-app/src/bh_cheats.cpp`. This project contains the
  most thorough public documentation of BH's internals we're aware of:
  full cheat catalog, weapon ID table, RAM address map, render
  pipeline analysis, terrain bit layout, shield wall mechanics, save
  state limitations, etc.

## What this is NOT

- **A ROM** — Body Harvest is © 1998 DMA Design / Gremlin Interactive.
  You must supply your own ROM file. This project ships zero BH binary
  content.

- **A standalone game** — without a BH ROM, the build won't produce a
  working executable.

- **A decompilation** — see
  [body-harvest-decompilation](https://github.com/jaytheham/body-harvest-decompilation) for that work. This
  project consumes the decomp's findings as documentation, but the
  build pipeline is recompilation (N64Recomp), not decompilation. We're
  a downstream beneficiary, not a competitor.

## Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| Body Harvest US ROM | SHA1 of the .z64 — see `bh-recomp/README.md` | Source binary |
| Windows | 10 or 11 (x64) | Runtime |
| Visual Studio Build Tools | 2019 or 2022 (MSVC v142+) | C++ toolchain |
| LLVM (clang-cl) | 17+ | Compiler used by recomp |
| CMake | 3.20+ | Build system |
| Ninja | 1.10+ | Build executor |
| Windows SDK | 10.0.19041 or later | Includes / libs |
| Python | 3.10+ | Build-side scripts |
| Git | Any | Cloning dependencies |

## High-level build flow

1. Clone this repo
2. Clone the supporting projects (N64Recomp, N64ModernRuntime, RT64) into
   a sibling layout — see `docs/BUILDING.md`
3. Place your BH US ROM and let the recomp generate `bh-recomp/RecompiledFuncs/`
4. Build with CMake + Ninja + clang-cl
5. Run `bh_app.exe`, press F1 in-game for the cheats menu

The full step-by-step is in **[docs/BUILDING.md](docs/BUILDING.md)**.

## Documentation

- **[docs/BUILDING.md](docs/BUILDING.md)** — full Windows build instructions
- **[docs/WEAPONS.md](docs/WEAPONS.md)** — complete weapon ID table (the verified one — most published lists are wrong about Arme/Waffe)
- **[docs/CHEATS.md](docs/CHEATS.md)** — every cheat in this toolkit (BH built-ins + custom additions + tool sub-dialogs)
- **[docs/MEMORY_MAP.md](docs/MEMORY_MAP.md)** — every RAM address this project touches, what it means, how it's used
- **[docs/HANDOFF.md](docs/HANDOFF.md)** — session-by-session development log (informal but exhaustive)

## Layout

```
bh-recomp-toolkit/
├── README.md             ← you are here
├── LICENSE               ← MIT for original code
├── THANKS.md             ← credits to upstream projects
├── docs/
│   ├── BUILDING.md       ← detailed build instructions
│   ├── CHEATS.md         ← full cheat catalog
│   ├── WEAPONS.md        ← verified weapon ID table
│   ├── MEMORY_MAP.md     ← every RAM address documented
│   └── HANDOFF.md        ← session-by-session research log
├── bh-app/               ← the host C++ app + RT64 renderer wrapper
│   ├── src/              ← cheats, multiplayer, renderer integration
│   ├── include/
│   ├── tools/            ← standalone tools (terrain editor)
│   ├── CMakeLists.txt
│   ├── gen_recomp_extras.py
│   └── hash_rom.cpp      ← helper to verify your ROM matches the
│                            supported US version
├── bh-recomp/            ← config for N64Recomp + per-function overrides
│   ├── bh.us.toml        ← recomp config (which addresses to skip,
│   │                       ignore, etc.)
│   ├── overlays.us.txt   ← named BH overlay segments
│   └── fixup_func_sizes.py
└── bh-rsp/               ← RSP microcode handling for BH's audio path
    ├── aspMain.cpp       ← hand-converted RSP code
    └── aspMain.toml
```

## A note on the research

Lots of BH's internals have been documented across cheat databases,
GameShark code lists, and the
[decomp project](https://github.com/) over the years — but plenty of
it was speculation, misattribution, or just plain wrong (e.g. the
"weapon list" that everyone copies includes two phantom entries that
push every vehicle weapon's ID up by 2). This project tries to verify
things in-game and document the actual mechanism in code comments.

If you find something here that's wrong, please file an issue — that's
the whole point of putting this on GitHub.

## License

MIT — see [LICENSE](LICENSE). The original code in this repository is
under MIT. Upstream dependencies (N64Recomp, N64ModernRuntime, RT64,
SDL2, XAudio2, ImGui, etc.) retain their own licenses. Documented
memory addresses, struct offsets, and game-data observations are facts
about the BH binary and not the IP of any party.

## Credits

See [THANKS.md](THANKS.md) for the full list. Short version:
- The N64Recomp / N64ModernRuntime / RT64 projects make this possible
- The Body Harvest decompilation project's reverse-engineering work
- Decades of community cheat code research (even where wrong, it pointed at the right addresses)
