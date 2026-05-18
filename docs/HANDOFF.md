# Body Harvest PC — Handoff (post Phase 12c)

Status snapshot for picking this back up after a break. **Rendering is working
and the game is playable end-to-end through level transitions.** Phase 12c
landed a four-line fix that resolved every visible rendering symptom from the
prior session.

## TL;DR

- `bh_app.exe` boots, audio plays, **graphics render correctly**, and the
  recompiled game progresses through gameplay (Greece intro → final boss →
  Java) using the actual ROM logic.
- stderr log shrank from **26,746 lines → 556 lines** (98% drop) on a
  comparable run. Every error category previously firing is now silent.
- Known remaining issue category: **event ordering / save-state weirdness**
  (computer-screen reads triggering NPC dialogue, a save unexpectedly
  jumping to Greece final boss). These are *not* rendering bugs and live
  in a separate problem space (libultra timing, save layout, input
  scheduling). Not on fire.

## The Phase 12c fix

**File:** `recomp/rt64/src/gbi/rt64_gbi.cpp`, lines 150–153.

**Symptom:** BH submits two microcodes per frame — F3DEX 1.21 for the main
scene and L3DEX 1.21 for the HUD/text overlay. The F3DEX path worked from
the start; the L3DEX path produced ~16 BOGUS `setColorImage` events, 4
DL runaway iter-cap events, ~16 RT64 framebuffer-copy rejections (3068×1022
siz=3, etc.), and intermittent OOB-RDRAM DL-pointer walks per playthrough.

**Root cause:** RT64's `GBIUCode` enum (in `rt64_gbi.h`) contains `L3DEX2`
(rev 2) but **no `L3DEX` (rev 1)**. The four `L3DEX_1_xx` entries in the
GBI database were therefore registered with `GBIUCode::Unknown = 0`. When
BH dispatched an L3DEX 1.21 task, `getGBIForUCode` returned the
`Unknown`-typed cached GBI. The switch initializing it has no
`case GBIUCode::Unknown` arm, so it fell to `default: assert(false)` —
which is a no-op in Release. `GBI_RDP::setup(&gbi, true)` had already run
just above the switch, but that only installs RDP-side handlers
(G_SETCIMG=0xFF, G_SETTIMG, G_LOADTLUT, G_SETCOMBINE, …). All RSP-side
handlers — G_VTX (0x04), G_DL (0x06), G_ENDDL (0xB8), G_MTX, G_MOVEWORD —
stayed `nullptr`.

The interpreter then dispatched the L3DEX DL with this half-populated map:

- G_DL (0x06) → nullptr handler → silent `dl++` (**no jump**)
- G_ENDDL (0xB8) → nullptr → silent `dl++` (**list never terminates**)
- G_VTX (0x04) → nullptr → silent `dl++`
- Any 8-byte word whose top byte happened to be 0xFF (vertex Z<0 in 16.16
  fixed-point packs that way) → `setColorImage` handler **was** installed
  → fired with garbage payload → BOGUS trap

The parser slid linearly through vertex/material data until the 256K
iter-cap or RDRAM-OOB guard saved the process. Each frame's L3DEX task
produced thousands of garbage commands.

**Fix:** L3DEX rev 1 = F3DEX rev 1 + line primitives. They share the same
F3D opcode numbering (G_VTX=0x04, G_DL=0x06, G_ENDDL=0xB8). Routing the
four L3DEX rev 1 instances through `GBIUCode::F3DEX` plumbs them into the
working F3DEX setup. Line primitives (G_LINE3D etc.) silently no-op via
the existing unknown-opcode path, which is fine for BH (it doesn't use
3D lines for HUD).

```cpp
// Before:
const GBIInstance L3DEX_1_00 = { "L3DEX 1.00", GBIUCode::Unknown, … };
const GBIInstance L3DEX_1_21 = { "L3DEX 1.21", GBIUCode::Unknown, … };
const GBIInstance L3DEX_1_23 = { "L3DEX 1.23", GBIUCode::Unknown, … };
const GBIInstance L3DEX_1_23_A = { "L3DEX 1.23 (Variant)", GBIUCode::Unknown, … };

// After:
const GBIInstance L3DEX_1_00 = { "L3DEX 1.00", GBIUCode::F3DEX, … };
const GBIInstance L3DEX_1_21 = { "L3DEX 1.21", GBIUCode::F3DEX, … };
const GBIInstance L3DEX_1_23 = { "L3DEX 1.23", GBIUCode::F3DEX, … };
const GBIInstance L3DEX_1_23_A = { "L3DEX 1.23 (Variant)", GBIUCode::F3DEX, … };
```

## How we narrowed it down

Two cheap diagnostic patches that landed before the fix and are still in
place — keep them in tree for next time something goes sideways:

| File | Edit |
|---|---|
| `recomp/rt64/src/gbi/rt64_gbi.cpp` | First 16 GBI selections print `[gbi] selected: ucode=N name="…" text=0x… data=0x… flags{…}` |
| `recomp/rt64/src/hle/rt64_state.cpp` | First 16 each of push / pop / pop-underflow on the DL return-address stack print `[stack] …` |

The smoking gun was that `[gbi] selected: ucode=0 name="L3DEX 1.21"`
appeared on the line **immediately before** the first BOGUS event, and
every iter-cap event was on the line immediately before the next switch
*back* to F3DEX — proving the runaway DLs were exactly the L3DEX ones.

## What's still instrumented

These are observability, not workarounds. Keep them until we have a
reason to remove:

| File | What it logs |
|---|---|
| `recomp/rt64/src/hle/rt64_interpreter.cpp` | First 8 DLs' start/end (`addr`, `ucode`, `w0/w1`, iteration count, exit reason). RDRAM-range guard + 256K iter-cap on the DL cursor. Never fired in the post-fix run. |
| `recomp/rt64/src/gbi/rt64_gbi_rdp.cpp` | `setColorImage` BOGUS trap (fmt>5) with surrounding DL context. `loadTLUTOperation` / `loadTileOperation` defer-and-skip on `textureStart > end`. Defers still fire 16× in the post-fix run — see "Open issues" §A. |
| `recomp/rt64/src/hle/rt64_state.cpp` | `[stack]` push/pop/underflow traces. |
| `recomp/rt64/src/gbi/rt64_gbi.cpp` | `[gbi] selected:` traces. |
| `recomp/bh-app/src/bh_renderer.cpp` | OOB gfx-task filter (rejects tasks with `data_ptr`/`ucode`/`ucode_data` outside the 8 MiB RDRAM window). Never fired in the post-fix run. |

## Open issues (in priority order)

### A — Residual OOB segment resolves (16 per run)

The post-fix log still contains 16 `[rsp/setTextureImage] OOB resolve`
events where `seg_table[N] = 0x00000000` for some N (seen: seg 5, 10,
13). These resolve to phys-addrs that look like garbage but RT64's
defer-and-skip path catches them — they don't crash. **Cosmetically the
game looks correct, so this is informational only**; the cap was raised
in Phase 12b to keep these from spamming the log. Worth chasing if a
later test reveals a missing-texture symptom.

Hypothesis: BH leaves certain segments unmapped between scene
transitions and the segments aren't getting reset on the right
boundary. Look at `rsp->setSegment` calls in `recomp/rt64/src/hle/`
and where BH issues `G_MW_SEGMENT` (handled by `GBI_F3D::moveWord`
case `G_MW_SEGMENT`).

### B — L3DEX rev 1 line primitives unhandled (low)

The Phase 12c fix routes L3DEX rev 1 through F3DEX, which means
`G_LINE3D` and similar L3DEX-specific opcodes fall into the
"unknown opcode" path and silently no-op. BH doesn't seem to use them
(nothing visibly missing in confirmed playthrough), but a proper fix
later would add a `GBIUCode::L3DEX` to the enum + a small
`rt64_gbi_l3dex.cpp` that reuses F3DEX setup and adds the line ops.
~30 minutes of work.

### C — Dev-disabled cheats: ✅ restored via R-keybind side-channel

**Status:** working. The six `z`-prefixed cheats (`zfarewell`,
`zwander`, `zaward`, `zsnared`, `zfreed`, `zdefender`) ship with the
game but were never typeable because no controller button produced
the `'z'` character that prefixes their patterns. The fix lives in
`bh-app/src/bh_cheats.cpp`:

- A side-channel hook runs at the end of `stub_poll_input`.
- On a fresh N64 R-trigger press (XInput RB or X), if BH's own
  `isCheatingEnabled` flag is set (player has loaded a save named
  `ICHEAT`), we shift `cheatInputBuffer[]` and write `'z'` at slot
  0 — exactly what BH's `addCharToCheatInputBuffer` would do.
- The recompiled mapper then runs as usual; its pattern matcher
  sees the new `'z'` in the buffer and fires the matching cheat.

Avoided L because L pops up the in-game objective screen and
swallows subsequent input. R is benign on player 1 (only used
for aiming a weapon).

Implementation notes for next time we touch the cheat plumbing:

- `isCheatingEnabled` (at virt 0x80149460) is a **u32**, not a u8.
  The recompiled `addCharToCheatInputBuffer` reads it with `lw`.
  Reading it as a single byte through the XOR-3 byte-swap trick
  always gives `0x00` (the MSB of host-little-endian word 1), so
  the gate appears stuck closed. Read as u32.
- `cheatInputBuffer` (at virt 0x80149450) IS a true `u8[10]`, so
  byte access via XOR-3 is correct for those writes.
- Captured live `rdram` pointer from a wrapper around the
  recomp entrypoint (`bh_recomp_entrypoint_with_rdram_capture`
  in `main.cpp`). Future host-side hooks that need to poke N64
  RAM by symbol can read `g_recomp_rdram.load(...)`.

Discovered effect during testing: `zaward` sets `D_80047FA0 = 5`
and the in-game message reads **"Completed All Levels"** —
flagging all 5 main missions as complete. The decomp had this
as "Unknown effect".

### D — Cheats menu: ✅ Win32 popup overlay (F1 toggle)

**Status:** working. All 21 cheats in BH's `cheatData[]` are listed
and individually activatable. Verified end-to-end: every cheat fires
when selected. Lives in `bh-app/src/bh_cheats.cpp` (Win32 dialog
code appended to the existing R-trigger keybind code).

How it works:

- A dedicated thread owns a modeless Win32 popup created via
  `CreateWindowEx` (no resource file). Listbox of 21 cheats, a
  description pane, and Activate / Close buttons.
- F1 toggle: detected in `stub_poll_input` via `GetAsyncKeyState`,
  posts `WM_BH_SHOW` / `WM_BH_HIDE` to the dialog thread.
- Activation: writes the cheat's pattern *reversed* into
  `cheatInputBuffer[0..N-1]`. BH's recompiled matcher walks the
  buffer such that `buffer[i]` must equal `pattern[N-1-i]`, so a
  reversed write is a match on the very next frame.
- **No ICHEAT gate on the menu path** (intentional — modding UI
  shouldn't require the save-name puzzle). The R-trigger keybind
  in §C still respects the gate so the original system stays
  vanilla for normal play.

Trade-off picked: Win32 popup (Explorer-styled window) over an
in-game ImGui overlay. Zero RT64 modifications, ~250 LOC self-
contained. If we later want it integrated into BH's pause menu
proper, the activation logic is reusable — we'd swap out the
dialog code for an ImGui overlay called from RT64's render path,
or a strong-override of BH's pause-menu draw function.

Known limitation:

- Cheat `annul` requires D-LEFT held when its function fires. The
  typed-input path satisfies this naturally (typing the trailing
  `'l'` IS pressing D-LEFT). The menu path doesn't, so rapid-tap
  D-LEFT while clicking Activate. Future fix: have the menu
  stamp a forced D-LEFT into the controller snapshot for the
  frame the cheat fires.

Documentation status: **all 21 cheats play-test-confirmed**.
Each entry in `kCheats[]` has its real effect + in-game toast
label, replacing the decomp's stale or speculative comments
(notable corrections: `snuffle` is NOT a model toggle but a
Serious-Weapons save flag; `alfa` spawns weapon pickups, not
aliens; `bleed` is a self-kill, not an ammo setter). Worth
upstreaming to the body-harvest-decompilation project as
comment cleanups in `cheats.c` and as symbol annotations in
`symbol_addrs.us.txt` (we identified `D_80157A3C` = player
Z-scale, `D_80052ACD` = packed save-data modifier flags,
`D_8004D148` = AI active flag).

Two design generations are visible in BH's cheats: an older
"set explicit value" style (paired cheats like `zsnared`/
`zfreed`, plus 5 of the 6 disabled z-cheats) and a newer
"XOR-toggle" style (`snuffle ^= 4`, `weasel ^= 2`,
`surreal ^= 0x80`). The z-prefix on the disabled cheats was
DMA's way of locking out debug tools without removing them —
no controller button produces `'z'`, so they could never be
typed in the shipped game.

### E — Custom (modder-added) cheats in the overlay menu

**Status:** working. The overlay's "── Custom (modder) cheats ──"
section currently has 6 entries, all implemented as direct RDRAM
pokes from bh-app/src/bh_cheats.cpp:

1. **Refill HP & Fuel (godlike, 0x7FFF)** — writes 0x7FFF to
   `D_80052B34->unk1C/unk3C`. Works in any vehicle or on foot.
2. **Refill HP & Fuel (natural, 0x258)** — writes 600 to
   `vehicleInstances[0]` directly. Player slot only; clean bar.
3. **All Items (incl. Hangar Key)** — writes 0xFFFF to the
   level-items bitmask at `0x8004DC4E`. Per-level scope: bitmask
   reloads on level transition. May fire NPC dialogue/cutscenes
   for items that trigger mission-progression events.
4. **Infinite Ammo (all 17 weapons)** — writes 0x8000 to every
   ammo counter at `0x80048146..0x80048166`. Covers player AND
   vehicle weapons.
5. **Reset Humans Killed (pacifist)** — zeros the counter at
   `0x8004816A`.
6. **Toggle Noclip Mode** — true noclip with no boundary clamping
   (bypasses both inner and outer shield walls). See "Noclip
   implementation" below.

Key addresses harvested from a community GameShark code page,
cross-referenced with the decomp's `VehicleInstance` struct:

| Symbol | Address | Width | Notes |
|---|---|---|---|
| Player slot HP | `0x8004DCEC` | s16 | vehicleInstances[0].unk1C, natural max 0x258 |
| Humans killed | `0x8004816A` | s16 | counter for no-civ-casualties ending |
| Items bitmask | `0x8004DC4E` | u16 | per-level; bit 0x0400 = Hangar Key |
| Weapon slots 1-8 | `0x80048138`+i (u8) | u8 × 8 | values 0x00-0x13 select weapon |
| Ammo counters | `0x80048146`+2i (s16) | s16 × 17 | player + vehicle weapons; 0x8000 = infinite |

### Noclip implementation (Phase 14c)

The Win32 menu's "Toggle Noclip Mode" toggles
`g_noclip_active`; while on, `stub_poll_input` in main.cpp calls
`apply_noclip_movement(rdram, stick_x, stick_y)` each input-poll
tick (~60Hz). The function:

1. Reads the **f32 cached position** at `unk4C/unk50/unk54`
   (BH's setX/setY/setZ helpers write these alongside the s16
   raw fields at `unk0/unk2/unk4`; the camera/render/physics
   paths read the f32 cache, so writing only the s16 produces
   "camera lags, axis-asymmetric movement" symptoms).
2. Reads the player's facing direction at `unkE` (s16, full
   360° spanning [-32768, 32767]; convention θ=0 = +X (east),
   increases clockwise).
3. Rotates the stick input by `theta` so forward-stick = direction
   the player is facing.
4. Reads PageUp/PageDown via `GetAsyncKeyState` for vertical
   movement.
5. Writes **both** the f32 cache and the s16 raw position fields.
6. Zeros velocity floats at `unk30/unk34/unk38`.
7. Forces the AIRBORNE flag (`unk20 |= 0x2`) so gravity/collision
   don't snap us back to the ground.

Known limitations (not blockers):

- **Movement tracks player rotation, not camera rotation.** When
  you swing the camera with C-buttons without moving, the player
  stays facing the old direction; your next "forward" push goes
  in that old direction and the player snaps to follow. Fix
  requires finding the camera-angle global (not in the named
  symbol map — needs memory-watch investigation or decomp dive).
- **Player rotation/facing isn't locked.** Pushing the stick still
  rotates the player model. Could lock to a fixed heading like
  zwander does if it bothers anyone (write `-0x4000` to unk6 + unkE
  each frame).
- Far out-of-bounds (well past ±27648) may cause terrain LOD
  issues or trigger "out of map" scripts. Don't save while there.

### F — Vehicle Morpher tool (Phase 14d)

**Status:** working as effectively-a-vehicle-spawner with one extra
manual step. Lives as the first entry in the menu's "── Tools ──"
section. Opens a dedicated sub-dialog with a level-aware vehicle
picker.

How it works:

- Reads `0x80047F93` (u8) for the current level index. Values:
  1=Greece, 2=Java, 3=America, 4=Siberia, 5=Comet.
- Looks up the per-level vehicle ID→name table (hardcoded from a
  community GameShark page in `kVehiclesGreece` / `kVehiclesJava` /
  `kVehiclesAmerica` / `kVehiclesSiberia` / `kVehiclesComet`).
- Writes the chosen u8 ID to `entity->unk1A` (spec index), where
  `entity` is `*D_80052B34` (the live current-entity pointer).
  This is strictly better than the GameShark code which hardcodes
  `0x8004DCEA` (only targets slot 0, no-op when in a vehicle).
- Sub-dialog has Refresh (re-read level), Morph (apply), Close.

Workflow for the "spawner" use case (cleanest):
1. Enter ANY vehicle.
2. F1 → Vehicle Morpher → pick target → Morph.
3. EXIT and RE-ENTER the vehicle.

The re-entry is the critical step: BH's vehicle-entry function
reads the spec at entry time and populates weapons + per-spec
state from `vehicleSpecs[]`. Morphing alone changes the visible
model but doesn't trigger that entry path, so weapons stay as
the old vehicle's until you re-enter. Morphed slots are
persistent — they stay as the new type even after you exit.

Morphing while on foot wraps the player in a temporary vehicle
(no weapons, despawns on exit). Useful for quick model swaps
but not for persistent acquisition.

Limitations:

- Collision volume comes from the spec at entity creation, not
  from current `unk1A`. Morphing a small car to a Howitzer keeps
  the small car's collision shape until something re-creates the
  entity. (Re-entering doesn't fully re-create — only weapons
  reload from spec.)
- "(Glitch)" / "Incomplete" entries in the tables are included
  for completeness but may misbehave.

Same UI pattern is ready to clone for:

- **Harvester / enemy replacer** (~30 min): change the spec of
  enemy slots using the same per-level table approach.
- **Level warp** (~30 min, risk of crash): write to 0x80047F93
  and 0x800D6D90-92 to jump to another level.

### F.2 — Quick Save (Phase 14e)

**Status:** working as a checkpoint feature. Triggered via the
"Quick Save (respawn at first beacon)" custom cheat. Saves all
state BH normally saves (stats, items, weapons, mission flags,
items bitmask). Respawn on load goes to the first save beacon
of the current level, NOT to the player's actual position when
the save fired.

Mechanism:

- The cheat hijacks `cheatData[18]` (the dundee dance slot) by
  rewriting its pattern to `"save"` and its funcPtr to BH's
  save function `func_80002CA4_38A4` (0x80002CA4).
- Writes the reversed pattern (`"evas"`) into `cheatInputBuffer`.
- BH's cheat matcher runs `activateCheat` for slot 18, which
  calls the redirected funcPtr on the game thread with a valid
  `recomp_context` — this is exactly what `func_80002CA4_38A4`
  needs.
- Side effect: dundee dance cheat no longer fires.

Why save-anywhere doesn't work (yet):

BH stores only a single byte in the save: `D_80047F9C` =
"which save beacon you activated + 1". On load, BH looks up
`beacon[N].position` from per-level beacon table (loaded from
level layout data, NOT save data) and spawns there. So even
when we hack `beacon[0].position` to match the player's current
location before saving (the cheat does this), the level-load
phase overwrites our hack with the level's original beacon
position. Tested and confirmed.

For true save-anywhere we'd need to either:

- Strong-override the post-load respawn function (find it first
  — probably one of `func_8000xxxx` in the spawn-on-level-load
  code path) to read player position from a custom file/RAM area
  we control.
- Persist position outside BH's save format (e.g. a sidecar
  file in our config dir) and apply it on load via the hook.

Tracked as Phase 14h.

### G.5 — Alien Morpher + Spawner Tool (Phase 14f)

**Status:** both functions working. Second entry in the menu's
"── Tools ──" section. Two actions in one sub-dialog:

- **Morph All:** walks `alienInstances[0..253]` and rewrites
  every alive slot's `specIndex` (offset 0x1A) to the chosen
  type. Active-alien count and "alive" check (`specIndex >= 2`)
  borrowed from the `annul` cheat. Some models render garbled
  initially because BH caches the model handle at spawn time —
  walk far enough to cull the alien then back, and the model
  refreshes correctly.
- **Spawn at Player:** replicates BH's
  `func_8007956C_8851C(u8 type)` in host-side RDRAM writes.
  Reads activeCount from `D_8014ECC8`, pulls a free slot from
  `D_8014D308[activeCount]`, copies the 0x50-byte template at
  `D_8013C1EC` into the slot, sets specIndex, reads HP from
  `alienSpecs[type] + 0x3A` (s16), reads spec flags at
  `alienSpecs[type] + 0x54` and sets the corresponding
  alien-flag bits, increments the counter, then writes player's
  current position (f32 cache at 0x4C/0x50/0x54 + s16 raw at
  0x00/0x02/0x04) to the new alien. All slot-allocation and
  template-init done direct from menu code — no recompiled
  function call needed.

Verified spawns: Harvester, Ooze, Fly, Big Ant, Flying
Scorpion, Mantis, Turret Bug, Exploding Frog, Bad Adam, Piranha
(works in any water-containing level, not just Java), Hopping
Turret, Shield Boss. Crashes after spawn: **Boss (0x1B)** —
shortly after spawning it fires at the player then crashes,
almost certainly due to a missing arena-bound entity that the
boss code assumes exists; the spawner deliberately skips the
recursive helper-spawn step (`func_80079510_884C0`) that the
original spawn function does for boss types.

Skipped vs the original spawn function:

- Recursive helper for type 0x19/0x1B (`alien->unk25 = func_8007956C_8851C(0)` + `func_80079510_884C0(slot)`).
  Harvesters spawn fine without it, but bosses crash without
  their companion entity.
- Level-5 piranha (0x14) special-case (`func_80088000_96FB0(-1)`).

### G.6 — Vehicle Spawner (PARTIAL; Phase 14g)

**Status:** spawn + render + entry work. Driving, reload, and
damage don't. Documented here in detail so future-you can pick up
without re-deriving everything.

What works:

- Free-slot allocation. Walk `vehicleInstances[1..127]` looking
  for `unk1A == 0` (unused). Same scan pattern as alien spawner.
- Template copy from slot 0 (Adam avatar) — the cleanest baseline.
- Spec-driven reset replicating `func_800FAE84_109E34` in
  `src.us/overlay_gameplay/outside/F9230.c` lines 3353-3389:
  zeros position/orientation/velocity/counter fields, sets
  `unk20 = spec->unk4C` (initial flags from spec), `unk1C =
  spec->hitPoints`, `unk3C = spec->unk61 << 8` (max fuel).
- Force `unk20 |= 0x8000` (active-vehicle bit, required for
  inclusion in `D_80158E80` active list).
- Force `D_80159320 |= 0x2000` (scene-flag rebuild bit, triggers
  `func_800FAD10_109CC0` next time it's checked, which walks all
  slots and rebuilds the `D_80158E80` active-vehicle index list).
- Position write to both s16 raw (unk0/2/4) and f32 cache
  (unk4C/50/54), mirroring BH's setX/setY/setZ helpers.
- Spawn-time HP and fuel show correctly in the HUD on entry.

What DOESN'T work:

- **Driving.** Engine sound plays briefly on entry then stops.
  Stick input doesn't translate to movement.
- **Weapon reload.** Player can fire ONCE — the reload counter
  doesn't tick down.
- **Damage.** Shooting the spawned vehicle from outside does
  nothing. Collisions don't process.

Hypothesis (un-tested):

The per-frame player-vehicle physics callback is stored in
`D_8015920C` (function pointer). It's normally set by the
category-init functions (`func_800FD2AC_10C25C` /
`func_800FD2F8_10C2A8` / `func_800FD344_10C2F4` /
`func_800FD390_10C340`) called from `func_800FD410_10C3C0`
during vehicle entry. Those functions all gate on:

```c
if (D_80052B34->unk3C != 0 && D_8015920C == 0) {
    D_8015930E = 1;
    D_8015920C = (s32)func_800FD0EC_10C09C;  // (or similar)
    D_80159310 = 0;
}
```

i.e. "if vehicle has fuel AND no callback set, install callback".
Most likely failure modes:

1. `D_8015920C` is non-zero from a prior vehicle session — the
   exit code doesn't fully reset it. When we enter, the `if (==
   0)` check fails so the callback is never installed for our
   slot.
2. Something on the frame after entry resets `D_8015920C` to a
   non-functional value (some "current player vehicle" state
   machine).
3. Category-init isn't running because of a state check we
   missed.

Things to try next session:

- Force `D_8015920C = 0` from our spawner before the user enters
  (so the entry's category-init runs and installs the callback).
- Strong-override one of the category-init functions to log
  what it sees in `D_8015920C` when entry runs on our vehicle.
- Diff `D_8015920C` value before/after entering a NORMAL vehicle
  vs entering our spawned one. The differential tells us what's
  missing.
- Look at the vehicle-exit code to see what it does to
  `D_8015920C` and reverse it on spawn.

Key addresses + functions:

| Item | Value |
|---|---|
| `vehicleInstances` | `0x8004DCD0` (size 0x2E00, 128 × 0x5C) |
| `vehicleSpecs` | `0x80257A00` (size 0x1380, 25-ish × 0x70) |
| `D_8014ECC8` | alien active counter (analog) |
| `D_80158E80` | vehicle active-index list (read by render + AI loops) |
| `D_80158FD8` | count of valid entries in D_80158E80 |
| `D_80159320 & 0x2000` | "rebuild D_80158E80 needed" dirty bit |
| `D_8015920C` | **player-vehicle physics callback pointer** (the missing link) |
| `D_8015930E` | flag set alongside D_8015920C install |
| `D_80158C58` | per-slot float array (zeroed by reset function) |
| `func_800FAD10_109CC0` | rebuilds D_80158E80 from slots with unk20 & 0x8000 |
| `func_800FAE84_109E34` | "reset vehicle slot from spec" — we replicate |
| `func_800FD510_10C4C0` | vehicle-entry function (player presses A) |
| `func_800FD410_10C3C0` | category-init dispatcher (called by entry) |
| `func_800FD2AC/2F8/344/390` | category-specific callback installers |

VehicleInstance fields confirmed:

| Offset | Field | Notes |
|---|---|---|
| 0x00/02/04 | s16 X/Y/Z (raw) | derived from f32 cache below |
| 0x1A | u8 specIndex | morpher writes this |
| 0x1C | s16 hitPoints | natural max from spec |
| 0x1E | s16 weapon-related | |
| 0x20 | u16 flags | 0x8000=active, 0x1=player-controlled |
| 0x30/34/38 | f32 velocity X/Y/Z | "Inc" |
| 0x3C | s16 fuel | (spec->unk61 << 8) at init |
| 0x4C/50/54 | f32 X/Y/Z cache | authoritative position |

VehicleSpec field offsets used:

| Offset | Field |
|---|---|
| 0x3A | u16 hitPoints |
| 0x4C | u32 initial flags template |
| 0x58 | u8 category (1/3/5/7 = different init paths) |
| 0x61 | u8 max-fuel raw |

### G.7 — F1 menu: tabs + resizable (Phase 15a)

**Status:** done. The F1 overlay window now has three native Win32
tabs and a resizable border, ready for the per-tab enhancement
controls to be filled in.

Layout:

- **Cheats** tab — existing listbox + description + Activate button.
  No functional change from earlier phases.
- **Enhancements** tab — placeholder text. Targets for Phase 15b+:
  fog distance slider, resolution scale, antialiasing, texture
  filtering, aspect ratio.
- **Options** tab — placeholder text. Targets: graphics API picker,
  refresh rate target, diagnostics log level.

Close button lives outside the tabs (always visible).

Implementation:

- `WC_TABCONTROLW` strip across the top, tabs inserted via
  `TCM_INSERTITEMW`. `WM_NOTIFY` + `TCN_SELCHANGE` triggers a
  `show_tab_controls(hwnd, tab_idx)` helper that walks the known
  control IDs and shows/hides per tab.
- Window has `WS_THICKFRAME`. `layout_overlay(hwnd)` reflows all
  child controls from the client rect on `WM_SIZE`. Min size is
  enforced via `WM_GETMINMAXINFO` (360×320).
- All control IDs grouped by tab in the `IDC_*` constants block;
  new tab = new ID + add a row to `show_tab_controls`.

To add a new enhancement control:

1. Define an `IDC_*` constant for the control.
2. Create it in `WM_CREATE` (initial position doesn't matter much
   — `layout_overlay` will reposition).
3. Add the control to the appropriate tab's visibility row in
   `show_tab_controls`.
4. Extend `layout_overlay` to give it a position computed from
   client width/height.
5. Handle its events in `WM_COMMAND` / `WM_NOTIFY`.
6. Plumb changes into RT64's `userConfig` or `enhancementConfig`
   (most rendering tunables already exist there).

### G.8 — Enhancements: aspect ratio + refresh rate (Phase 15b)

**Status:** working. Enhancements tab on the F1 overlay now has two
live-applied combo boxes:

- **Aspect Ratio**: 4:3 Original, 16:9, 16:10, 21:9, 32:9, Stretch
  to window. Selecting → `bh::renderer::set_aspect_ratio(idx)` →
  modifies `app->userConfig.aspectRatio/aspectTarget` → calls
  `app->updateUserConfig(true)` (true = discard FBs so RT64
  recreates at new aspect).
- **Refresh Rate**: Native (~30 Hz), 60, 75, 120, 144, Match
  display. Same pattern: `bh::renderer::set_refresh_rate(idx)` →
  modifies `app->userConfig.refreshRate/refreshRateTarget` →
  `app->updateUserConfig(false)`.

Plumbing — for future enhancement work:

- `bh_renderer.cpp` exposes `std::atomic<RT64::Application*> g_rt64_app`
  populated in the BHRendererContext constructor (cleared in
  destructor). The `bh::renderer::*` helpers read this and call
  RT64's `updateUserConfig` / `updateEmulatorConfig` /
  `updateEnhancementConfig` methods to push changes live. RT64's
  inspector uses the same pattern from its own UI thread, so
  cross-thread calling is safe.
- `bh_cheats.cpp` declares the helpers via forward declaration
  outside the anon namespace, keeps the RT64 dependency contained
  to `bh_renderer.cpp`.

Confirmed playtest observations (32:9 screenshot):

- 60 Hz rendering works with mostly clean motion interpolation;
  minor character-animation artifacts (ignorable).
- Aspect changes apply instantly without restart.
- **Wide aspects reveal expected limitations**:
  - HUD anchored to 4:3 coordinates (now floating in mid-screen
    on ultrawide instead of edge).
  - Visible terrain edge / map cutoff because BH's culling is
    sized for 4:3 view frustum.
  - No FOV expansion — projection matrix unchanged, so wider
    view looks slightly "telephoto'd" rather than truly wider.

Future enhancements (post-15b):

| Item | Effort | Notes |
|---|---|---|
| Try `Upscale2D::ScaledOnly` for HUD | 5 min | RT64 may already reposition 2D elements with this setting; surface it as a checkbox |
| Resolution scale combobox | 30 min | Just expose `userConfig.resolutionMultiplier` (1x, 2x, 4x) |
| MSAA / filtering pickers | 30 min each | All in `userConfig`, same pattern |
| Fog distance slider | 1.5 hr | Needs new `enhancementConfig.fog.scaleFactor` field in RT64 + handler in `rt64_gbi_f3d.cpp` |
| FOV correction at wide aspects | ?? | RT64 doesn't appear to expose this; would need investigation, possibly an FOV override hook |
| HUD anchor remapping | ?? | Needs intercept of 2D draw calls or BH-side HUD coord patching |

### G.9 — Enhancements: Upscale2D + Fog slider (Phase 15c)

**Status:** both shipped. Enhancements tab now has 4 controls:

- **Aspect Ratio** combo (Phase 15b, working).
- **Refresh Rate** combo (Phase 15b, working).
- **HUD / 2D Upscale** combo (Phase 15c-pre): `userConfig.upscale2D`
  with Original / ScaledOnly / All. Smooths 2D sprite + HUD pixels.
  Doesn't reposition HUD elements (BH-side coord patching would be
  required for that, deferred).
- **Fog Distance** trackbar slider (Phase 15c): exponential mapping,
  position 0..100 → scale 0.5x..8x with default at position 25
  (= 1.0x native). Plumbs to `enhancementConfig.fog.scaleFactor` via
  `bh::renderer::set_fog_scale()` → `app->updateEnhancementConfig()`.

RT64 modifications for fog (Phase 15c):

- New field `EnhancementConfiguration::Fog::scaleFactor` (float).
- New default `fog.scaleFactor = 1.0f` in
  `rt64_enhancement_configuration.cpp`.
- `G_MW_FOG` handler in `rt64_gbi_f3d.cpp` now divides incoming
  fog multiplier by `scaleFactor` before forwarding to RSP. This
  is what shifts the fog-density-vs-distance curve.

### G.10 — Open: render-distance / cull-line extension

Confirmed at high fog scale: even with fog effectively off, BH
shows a hard "cull line" at the far render distance. The N64
engine simply doesn't draw geometry past this point. Two paths
forward when/if we tackle it:

1. **Far-plane patch via recomp**. BH calls `guPerspective(...,
   near, far, ...)` somewhere with a hardcoded far. Find via
   grep/decomp, strong-override the call site (or the matrix-
   setup function) to substitute a larger far value. Risk: BH
   may cull on the CPU side as well, skipping draws for objects
   it knows are beyond render distance — matrix change alone
   wouldn't help then.

2. **RT64-side cull bypass**. Some N64 enhancements ignore
   GBI cull commands (`G_BRANCH_Z`, `G_CULLDL`) so RT64 draws
   everything regardless of distance flags. Quick to try via a
   toggle in `rt64_gbi_f3d.cpp` / `rt64_gbi_f3dex.cpp` but may
   destroy performance or expose other glitches.

Suggested order: try #2 first (one-line experiment), then #1
if it works. If neither, BH's level data may simply not contain
detailed geometry past the cull boundary — would need texture/
mesh detail to be added to BH's data, which is way out of scope.

### H — Multiplayer POC (Phase 16)

**Status:** working. Two-instance loopback over UDP. Each instance
sends its player position to the other; each instance renders the
other player as a "ghost" Bad Adam alien (type 0x12 — humanoid
+ stationary AI so it doesn't fight our position writes).

Files:

- `bh-app/src/bh_net.cpp` — the whole net module (init, tick, send/recv).
- `bh_cheats.cpp` — Options tab checkbox + `bh::spawn::alien_simple()`
  wrapper that the net module uses to allocate the ghost slot.
- `main.cpp` — commandline parsing (`--port`, `--peer-port`) +
  init call + per-frame `bh::net::tick(rdram)`.

Recipe:

```
Instance A:  bh_app.exe                                  (listen 12345 -> send 12346)
Instance B:  bh_app.exe --port 12346 --peer-port 12345
```

Both instances: F1 → Options → tick "Enable multiplayer POC". Each
sees the other player as a Bad Adam moving in real time through
its own copy of the world.

Notable details:

- **Packet format** is 24-byte tight-packed: magic + version +
  X/Y/Z (f32) + rot_y (s16) + seq (u16). ~1.5 KB/s each way at 60 Hz.
- **Ghost slot validation each tick**: BH repopulates
  `alienInstances[]` on level reload / save load, which can
  silently change what our cached `g_ghost_slot` index points at.
  `verify_ghost_slot()` checks the slot's specIndex still matches
  Bad Adam and forces re-spawn if not. Without this you'd see the
  ghost become a random level entity after reload.
- **Lazy ghost spawn**: only allocated on first received packet,
  not at toggle-on. So a solo instance with networking enabled
  doesn't spawn an extra alien.

What's NOT implemented (defer until needed):

- Game-state sync (missions, AI, terrain edits). Each instance is
  fully independent — only positions are exchanged.
- Smoothing / dead reckoning — raw position writes look jittery.
- Configurable ghost type (combobox picker for Bad Adam / Harvester
  / Big Ant / Alpha 1 vehicle etc.).
- LAN / WAN support — currently hardcoded loopback. Trivial swap
  to `inet_pton(AF_INET, "x.x.x.x", ...)` instead of `INADDR_LOOPBACK`.
- Connection-loss detection (ghost stays at last-known position forever).
- Authoritative host model — both instances are co-equals with
  independent worlds.

### Future cheat — "Play as Alien"

User-proposed: let the player BE an alien instead of Adam. Concrete
implementation sketch:

1. Hide `vehicleInstances[0]` (player avatar) — set invisible flag
   bits, or teleport it to a far-off "parking" coord each frame.
2. Spawn an alien at player position via `bh::spawn::alien_simple()`.
3. Route player input through `apply_noclip_movement`-style code
   but targeting the alien's `alienInstances[N]` slot instead of
   `D_80052B34`'s entity.
4. Camera-follow: either point camera at the alien directly, or
   keep slot 0 at the alien's position so the existing camera
   logic tracks correctly.
5. Optional — fire-button → triggers the alien's natural attack
   (harvester abduction, frog explosion, etc.).
6. UI: combobox of alien types (Harvester, Exploding Frog, Big Ant,
   Bad Adam, etc.) + on/off toggle.

Effort: ~3-4 hours for a basic version. Would build cleanly on
top of the existing spawn + noclip infrastructure.

### G — BH memory map (community GameShark + decomp cross-reference)

Most useful addresses gathered, beyond the basic struct offsets:

| Addr | Width | Meaning |
|---|---|---|
| `0x80047F93` | u8 | Current level (1=Greece..5=Comet) |
| `0x800D6D90-92` | 3 bytes | "Current Level" — alternative? |
| `0x800D6D98-A2` | bytes | Score values |
| `0x8004816A` | s16 | Humans-killed counter |
| `0x8004DC4E` | u16 | **Per-level item bitmask** (e.g. 0x0400 = Hangar Key in Greece) |
| `0x8004DC5E` | s16 | Alien artifact counter |
| `0x8004DCD0` | start | `vehicleInstances[0]` (player slot) |
| `0x8004DCEA` | u8 | Player slot spec index |
| `0x8004DCEC` | s16 | Player slot HP (natural max 0x258) |
| `0x8004DCEE` | u16 | "Can't Fire" flag |
| `0x8004DCF0` | u16 | Flash/Invisible flags |
| `0x8004DD20` | f32 | Player slot Y cache (height) |
| `0x8004E43C` | u8 | Infinite Fuel flag |
| `0x80048138-3F` | u8×8 | Weapon slot types (0x02 pistol .. 0x13 plasma bombs) |
| `0x80048146-66` | s16×17 | Per-weapon ammo (0x8000 = infinite via wraparound) |
| `0x80052ACC` | u16 | Bad Adam (=0xC2 turns player into the antagonist) |
| `0x80052ACD` | u8 packed | Save-data modifier flags: bit 0x02 weasel, bit 0x04 snuffle |
| `0x80052ACE` | u16 | Weapon Power Up (=0xFFFF; same bit family as snuffle) |
| `0x80157A3C` | u8 | Player Z-scale (banana=0x22 tall, dwarf=0x68 short) |
| `0x80157FF0` | u8 | Bugs Have No Legs cosmetic flag |
| `0x80159322` | u8 | Surreal Mode (bouncing buildings) |
| `0x80160104` | u8 | Camera Doesn't Move (debug) |
| `0x8013BD04` | s32 | Weak-bosses flag (`feeble`) |
| `0x8013FCD0` | s32 | `lard` flag (fat alien legs) |

Not yet captured: camera angle (needed for camera-relative
noclip), free vehicleInstances slots for true spawning,
mission-state granular flags.

### H — Pause-menu integration (stretch goal, not pursued yet)

If/when the Win32 popup outgrows its welcome, the in-game options are:

1. **ImGui overlay via RT64 hook.** Plumb a per-frame callback
   from RT64 into bh-app so we can `ImGui::Begin("Cheats")` from
   within RT64's existing ImGui frame lifecycle. Needs lazy
   Inspector creation (currently gated on `developerMode`) and
   probably a Win32 WndProc routing change for mouse clicks.
2. **Strong-override BH's pause menu.** Find the recompiled
   pause-menu draw + input handler in `bh-recomp/RecompiledFuncs/`,
   override them, add a "Cheats" entry that opens a sub-menu of
   the 21 entries. Most native-feeling but requires reading
   BH's menu rendering code.

Approach #2 is the modernization play (looks like part of the
game). Approach #1 is faster (modern UI library, less BH-code
spelunking). Either reuses §D's activation logic verbatim.

## What's known-good

- ✅ Boot, ROM validation, audio init.
- ✅ libultra startup; recomp::start enters main loop.
- ✅ Frame submission, VI buffer swaps.
- ✅ XAudio2 output (32 kHz stereo S16, non-silent samples).
- ✅ F3DEX 1.21 main-scene rendering.
- ✅ L3DEX 1.21 HUD/text rendering (via F3DEX fallback).
- ✅ Scene transitions (Greece → Java observed).
- ✅ Save/load (confirmed working end-to-end in playthrough).
- ✅ Input + event ordering (NPC dialogue, computer prompts, scripted
  events all fire correctly).
- ✅ In-game cheats (the ones the devs left enabled).
- ✅ Clean shutdown on WM_CLOSE.

## How to build + run

```bat
cd C:\BodyHarvestPC\recomp
build-rt64.bat       :: relinks rt64.lib (~5–15 s incremental, ~2 min cold)
build-bh-app.bat     :: relinks bh_app.exe (~5–30 s incremental, recomp\ funcs rebuild on header touch)

cd bh-app\build
bh_app.exe > stdout.txt 2> stderr.txt
```

Build scripts are in `C:\BodyHarvestPC\recomp\`. Both `.bat` files
prepend Clang to PATH and call vcvars64 — no need to source a dev shell
first.

## Files touched in Phase 12b–12c

For context if you're auditing diffs:

- `recomp/rt64/src/gbi/rt64_gbi.cpp` — the fix (lines 150–153) + GBI
  selection logging.
- `recomp/rt64/src/hle/rt64_state.cpp` — stack push/pop logging.
- `recomp/rt64/src/hle/rt64_interpreter.cpp` — per-DL diagnostics +
  RDRAM/iter guards.
- `recomp/rt64/src/gbi/rt64_gbi_rdp.cpp` — `setColorImage` BOGUS trap,
  texture OOB defer/skip.
- `recomp/bh-app/src/bh_renderer.cpp` — OOB gfx-task filter.

No edits touched the recompiled funcs in `recomp/bh-recomp/`.

— end of handoff (Phase 12c complete) —
