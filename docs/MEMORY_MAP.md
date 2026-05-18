# Body Harvest memory map

Every RAM address this toolkit reads from or writes to, with what
each address contains, how big it is, what it does, and what cheats
or features depend on it.

Addresses are KSEG0 virtual (the `0x80...` namespace BH itself uses
in C source). Most are documented either by the
[body-harvest-decompilation](https://github.com/) project (whose
research forms the foundation), by community cheat databases, or by
our own verification.

The live source of truth is the constants block at the top of
`bh-app/src/bh_cheats.cpp`. This doc is the human-readable extraction.

## Global game state

| Address | Type | Symbol | Purpose |
|---------|------|--------|---------|
| `0x80047F90` | u32 (enum) | `currentLevel` | 1=Greece, 2=Java, 3=America, 4=Siberia, 5=Comet |
| `0x80047F93` | u8 | (low byte of currentLevel) | Used by some legacy read paths |
| `0x80047F94` | u32 | `D_80047F94` | Active wall index — selects which wall in `D_80147C30` gets loaded into the working copy |
| `0x80047F9C` | u32 | `D_80047F9C` (savedBeaconId) | Beacon ID for save/respawn (saved to EEPROM as low byte; D_80047F9C = id + 1) |
| `0x80052ADC` | u32 (enum) | `gameplayMode` | 0=Map/menu, 1-9=in-game states, 0xA=end-of-level, 0x10=inventory |
| `0x80149460` | u32 | `isCheatingEnabled` | BH's cheat gate (1 = unlocked, 0 = locked). This toolkit forces it on. |
| `0x80149450` | u8[10] | `cheatInputBuffer` | Rolling input buffer for typed cheats (newest at index 0) |

## Player entity (current-controlled)

The pointer `D_80052B34` (at `0x80052B34`) holds a *virtual address*
pointing to the entity the player currently controls — either the
on-foot avatar (`vehicleInstances[0]`) or the VehicleInstance they're
driving. Dereference to get the entity, then add offsets below.

| Address | Type | Name | Purpose |
|---------|------|------|---------|
| `0x80052B34` | u32 ptr | `D_80052B34` (currentEntityPtr) | Points to the active player entity |

### VehicleInstance offsets (player's current entity)

| Offset | Type | Name | Purpose |
|--------|------|------|---------|
| `0x00` | s16 | `unk0` (posX) | X position (truncated from f32 cache at +0x4C) |
| `0x02` | s16 | `unk2` (posY) | Y position (height, derived) |
| `0x04` | s16 | `unk4` (posZ) | Z position (truncated from f32 cache at +0x54) |
| `0x0E` | s16 | `unkE` (direction) | Yaw, 0x10000 = 360° |
| `0x1A` | u8 | `unk1A` (specIndex) | Vehicle type ID — index into `vehicleSpecs[]` |
| `0x1C` | s16 | `unk1C` (hitPoints) | HP — the value `durable` cheat fills, `bleed` cheat zeros |
| `0x20` | u16 | `unk20` (flags) | Bit field: 0x0002=AIRBORNE, 0x0040=in-shield, 0x0800=in-building, 0x8000=ACTIVE (renderable) |
| `0x30` | f32 | `unk30` (velX) | Velocity X ("Inc X") |
| `0x34` | f32 | `unk34` (velY) | Velocity Y |
| `0x38` | f32 | `unk38` (velZ) | Velocity Z |
| `0x3C` | s16 | `unk3C` (fuel) | Fuel level |
| `0x4C` | f32 | `unk4C` (cacheX) | AUTHORITATIVE X position (camera + physics read these) |
| `0x50` | f32 | `unk50` (cacheY) | Authoritative Y |
| `0x54` | f32 | `unk54` (cacheZ) | Authoritative Z |

BH's `setX` / `setY` / `setZ` helpers (`func_800FB44C` / `_800FB468` /
`_800FB484`) write BOTH the s16 mirrors AND the f32 caches. Cheats
that only update the s16 fields cause the camera to lag behind and
movement effectively snaps back. Always write both.

## Player inventory + ammo

| Address | Type | Purpose |
|---------|------|---------|
| `0x80048138` | u8[8] | `weaponSlots[]` — 8 player inventory slots (weapon/item ID per slot) |
| `0x80048146` | s16[18] | Global ammo counters, 1 per weapon ID (see [WEAPONS.md](WEAPONS.md)) |
| `0x8004816A` | s16 | `humansKilled` — civilian casualty counter |
| `0x8004DC4E` | u16 | Per-level items bitmask (artifacts, hangar key, etc.) |
| `0x8004DCEA` | u8 | `vehicleInstances[0].unk1A` — spec index for player's main slot |
| `0x8004DCEC` | s16 | `vehicleInstances[0].unk1C` — HP for player's main slot |

## Vehicle pool

| Address | Type | Purpose |
|---------|------|---------|
| `0x8004DCD0` | VehicleInstance[128] | `vehicleInstances[]` — 128 slots × 0x5C bytes each |
| `0x80158C58` | (per-slot floats) | Per-vehicle scratch float array |
| `0x80257A00` | VehicleSpec[N] | `vehicleSpecs[]` — per-type configuration, 0x70 bytes each |
| `0x80158E80` | u8[N] | Active vehicle render list (filled by `func_800FAD10` each frame) |
| `0x80159320` | u32 | Scene flags (bit 0x2000 = "rebuild render list") |

### VehicleSpec offsets (per-type config at `vehicleSpecs[]`)

| Offset | Type | Purpose |
|--------|------|---------|
| `0x3A` | u16 | Max HP at spawn |
| `0x4C` | u32 | Initial flag template (OR'd into unk20 on spawn) |
| `0x61` | u8 | Fuel max (shifted << 8 for runtime) |

## Alien pool

| Address | Type | Purpose |
|---------|------|---------|
| `0x80048198` | AlienInstance[254] | `alienInstances[]` — 254 slots × 0x50 bytes each |
| `0x8014ECC8` | u32 | Active alien count |
| `0x8014D308` | u8[256] | Alien slot list (free-slot tracker) |
| `0x8013C1EC` | u8[0x50] | Spawn template (copied into new alien slots) |
| `0x80256680` | AlienSpec[N] | Per-type configuration, 0x68 bytes each |

### AlienSpec offsets

| Offset | Type | Purpose |
|--------|------|---------|
| `0x3A` | u16 | Max HP |
| `0x54` | u32 | Flag template |

## Buildings

| Address | Type | Purpose |
|---------|------|---------|
| `0x80050AD8` | BuildingInstance[255] | `buildingInstances[]` — 255 slots × 0x18 bytes each |

### BuildingInstance offsets

| Offset | Type | Purpose |
|--------|------|---------|
| `0x00` | s16 | X coord |
| `0x02` | s16 | Y coord |
| `0x04` | s16 | Z coord |
| `0x06` | u8 | Building type (0 = empty slot) |
| `0x08` | u32 | Flags (state, rotation, animation packed) |
| `0x0F` | u8 | Hit points |
| `0x12` | u8 | Door 1 interior ID (0 = no door) |
| `0x13` | u8 | Door 2 interior ID |
| `0x14` | u8 | Door 3 interior ID |

## Terrain

| Address | Type | Purpose |
|---------|------|---------|
| `0x8014F8A0` | ptr→s16[256][256] | Dynamic terrain tile grid pointer. Each tile is a 16-bit value: bits 0-5 = height (0-63), bits 6-9 = state flags, bits 10-15 = texture/type |
| `0x80148620` | Beacon[14] | Save beacons (8 bytes each, fields: building slot link, level, etc.) |

## Camera / view

| Address | Type | Purpose |
|---------|------|---------|
| `0x80052B2C` | u32 ptr | Camera struct pointer |
| `0x80149434` | s16 | Camera-render center X (used by entity bbox cull) |
| `0x80149436` | s16 | Camera-render center Z |
| `0x80222A70` | f32 | Water surface Y level (see "Water level" below) |
| `0x80222A72` | s16 | Lower half of D_80222A70 (alias access) |
| `0x801411A4` | f32 | Camera far clipping plane |
| `0x8014FD2A` | u16 | Camera FOV — sentinel value 0x8000 disables frustum cull entirely |
| `0x80157590` | s16 | "Camera status" — non-zero bypasses several culling paths |

### Why the cull-related addresses matter

This toolkit's "Render all entities" cheat writes `D_80157590 = 1`
and `D_8014FD2A = 0x8000` per tick. Per BH's master entity cull
function `func_800B93AC_C835C` (in
`body-harvest-decompilation/src.us/.../BF9C0.c:1428`), either of those
non-zero conditions causes the cull to short-circuit and pass all
entities. Same applies to the building cull `func_800B960C_C85BC`.

The bbox cull `func_800703B0_7F360` checks (camera_X ± 0x800,
camera_Z ± 0x900) — a roughly 15×15-tile rectangular bbox. We
override this function entirely via the weak-symbol linkage trick,
replacing it with a configurable version driven by the Entity Render
Distance slider.

The terrain LOS occlusion `func_800E95BC_F856C` ray-marches from
camera to entity sampling terrain heights — if the LOS dips below
terrain at any sample, the entity is "occluded". We override this
too so distant entities aren't hidden behind hills.

## Water

| Address | Type | Purpose |
|---------|------|---------|
| `0x80222A70` | s32 (s16 alias at +2) | Water surface Y. Lower = water below map (drained). Negative values cause the in-game map screen to render water over the whole map. |

## Shield walls + gates

| Address | Type | Purpose |
|---------|------|---------|
| `0x80147C30` | u8[5][6][0x18] | Per-level shield wall source table (6 walls × 0x18 bytes per level × 5 levels) |
| `0x8014FD30` | (struct) | ACTIVE wall working copy (BH copies one wall from source here per region) |
| `0x8003E0EE` | s16[6+] | Per-level shield-wall render-iteration count. Indexed by `currentLevel`. Writing 0 hides the wall mesh entirely. |
| `0x8003E0FC` | GateEntry[5][8] | Per-level shield-wall gate (portal) data (10 bytes each) |

### Wall struct (Unk8014FD30Type, 12 × s16 = 0x18 bytes)

| Offset | Name | Purpose |
|--------|------|---------|
| 0x00 | minX | Bounding box min X (s16 world coord) |
| 0x02 | minZ | Bounding box min Z |
| 0x04 | maxX | Bounding box max X |
| 0x06 | maxZ | Bounding box max Z |
| 0x08-0x0E | gate1 | Gate 1 opening rect (x0, z0, x1, z1) |
| 0x10-0x16 | gate2 | Gate 2 opening rect (often unused) |

Wall coords are stored in world units (s16, signed). The `>> 10`
shift used in BH's collision code converts to a 64-cell grid index
relative to the world center.

### GateEntry struct (10 bytes per slot)

NOTE: the decomp's struct comments label offsets (0, 2, 4) as
(X, Z, Y) but the actual game code in `func_800BD688_CC638` uses
them as (X, Y, Z). The corrected layout:

| Offset | Type | Purpose |
|--------|------|---------|
| 0x00 | s16 | `unk0` — X coord, compressed (world_X = stored << 8) |
| 0x02 | s16 | `unk2` — Y coord, raw |
| 0x04 | s16 | `unk4` — Z coord, compressed (world_Z = stored << 8) |
| 0x06 | s8 | `unk6` — animation state (0 = closed, 0x50 = fully open) |
| 0x07 | u8 | `unk7` — unknown |
| 0x08 | u8 | `unk8` — unknown |
| 0x09 | u8 | `unk9` — gate type. Setting gate 0's type to 2 makes the gate render loop bail immediately, hiding all portals. |

## Cheat data table

| Address | Type | Purpose |
|---------|------|---------|
| `0x8013B940` | CheatEntry[21] | BH's cheat table (16 bytes each: pattern + function pointer + flags) |
| `0x80002CA4` | function | BH's save-game function (target of the `dundee` slot hijack used by Quick Save) |

### CheatEntry offsets

| Offset | Type | Purpose |
|--------|------|---------|
| 0x00 | char[10] | Pattern bytes (reversed; matches against `cheatInputBuffer`) |
| 0x0A-0x0B | u16 | Flags |
| 0x0C | u32 | Function pointer (called when pattern matches) |

## Bookmark file format

The bookmark system persists state to `bh_bookmarks.json` in the
exe's directory. See the file's auto-generated content for the exact
format — top-level keys are `magic`, `version`, `slot_count`,
`slots[]`. Each slot has `label`, `timestamp`, `level`, `level_name`,
`x/y/z`, `yaw`, `vehicle_spec`, `hp`, `fuel`, `items_bitmask`,
`weapon_slots[8]`, `ammo[18]`.

Empty slots are written as JSON `null`.

## Notes for adding new addresses

When you discover a new address worth poking from a cheat or
override, add it as a constexpr near the top of
`bh-app/src/bh_cheats.cpp`:

```cpp
constexpr uint32_t kMyNewAddr = 0x80XXXXXX; // u8/s16/u32 + what it does
```

Then use it via the existing `read_n64_*` / `write_n64_*` helpers,
which handle the recompiler's byte-swap conventions:

- `read_n64_u32` / `write_n64_u32` — aligned 32-bit, host-native order (no swap)
- `read_n64_s16` / `write_n64_s16` / `read_n64_u16` / `write_n64_u16` — halfword, XOR-2 swap
- `read_n64_u8` / `write_n64_u8` — byte, XOR-3 swap
- `read_n64_f32` / `write_n64_f32` — aligned float, host-native (no swap)

Update this doc when adding a new address category — the source
constants are authoritative but this doc is what people will read
first.
