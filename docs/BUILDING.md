# Building bh-recomp-toolkit from source

Complete step-by-step build instructions for Windows. The project has
been tested with VS2019 BuildTools + clang-cl 17 + Ninja, building
against a US-region Body Harvest ROM.

## 1. Required toolchain

Install these once, in this order:

1. **Visual Studio Build Tools 2019** (or 2022 — same MSVC v142+
   compiler). Install the "Desktop development with C++" workload
   plus the latest Windows SDK (10.0.19041 or newer).

2. **LLVM 17 or later** — provides `clang-cl.exe` which is the actual
   compiler used. Install to the default `C:\Program Files\LLVM\`.

3. **CMake 3.20+** — download from cmake.org. Add to PATH.

4. **Ninja** — Visual Studio installs a copy at
   `C:\Program Files (x86)\Microsoft Visual Studio\<year>\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe`
   so you may already have it.

5. **Python 3.10+** — for the build-time helper scripts.

6. **Git** — for cloning dependencies.

## 2. Folder layout convention

This project expects a sibling-folder layout. Pick a workspace root
(let's call it `C:\BodyHarvestPC\`) and clone everything as siblings:

```
C:\BodyHarvestPC\
├── bh-recomp-toolkit\        ← THIS REPO
├── N64ModernRuntime\         ← clone N64Recomp/N64ModernRuntime
├── rt64\                     ← clone rt64/rt64
└── body-harvest-decompilation\  (optional, for cross-reference)
```

Adjust the paths in `bh-recomp-toolkit/bh-app/CMakeLists.txt` if you
prefer a different layout.

## 3. Clone the dependencies

```powershell
cd C:\BodyHarvestPC

git clone https://github.com/N64Recomp/N64ModernRuntime.git
git clone --recursive https://github.com/rt64/rt64.git
```

Build N64ModernRuntime and RT64 per their respective READMEs first.
You'll end up with built static libraries under their `build/` folders
that this project's `CMakeLists.txt` links against.

## 4. Supply your own Body Harvest ROM

You need a US-region Body Harvest ROM file. **This project does not
ship one.** Acquire it from your own legally-purchased cartridge
(commonly via cartridge readers like the Retrode or
EverDrive's dump function).

Place the ROM at `bh-recomp/bh.us.z64`.

Verify the file matches the supported version:

```powershell
# Build the hash tool once
cd bh-recomp-toolkit\bh-app
clang-cl /EHsc hash_rom.cpp /Fe:hash_rom.exe
.\hash_rom.exe ..\..\bh-recomp\bh.us.z64
```

If the SHA1 doesn't match what the tool expects, the recomp may
produce code that doesn't behave correctly. Other regions (PAL,
European releases) are not currently supported — those would need
their own `bh.<region>.toml` config.

## 5. Generate the recompiled C code

This is where N64Recomp reads your ROM and emits the `RecompiledFuncs/`
folder of generated C source — basically every BH function translated
to a host-callable C function.

```powershell
cd C:\BodyHarvestPC

# Run N64Recomp against our config
.\N64ModernRuntime\build\librecomp\N64Recomp\Release\N64Recomp.exe `
    bh-recomp-toolkit\bh-recomp\bh.us.toml
```

You should now have:

```
bh-recomp-toolkit\bh-recomp\RecompiledFuncs\
├── funcs.h
├── funcs_0.c
├── funcs_1.c
... (about 60 files)
```

These files are gitignored — they're locally regenerated per user
because they're derived from the user's own ROM copy.

### Manual #if 0 stubs for weak-symbol overrides

This project overrides two BH functions via the linker's weak-symbol
mechanism. The recomp emits both originals; we need to silence them so
our strong-symbol overrides win at link time. After running N64Recomp,
you must manually wrap two functions in `#if 0 ... #endif` in the
generated files (OR re-run N64Recomp with the latest `bh.us.toml`
which lists them in the `ignored` array).

The current `bh.us.toml` lists these as `ignored`, so a fresh N64Recomp
run should automatically skip them. But if you're hitting "duplicate
symbol" link errors for `func_800703B0_7F360` or `func_800E95BC_F856C`,
you'll need to manually `#if 0` their bodies in the generated `.c` files
that contain them. The exact files vary by N64Recomp version.

## 6. Build

```powershell
cd bh-recomp-toolkit\bh-app

# Set up the VS / clang-cl environment
"C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

# Add LLVM + CMake + Ninja to PATH for this shell
$env:PATH = "C:\Program Files\LLVM\bin;" + `
            "C:\Program Files\CMake\bin;" + `
            "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;" + `
            "C:\Program Files (x86)\Microsoft Visual Studio\Installer;" + `
            $env:PATH

# Configure
mkdir build
cd build
cmake .. -G Ninja `
         -DCMAKE_BUILD_TYPE=Release `
         -DCMAKE_C_COMPILER=clang-cl `
         -DCMAKE_CXX_COMPILER=clang-cl

# Build
cmake --build . --config Release
```

You should end up with `build\bh_app.exe`.

## 7. Run

```powershell
.\bh_app.exe
```

The game window opens, BH boots normally. Press **F1** in-game to
toggle the cheats overlay.

### Command-line options

- `--port <N>` — UDP port to listen on for the multiplayer-POC feature
  (default 12345)
- `--peer-port <N>` — UDP port to send to (default 12346)

So a second instance to test the loopback multiplayer:

```powershell
.\bh_app.exe --port 12346 --peer-port 12345
```

## 8. Common build issues

**`'cmake' is not recognized`** — CMake isn't on your PATH. Either
install it system-wide or add the install dir to PATH for this shell.

**`'ninja' is not recognized`** — same issue, point at the Ninja
inside the VS BuildTools install (see the PATH line in step 6).

**`'vswhere.exe' is not recognized`** — leak from `vcvars64.bat` that's
harmless. The build proceeds anyway.

**`fatal error: 'atomic' file not found`** — MSVC stdlib not on the
include path. You forgot to run `vcvars64.bat`.

**`duplicate symbol: func_800703B0_7F360`** — see the
"#if 0 stubs" note in step 5.

**`CMake Error: Generator: execution of make failed`** — CMake cache is
stale or `CMAKE_MAKE_PROGRAM` got cleared. Easiest fix: delete `build/`
and reconfigure with the full command from step 6.

**`'atomic' file not found` after a working build** — usually means a
CMake reconfigure happened in a bad shell environment. Delete the
build folder and reconfigure from a fresh shell that ran
`vcvars64.bat`.

## 9. Updating

When pulling new changes from this repo:

```powershell
cd C:\BodyHarvestPC\bh-recomp-toolkit
git pull origin master

cd bh-app\build
cmake --build . --config Release
```

If new files were added to `CMakeLists.txt`, you may need to rerun
`cmake ..` from the build folder first.

If `bh-recomp/bh.us.toml` changed (e.g. new functions added to the
`ignored` array), regenerate the recomp output:

```powershell
cd C:\BodyHarvestPC
.\N64ModernRuntime\build\librecomp\N64Recomp\Release\N64Recomp.exe `
    bh-recomp-toolkit\bh-recomp\bh.us.toml
```
