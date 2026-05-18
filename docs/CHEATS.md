# Body Harvest cheat catalog

A comprehensive list of every cheat exposed by this toolkit, organized
into the three sources: BH's built-in cheats (21 of them), the custom
cheats we added on the host side, and the sub-dialog tools.

The source of truth for all descriptions is the `kCheats[]`,
`kCustomCheats[]`, and `kTools[]` arrays in
`bh-app/src/bh_cheats.cpp`. This doc mirrors that content for easy
reference.

## Built-in BH cheats (21)

These are the cheats baked into Body Harvest itself. Originally
activated by typing the pattern letter-by-letter into the cheat input
buffer; this toolkit lets you fire any of them with a single click.

Patterns are documented because some lists online have them wrong —
`zfarewell`, `zsnared`, `zfreed`, `zaward`, `zwander`, `zdefender`
are all *dev cheats* with a `z` prefix that BH normally blocks unless
you've saved with a specific name. This toolkit bypasses that gate
and exposes them all.

| Pattern | Effect | Toast |
|---------|--------|-------|
| `annul` | Smart Bomb: kill all on-screen aliens. Requires D-LEFT held when function fires; from menu, rapid-tap D-LEFT while clicking Activate. | "Smart Bomb" |
| `zfarewell` | Skip to next level | — |
| `arsenal` | Give every weapon available in current level | "Weapon Cheat" |
| `durable` | Heal player; if in vehicle, heal vehicle and refill fuel | "Health Cheat" |
| `zwander` | Noclip/flight: player floats at fixed height; stick moves in WORLD coords (forward=north regardless of camera), Z to land | "Wandering" |
| `snuffle` | Toggle Serious Weapons flag (saved to EEPROM). Save + reload to see upgrades (pistol→laser, MG→homing missiles) | "Serious Weapons Cheat" |
| `zaward` | Mark all levels complete (replay maps unlock) | — |
| `zsnared` | Freeze ALL AI (aliens AND humans; bullets still hit, deaths don't process) | "Aliens Snared" |
| `zfreed` | Resume all AI (pair with zsnared — clears D_8004D148) | "Aliens Freed" |
| `alfa` | Spawn weapon/ammo pickups around player | "Welfare Cheat" |
| `surreal` | Cosmetic: all buildings bounce/squish forever | — |
| `zdefender` | Invulnerability shield against most attacks | "Invulnerability" |
| `bleed` | Self-kill (deals 0x7FFF damage to player). Blocked by zdefender | "Death Cheat" |
| `suffer` | Force any present harvesters to become mutants (rapid re-activation can spawn multiple mutants per slot) | "Mutant Cheat" |
| `weasel` | Player model becomes Black Adam antagonist (persists in save) | "Bad Cheat" |
| `useful` | Give all 3 alien artifacts for current level (shows in inventory menu) | "Artifacts Cheat" |
| `banana` | Cosmetic: player 2× tall | "sack cheat" |
| `dwarf` | Cosmetic: player half height | "Dwarf Cheat" |
| `dundee` | Cosmetic: player performs short dance animation | — |
| `lard` | Cosmetic: alien legs 2× size | "Fat Legs Cheat" |
| `feeble` | Weaken ALL enemies (most become 1-shot kills; bosses die in a couple shots) | "Big Blouse Cheat" |

### How BH's cheat gate works

BH stores cheat eligibility in `D_80149460` (`isCheatingEnabled`, u32).
It only enables cheats if your save file name contains a magic phrase
(commonly "ICHEAT"). This toolkit bypasses that gate by setting the
flag in RAM whenever you press the F1 hotkey — no save-file renaming
needed.

The `z`-prefixed cheats also require this gate to be open. They're
labeled "dev cheats" because BH normally hides them; with the gate
forced open via the toolkit, they all work.

## Custom cheats (host-side additions)

These don't exist in BH's `cheatData[]` table — they're implemented
on the host side via direct RDRAM reads/writes. Most operate
atomically on small field sets (safe under any game state); a few
require per-tick re-application (handled by the input poll).

### Quick stats / inventory

| Name | Effect |
|------|--------|
| Refill HP & Fuel (godlike, 0x7FFF) | Set current entity's HP and fuel to INT16 max. Bar overshoots visually but you're effectively immortal until 32k cumulative damage. Works on foot AND in any vehicle. |
| Refill HP & Fuel (natural, 0x258) | Set on-foot player HP to 600 (BH's natural max). Bar renders cleanly. Only useful when on foot — vehicles have varying max HP. |
| All Items (incl. Hangar Key) | Write 0xFFFF to the level-items bitmask. Per-level — re-apply after switching levels. May fire mission-progression dialogue. |
| Infinite Ammo (all 18 weapons) | Write 0x8000 to each ammo counter at 0x80048146..0x80048168. Covers player weapons AND Alpha 1 weapons (Chaingun, Fragcannon, Lazer Missiles, Resonator, Plasma Bombs). |
| Reset Humans Killed (pacifist) | Zero the humans-killed counter at 0x8004816A. For the no-civilian-casualties ending after an accidental run-over. |

### Movement

| Name | Effect |
|------|--------|
| Toggle Noclip Mode | True noclip — no boundary clamping (bypasses both inner and outer shield walls, unlike `zwander`). Left stick = horizontal movement, rotated by your facing. PageUp/PageDown = ascend/descend. Velocity zeroed each frame, AIRBORNE flag forced. |

### Terrain manipulation

All operate on BH's 256×256 tile grid at `D_8014F8A0`. World origin
is at tile (128, 128); 1 tile = 256 world units. Height encoded in
bits 0-5 (0..63). Visual updates are lazy — walk out of the chunk
and back for BH to re-tessellate.

| Name | Effect |
|------|--------|
| Crater at player (4-tile radius) | Zeros a circle of tiles, dropping them to sea level. ~9×9 patch. |
| Flat terrain (zero entire grid) | Zeros every tile. Strips state bits (pits, walls, collision markup). Experimental — save first. |
| Raise terrain +2 (3-tile radius) | Adds 2 to height bits of every tile in a 3-tile circle. Stack for hills. |
| Lower terrain -2 (3-tile radius) | Subtracts 2. Stack for basins. |
| Match terrain to player Y | Reads your Y position, converts to tile-height (Y/32 per BH's internal scale), paints a 5-tile circle. Build a bridge under your feet. |
| Match terrain to local height | Sample the tile under the player, paint that height to a 5-tile circle. Flattens the local area without needing Y math. |
| Smooth terrain at player (5-tile, 3×3) | Two-pass blur: snapshot original heights, replace each tile with the 3×3 average. Stack for smoother results. |

### Shields

| Name | Effect |
|------|--------|
| Disable shield walls (current level) | Three-layer disable: (1) makes walls collision-trivial (full-world bbox + degenerate gate triggers the cull's "return 0" path), (2) sets the shield render count to 0, (3) flags the gate renderer to skip on iteration 0. Walls become invisible AND non-blocking. Per-tick re-apply survives building entry/exit. |

### Water level (Enhancements tab + cheats)

| Name | Effect |
|------|--------|
| Drain water (sink below map, level-wide) | Forces water Y (`D_80222A70`) to -2000. Re-applied every tick. Some submarine areas may behave strangely. |
| Restore water (release water override) | Snapshot-based restore of the pre-override water level. |

### Render distance (mostly Enhancements tab sliders)

| Name | Effect |
|------|--------|
| Toggle: Render all entities | Bypasses BH's frustum cull (writes `D_80157590=1`, `D_8014FD2A=0x8000`) AND overrides the entity bbox cull (weak-symbol replacement of `func_800703B0_7F360`) AND the terrain LOS occlusion (weak-symbol replacement of `func_800E95BC_F856C`). All loaded entities render regardless of distance/frustum. NOTE: terrain horizon still limits visible world — vehicles past the streamed area don't appear because they're not loaded as instances. |

### Save state / bookmarks

| Name | Effect |
|------|--------|
| Save State to file (bh_savestate.bin) | Full 8 MB RDRAM snapshot to disk. v2 header includes level + gameplayMode for context-mismatch detection. KNOWN ISSUE: saving while moving produces a torn snapshot — load when standing still for reliability. |
| Load State from file | Restore 8 MB RDRAM. Rejects mismatched level / gameplay_mode to avoid overlay-pointer-dangling crashes. |
| Bookmark position (pos + state, slot 0) | Capture player position + yaw + level + vehicle spec + HP + fuel + 18 ammo counters + 8 weapon slots + items bitmask into slot 0. Atomic field writes — safe at any time. |
| Teleport to bookmark (slot 0) | Restore from slot 0. Same-level only. Zeros velocity to prevent flying-off. |

### Per-level quick save

| Name | Effect |
|------|--------|
| Quick Save (respawn at first beacon) | Hijacks `cheatData[18]` (`dundee` slot) at runtime to redirect to BH's save function. Saves stats/items/weapons; respawn happens at the first save beacon. Side effect: the `dundee` dance cheat is permanently replaced. |

## Tool sub-dialogs (Tools section)

These open separate Win32 popup windows. Each has its own README in
the source comment block.

### Vehicle Morpher / Spawner…

- Level-aware vehicle picker
- **Morph**: rewrite your current entity to the chosen type (pair with exit + re-enter for proper weapons)
- **Spawn × N**: allocate free `vehicleInstances[]` slots and place vehicles in a ring around you (PARTIAL: vehicles render and can be entered but don't drive — physics callback `D_8015920C` not getting wired up to our slot)

### Alien Morpher / Spawner…

- Global alien type list
- **Morph All**: rewrite every alive slot's specIndex to the chosen type (instant chaos)
- **Spawn × N**: allocate slots via BH's slot list, copy template, place in a 192-unit ring (or exactly at player for N=1)
- WARNING: spawning type 0x1B (the level-1 boss) crashes shortly after spawn

### Bookmarks Manager…

- 10-slot multi-bookmark system
- Save / Teleport / Clear per slot
- Detail pane showing all captured state per slot
- Persisted to `bh_bookmarks.json` (auto-save on every change, lazy-load on first access)
- "Reload from disk" button if you edit the JSON externally

### Weapon Editor…

- 8 weapon slots, each with dropdown listing all 18 known weapon IDs
- Live ammo edits (per weapon ID into the global ammo table)
- Items bitmask edit (hex or decimal)
- Read from game / Apply to game buttons
- See `WEAPONS.md` for the full ID table

## Enhancements tab (sliders, not cheats per se)

| Control | Range | Effect |
|---------|-------|--------|
| Aspect Ratio | 4:3 / 16:9 / 21:9 / 32:9 | RT64 viewport widening. HUD stays anchored at 4:3 coords. |
| Refresh Rate | Native / 60 / 120 / 144 | Render frequency (game tick stays at original rate) |
| 2D Upscale | Original / Smooth | Smooths HUD icons + sprites |
| Fog Distance | 0.5× .. 8× (exponential) | Multiplies BH's fog scale factor |
| Water Level Override | 0 .. +2000 (clamped non-negative — negative values cause map-screen artifact) | Forces water Y. "Release override" restores the snapshot. |
| Entity Render Distance | 1× .. 16× (exponential) | Multiplies the entity bbox cull dimensions. Note: terrain horizon is still the visible cap. |
| Far Plane | 1× .. 16× (exponential) | Multiplies BH's camera far plane (`D_801411A4`). Snapshotted on first slider move, restored on Release. |

## Options tab

| Control | Effect |
|---------|--------|
| Enable multiplayer POC | Loopback UDP between two instances on local ports. Each instance sees the other player as a ghost alien (Bad Adam). No game-state sync. |
| Export current level (terrain) | Dumps 256×256 tile grid + shield wall + portal data to JSON. |
| Import from file (terrain) | Restore tile data. Cross-level imports require confirmation. |
| Export entities (buildings, vehicles, aliens, beacons) | Dumps all active entity positions + types to a separate JSON. Read-only. |
