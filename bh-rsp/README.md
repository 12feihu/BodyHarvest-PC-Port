# bh-rsp/

Recompilation of Body Harvest's **RSP microcode** (Reality Signal
Processor — the N64's coprocessor that handles audio decode, vertex
transform, and similar offloaded work).

BH ships with a custom-modified F3DEX-style microcode plus its own
audio microcode (`aspMain`). N64ModernRuntime can't run unmodified RSP
binaries — they need to be recompiled to host C too, separately from
the main CPU recomp.

## What's in here

- **`aspMain.cpp`** — partially-hand-authored C port of BH's audio
  microcode. Some routines were too unusual for the auto-recompiler
  and needed manual conversion.
- **`aspMain.toml`** — recomp config for the audio microcode portion.
- **`fixup_aspmain.py`** — helper to align/patch the RSP code segment
  before recompilation.

## Why this exists separately from `bh-recomp/`

`bh-recomp/` handles the **main MIPS CPU** code in BH. The RSP is a
separate processor with its own instruction set (it's a stripped-down
MIPS variant with vector extensions), and N64Recomp processes the two
streams independently. Splitting it into its own folder keeps the
configs from cross-contaminating.

## Notes

The audio output you hear in `bh_app.exe` flows roughly like:
1. BH's main code (recompiled via `bh-recomp/`) calls `osAiSetNextBuffer`
   to queue an audio chunk
2. The chunk's source is a list of "ASP commands" — instructions for
   the audio microcode to process samples (mixing, ADPCM decode, etc.)
3. The audio microcode (recompiled here in `bh-rsp/`) processes those
   commands into raw PCM samples
4. `bh_app.exe` forwards the PCM to XAudio2 for playback

If audio glitches or distorts, the bug is usually in this folder or
in librecomp's audio queue handling.
