# bh-recomp/

Config + tooling for **N64Recomp**, the static recompiler that turns
the Body Harvest ROM into C source compatible with N64ModernRuntime.

## What's in here

- **`bh.us.toml`** — recompiler config: input ROM path, output dir,
  per-function overrides (the `ignored` array lists functions whose
  recomp output we suppress so our hand-written overrides in
  `bh-app/src/bh_cheats.cpp` take precedence at link time).
- **`overlays.us.txt`** — names BH's overlay segments (one segment per
  level + frontend + gameplay base). Used by N64Recomp to organize
  the per-overlay output.
- **`fixup_func_sizes.py`** — small build helper to patch function
  sizes in the `.sized.elf` intermediate. Not always needed; run it
  if N64Recomp complains about function-size resolution.

## What's NOT in here (gitignored)

- **`bh.us.z64`** — the BH US ROM. You must supply your own.
- **`bh.us.sized.elf`** — intermediate ELF generated from your ROM.
- **`RecompiledFuncs/`** — the recompiled C output. ~60 files
  totaling ~150 MB. Locally regenerated, never committed.

## How to use

1. Place your legally-obtained US Body Harvest ROM at `bh.us.z64`
   (same directory as this README).
2. From the repo root:
   ```powershell
   ..\..\N64ModernRuntime\build\librecomp\N64Recomp\Release\N64Recomp.exe `
       bh-recomp\bh.us.toml
   ```
3. The `RecompiledFuncs/` folder will appear here, populated with
   generated C source.
4. Continue with the main build process in `docs/BUILDING.md`.

## Notes on the `ignored` list

The `ignored = [ ... ]` array in `bh.us.toml` lists functions where
N64Recomp should NOT emit a body. We do this for two reasons:

1. **Hand-coded MMIO / hardware-direct functions** — BH has a few
   leaf functions that bit-bang registers (PI status, AI length,
   EEPROM serial). The recompiled output would try to access N64
   hardware registers that don't exist on PC. Our `bh_stubs.cpp`
   provides PC-friendly replacements that forward to librecomp's
   equivalents.

2. **Functions we override for cheat/enhancement purposes** — e.g.,
   `func_800703B0_7F360` (entity bbox cull) is replaced by a
   configurable version in `bh-app/src/bh_cheats.cpp` so the
   "Entity Render Distance" slider can scale the bbox.

Adding a function to `ignored` means "trust me, I'll provide a body
elsewhere". If you forget to provide one, you'll get a link error.
