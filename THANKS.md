# Thanks

This project exists on the shoulders of a lot of other people's work.

## Direct dependencies

| Project | Role |
|---------|------|
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) | Static recompilation of N64 binaries to native code |
| [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) | Host runtime (audio, input, scheduling, save flash) |
| [RT64](https://github.com/rt64/rt64) | Modern HLE renderer (D3D12 / Vulkan) |
| [SDL2](https://www.libsdl.org/) | Cross-platform windowing and input |
| [ImGui](https://github.com/ocornut/imgui) | In-toolkit immediate-mode GUI (bundled via RT64) |
| [stb](https://github.com/nothings/stb) | Image / utility headers |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | OS file dialogs |
| LLVM + clang-cl | Compiler used for building |

## Research and reverse-engineering

| Project / Resource | Contribution |
|--------------------|--------------|
| [body-harvest-decompilation](https://github.com/) | The primary source of memory addresses, struct layouts, function signatures, and overlay structure. This project's source comments cite specific files and line numbers in the decomp throughout. Most of what we know about BH's internals traces back to this team's work. |
| Community cheat-code archives (GameShark, Action Replay, GameFAQs cheat pages) | Decades of poking at BH's memory established a baseline that we then verified, corrected, and expanded. Some specific addresses (player position, the items bitmask, the camera variables) were first documented this way. |
| BH speedrun + modding community | Behavioral knowledge — which gameplay states cause which side effects, which cheats activate which content. The "Serious Weapons" upgrade observations came from in-game testing aligned with community knowledge. |

## The game itself

*Body Harvest* was developed by **DMA Design** (now Rockstar North),
published by **Gremlin Interactive**, released for the N64 in 1998.
The game was directed by Mike Dailly with design contributions from
the team that later went on to create the Grand Theft Auto series.

This project is not affiliated with, endorsed by, or licensed from any
of those parties. It is a research and tooling effort built around a
ROM the user must legally obtain themselves.

## Special acknowledgement

To everyone who poked around BH's RAM in the early 2000s with a
GameShark and a notebook, then posted what they found to GameFAQs.
Some of it was wrong. Most of it was right. All of it gave the next
generation of investigators a starting point. This project is
descended from that work.
