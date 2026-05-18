// Phase 13: cheat-keybind side-channel.
//
// Body Harvest's cheat system maps each N64 button to one letter
// (A→'a', B→'b', Z→'f', D-pad → l/r/u/d, C-buttons → w/s/n/e). The
// devs disabled six cheats by giving them patterns that start with
// 'z' — a character no button on the real pad produces, so those
// cheats can never be typed:
//
//   zfarewell  zwander  zaward  zsnared  zfreed  zdefender
//
// This file restores them by adding a single new keybinding on the
// host side: when the player presses the N64 R trigger (XInput X
// or RB), we inject a 'z' into BH's cheatInputBuffer the same way
// addCharToCheatInputBuffer would.
//
// We avoid L on purpose: L is bound to the in-game "show current
// objective" screen, which interrupts gameplay and swallows the
// next several inputs. R has essentially no in-game action for
// player 1 (one obscure suppress-while-held check in 1416E0.c
// and a controller-2 title-screen check), so pressing it briefly
// to type cheat letters is invisible.
//
// Why a side-channel and not a strong override of
// func_80073B78_82B28 (the recompiled mapper)? Because the mapper
// also runs the pattern-match loop every frame; overriding it
// means reimplementing that loop too, which duplicates logic that
// the recomp output is already correctly producing. By instead
// injecting into the buffer from the same thread that BH polls
// input on (the input thread feeding stub_poll_input — same
// cadence as osContStartReadData), we let BH's mapper do all the
// matching itself and only contribute the missing letter.
//
// Cheats are still gated by BH's `isCheatingEnabled` flag, which
// only flips to 1 when the active save slot is named "ICHEAT".
// We respect that gate here so this binding is no more permissive
// than the original system.
//
// Symbols (from body-harvest-decompilation/symbol_addrs.us.txt):
//   currentControllerStates = 0x80047588  // OSContPad[4]
//   cheatData               = 0x8013B940  // 21 × 16-byte entries
//   cheatInputBuffer        = 0x80149450  // 10 bytes, rolling
//   isCheatingEnabled       = 0x80149460  // u8 (effective)
//
// N64 RAM stored in our rdram[] is big-endian-as-the-N64-sees-it,
// so byte access from the host needs an XOR-3 on the offset.

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

// recomp.h gives us recomp_context (gpr/fpr registers + state) — needed
// to define a strong-symbol override of a recompiled function. The
// recompiled BH functions are emitted with `RECOMP_FUNC` which expands
// to `extern inline __attribute__((weak, noinline))` under clang, so
// defining a strong (non-weak) version with the same name wins at link
// time. We use this to replace BH's entity-bbox cull
// (`func_800703B0_7F360`) with a wider/configurable one.
#include "recomp.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>   // GetSaveFileNameW / GetOpenFileNameW for terrain export/import

namespace {

constexpr uint32_t kVirtBase           = 0x80000000u;
constexpr uint32_t kCheatInputBuffer   = 0x80149450u;
constexpr uint32_t kIsCheatingEnabled  = 0x80149460u;
constexpr size_t   kCheatBufferLen     = 10;
constexpr uint16_t kN64_BUTTON_R       = 0x0010;

// D_80052B34 holds a *virtual address* pointer to the entity the
// player is currently controlling — either their on-foot avatar or
// the VehicleInstance they're driving. BH's own `durable` cheat
// dereferences it the same way and reads/writes unk1C/unk3C, so we
// can do the same from the host side.
constexpr uint32_t kCurrentEntityPtr   = 0x80052B34u;
constexpr uint32_t kEntityOffsetSpecIdx= 0x1Au;  // VehicleInstance.unk1A  (u8 spec/type index)
constexpr uint32_t kEntityOffsetHP     = 0x1Cu;  // VehicleInstance.unk1C  (s16 hitPoints)
constexpr uint32_t kEntityOffsetFuel   = 0x3Cu;  // VehicleInstance.unk3C  (s16 fuel)

// Position + physics offsets within VehicleInstance / on-foot avatar.
// See body-harvest-decompilation/include/structs.us.h ~line 631 for
// the documented field meanings.
constexpr uint32_t kEntityOffsetPosX    = 0x00u; // s16 X (truncated copy of f32 cache at 0x4C)
constexpr uint32_t kEntityOffsetPosY    = 0x02u; // s16 Y (height) — derived
constexpr uint32_t kEntityOffsetPosZ    = 0x04u; // s16 Z (truncated copy of f32 cache at 0x54)
constexpr uint32_t kEntityOffsetDir     = 0x0Eu; // s16 "Direction" (yaw, 0x10000 = 360°)
constexpr uint32_t kEntityOffsetFlags   = 0x20u; // u16 flags (VEHICLE_FLAG_AIRBORNE = 0x2)
constexpr uint32_t kEntityOffsetVelX    = 0x30u; // f32 velocity X ("Inc X")
constexpr uint32_t kEntityOffsetVelY    = 0x34u; // f32 velocity Y
constexpr uint32_t kEntityOffsetVelZ    = 0x38u; // f32 velocity Z

// The f32 *cache* fields the camera + physics actually read. BH's
// setX / setY / setZ helpers (func_800FB44C / 800FB468 / 800FB484)
// write BOTH the s16 raw fields above AND these f32 caches. If you
// only update the s16s, the camera/render lag behind and movement
// effectively snaps back. Always update both halves.
constexpr uint32_t kEntityOffsetCacheX  = 0x4Cu; // f32
constexpr uint32_t kEntityOffsetCacheY  = 0x50u; // f32
constexpr uint32_t kEntityOffsetCacheZ  = 0x54u; // f32

constexpr uint16_t kVehicleFlagAirborne = 0x0002u;

// Addresses recovered from a GameShark code page for BH (US). These
// are stable known-good locations confirmed by the code-page author
// and cross-referenced with the decomp where possible.
constexpr uint32_t kPlayerVehSlotHP    = 0x8004DCEC; // vehicleInstances[0].unk1C  (s16, "Infinite Health" target)
constexpr uint32_t kHumansKilled       = 0x8004816A; // s16 humans-killed counter
constexpr uint32_t kItemsBitmask       = 0x8004DC4E; // u16 level-progression items bitmask
                                                     //   0x0002 Howitzer Shells
                                                     //   0x0040 Crank
                                                     //   0x0080 Windmill Cog
                                                     //   0x0100 Hieroglyph Map Piece
                                                     //   0x0400 Hangar Key (likely vehicle unlock!)
                                                     //   0xFFFF All
constexpr uint32_t kWeaponSlotBase     = 0x80048138; // u8 × 8 weapon slots (player carries these)
constexpr uint32_t kAmmoSlotBase       = 0x80048146; // s16 × 17 per-weapon ammo counters

// Level + vehicle-morpher state (from community GameShark codes).
constexpr uint32_t kLevelByte          = 0x80047F93; // u8 current level: 1=Greece, 2=Java, 3=America, 4=Siberia, 5=Comet
constexpr uint32_t kVehicleSpecIndex   = 0x8004DCEA; // vehicleInstances[0].unk1A — u8 spec index of player's current vehicle

// Alien morpher state. AlienInstance shares the same specIndex
// offset (0x1A) as VehicleInstance — and per the decomp comment,
// alien specs are GLOBAL (same across all levels). So one flat
// type table works everywhere, no per-level dropdown needed.
constexpr uint32_t kAlienInstancesBase = 0x80048198;
constexpr uint32_t kAlienInstanceSize  = 0x50;
constexpr int      kAlienInstanceCount = 254;        // 0x4FB0 / 0x50
constexpr uint8_t  kAlienAliveMinSpec  = 2;          // per annul cheat, specIndex>=2 means "alive alien"

// Direct-RAM alien spawner — replicates what func_8007956C_8851C
// does, doing everything via RDRAM writes so we don't need to call
// into the recompiled code. References to:
//   D_8014ECC8       u8  active-alien counter
//   D_8014D308       u8  next-slot list (256 entries of slot indices)
//   D_8013C1EC       0x50-byte template AlienInstance copied into
//                    the freshly-allocated slot
//   alienSpecs       per-type definitions at 0x80256680, 0x68 each.
//                    hp at offset 0x3A (s16), flags at 0x54 (s32).
constexpr uint32_t kAlienActiveCount     = 0x8014ECC8;
constexpr uint32_t kAlienSlotList        = 0x8014D308;
constexpr uint32_t kAlienTemplate        = 0x8013C1EC;
constexpr uint32_t kAlienSpecsBase       = 0x80256680;
constexpr uint32_t kAlienSpecSize        = 0x68;
constexpr uint32_t kAlienSpecHpOffset    = 0x3A;
constexpr uint32_t kAlienSpecFlagsOffset = 0x54;

// Vehicle spawner constants. vehicleInstances has 128 slots
// (0x2E00 / 0x5C). Slot 0 is the on-foot player avatar — always a
// valid VehicleInstance shape. Unused slots have unk1A (specIndex)
// == 0, the same value that means "Adam (on foot)" for slot 0.
//
// `func_800FAD10_109CC0` in src.us/overlay_gameplay/outside/F9230.c
// walks all 128 slots and builds D_80158E80 (the active-vehicle
// index list) from those with `unk20 & 0x8000` set. That's the
// "this slot is an active vehicle" bit, required for physics ticks
// + render. After rebuilding, it clears D_80159320 & ~0x2000
// (the "rebuild needed" dirty bit). So our spawner needs to:
//   1. Set 0x8000 on the new slot's unk20
//   2. Set 0x2000 on D_80159320 so BH re-runs the rebuild
constexpr uint32_t kVehicleInstancesBase = 0x8004DCD0;
constexpr uint32_t kVehicleInstanceSize  = 0x5C;
constexpr int      kVehicleInstanceCount = 128;
constexpr uint32_t kVehicleActiveFlagBit = 0x8000u;
constexpr uint32_t kSceneFlagsAddr       = 0x80159320;
constexpr uint32_t kSceneFlagsRebuildBit = 0x2000u;

// vehicleSpecs at 0x80257A00, per-spec size 0x70.
// Field offsets relevant to spawn-init (mirrors func_800FAE84_109E34
// in src.us/overlay_gameplay/outside/F9230.c lines 3353-3389).
constexpr uint32_t kVehicleSpecsBase     = 0x80257A00;
constexpr uint32_t kVehicleSpecSize      = 0x70;
constexpr uint32_t kVehicleSpecFlagsOff  = 0x4C;   // u32 initial flag template
constexpr uint32_t kVehicleSpecHpOff     = 0x3A;   // u16 max HP
constexpr uint32_t kVehicleSpecFuelOff   = 0x61;   // u8 fuel-max raw (shifted << 8 for init)

// Per-vehicle-slot float array (zeroed by the reset function).
constexpr uint32_t kVehiclePerSlotFloats = 0x80158C58;

// Dynamic terrain tile grid. D_8014F8A0 is a pointer (set at level
// load) to an s16[256][256] tile array. Each tile = 256 world units.
// World origin = tile (128, 128). Bits 6-9 of each tile encode a
// "state" enum; `0x380` is the Java "fell in pit" marker, observed
// to look hole-ish elsewhere too. See
// src.us/overlay_gameplay/outside/BF9C0.c:1958 and lines 482-490 for
// BH's own tile-modify helpers.
constexpr uint32_t kTerrainTilesPtr = 0x8014F8A0;
constexpr int      kTerrainW        = 256;
constexpr int      kTerrainH        = 256;
constexpr uint16_t kTilePitBits     = 0x0380u;
constexpr uint16_t kTileStateMask   = 0x03C0u;  // bits 6-9

// currentLevel is a 32-bit `enum Level` (1=Greece, 2=Java, 3=America,
// 4=Siberia, 5=Comet) at 0x80047F90. Per
// body-harvest-decompilation/include/variables.us.h:55-61. Used by
// the terrain export/import to tag and validate save files.
constexpr uint32_t kCurrentLevelAddr = 0x80047F90;
inline const char* level_name(uint32_t lvl) {
    switch (lvl) {
        case 1: return "Greece";
        case 2: return "Java";
        case 3: return "America";
        case 4: return "Siberia";
        case 5: return "Comet";
        default: return "Unknown";
    }
}

// Shield wall + shield gate (portal) tables. Per BH's
// body-harvest-decompilation/include/variables.us.h:
//   D_80147C30_156BE0 — u8[] base; per-level stride 0x90, 6 walls of
//                       0x18 bytes each. Wall struct is 12 × s16:
//                       minX, minZ, maxX, maxZ,
//                       gate1_x0, gate1_z0, gate1_x1, gate1_z1,
//                       gate2_x0, gate2_z0, gate2_x1, gate2_z1.
//                       Coords stored such that `value >> 10` produces
//                       a 0..63 grid index over D_8021EA30; the
//                       bounding-box X values are NOT shifted by callers
//                       in some code paths, the Z values are. We export
//                       the raw s16 values verbatim and let the editor
//                       decide how to project them.
//   D_8003E0FC GateEntry[5][8] — per-level shield-wall gate positions,
//                       0x0A bytes each: world X (s16), world Z (s16),
//                       world Y (s16), gate open-state byte, type
//                       byte. World coords are direct, no shift.
constexpr uint32_t kShieldWallsBase = 0x80147C30;
constexpr uint32_t kShieldWallsLevelStride = 0x90;
constexpr uint32_t kShieldWallSize         = 0x18;
constexpr int      kShieldWallsPerLevel    = 6;

constexpr uint32_t kShieldGatesBase = 0x8003E0FC;
constexpr uint32_t kShieldGateSize  = 0x0A;
constexpr int      kShieldGatesPerLevel = 8;

// Building instances. Per body-harvest-decompilation/include/structs.us.h
// BuildingInstance is 0x18 bytes:
//   0x00 s16 xCoord  (world coords, direct)
//   0x02 s16 yCoord
//   0x04 s16 zCoord
//   0x06 u8  buildingType    (0 = empty slot / not placed)
//   0x07 u8  unk7
//   0x08 u8  isDestroyable
//   0x09 u8  unk9
//   0x0A u8  state           (animation/destroyed state)
//   0x0B u8  rotation        (8-bit angle, 0..0xFF spans 360°)
//   0x0F u8  hitPoints
//   0x10 u8  unk10
//   0x11 s8  unk11
//   0x12 u8  door1InteriorId (interior level ID; 0 = no interior)
//   0x13 u8  door2InteriorId
//   0x14 u8  door3InteriorId
// Symbol map says buildingInstances size is 0x23DC; with 0x18 stride
// that's 0x17E (382) slots' worth of bytes — but the public header
// declares the array as [0xFF] (255). We iterate 255 to match the
// header and treat coords-all-zero + type-zero as "not placed".
constexpr uint32_t kBuildingInstancesBase = 0x80050AD8;
constexpr uint32_t kBuildingInstanceSize  = 0x18;
constexpr int      kBuildingInstanceCount = 255;

// Entity render cull. Per BH's master cull function
// func_800B93AC_C835C in BF9C0.c:
//
//   if (D_80157590 != 0 || D_8014FD2A == 0x8000) return 1;
//   // ... frustum tests using D_8014FD2A as FOV ...
//   if (dist < 0xFA0) return 1;  // < 4000 world units = render
//   return 0;                     // too far/outside frustum = cull
//
// So setting D_8014FD2A (the FOV "view angle") to 0x8000 triggers the
// early-bypass that returns "render" unconditionally — every active
// entity passes the cull regardless of distance or camera orientation.
// This is the same value BH sets itself in func_800B33BC_C236C when
// the camera angle exceeds 0x2E39, so the renderer already handles
// this state correctly (it's "360° render-everything" mode).
//
// D_8014FD2A is recomputed each camera-update tick by BH, so the
// override has to re-write every input-poll to win the race between
// BH's FOV recompute and the cull test.
constexpr uint32_t kCullFovAddr     = 0x8014FD2A;
constexpr uint16_t kCullFovBypass   = 0x8000;

// Camera far clipping plane (f32). Per body-harvest-decompilation/.../
// 7F220.c:741, BH's main outside-gameplay perspective is set via
//   guPerspective(..., fovy, 1.3333334f, 10.0f, D_801411A4, 1.0f);
// where the 5th argument is the far Z. Extending this lets the GPU
// rasterize farther geometry — both terrain AND entities — past
// BH's vanilla horizon. Once we have more far plane, terrain
// streaming becomes the next bottleneck (terrain won't be there to
// render unless the streaming window also extends), but entities
// at distance (e.g. spawned via cheat) should now render rather
// than getting Z-clipped.
constexpr uint32_t kFarPlaneAddr    = 0x801411A4;
// Secondary cull bypass: the cull function checks
// `D_80157590 != 0 || D_8014FD2A == 0x8000` so writing 1 to
// D_80157590 ALSO triggers the bypass. D_80157590 is BH's "outside
// camera status" and is read by many systems with specific values
// (0 = normal, 3 = alternate, 4 = ?), so writing 1 (an unused
// value) shouldn't hit those branches but it might still trigger
// "!= 0" checks in 1416E0.c / 7F220.c / F6A50.c. Used as a more
// aggressive override when the FOV-only approach loses the race
// against BH's per-frame FOV recompute.
constexpr uint32_t kCullStatusAddr  = 0x80157590;

// Water level. Per body-harvest-decompilation/include/variables.us.h:
//   extern s32 D_80222A70;
//   extern s16 D_80222A72;       (lower-half alias of the s32 above)
// Used by:
//   - DrawVtxBufferWater (ASM-only func_800BA5B0_C9560) - the water
//     surface renderer
//   - func_800BD688_CC638: `if (height < D_80222A70) height = D_80222A70`
//     clamps a height fetch upward to the water level (so under-water
//     terrain is treated as water surface for some checks)
//   - Java/Greece level scripts: `(s16)(D_80222A70 + 0x96)` to spawn
//     entities at water-level + offset
//
// Setting D_80222A70 to a low value drops the water surface below the
// map terrain — visually "drains" the level. Per-level scripts may
// set the water level on state changes (Java boss lowers it), so the
// disable cheat re-applies each tick like the shield disable.
constexpr uint32_t kWaterLevelAddr  = 0x80222A70;
constexpr int32_t  kWaterDisabledY  = -2000;   // well below the s16 world Y range BH uses

// D_8014FD30 is the *active wall working copy* — BH's collision code
// reads from this single struct, not from the per-level table above.
// func_800B0C80_BFC30 copies one entry from D_80147C30 into D_8014FD30
// based on D_80047F94 (the current wall index for the player's region).
// Crossing a region boundary triggers a copy. So to fully disable
// walls, we have to neuter D_8014FD30 too, not just the source table.
constexpr uint32_t kActiveWallAddr  = 0x8014FD30;
constexpr uint32_t kCurrentWallIdx  = 0x80047F94;

// D_8003E0EE is an s16 array indexed by currentLevel — controls how
// many shield-wall draw-elements the renderer iterates. The ASM in
// func_800BB5E0_CA590 (DrawShieldWalls) does `t8 = D_8003E0EE[lvl] * 8;
// if (t8 <= 0) skip;` — so zeroing the per-level entry hides shield
// visuals entirely. This is INDEPENDENT of the collision data (which
// lives in D_80147C30 / D_8014FD30), so we can disable visuals
// without touching gameplay logic that depends on wall presence.
// The array is also used by 40720.c on level start to initialize
// D_80047F94, but that read happens once at load time — patching
// mid-session doesn't retroactively affect gameplay state.
constexpr uint32_t kShieldVisCountBase = 0x8003E0EE;

// "Save game now" custom cheat plumbing. We hijack cheatData[18]
// (the 'dundee' slot — a cosmetic dance animation, the most
// expendable BH cheat) by overwriting its pattern + funcPtr at
// runtime, then firing the cheat via the normal matcher path.
// BH's matcher runs activateCheat on the game thread with a valid
// recomp_context, which is what func_80002CA4_38A4 (the save
// function) needs to run safely.
constexpr int      kSaveCheatSlotIndex = 18;
constexpr uint32_t kCheatDataBase      = 0x8013B940;
constexpr uint32_t kCheatEntrySize     = 16;
constexpr uint32_t kCheatEntryFuncPtrOffset = 12;
constexpr uint32_t kSaveGameFunctionVirt    = 0x80002CA4;

// Save-beacon table + active-beacon ID. BH's save format only
// stores a beacon-ID byte (D_80047F9C) — on load, BH looks up
// beacon[ID].position and spawns the player there. For
// save-anywhere, we hijack beacon 0: write player's current X/Z
// + current level, set D_80047F9C = 1, then trigger save. The
// open question is whether the beacon table persists across
// save/load (lives in save data) or gets re-loaded from per-level
// layout (clobbers our hack). The test reveals which.
constexpr uint32_t kBeaconArrayBase    = 0x80148620; // Unk80148620 array (size 0x08 each, 14 entries)
constexpr uint32_t kBeaconStride       = 8;
constexpr int      kBeaconHijackIndex  = 0;          // hijack beacon 0
constexpr uint32_t kSavedBeaconId      = 0x80047F9C; // u32; serialized to save as low byte (D_80047F9C = id + 1)

// Natural HP max for the on-foot player slot (per the GameShark
// "Infinite Health" code value). Vehicles' natural max varies by
// type — we use the godlike 0x7FFF for the generic refill.
constexpr int16_t  kPlayerNaturalHP    = 0x0258;     // 600
constexpr int16_t  kGodlikeHP          = 0x7FFF;     // overflows bar but never depletes
constexpr int16_t  kInfiniteAmmoMagic  = int16_t(0x8000); // INT16_MIN; wraps on decrement

// Byte access into RDRAM. The recompiler stores aligned 32-bit
// words in host-native (little-endian on x86) order. To access a
// single byte that BH treats as part of a big-endian-on-N64 word,
// XOR the low two bits of the byte offset with 3 — same trick the
// recompiler's MEM_BU macro uses.
inline uint8_t read_n64_u8(const uint8_t* rdram, uint32_t virt) {
    return rdram[(virt - kVirtBase) ^ 3u];
}

inline void write_n64_u8(uint8_t* rdram, uint32_t virt, uint8_t v) {
    rdram[(virt - kVirtBase) ^ 3u] = v;
}

// Word access into RDRAM. Mirrors the recompiler's MEM_W macro: no
// byte-swap, because aligned word accesses are stored in host-native
// byte order. Required for u32 fields like isCheatingEnabled — if
// we read those byte-by-byte with the XOR-3 trick we'd be looking
// at the MSB of a host-little-endian word, which is always 0 for
// small values like 0 or 1.
inline uint32_t read_n64_u32(const uint8_t* rdram, uint32_t virt) {
    uint32_t v;
    std::memcpy(&v, rdram + (virt - kVirtBase), sizeof(v));
    return v;
}

// Halfword writes use the same XOR-2 byte-swap the recompiler's
// MEM_H macro does — that's how BH-side `entity->unk1C = X` lays
// out the int16 in our host-side rdram buffer.
inline void write_n64_s16(uint8_t* rdram, uint32_t virt, int16_t v) {
    std::memcpy(rdram + ((virt - kVirtBase) ^ 2u), &v, sizeof(v));
}

inline int16_t read_n64_s16(const uint8_t* rdram, uint32_t virt) {
    int16_t v;
    std::memcpy(&v, rdram + ((virt - kVirtBase) ^ 2u), sizeof(v));
    return v;
}

inline uint16_t read_n64_u16(const uint8_t* rdram, uint32_t virt) {
    uint16_t v;
    std::memcpy(&v, rdram + ((virt - kVirtBase) ^ 2u), sizeof(v));
    return v;
}

inline void write_n64_u16(uint8_t* rdram, uint32_t virt, uint16_t v) {
    std::memcpy(rdram + ((virt - kVirtBase) ^ 2u), &v, sizeof(v));
}

// Aligned 32-bit writes don't need byteswap (host-native order, like MEM_W).
inline void write_n64_f32(uint8_t* rdram, uint32_t virt, float v) {
    std::memcpy(rdram + (virt - kVirtBase), &v, sizeof(v));
}

inline void write_n64_u32(uint8_t* rdram, uint32_t virt, uint32_t v) {
    std::memcpy(rdram + (virt - kVirtBase), &v, sizeof(v));
}

inline float read_n64_f32(const uint8_t* rdram, uint32_t virt) {
    float v;
    std::memcpy(&v, rdram + (virt - kVirtBase), sizeof(v));
    return v;
}

// One-time announce when we first inject so the log shows the
// keybind is wired up. Capped so a held button can't spam the log.
std::atomic<uint64_t> g_z_inject_count{0};

// Separate counter for "R press detected but gate closed", so we
// can distinguish "binding never fires" from "binding fires but
// isCheatingEnabled is 0 because save isn't named ICHEAT".
std::atomic<uint64_t> g_r_press_blocked_count{0};

} // namespace

namespace bh::cheats {

// Inject 'z' into BH's cheatInputBuffer at index 0, shifting the
// existing 9 characters one slot toward index 9. Mirrors what the
// recompiled addCharToCheatInputBuffer (func_80073A20_829D0) does,
// minus the recomp-context dance — we're on the input thread, so
// touching rdram bytes directly is safer than borrowing a stale
// recomp_context.
//
// No-op if rdram isn't ready yet or if BH hasn't enabled cheating
// (save slot != "ICHEAT").
void inject_z_if_r_pressed(uint8_t* rdram, uint16_t buttons_now) {
    static bool s_r_prev = false;

    if (rdram == nullptr) {
        // Game thread hasn't entered the recomp entrypoint yet —
        // rdram capture not done. Reset our edge detector so the
        // first real press is treated as a press.
        s_r_prev = false;
        return;
    }

    const bool r_now = (buttons_now & kN64_BUTTON_R) != 0;
    const bool rising_edge = r_now && !s_r_prev;
    s_r_prev = r_now;

    if (!rising_edge) {
        return;
    }

    // Respect BH's own enable flag — only inject when the player
    // has unlocked cheating by naming a save "ICHEAT". The flag is
    // a 32-bit word per the recompiled addCharToCheatInputBuffer's
    // `lw $t7, -0x6BA0($t7)` access pattern, so we must read it as
    // a u32 not a u8 (a u8 read of the MSB would always see 0).
    const uint32_t cheating_enabled = read_n64_u32(rdram, kIsCheatingEnabled);
    if (cheating_enabled == 0) {
        const uint64_t n = g_r_press_blocked_count.fetch_add(1, std::memory_order_relaxed);
        if (n < 4) {
            std::fprintf(stderr,
                "[cheats] R pressed but isCheatingEnabled=0 — name a save 'ICHEAT' "
                "and reload it to unlock the cheat system (block#%llu)\n",
                (unsigned long long)(n + 1));
        }
        return;
    }

    // Shift cheatInputBuffer[9..1] = cheatInputBuffer[8..0], then
    // write 'z' into slot 0. Walk from high index down so we don't
    // clobber what we're about to read.
    for (size_t i = kCheatBufferLen - 1; i > 0; --i) {
        uint8_t b = read_n64_u8(rdram, kCheatInputBuffer + uint32_t(i - 1));
        write_n64_u8(rdram, kCheatInputBuffer + uint32_t(i), b);
    }
    write_n64_u8(rdram, kCheatInputBuffer, uint8_t('z'));

    const uint64_t n = g_z_inject_count.fetch_add(1, std::memory_order_relaxed);
    if (n < 4) {
        std::fprintf(stderr,
            "[cheats] R pressed -> injected 'z' into cheatInputBuffer (event#%llu)\n",
            (unsigned long long)(n + 1));
    }
}

} // namespace bh::cheats

// ===========================================================================
// Phase 14: Win32 cheat-overlay window.
//
// Pressing F1 toggles a modeless popup listing all 21 cheats from
// BH's cheatData[] table, with a button that activates the selected
// one. Activation writes the cheat's pattern (reversed) into
// cheatInputBuffer; on the next frame BH's existing recompiled
// matcher fires the corresponding cheat function.
//
// The menu intentionally bypasses BH's isCheatingEnabled gate — that
// gate is the "name a save ICHEAT" puzzle and is preserved for the
// R-button typing path, but a modder UI shouldn't require it.
//
// Implementation notes:
// - The window runs on its own thread with its own message pump
//   (we can't piggyback on the main thread, which is parked inside
//   recomp::start). F1 detection runs on the input-poll thread and
//   forwards show/hide via PostMessage.
// - Mouse + keyboard input to the dialog work because it owns its
//   own HWND; the game window isn't affected.
// - All RDRAM access is mediated through the same byte-XOR helpers
//   the R-trigger path uses, so we mirror BH's byte ordering.
// ===========================================================================

namespace {

// One row of the cheats table. Pattern strings are how BH's matcher
// sees them in memory (lowercase, null-terminated, max 10 chars).
// Descriptions are summarised from body-harvest-decompilation/src.us/
// overlay_gameplay/outside/cheats.c, then corrected/expanded by
// play-testing each one and reading the in-game toast banner.
//
// Two design generations are visible in this table:
//
//   - "Set explicit state" cheats (older / debug-era): each cheat
//     unconditionally assigns a known value to a global. To toggle
//     the feature off you need a SECOND cheat that sets a different
//     value. Used by 5 of the 6 z-prefixed cheats (zfarewell,
//     zaward, zsnared/zfreed pair, zwander) and a handful of the
//     enabled ones — looks like the original debug-tool style.
//   - "XOR toggle" cheats (later / user-facing): one cheat XORs a
//     flag bit, so re-typing the cheat is the off switch too.
//     Used by snuffle (^= 4), weasel (^= 2), surreal (^= 0x80).
//     Cleaner UX for player discovery.
//
// The 'z' prefix on the six disabled cheats was DMA's way of
// shipping the binary intact while locking the dev tools out —
// no controller button produces 'z', so they can't be typed.
// We restored access on the host side: §C wires N64 R-trigger
// to 'z' for in-game typing, §D bypasses the typing entirely.
struct CheatInfo {
    const char* name;          // pattern as it appears in cheatData[].pattern
    const char* description;
};

constexpr CheatInfo kCheats[] = {
    { "annul",      "Smart Bomb: kill all on-screen aliens. Requires D-LEFT held when function fires; from menu, rapid-tap D-LEFT while clicking Activate. Toast: \"Smart Bomb\"" },
    { "zfarewell",  "Skip to next level" },
    { "arsenal",    "Give every weapon available in current level. Toast: \"Weapon Cheat\"" },
    { "durable",    "Heal player; if in vehicle, heal vehicle and refill fuel. Toast: \"Health Cheat\"" },
    { "zwander",    "Noclip/flight: player floats at fixed height; stick moves in WORLD coords (forward=north regardless of camera), Z to land. Toast: \"Wandering\"" },
    { "snuffle",    "Toggle Serious Weapons flag (saved to EEPROM). Save + reload to see upgrades (pistol->laser, MG->homing missiles). Toast: \"Serious Weapons Cheat\"" },
    { "zaward",     "Mark all levels complete (replay maps unlock)" },
    { "zsnared",    "Freeze ALL AI (aliens AND humans; bullets still hit, deaths don't process). Toast: \"Aliens Snared\"" },
    { "zfreed",     "Resume all AI (pair with zsnared — set/clear D_8004D148). Toast: \"Aliens Freed\"" },
    { "alfa",       "Spawn weapon/ammo pickups around player (toast: \"Welfare Cheat\")" },
    { "surreal",    "Cosmetic: all buildings bounce/squish forever" },
    { "zdefender",  "Invulnerability shield against most attacks (toast: \"Invulnerability\")" },
    { "bleed",      "Self-kill (deals 0x7FFF damage to player). Blocked by zdefender. Toast: \"Death Cheat\"" },
    { "suffer",     "Force any present harvesters to become mutants (rapid re-activation can spawn multiple mutants per slot). Toast: \"Mutant Cheat\"" },
    { "weasel",     "Player model becomes Black Adam antagonist (persists in save; toast: \"Bad Cheat\")" },
    { "useful",     "Give all 3 alien artifacts for current level (shows in inventory menu). Toast: \"Artifacts Cheat\"" },
    { "banana",     "Cosmetic: player 2x tall (in-game toast: \"sack cheat\")" },
    { "dwarf",      "Cosmetic: player half height (in-game toast: \"Dwarf Cheat\")" },
    { "dundee",     "Cosmetic: player performs short dance animation" },
    { "lard",       "Cosmetic: alien legs 2x size (toast: \"Fat Legs Cheat\")" },
    { "feeble",     "Weaken ALL enemies (most become 1-shot kills; bosses die in a couple shots). Toast: \"Big Blouse Cheat\"" },
};
static_assert(sizeof(kCheats) / sizeof(kCheats[0]) == 21,
              "BH cheatData[] has exactly 21 entries");

// Custom cheats — not in BH's cheatData[] table. These are
// implemented entirely on the host side via direct RDRAM writes
// (or in future, deferred recomp-function calls). Activation routes
// through `activate` instead of writing into cheatInputBuffer, so
// they don't depend on BH's matcher.
struct CustomCheat {
    const char* name;
    const char* description;
    void (*activate)(uint8_t* rdram);
};

// Forward declarations of custom activators (defined below).
void custom_refill_hp_fuel_godlike(uint8_t* rdram);
void custom_refill_hp_fuel_natural(uint8_t* rdram);
void custom_all_items(uint8_t* rdram);
void custom_infinite_ammo(uint8_t* rdram);
void custom_reset_humans_killed(uint8_t* rdram);
void custom_toggle_noclip(uint8_t* rdram);
void custom_save_game_now(uint8_t* rdram);
void custom_crater_at_player(uint8_t* rdram);
void custom_flatten_terrain(uint8_t* rdram);
void custom_raise_terrain_step(uint8_t* rdram);
void custom_lower_terrain_step(uint8_t* rdram);
void custom_match_terrain_to_player_y(uint8_t* rdram);
void custom_match_terrain_to_local(uint8_t* rdram);
void custom_smooth_terrain(uint8_t* rdram);
void custom_disable_shield_walls(uint8_t* rdram);
void custom_drain_water(uint8_t* rdram);
void custom_restore_water(uint8_t* rdram);
void custom_toggle_render_cull_bypass(uint8_t* rdram);
void custom_save_state(uint8_t* rdram);
void custom_load_state(uint8_t* rdram);
void custom_bookmark_position(uint8_t* rdram);
void custom_teleport_to_bookmark(uint8_t* rdram);

// Terrain export/import buttons (Options tab). Return true on success.
// On failure, status_out gets a human-readable error suitable for the
// status STATIC. Dialog HWND is for parent-modal file dialog.
bool export_terrain_to_file(uint8_t* rdram, HWND parent, std::string& status_out);
bool import_terrain_from_file(uint8_t* rdram, HWND parent, std::string& status_out);
// Entity dump (read-only for now) — writes a separate JSON listing
// active buildings + vehicles + aliens + save beacons in the current
// level. Editor uses it to overlay markers on the terrain heightmap.
bool export_entities_to_file(uint8_t* rdram, HWND parent, std::string& status_out);

// Forward declaration for the dialog-side helper used by Save Now
// (defined inside the Win32 dialog block further down).
void write_cheat_to_buffer(uint8_t* rdram, const char* pattern);

// Forward declaration for direct-RAM vehicle spawn used by the
// vehicle morpher sub-dialog (defined alongside the alien spawner
// further down in the file).
int direct_spawn_vehicle(uint8_t* rdram, uint8_t veh_type);

} // namespace

// Phase 15b — renderer helpers exposed by bh_renderer.cpp. The
// Enhancements tab calls these on combobox change events.
namespace bh::renderer {
    void set_aspect_ratio(int mode_index);
    void set_refresh_rate(int mode_index);
    void set_upscale_2d(int mode_index);
    void set_fog_scale(float scale);
}

namespace bh::net {
    void set_enabled(bool on);
    bool enabled();
}

namespace {

// Noclip state — flipped by custom_toggle_noclip, applied each
// input-poll tick by apply_noclip_movement (called from main.cpp's
// stub_poll_input).
std::atomic<bool> g_noclip_active{false};

// "Disable shield walls" state — set by custom_disable_shield_walls,
// re-applied each input-poll tick by apply_disable_shields_tick.
// Re-application is needed because BH reloads the wall source table
// from ROM on building entry/exit and level transitions, which
// otherwise restores collision. Once enabled, stays enabled for the
// whole session (re-apply is idempotent and cheap).
std::atomic<bool> g_disable_shields_active{false};

// Water level override. State:
//   g_water_override_active = true  -> each tick, force D_80222A70 to
//                                      g_water_target_y (defeating
//                                      level scripts that try to
//                                      change it)
//   g_water_override_active = false -> game-side water Y stands
//   g_water_original_y      -> snapshot of the pre-override water Y,
//                              captured on the off->on transition.
//                              Restore writes this back so the user
//                              can revert to whatever water level
//                              the game had before they fiddled.
//   g_water_snapshot_valid  -> true once we've captured at least once
// g_water_target_y is the s32 to write each tick when active. Set by
// "Drain water" cheat, by the Restore cheat (writes original on the
// final tick), or by the Enhancements-tab water slider (user-picked Y).
std::atomic<bool>    g_water_override_active{false};
std::atomic<int32_t> g_water_target_y{0};
std::atomic<int32_t> g_water_original_y{0};
std::atomic<bool>    g_water_snapshot_valid{false};

// Entity render-cull bypass state. When on, the per-tick hook writes
// D_8014FD2A = 0x8000 each input poll, which forces BH's cull
// function (func_800B93AC_C835C) to take its "render everything"
// early-return. Effect: all active vehicles, aliens, buildings (and
// anything else that goes through this cull) render regardless of
// distance or whether they're in the camera frustum — including
// behind the camera. Fixes the "entity pops in past N tiles" / "wider
// aspect ratios show culled sides" issues. Some CPU/GPU overhead
// since BH stops doing the cheap-out culling, but PC easily handles
// it.
std::atomic<bool> g_render_cull_bypass_active{false};
// Per-toggle diagnostic counter: log the first N tick fires after each
// toggle ON so we can verify the wiring and inspect BH's pre-write
// values for D_8014FD2A and D_80157590.
std::atomic<int>  g_cull_tick_log_count{0};
// One-shot diagnostic: did the weak-symbol override of
// `func_800703B0_7F360` actually replace BH's recompiled version?
// We can't tell at link time (linker won't complain if both versions
// exist with right linkage attributes), so the override logs its
// first call to confirm it's being invoked. Reset on each toggle.
std::atomic<int>  g_cull_override_log_count{0};
// Separate diagnostic for the LOS-occlusion override so its log
// budget isn't consumed by the bbox override running first.
std::atomic<int>  g_los_override_log_count{0};
// Multiplier applied to the entity-bbox cull dimensions in our override
// of `func_800703B0_7F360`. 1.0 = BH-faithful (default ~15x15 tile
// bbox); >1.0 grows the bbox so entities further from the camera pass
// the cull. Bound to the Enhancements-tab "Entity render distance"
// slider. The bypass toggle sets this to a huge value AND flips the
// bypass flag for short-circuit behavior.
std::atomic<float> g_entity_cull_scale{1.0f};

// Camera far-plane override state. Mirrors the water-override pattern:
//   active = true  -> each tick, write `original * scale` to
//                     D_801411A4 (the f32 far Z used by BH's main
//                     perspective matrix setup)
//   active = false -> let BH's natural far plane stand
// We snapshot the original on first activation so Release can restore
// it. BH may rewrite D_801411A4 on level transitions or camera mode
// changes; the tick override defeats that.
std::atomic<bool>  g_far_plane_override_active{false};
std::atomic<float> g_far_plane_scale{1.0f};
std::atomic<float> g_far_plane_original{0.0f};
std::atomic<bool>  g_far_plane_snapshot_valid{false};

// Save state. Two flags that the input tick checks each frame: if
// set, performs the I/O between game frames so the operation lands
// at a safe boundary.
//   g_save_state_pending  -> snapshot RDRAM -> disk
//   g_load_state_pending  -> disk -> RDRAM
//
// KNOWN LIMITATIONS (per testing 2026-05-17):
//   1. Saving WHILE THE PLAYER IS MOVING produces a torn snapshot —
//      the input thread reads 8 MB while BH's game thread is still
//      mutating physics/positions. Loading that snapshot crashes
//      because internal pointer consistency is broken. Workaround:
//      save when standing still. A proper fix needs to either snapshot
//      the recomp_context (CPU register state) or pause BH's game
//      thread during the memcpy — both invasive.
//
//   2. Loading an IN-GAME save from the main menu (or vice versa)
//      crashes because BH loads different code overlays for menu vs
//      gameplay. The RAM snapshot has function pointers into the
//      wrong overlay → dispatched call → crash. Bumping version 2:
//      header now includes gameplayMode and currentLevel and load
//      REJECTS mismatched contexts to stop the crash.
//
//   3. Loading between different menu screens causes sequence breaks
//      — the new menu's state machine is in a different position than
//      what RAM expects.
//
// We do raw 8 MB dumps. The recomp_context (CPU register state) is
// implicit in BH's game-loop position; between input polls BH should
// be at a frame boundary so a full-RAM replace gets picked up cleanly.
// RT64-side caches (textures, display lists, framebuffer) survive
// the snapshot — they'll naturally re-sync once the loaded game
// state issues new GBI commands.
std::atomic<bool> g_save_state_pending{false};
std::atomic<bool> g_load_state_pending{false};

// Minimal save state — multi-slot bookmark system. Stores per-slot:
// player position, yaw, level, vehicle spec, HP, fuel, all 17 weapon
// ammo counters, label, and timestamp. Avoids the 8 MB-memcpy race
// problem by touching only well-defined small fields atomically (the
// exact pattern noclip movement uses, which is rock-solid).
//
// 10 slots. Slot 0 is the "quick" slot driven by the simple cheats
// ("Bookmark position" / "Teleport to bookmark"). Slots 1-9 are
// managed via the Bookmarks Manager sub-dialog (Tools section).
//
// Persisted to bh_bookmarks.json in the exe's directory. Auto-saved
// on every change; lazy-loaded on first access.
struct Bookmark {
    bool     valid       = false;
    uint32_t level       = 0;     // 1..5 (currentLevel)
    float    fx          = 0.0f;  // f32 position cache (authoritative)
    float    fy          = 0.0f;
    float    fz          = 0.0f;
    int16_t  yaw         = 0;     // facing direction
    uint8_t  vehicle_spec = 0;    // entity specIndex (for info; not auto-morphed)
    int16_t  hp          = 0;
    int16_t  fuel        = 0;
    int16_t  ammo[18]    = {0};   // 18 weapon ammo counters
                                  //   (IDs 0x00..0x11; was 17 in v1
                                  //    which missed Plasma Bombs ammo
                                  //    at index 0x11. The next variable
                                  //    after the table is humansKilled
                                  //    at 0x8004816A, confirming 18.)
    uint8_t  weapon_slots[8] = {0}; // player carries 8 weapons (IDs into the
                                  // 14-entry WeaponSpecEntry table; 0 = empty)
    uint16_t items_bitmask = 0;   // u16 per-level inventory bitmask
                                  // (D_8004DC4E — same field 'All Items'
                                  // cheat sets to 0xFFFF)
    char     label[40]   = {0};   // user-friendly description (auto-generated)
    char     timestamp[24] = {0}; // local time of save
};
constexpr int kBookmarkSlotCount = 10;
constexpr const char* kBookmarkFilename = "bh_bookmarks.json";

// Single mutex protecting the entire slot array. Operations are tiny
// (capture ~30 fields, write back same), so contention is negligible.
std::mutex                                    g_bookmark_mutex;
std::array<Bookmark, kBookmarkSlotCount>      g_bookmarks{};
std::atomic<bool>                             g_bookmarks_loaded{false};
constexpr uint32_t kRdramSize         = 8u * 1024u * 1024u;
constexpr uint32_t kSaveStateMagic    = 0x42485353; // 'BHSS'
constexpr uint32_t kSaveStateVersion  = 2;          // bumped: now stores gameplayMode

// gameplayMode address — per body-harvest-decompilation symbol_addrs:
// `gameplayMode = 0x80052ADC`. Values from
// body-harvest-decompilation/include/variables.us.h:35-53:
//   0 = LEVEL_MAP (main menu / level select)
//   1-9 = various in-game states (pause menu, cutscenes, NPC, etc.)
//   0xA = END_OF_LEVEL
//   0x10 = INVENTORY
// We include this in the save header to detect menu↔gameplay loads.
constexpr uint32_t kGameplayModeAddr  = 0x80052ADC;

constexpr CustomCheat kCustomCheats[] = {
    { "Refill HP & Fuel (godlike, 0x7FFF)",
      "Set current entity's HP and fuel to 0x7FFF (INT16 max). Bar overshoots "
      "visually but you're effectively immortal until 32k cumulative damage. "
      "Works on foot AND in any vehicle (uses D_80052B34).",
      &custom_refill_hp_fuel_godlike },
    { "Refill HP & Fuel (natural, 0x258)",
      "Set on-foot player HP to 600 (BH's natural max). Bar renders cleanly. "
      "Writes vehicleInstances[0] directly — only useful when on foot; in a "
      "vehicle the natural HP varies by type so use the godlike version.",
      &custom_refill_hp_fuel_natural },
    { "All Items (incl. Hangar Key)",
      "Write 0xFFFF to the level-items bitmask (0x8004DC4E). PER-LEVEL: the "
      "bitmask is re-interpreted/reloaded on each level transition, so this "
      "only grants items for the level you're currently in (re-apply after "
      "switching levels). Bits triggering mission-progression events may "
      "fire NPC dialogue or cutscenes on activation.",
      &custom_all_items },
    { "Infinite Ammo (all 17 weapons)",
      "Write 0x8000 to each ammo counter at 0x80048146..0x80048166. Covers "
      "player weapons (Shotgun..Tri-Spinner) AND vehicle weapons "
      "(Chaingun, Fragcannon, Lazer Missiles, Resonator, Plasma Bombs, etc.).",
      &custom_infinite_ammo },
    { "Reset Humans Killed (pacifist)",
      "Zero the humans-killed counter at 0x8004816A. Useful when going for "
      "the no-civilian-casualties ending after an accidental run-over.",
      &custom_reset_humans_killed },
    { "Toggle Noclip Mode",
      "True noclip with no boundary clamping (bypasses both inner and outer "
      "shield walls, unlike 'zwander'). LEFT STICK = horizontal movement, "
      "rotated by player's facing direction (forward = wherever you're "
      "looking). PageUp = ascend, PageDown = descend. Velocity zeroed each "
      "frame, AIRBORNE flag forced. Click again to toggle off.",
      &custom_toggle_noclip },
    { "Crater at player (4-tile radius depression)",
      "Zeros a circle of tiles in BH's terrain grid (D_8014F8A0), dropping "
      "them to sea-level elevation. One tile = 256 world units, so this is "
      "a ~9x9 patch (~2300x2300 world units). VISUAL UPDATE IS LAZY: you "
      "have to walk out of the chunk and back in for BH to re-tessellate "
      "the new geometry. The map screen updates immediately though.",
      &custom_crater_at_player },
    { "Flat terrain (zero entire tile grid)",
      "Zeros every entry in the 256x256 terrain tile array (~128 KB of "
      "writes). Strips all per-tile state bits — pits, walls, collision "
      "markup, terrain-type flags. EXPERIMENTAL: may cause weird collision, "
      "render glitches, or crashes since BH assumes some bits are set by "
      "level data. Save first.",
      &custom_flatten_terrain },
    { "Raise terrain at player (+2 height, 3-tile radius)",
      "Adds 2 to the height value (bits 0-5) of every tile within a 3-tile "
      "circle of the player, clamped at the max height (63). Preserves "
      "state bits (pits, walls) and texture-type bits, only the elevation "
      "changes. Stack multiple activations to build a hill. Same lazy "
      "visual update as crater — walk away and back to see the new mesh.",
      &custom_raise_terrain_step },
    { "Lower terrain at player (-2 height, 3-tile radius)",
      "Subtracts 2 from the height value (bits 0-5) of every tile within a "
      "3-tile circle of the player, clamped at 0 (sea level). Preserves "
      "state and texture-type bits. Stack multiple activations to dig a "
      "smooth basin instead of the crater cheat's all-or-nothing pit.",
      &custom_lower_terrain_step },
    { "Match terrain to player Y (5-tile radius plateau)",
      "Reads the player's current Y position, converts to tile-height "
      "(world_Y / 32, clamped to 0-63), and paints that height onto every "
      "tile in a 5-tile circle. Build a bridge or landing pad wherever you "
      "stand. Player Y must be on the ground for cleanest results — if "
      "you're airborne or in a flying vehicle (Alpha 1), the resulting "
      "plateau will be raised under your feet.",
      &custom_match_terrain_to_player_y },
    { "Match terrain to local height (5-tile radius plateau)",
      "Samples the tile height directly under the player and applies that "
      "exact value to every tile in a 5-tile circle. Smooths out cliffs and "
      "bumps without needing world-Y math — the local terrain stays at "
      "essentially the same elevation but becomes a flat disc. Best for "
      "cleaning up uneven ground before placing vehicles.",
      &custom_match_terrain_to_local },
    { "Drain water (sink below map, level-wide)",
      "Forces the global water Y (D_80222A70) to a value far below the "
      "terrain so the water surface vanishes. Re-applied every tick so "
      "boss scripts or level transitions can't bring it back. Some "
      "submarine/diving areas may look strange since the game still "
      "thinks you're underwater for those interactions — fire the "
      "'Restore water' companion cheat to turn it off.",
      &custom_drain_water },
    { "Toggle: Render all entities (no distance/frustum cull)",
      "Forces BH's master entity-cull function (func_800B93AC_C835C) "
      "to skip both its 4000-unit distance check and its camera-frustum "
      "angle test. Every active vehicle, alien, and building renders "
      "regardless of where the camera is looking — fixes the pop-in "
      "horizon AND the cull bars at wide aspect ratios. "
      "Implemented by writing D_8014FD2A = 0x8000 every tick (BH's own "
      "'360° render mode' value; idiomatic, not a hack). Re-fire to "
      "toggle off — game's natural FOV recompute resumes next frame. "
      "Some GPU overhead from drawing more geometry; PC handles it fine.",
      &custom_toggle_render_cull_bypass },
    { "Restore water (release water override)",
      "Turns off the water-Y override so the game's natural per-level "
      "water level returns. Doesn't reset the level itself — if a "
      "boss script lowered water before you fired Drain, that lowered "
      "value will come back; if it was at the normal level, that comes "
      "back. Pairs with 'Drain water' and the Water slider in the "
      "Enhancements tab.",
      &custom_restore_water },
    { "Disable shield walls (current level)",
      "Zeros every wall's bounding box (min/max X/Z) in the current "
      "level's slot at 0x80147C30 so wall collision becomes a 0-area "
      "no-op. Gates and gate animations are left untouched in case the "
      "visual portals are still wanted. PER-LEVEL: only affects the "
      "level you're currently in; walls in other levels are unchanged "
      "until you fire this cheat there too. Re-fires harmless if "
      "already zeroed.",
      &custom_disable_shield_walls },
    { "Smooth terrain at player (5-tile radius, 3x3 average)",
      "For each tile in a 5-tile circle, replace its height with the "
      "average of itself + its 8 neighbors. Snapshots original heights "
      "first so the average isn't biased by tiles modified earlier in the "
      "loop. Run multiple times to progressively round off sharp edges. "
      "Preserves state and texture bits like the other height cheats.",
      &custom_smooth_terrain },
    { "Quick Save (respawn at first beacon)",
      "Save stats/items/weapons/progress at any time; respawn at the first "
      "save beacon in your current level (typically the boss-1 save station "
      "— BH's beacon table is per-level layout, so our in-RAM position hack "
      "for beacon 0 gets overwritten when the level reloads). Acts as a "
      "free checkpoint system. True save-anywhere (custom respawn position) "
      "is a future feature; tracked in handoff.md. Side effect: dundee "
      "dance cheat no longer works.",
      &custom_save_game_now },
    { "Save State to file (bh_savestate.bin)",
      "TRUE save-anywhere — snapshots the entire 8 MB of N64 RAM to disk. "
      "Captures everything: exact player position, vehicle state, mission "
      "progress, alien population, animation frames, all of it. The actual "
      "I/O happens on the next input-poll tick (frame boundary safe). "
      "Pairs with 'Load State from file'. The renderer's texture/display-"
      "list cache survives the snapshot, so loading produces a brief one-"
      "frame visual hiccup as RT64 resyncs. File: bh_savestate.bin in the "
      "exe's directory. Single slot for now.",
      &custom_save_state },
    { "Load State from file (bh_savestate.bin)",
      "Restore the previously-saved 8 MB RDRAM snapshot from disk. The "
      "game instantly jumps to whatever state was captured. Validates "
      "the file's magic + version + level header — if you load a save "
      "from a different level than you're currently in, BH may behave "
      "weirdly (level overlay code is still the new level's). Best used "
      "on the same level you saved from.",
      &custom_load_state },
    { "Bookmark position (minimal save — pos + yaw only)",
      "Saves ONLY the player's current position (X/Y/Z) and facing "
      "direction. In-memory single slot, not persisted to disk. No "
      "8 MB RAM dump, no torn-snapshot risk — atomic 6-field capture. "
      "Pairs with 'Teleport to bookmark'. Does NOT preserve HP, fuel, "
      "weapons, mission progress, or anything else — only WHERE you "
      "are. Think of it as a fast-travel marker. Safe to fire while "
      "running, in vehicles, or any state.",
      &custom_bookmark_position },
    { "Teleport to bookmark (minimal load)",
      "Warp the player back to the bookmarked position with original "
      "facing. Velocity is zeroed so you don't fly off. Only works in "
      "the same level you bookmarked in (refuses otherwise). Works on "
      "foot AND in vehicles — whatever you control gets teleported. "
      "All other game state (HP, items, enemies, mission flags) stays "
      "as-is. The reliable alternative to the full save state for "
      "'I want to retry this jump' workflows.",
      &custom_teleport_to_bookmark },
};

// Sanity-check that an N64 KSEG0 RAM pointer lands inside the 8 MiB
// RDRAM window. BH may null D_80052B34 during scene transitions —
// we bail rather than scribbling at offset 0.
inline bool ram_ptr_valid(uint32_t virt) {
    constexpr uint32_t kRamHi = 0x80800000u;
    return virt >= kVirtBase && virt < kRamHi;
}

// ===========================================================================
// Vehicle ID tables per level — from a community cheat-page that documented
// the full vehicle list for each of BH's levels. ID values are written to
// vehicleInstances[0].unk1A (= 0x8004DCEA byte) to morph the player's
// currently-driven vehicle into that type. ID 0x00 is the on-foot avatar
// ("Adam") and ID 0x13 is Alpha 1 in every level (player's main vehicle).
// "(Glitch)", "(no message)", "Incomplete" entries are included for
// completeness but may misbehave when applied.
// ===========================================================================

struct VehicleEntry {
    uint8_t id;
    const char* name;
};

constexpr VehicleEntry kVehiclesGreece[] = {
    { 0x00, "Adam (on foot)" },          { 0x01, "Riley 150" },
    { 0x02, "Sapworth Camel" },          { 0x03, "Sapworth Trainer" },
    { 0x04, "Cruiser" },                 { 0x05, "Fire Engine" },
    { 0x06, "Howitzer" },                { 0x07, "Panzerkampfwagen" },
    { 0x08, "Mk 1 Crocodile" },          { 0x09, "Grimly Transport" },
    { 0x0A, "Nico's Supplies" },         { 0x0B, "SR Shadow" },
    { 0x0C, "Transport Truck (Ambulance)" }, { 0x0D, "Alder DR1" },
    { 0x0E, "Saloon" },                  { 0x0F, "Bulldog" },
    { 0x10, "Lifeboat" },                { 0x11, "Sapworth Camel (glitch)" },
    { 0x12, "Sapworth Camel (glitch)" }, { 0x13, "Alpha 1" },
};

constexpr VehicleEntry kVehiclesJava[] = {
    { 0x00, "Adam (on foot)" },          { 0x01, "Gun Turret" },
    { 0x02, "Hoverboat" },               { 0x03, "Armoured Car" },
    { 0x04, "Bomber" },                  { 0x05, "Rope Car" },
    { 0x06, "Gyrocopter" },              { 0x07, "Artillery Gun" },
    { 0x08, "Tank Destroyer Tank" },     { 0x09, "Kubelwagon" },
    { 0x0A, "Landing Craft (looks like Tracker)" }, { 0x0B, "Tracker ATJ" },
    { 0x0C, "Gunboat" },                 { 0x0D, "Red Airplane" },
    { 0x0E, "Tank" },                    { 0x0F, "Truck" },
    { 0x10, "Cargo Boat" },              { 0x11, "Japanese Float Plane" },
    { 0x12, "Incomplete Zero (crashes)" }, { 0x13, "Alpha 1" },
};

constexpr VehicleEntry kVehiclesAmerica[] = {
    { 0x00, "Adam (on foot)" },          { 0x01, "Sand Minx" },
    { 0x02, "Monster Bug" },             { 0x03, "Mr. Lolly" },
    { 0x04, "V-8 Hiboy" },               { 0x05, "Dusty" },
    { 0x06, "Edzil" },                   { 0x07, "Huey" },
    { 0x08, "Hugh's 500" },              { 0x09, "School Bus" },
    { 0x0A, "Miller J3p" },              { 0x0B, "S.P.D Patrol" },
    { 0x0C, "Rapier Launcher" },         { 0x0D, "Alien Tank (UNUSED)" },
    { 0x0E, "RGM Paton" },               { 0x0F, "Checker Cab" },
    { 0x10, "Tipper" },                  { 0x11, "UFO" },
    { 0x12, "No Message (Tipper)" },     { 0x13, "Alpha 1" },
};

constexpr VehicleEntry kVehiclesSiberia[] = {
    { 0x00, "Adam (on foot)" },          { 0x01, "APC" },
    { 0x02, "Dozer" },                   { 0x03, "Incomplete APC (no msg)" },
    { 0x04, "T-341 'fist'" },            { 0x05, "Hangman B" },
    { 0x06, "Combine" },                 { 0x07, "Gunboat" },
    { 0x08, "Spectre VTOL" },            { 0x09, "MK.3 Halo" },
    { 0x0A, "Polokov 3850" },            { 0x0B, "Vladacar" },
    { 0x0C, "Skorpion RAV" },            { 0x0D, "Scud Launcher" },
    { 0x0E, "Scud Missile" },            { 0x0F, "Proto-RNV" },
    { 0x10, "Fuelski" },                 { 0x11, "Locomov" },
    { 0x12, "(No Vehicle)" },            { 0x13, "Alpha 1" },
};

constexpr VehicleEntry kVehiclesComet[] = {
    { 0x00, "Adam (on foot)" },
    { 0x09, "Glitch Vehicle" },
    { 0x13, "Alpha 1" },
};

struct LevelInfo {
    const char* name;
    const VehicleEntry* vehicles;
    size_t vehicle_count;
};

// Indexed by the byte at 0x80047F93. Index 0 = "not in a level" (boot,
// menus, transitions); 1..5 are the playable levels.
constexpr LevelInfo kLevels[] = {
    { "(none)",  nullptr,           0 },
    { "Greece",  kVehiclesGreece,   std::size(kVehiclesGreece) },
    { "Java",    kVehiclesJava,     std::size(kVehiclesJava) },
    { "America", kVehiclesAmerica,  std::size(kVehiclesAmerica) },
    { "Siberia", kVehiclesSiberia,  std::size(kVehiclesSiberia) },
    { "Comet",   kVehiclesComet,    std::size(kVehiclesComet) },
};
constexpr int kLevelCount = int(std::size(kLevels));

// Alien type table — global across all levels per decomp comment
// in include/structs.us.h line 733 ("Specs are the same every level?
// e.g. 0x19 is harvester, 0x1B is Boss"). Values from a community
// cheat-page documenting the Harvester Replace cheat values.
// Morphing to a type not normally present in the current level may
// trigger missing-asset issues (model not loaded for this map).
struct AlienTypeEntry {
    uint8_t id;
    const char* name;
};
constexpr AlienTypeEntry kAlienTypes[] = {
    { 0x00, "(empty / despawn)" },
    { 0x02, "Ooze" },
    { 0x03, "Fly" },
    { 0x04, "Big Ant" },
    { 0x06, "Flying Scorpion-Type Bug" },
    { 0x08, "Giant Praying Mantis-Type Bug" },
    { 0x09, "Double-Arm Gun Turret Bug" },
    { 0x0C, "Giant Praying Mantis-Type Bug (variant)" },
    { 0x0D, "Exploding Frog" },
    { 0x0E, "Small Ant (doesn't die)" },
    { 0x12, "Bad Adam (doesn't move, can't die)" },
    { 0x14, "Piranha (any water-containing level)" },
    { 0x16, "Hopping Turret" },
    { 0x19, "Harvester" },
    { 0x1A, "Shield Boss" },
    { 0x1B, "Boss (level-specific) — SPAWN-OK, but tends to crash shortly after "
            "due to missing arena-bound entity / skipped recursive helper" },
};

// "Refill HP & Fuel (godlike)" — uses D_80052B34 so it works on
// foot AND in any vehicle. Writes INT16 max; bar overshoots but you
// effectively never die.
void custom_refill_hp_fuel_godlike(uint8_t* rdram) {
    if (rdram == nullptr) return;
    const uint32_t entity_virt = read_n64_u32(rdram, kCurrentEntityPtr);
    if (!ram_ptr_valid(entity_virt)) {
        std::fprintf(stderr,
            "[cheats] Refill HP godlike: D_80052B34 = 0x%08X (no current entity); skipped\n",
            entity_virt);
        return;
    }
    write_n64_s16(rdram, entity_virt + kEntityOffsetHP,   kGodlikeHP);
    write_n64_s16(rdram, entity_virt + kEntityOffsetFuel, kGodlikeHP);
    std::fprintf(stderr,
        "[cheats] custom -> Refill HP & Fuel (godlike, 0x7FFF) applied to entity 0x%08X\n",
        entity_virt);
}

// "Refill HP & Fuel (natural)" — writes vehicleInstances[0] directly
// at the address the GameShark "Infinite Health" code uses. Player
// slot natural max is 600; bar renders cleanly. Only meaningful on
// foot — in a vehicle the spec's natural max varies by type.
void custom_refill_hp_fuel_natural(uint8_t* rdram) {
    if (rdram == nullptr) return;
    write_n64_s16(rdram, kPlayerVehSlotHP, kPlayerNaturalHP);
    std::fprintf(stderr,
        "[cheats] custom -> Refill HP natural (player slot HP = %d)\n",
        kPlayerNaturalHP);
}

// "All Items" — set every level-progression item bit.
// May unlock vehicles that require items like the Hangar Key.
void custom_all_items(uint8_t* rdram) {
    if (rdram == nullptr) return;
    write_n64_s16(rdram, kItemsBitmask, int16_t(0xFFFF));
    std::fprintf(stderr,
        "[cheats] custom -> All Items (0xFFFF written to 0x%08X)\n",
        kItemsBitmask);
}

// "Infinite Ammo (all 18 weapons)" — write 0x8000 to every ammo
// counter. INT16_MIN is treated as "huge unsigned" by the game's
// ammo display, and decrement wraps around so it never depletes.
// Bumped from 17 → 18 counters after discovering Plasma Bombs ammo
// (ID 0x11) sits in the last slot, just before humansKilled.
void custom_infinite_ammo(uint8_t* rdram) {
    if (rdram == nullptr) return;
    constexpr int kAmmoSlotCount = 18;
    for (int i = 0; i < kAmmoSlotCount; ++i) {
        write_n64_s16(rdram, kAmmoSlotBase + uint32_t(i * 2),
                      kInfiniteAmmoMagic);
    }
    std::fprintf(stderr,
        "[cheats] custom -> Infinite Ammo (%d slots set to 0x%04X)\n",
        kAmmoSlotCount, uint16_t(kInfiniteAmmoMagic));
}

// "Reset Humans Killed" — zero the humans-killed counter.
void custom_reset_humans_killed(uint8_t* rdram) {
    if (rdram == nullptr) return;
    write_n64_s16(rdram, kHumansKilled, 0);
    std::fprintf(stderr,
        "[cheats] custom -> Reset Humans Killed (counter at 0x%08X = 0)\n",
        kHumansKilled);
}

// "Save Game Now" — patches cheatData[18] in-place to point its
// funcPtr at func_80002CA4_38A4 (BH's save function), then writes
// the matching reversed pattern into cheatInputBuffer so the next
// matcher tick fires the redirected entry. Save runs on the game
// thread with a valid recomp_context (matcher's own context),
// which is what the save function needs to allocate its stack
// frame and call sub-functions like guess_prepareToSaveGame +
// osEepromLongWrite.
//
// Repeated activations re-apply the patch — idempotent.
void custom_save_game_now(uint8_t* rdram) {
    if (rdram == nullptr) {
        std::fprintf(stderr, "[cheats] Save Now: rdram not ready, skipping\n");
        return;
    }

    // ── Phase 1: beacon hack ──
    // Hijack beacon 0 to point at player's current X/Z + current level,
    // then write D_80047F9C = 1 (= beacon_id+1) so the save records
    // "respawn at beacon 0". On load, BH looks up beacon[0].position
    // and spawns there. If the beacon table is per-level (re-loaded
    // each level), this hack only persists for this session — load
    // will still go to wherever the level data says beacon 0 lives.
    {
        const uint8_t lv = read_n64_u8(rdram, kLevelByte);
        const uint32_t entity_virt = read_n64_u32(rdram, kCurrentEntityPtr);
        if (lv >= 1 && lv <= 5 && ram_ptr_valid(entity_virt)) {
            const int16_t px = read_n64_s16(rdram, entity_virt + kEntityOffsetPosX);
            const int16_t pz = read_n64_s16(rdram, entity_virt + kEntityOffsetPosZ);
            const uint32_t beacon_virt = kBeaconArrayBase
                                       + uint32_t(kBeaconHijackIndex) * kBeaconStride;
            write_n64_s16(rdram, beacon_virt + 0, px);          // unk0 = X
            write_n64_s16(rdram, beacon_virt + 2, pz);          // unk2 = Z
            write_n64_s16(rdram, beacon_virt + 6, int16_t(lv)); // unk6 = level

            // D_80047F9C is u32; aligned word write uses MEM_W convention
            // (host-native byte order, no swap).
            const uint32_t beacon_save_val = uint32_t(kBeaconHijackIndex + 1);
            std::memcpy(rdram + (kSavedBeaconId - kVirtBase),
                        &beacon_save_val, sizeof(beacon_save_val));

            std::fprintf(stderr,
                "[cheats] Save Now: hijacked beacon[%d] -> (X=%d, Z=%d, level=%u), "
                "D_80047F9C = %u\n",
                kBeaconHijackIndex, int(px), int(pz), unsigned(lv),
                unsigned(beacon_save_val));
        } else {
            std::fprintf(stderr,
                "[cheats] Save Now: skipping beacon hack (level=%u, entity=0x%08X)\n",
                unsigned(lv), entity_virt);
        }
    }

    // ── Phase 2: patch cheat slot + fire save ──
    const uint32_t entry_virt = kCheatDataBase
                              + uint32_t(kSaveCheatSlotIndex) * kCheatEntrySize;

    // Patch pattern field (offset 0, 12 bytes). Set to "save\0...".
    // Each byte goes through write_n64_u8 (XOR-3 byte swap).
    const char kNewPattern[] = "save";
    constexpr size_t kPatternFieldLen = 12;
    for (size_t i = 0; i < kPatternFieldLen; ++i) {
        const uint8_t b = (i < std::size(kNewPattern) - 1)
            ? uint8_t(kNewPattern[i])
            : 0;
        write_n64_u8(rdram, entry_virt + uint32_t(i), b);
    }

    // Patch funcPtr field (offset 12, u32). MEM_W convention =
    // host-native byte order, no byteswap.
    {
        const uint32_t addr = kSaveGameFunctionVirt;
        std::memcpy(rdram + (entry_virt + kCheatEntryFuncPtrOffset - kVirtBase),
                    &addr, sizeof(addr));
    }

    // Write reversed pattern to cheatInputBuffer. Matcher walks the
    // buffer such that buffer[0..N-1] must equal pattern reversed.
    write_cheat_to_buffer(rdram, kNewPattern);

    std::fprintf(stderr,
        "[cheats] custom -> Save Now: patched cheatData[%d] -> funcPtr=0x%08X "
        "(BH save fn), wrote 'evas' to cheatInputBuffer. BH matcher fires "
        "next frame.\n",
        kSaveCheatSlotIndex, kSaveGameFunctionVirt);
}

// "Crater at player" — zero a circle of terrain tiles centered on
// the player's current position. Same trick as Flat Terrain but
// localized: zeroing tiles drops them to sea-level elevation,
// producing a visible depression once BH re-streams that chunk
// (player must walk out of the modified area and back to see it).
//
// Earlier version OR'd Java's pit-state bits (0x0380) — that
// doesn't actually change height, just sets a "this is a pit"
// flag that Java's water-fall logic looks for. Other levels
// ignore the flag, so the modification was invisible.
void custom_crater_at_player(uint8_t* rdram) {
    if (rdram == nullptr) return;

    // Dereference the terrain-tiles pointer (set at level load).
    const uint32_t tiles_virt = read_n64_u32(rdram, kTerrainTilesPtr);
    if (!ram_ptr_valid(tiles_virt)) {
        std::fprintf(stderr, "[cheats] Crater: terrain pointer invalid (0x%08X)\n",
                     tiles_virt);
        return;
    }

    // Read player position from D_80052B34 (works on foot or in vehicle).
    const uint32_t entity = read_n64_u32(rdram, kCurrentEntityPtr);
    if (!ram_ptr_valid(entity)) {
        std::fprintf(stderr, "[cheats] Crater: no current entity\n");
        return;
    }
    const int16_t px = read_n64_s16(rdram, entity + kEntityOffsetPosX);
    const int16_t pz = read_n64_s16(rdram, entity + kEntityOffsetPosZ);
    const int tile_x = (int(px) >> 8) + 128;
    const int tile_z = (int(pz) >> 8) + 128;

    constexpr int kCraterRadius = 4;
    int modified = 0;
    for (int dz = -kCraterRadius; dz <= kCraterRadius; ++dz) {
        for (int dx = -kCraterRadius; dx <= kCraterRadius; ++dx) {
            if (dx*dx + dz*dz > kCraterRadius*kCraterRadius) continue;
            const int tx = tile_x + dx;
            const int tz = tile_z + dz;
            if (tx < 0 || tx >= kTerrainW || tz < 0 || tz >= kTerrainH) continue;
            const uint32_t tile_addr = tiles_virt + uint32_t(tz * kTerrainW + tx) * 2;
            write_n64_u16(rdram, tile_addr, 0);
            ++modified;
        }
    }
    std::fprintf(stderr,
        "[cheats] Crater at player: tiles=(%d,%d), %d tiles zeroed "
        "(walk away + back to see geometry update)\n",
        tile_x, tile_z, modified);
}

// "Flat terrain" — zero the entire 256x256 u16 tile array.
void custom_flatten_terrain(uint8_t* rdram) {
    if (rdram == nullptr) return;
    const uint32_t tiles_virt = read_n64_u32(rdram, kTerrainTilesPtr);
    if (!ram_ptr_valid(tiles_virt)) {
        std::fprintf(stderr, "[cheats] Flatten: terrain pointer invalid (0x%08X)\n",
                     tiles_virt);
        return;
    }
    const uint32_t bytes = uint32_t(kTerrainW) * uint32_t(kTerrainH) * 2u;
    // Sanity check: the array end must stay inside RDRAM.
    if (tiles_virt + bytes > kVirtBase + 0x800000u) {
        std::fprintf(stderr,
            "[cheats] Flatten: array would overflow RDRAM "
            "(start=0x%08X, bytes=%u); skipped\n",
            tiles_virt, unsigned(bytes));
        return;
    }
    // u16 zero = all bytes zero, so memset works regardless of N64 endianness.
    std::memset(rdram + (tiles_virt - kVirtBase), 0, bytes);
    std::fprintf(stderr,
        "[cheats] Flatten terrain: zeroed %u bytes at 0x%08X\n",
        unsigned(bytes), tiles_virt);
}

// Shared core for the raise/lower-terrain cheats. BH's tile format
// (per src.us/overlay_gameplay/outside/BF9C0.c lines 1181-1191) puts
// the height value in bits 0-5 (mask 0x3F, max value 63 = peak,
// 0 = sea level). Bits 6-9 are state flags (pit/wall/etc.) and
// bits 10-15 encode texture/type — both must be preserved or
// terrain rendering and collision break. We read-modify-write each
// tile in a circle around the player, clamping the new height to
// [0, 0x3F].
//
// delta is signed: positive raises, negative lowers. radius is
// in tile units (1 tile = 256 world units).
void adjust_terrain_height(uint8_t* rdram, int delta, int radius,
                           const char* label) {
    if (rdram == nullptr) return;

    const uint32_t tiles_virt = read_n64_u32(rdram, kTerrainTilesPtr);
    if (!ram_ptr_valid(tiles_virt)) {
        std::fprintf(stderr,
            "[cheats] %s: terrain pointer invalid (0x%08X)\n",
            label, tiles_virt);
        return;
    }

    const uint32_t entity = read_n64_u32(rdram, kCurrentEntityPtr);
    if (!ram_ptr_valid(entity)) {
        std::fprintf(stderr, "[cheats] %s: no current entity\n", label);
        return;
    }
    const int16_t px = read_n64_s16(rdram, entity + kEntityOffsetPosX);
    const int16_t pz = read_n64_s16(rdram, entity + kEntityOffsetPosZ);
    const int tile_x = (int(px) >> 8) + 128;
    const int tile_z = (int(pz) >> 8) + 128;

    constexpr uint16_t kHeightMask = 0x003Fu;
    constexpr int      kHeightMax  = 0x3F;

    int modified = 0;
    int clamped  = 0;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx*dx + dz*dz > radius*radius) continue;
            const int tx = tile_x + dx;
            const int tz = tile_z + dz;
            if (tx < 0 || tx >= kTerrainW || tz < 0 || tz >= kTerrainH) continue;
            const uint32_t tile_addr =
                tiles_virt + uint32_t(tz * kTerrainW + tx) * 2;
            const uint16_t tile  = read_n64_u16(rdram, tile_addr);
            const int      h_old = int(tile & kHeightMask);
            int            h_new = h_old + delta;
            if (h_new < 0)            { h_new = 0;           ++clamped; }
            else if (h_new > kHeightMax) { h_new = kHeightMax; ++clamped; }
            const uint16_t new_tile =
                uint16_t((tile & ~kHeightMask) | uint16_t(h_new));
            write_n64_u16(rdram, tile_addr, new_tile);
            ++modified;
        }
    }
    std::fprintf(stderr,
        "[cheats] %s: tiles=(%d,%d), delta=%+d, radius=%d, "
        "%d modified (%d clamped) — walk away + back for new mesh\n",
        label, tile_x, tile_z, delta, radius, modified, clamped);
}

// "Raise terrain at player" — +2 to height bits, 3-tile radius.
void custom_raise_terrain_step(uint8_t* rdram) {
    adjust_terrain_height(rdram, +2, 3, "Raise terrain");
}

// "Lower terrain at player" — -2 to height bits, 3-tile radius.
void custom_lower_terrain_step(uint8_t* rdram) {
    adjust_terrain_height(rdram, -2, 3, "Lower terrain");
}

// Shared core for the "match height to value" cheats. Paints
// `target_height` (clamped 0-63) onto every tile within `radius`
// tiles of (tile_x, tile_z), preserving the non-height bits.
void paint_terrain_height(uint8_t* rdram, uint32_t tiles_virt,
                          int tile_x, int tile_z,
                          int target_height, int radius,
                          const char* label) {
    constexpr uint16_t kHeightMask = 0x003Fu;
    if (target_height < 0)        target_height = 0;
    if (target_height > 0x3F)     target_height = 0x3F;
    const uint16_t h_bits = uint16_t(target_height) & kHeightMask;

    int modified = 0;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx*dx + dz*dz > radius*radius) continue;
            const int tx = tile_x + dx;
            const int tz = tile_z + dz;
            if (tx < 0 || tx >= kTerrainW || tz < 0 || tz >= kTerrainH) continue;
            const uint32_t tile_addr =
                tiles_virt + uint32_t(tz * kTerrainW + tx) * 2;
            const uint16_t tile = read_n64_u16(rdram, tile_addr);
            const uint16_t new_tile = uint16_t((tile & ~kHeightMask) | h_bits);
            write_n64_u16(rdram, tile_addr, new_tile);
            ++modified;
        }
    }
    std::fprintf(stderr,
        "[cheats] %s: tiles=(%d,%d), height=%d, radius=%d, %d painted "
        "— walk away + back for new mesh\n",
        label, tile_x, tile_z, target_height, radius, modified);
}

// "Match terrain to player Y" — convert player world-Y (s16) to tile
// height via Y >> 5 (i.e. divide by 32) and paint a 5-tile disc.
// Scale derived by decoding BH's func_800B84D0_C7480 in
// asm/nonmatchings/overlay_gameplay/outside/BF9C0/func_800B84D0_C7480.s:
// it bilinearly interpolates 4 corner tile-heights as
// `((h_base * 256) + dh_x*fracX + dh_z*fracZ) << 5`, returning a
// fixed-point value of `tile_height * 8192`. Callers then `>> 8`
// (e.g. src.us/overlay_level/java/1ED9E0.c:325) to get the final
// entity Y, which therefore equals `tile_height * 32`. Reversing
// that: `tile_height = entity_Y / 32 = entity_Y >> 5`.
void custom_match_terrain_to_player_y(uint8_t* rdram) {
    if (rdram == nullptr) return;

    const uint32_t tiles_virt = read_n64_u32(rdram, kTerrainTilesPtr);
    if (!ram_ptr_valid(tiles_virt)) {
        std::fprintf(stderr,
            "[cheats] Match-Y: terrain pointer invalid (0x%08X)\n",
            tiles_virt);
        return;
    }
    const uint32_t entity = read_n64_u32(rdram, kCurrentEntityPtr);
    if (!ram_ptr_valid(entity)) {
        std::fprintf(stderr, "[cheats] Match-Y: no current entity\n");
        return;
    }
    const int16_t px = read_n64_s16(rdram, entity + kEntityOffsetPosX);
    const int16_t py = read_n64_s16(rdram, entity + kEntityOffsetPosY);
    const int16_t pz = read_n64_s16(rdram, entity + kEntityOffsetPosZ);

    const int tile_x = (int(px) >> 8) + 128;
    const int tile_z = (int(pz) >> 8) + 128;
    // Arithmetic shift on a signed int keeps negative Y → negative
    // height, which then clamps to 0 (sea level) in paint helper.
    const int target_h = int(py) >> 5;

    paint_terrain_height(rdram, tiles_virt, tile_x, tile_z,
                         target_h, 5, "Match-Y");
    std::fprintf(stderr,
        "[cheats]   player_Y=%d (s16), target_tile_h=%d (Y>>5, clamped 0..63)\n",
        int(py), target_h);
}

// "Match terrain to local height" — sample tile under player, paint
// that exact height to a 5-tile disc. No Y math needed.
void custom_match_terrain_to_local(uint8_t* rdram) {
    if (rdram == nullptr) return;

    const uint32_t tiles_virt = read_n64_u32(rdram, kTerrainTilesPtr);
    if (!ram_ptr_valid(tiles_virt)) {
        std::fprintf(stderr,
            "[cheats] Match-Local: terrain pointer invalid (0x%08X)\n",
            tiles_virt);
        return;
    }
    const uint32_t entity = read_n64_u32(rdram, kCurrentEntityPtr);
    if (!ram_ptr_valid(entity)) {
        std::fprintf(stderr, "[cheats] Match-Local: no current entity\n");
        return;
    }
    const int16_t px = read_n64_s16(rdram, entity + kEntityOffsetPosX);
    const int16_t pz = read_n64_s16(rdram, entity + kEntityOffsetPosZ);

    const int tile_x = (int(px) >> 8) + 128;
    const int tile_z = (int(pz) >> 8) + 128;
    if (tile_x < 0 || tile_x >= kTerrainW ||
        tile_z < 0 || tile_z >= kTerrainH) {
        std::fprintf(stderr,
            "[cheats] Match-Local: player tile (%d,%d) out of range\n",
            tile_x, tile_z);
        return;
    }
    const uint32_t player_tile_addr =
        tiles_virt + uint32_t(tile_z * kTerrainW + tile_x) * 2;
    const uint16_t player_tile = read_n64_u16(rdram, player_tile_addr);
    const int target_h = int(player_tile & 0x003Fu);

    paint_terrain_height(rdram, tiles_virt, tile_x, tile_z,
                         target_h, 5, "Match-Local");
}

// "Smooth terrain at player" — replace each tile's height in a 5-tile
// disc with the average of itself + its 8 neighbors. Two-pass: read
// all original heights into a buffer first so later writes don't bias
// the average. Preserves all non-height bits.
void custom_smooth_terrain(uint8_t* rdram) {
    if (rdram == nullptr) return;

    const uint32_t tiles_virt = read_n64_u32(rdram, kTerrainTilesPtr);
    if (!ram_ptr_valid(tiles_virt)) {
        std::fprintf(stderr,
            "[cheats] Smooth: terrain pointer invalid (0x%08X)\n",
            tiles_virt);
        return;
    }
    const uint32_t entity = read_n64_u32(rdram, kCurrentEntityPtr);
    if (!ram_ptr_valid(entity)) {
        std::fprintf(stderr, "[cheats] Smooth: no current entity\n");
        return;
    }
    const int16_t px = read_n64_s16(rdram, entity + kEntityOffsetPosX);
    const int16_t pz = read_n64_s16(rdram, entity + kEntityOffsetPosZ);
    const int tile_x = (int(px) >> 8) + 128;
    const int tile_z = (int(pz) >> 8) + 128;

    constexpr int kRadius = 5;
    constexpr uint16_t kHeightMask = 0x003Fu;

    // Snapshot neighborhood big enough to read 1-tile margin around
    // the radius (so corner tiles can sample their full 3x3).
    constexpr int kSnapHalf = kRadius + 1;
    constexpr int kSnapSide = kSnapHalf * 2 + 1;
    uint8_t snap_h[kSnapSide][kSnapSide]; // height-only (0-63)

    auto sample_height = [&](int tx, int tz) -> int {
        if (tx < 0 || tx >= kTerrainW || tz < 0 || tz >= kTerrainH) return -1;
        const uint32_t addr =
            tiles_virt + uint32_t(tz * kTerrainW + tx) * 2;
        return int(read_n64_u16(rdram, addr) & kHeightMask);
    };

    for (int dz = -kSnapHalf; dz <= kSnapHalf; ++dz) {
        for (int dx = -kSnapHalf; dx <= kSnapHalf; ++dx) {
            const int h = sample_height(tile_x + dx, tile_z + dz);
            snap_h[dz + kSnapHalf][dx + kSnapHalf] =
                uint8_t(h < 0 ? 0 : h);
        }
    }

    int modified = 0;
    for (int dz = -kRadius; dz <= kRadius; ++dz) {
        for (int dx = -kRadius; dx <= kRadius; ++dx) {
            if (dx*dx + dz*dz > kRadius*kRadius) continue;
            const int tx = tile_x + dx;
            const int tz = tile_z + dz;
            if (tx < 0 || tx >= kTerrainW || tz < 0 || tz >= kTerrainH) continue;

            // 3x3 average from the snapshot (skip out-of-grid samples).
            int sum = 0, count = 0;
            for (int nz = -1; nz <= 1; ++nz) {
                for (int nx = -1; nx <= 1; ++nx) {
                    const int sx = dx + nx + kSnapHalf;
                    const int sz = dz + nz + kSnapHalf;
                    const int wx = tx + nx;
                    const int wz = tz + nz;
                    if (wx < 0 || wx >= kTerrainW ||
                        wz < 0 || wz >= kTerrainH) continue;
                    sum   += int(snap_h[sz][sx]);
                    count += 1;
                }
            }
            if (count == 0) continue;
            int avg = (sum + count / 2) / count; // rounded
            if (avg < 0)        avg = 0;
            if (avg > 0x3F)     avg = 0x3F;

            const uint32_t tile_addr =
                tiles_virt + uint32_t(tz * kTerrainW + tx) * 2;
            const uint16_t tile = read_n64_u16(rdram, tile_addr);
            const uint16_t new_tile =
                uint16_t((tile & ~kHeightMask) | uint16_t(avg));
            write_n64_u16(rdram, tile_addr, new_tile);
            ++modified;
        }
    }
    std::fprintf(stderr,
        "[cheats] Smooth terrain: tiles=(%d,%d), radius=%d, %d averaged "
        "— stack runs for smoother results\n",
        tile_x, tile_z, kRadius, modified);
}

// "Disable shield walls" — patch each wall so the collision DETECTOR
// returns 0 (no collision) instead of 1 (skip but enter the resolver).
//
// The trap I hit first: setting minX = INT16_MAX made the detector
// return 1 — but the caller in F9230.c:6476 then calls the resolver
// `func_800B0DF4_BFDA4` with arg3 = detector return. The resolver in
// `arg3 == 1` mode checks `(arg0 - radius) < D_8014FD30.unk0` and
// returns 0x4000 (push right) when true — which it always was with
// INT16_MAX as minX. Result: phantom right-side wall pushing the
// player every frame. The detector says "skip", the resolver still
// runs and pushes.
//
// The fix is to make the detector return 0 outright. Looking at
// `func_800B0D10_BFCC0`:
//   if (player_outside_bbox) return 1;          // <-- BAD path
//   do {
//     if (gate1_x0 == gate1_x1) return 0;       // <-- GOOD path
//     ...
//   } while (...);
//   return 0;                                    // also good
// So: make player be INSIDE the bbox (set bbox to full s16 range)
// AND make the first gate degenerate (gate1_x0 == gate1_x1 == 0).
// First do-while iteration hits the degenerate check, returns 0,
// resolver is never called, no push.
//
// CAVEAT: the wall-stamping function `func_800B165C_C060C` iterates
// z from minZ>>10 to maxZ>>10 and stamps wall bits into D_8021EA30
// for each row. With a full-world bbox it would try to stamp the
// entire grid — possibly causing other systems (line clipping, AI
// pathfinding) to see walls everywhere. We accept that risk because
// the stamping is normally run once at region transition, so the
// grid state after our patch depends on whether anything triggers
// a re-stamp. In practice the user can fall back to noclip if any
// side effect shows up.
//
// D_8014FD30 is the active working copy that gets reloaded from the
// per-level source table when the player crosses regions; we patch
// both so the change is instant and survives reloads.
// Shared core: neuter every wall slot + the active working copy +
// the renderer's iteration count + the gate-renderer terminator for
// the given level. Idempotent — re-running on the same level just
// rewrites the same disabled state. Called both from the activation
// cheat (for instant effect) and from the per-tick re-apply (to
// survive building entry/exit + level transitions that BH satisfies
// by re-copying wall data from ROM).
void apply_shield_disable_for_level(uint8_t* rdram, uint32_t lvl) {
    if (lvl < 1 || lvl > 5) return;

    constexpr int16_t kBboxMin = INT16_MIN; // -32768
    constexpr int16_t kBboxMax = INT16_MAX; // +32767

    auto neuter_wall = [&](uint32_t addr) {
        // bbox = entire world — detector's "is player outside?" fails,
        // execution falls into the do-while gate loop.
        write_n64_s16(rdram, addr + 0x00, kBboxMin); // minX
        write_n64_s16(rdram, addr + 0x02, kBboxMin); // minZ
        write_n64_s16(rdram, addr + 0x04, kBboxMax); // maxX
        write_n64_s16(rdram, addr + 0x06, kBboxMax); // maxZ
        // gate1: degenerate so the first do-while iteration hits the
        // `if (unk8 == unkC) return 0` and bails — resolver never
        // gets called, no phantom push.
        write_n64_s16(rdram, addr + 0x08, 0); // gate1_x0
        write_n64_s16(rdram, addr + 0x0A, 0); // gate1_z0
        write_n64_s16(rdram, addr + 0x0C, 0); // gate1_x1
        write_n64_s16(rdram, addr + 0x0E, 0); // gate1_z1
        write_n64_s16(rdram, addr + 0x10, 0); // gate2_x0
        write_n64_s16(rdram, addr + 0x12, 0); // gate2_z0
        write_n64_s16(rdram, addr + 0x14, 0); // gate2_x1
        write_n64_s16(rdram, addr + 0x16, 0); // gate2_z1
    };

    // 1) Per-level source table — survives next region/building
    //    transition's reload of the working copy.
    const uint32_t lvl_base =
        kShieldWallsBase + (lvl - 1u) * kShieldWallsLevelStride;
    for (int i = 0; i < kShieldWallsPerLevel; ++i) {
        neuter_wall(lvl_base + uint32_t(i) * kShieldWallSize);
    }
    // 2) Active working copy — instant effect.
    neuter_wall(kActiveWallAddr);

    // 3) Hide the shield-WALL visuals. D_8003E0EE[lvl] is the
    //    iteration count for func_800BB5E0_CA590 (DrawShieldWalls);
    //    its ASM does `blez count*8, skip` so 0 = render nothing.
    write_n64_s16(rdram, kShieldVisCountBase + lvl * 2u, 0);

    // 4) Hide the shield-GATE (portal) visuals. The gate renderer
    //    func_800BD360_CC310 (BF9C0.c:2116-2197) iterates 8 gate
    //    slots; if it finds one with `unk9 == 2`, both inner
    //    branches return immediately (loop terminates). Setting
    //    gate 0's type byte (offset 0x09) to 2 makes the renderer
    //    bail at iteration 0 with no portals drawn. unk9 is also
    //    used in the render call itself (line 2190) and nowhere
    //    else in gameplay logic, so this is safe.
    const uint32_t gate0_addr =
        kShieldGatesBase + (lvl - 1u) * uint32_t(kShieldGatesPerLevel) *
        kShieldGateSize;
    write_n64_u8(rdram, gate0_addr + 0x09, 2);
}

// Water level override — set/clear cheats.
//
// We write D_80222A70 as a 32-bit value (the canonical s32 view).
// The s16 alias D_80222A72 covers the lower half; both views stay
// consistent because the upper 16 bits of the s32 are sign-extended
// from our int32_t target (e.g. -1000 -> 0xFFFFFC18, so the s16
// read at +2 sees 0xFC18 = -1000).
//
// The tick re-applies the value because per-level scripts (e.g.
// Java boss arena lowering water for the fight) may stomp our
// write between frames. Restore cheat just clears the flag — the
// game's own water-Y management takes over again.
// Capture the current in-game water Y before we start overriding it,
// so Restore has something to write back to. Called on the off->on
// transition by both the Drain cheat and the slider, and only takes
// effect on the *first* transition per session (snapshot persists
// across multiple drain/restore cycles).
void water_snapshot_if_needed(uint8_t* rdram) {
    if (rdram == nullptr) return;
    if (g_water_snapshot_valid.load(std::memory_order_acquire)) return;
    const uint32_t cur = read_n64_u32(rdram, kWaterLevelAddr);
    g_water_original_y.store(int32_t(cur), std::memory_order_release);
    g_water_snapshot_valid.store(true, std::memory_order_release);
    std::fprintf(stderr,
        "[cheats] Water snapshot: original D_80222A70 = %d "
        "(restored on next Release / Restore Water).\n",
        int32_t(cur));
}

// Save state cheats — just flip flags; the tick does the actual I/O
// between game frames so it lands at a safe boundary.
void custom_save_state(uint8_t* /*rdram*/) {
    g_save_state_pending.store(true, std::memory_order_release);
    std::fprintf(stderr,
        "[cheats] Save State queued — next input tick writes "
        "bh_savestate.bin\n");
}
void custom_load_state(uint8_t* /*rdram*/) {
    g_load_state_pending.store(true, std::memory_order_release);
    std::fprintf(stderr,
        "[cheats] Load State queued — next input tick reads "
        "bh_savestate.bin\n");
}

// Forward declaration of the disk-persistence functions defined below.
void bookmarks_save_to_disk();
void bookmarks_load_from_disk_if_needed();

// Build a human-friendly label for a freshly-captured bookmark when
// the user hasn't provided one. Format: "Lvl N (X, Z) HP:H".
void make_default_label(char* dst, size_t cap, const Bookmark& b) {
    std::snprintf(dst, cap, "Lvl %u (%d,%d) HP:%d",
                  b.level, int(b.fx), int(b.fz), int(b.hp));
}

void make_timestamp(char* dst, size_t cap) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(dst, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

// Capture the full bookmark state (pos/yaw/vehicle/hp/fuel/ammo) into
// the given slot. Refreshes label + timestamp. Returns true on success.
// Called from cheat fires (input thread) and the manager dialog.
bool bookmark_capture_slot(uint8_t* rdram, int slot,
                           const char* user_label /* may be nullptr */) {
    if (rdram == nullptr) return false;
    if (slot < 0 || slot >= kBookmarkSlotCount) return false;

    const uint32_t entity = read_n64_u32(rdram, kCurrentEntityPtr);
    if (!ram_ptr_valid(entity)) {
        std::fprintf(stderr,
            "[cheats] Bookmark slot %d: no current entity "
            "(cutscene/transition?)\n", slot);
        return false;
    }

    Bookmark b;
    b.valid       = true;
    b.level       = read_n64_u32(rdram, kCurrentLevelAddr);
    b.fx          = read_n64_f32(rdram, entity + kEntityOffsetCacheX);
    b.fy          = read_n64_f32(rdram, entity + kEntityOffsetCacheY);
    b.fz          = read_n64_f32(rdram, entity + kEntityOffsetCacheZ);
    b.yaw         = read_n64_s16(rdram, entity + kEntityOffsetDir);
    b.vehicle_spec = read_n64_u8(rdram, entity + kEntityOffsetSpecIdx);
    b.hp          = read_n64_s16(rdram, entity + kEntityOffsetHP);
    b.fuel        = read_n64_s16(rdram, entity + kEntityOffsetFuel);
    for (int i = 0; i < 18; ++i) {
        b.ammo[i] = read_n64_s16(rdram, kAmmoSlotBase + uint32_t(i * 2));
    }
    // weaponSlots[8] — what weapon types the player CURRENTLY owns
    // (vs. ammo above, which is how MUCH ammo each weapon has).
    // Restoring these is the difference between "you have a shotgun
    // with 5 shells" and "you have 5 shells but nothing to fire them
    // from" after a bookmark load before pickup.
    for (int i = 0; i < 8; ++i) {
        b.weapon_slots[i] = read_n64_u8(rdram, kWeaponSlotBase + uint32_t(i));
    }
    // Items bitmask — picked-up artifacts, hangar key, etc.
    b.items_bitmask = read_n64_u16(rdram, kItemsBitmask);
    make_timestamp(b.timestamp, sizeof(b.timestamp));
    if (user_label && user_label[0]) {
        std::snprintf(b.label, sizeof(b.label), "%s", user_label);
    } else {
        make_default_label(b.label, sizeof(b.label), b);
    }

    {
        std::lock_guard<std::mutex> lk(g_bookmark_mutex);
        g_bookmarks[slot] = b;
    }
    bookmarks_save_to_disk();
    std::fprintf(stderr,
        "[cheats] Bookmark slot %d: \"%s\" — pos=(%.1f, %.1f, %.1f) "
        "yaw=0x%04X spec=%u hp=%d fuel=%d level=%u (%s) [%s]\n",
        slot, b.label, double(b.fx), double(b.fy), double(b.fz),
        unsigned(uint16_t(b.yaw)), unsigned(b.vehicle_spec),
        int(b.hp), int(b.fuel), b.level, level_name(b.level), b.timestamp);
    return true;
}

// Write the bookmark back to the player entity + ammo table. Refuses
// cross-level loads (the entity-pointer-based writes would target
// the wrong entity slot under the new level's overlay).
bool bookmark_restore_slot(uint8_t* rdram, int slot) {
    if (rdram == nullptr) return false;
    if (slot < 0 || slot >= kBookmarkSlotCount) return false;

    Bookmark b;
    {
        std::lock_guard<std::mutex> lk(g_bookmark_mutex);
        b = g_bookmarks[slot];
    }
    if (!b.valid) {
        std::fprintf(stderr,
            "[cheats] Teleport slot %d: slot is empty\n", slot);
        return false;
    }
    const uint32_t cur_lvl = read_n64_u32(rdram, kCurrentLevelAddr);
    if (cur_lvl != b.level) {
        std::fprintf(stderr,
            "[cheats] Teleport slot %d: REJECTED — bookmark is for "
            "level %u (%s), you're in level %u (%s).\n",
            slot, b.level, level_name(b.level),
            cur_lvl, level_name(cur_lvl));
        return false;
    }
    const uint32_t entity = read_n64_u32(rdram, kCurrentEntityPtr);
    if (!ram_ptr_valid(entity)) {
        std::fprintf(stderr,
            "[cheats] Teleport slot %d: no current entity "
            "(cutscene/transition?)\n", slot);
        return false;
    }

    // Position (both f32 cache + s16 mirrors — BH's setX/Y/Z helpers
    // write both halves and various code reads either form).
    write_n64_f32(rdram, entity + kEntityOffsetCacheX, b.fx);
    write_n64_f32(rdram, entity + kEntityOffsetCacheY, b.fy);
    write_n64_f32(rdram, entity + kEntityOffsetCacheZ, b.fz);
    write_n64_s16(rdram, entity + kEntityOffsetPosX, int16_t(b.fx));
    write_n64_s16(rdram, entity + kEntityOffsetPosY, int16_t(b.fy));
    write_n64_s16(rdram, entity + kEntityOffsetPosZ, int16_t(b.fz));
    write_n64_s16(rdram, entity + kEntityOffsetDir, b.yaw);
    // Zero velocity so we don't fly off after teleporting.
    write_n64_f32(rdram, entity + kEntityOffsetVelX, 0.0f);
    write_n64_f32(rdram, entity + kEntityOffsetVelY, 0.0f);
    write_n64_f32(rdram, entity + kEntityOffsetVelZ, 0.0f);
    // HP / fuel — restore even if the player is currently in a
    // different vehicle than they were when bookmarked. The whatever-
    // entity-they-control gets the HP/fuel value. Skip if 0 (probably
    // means we bookmarked while on foot or the field was unused).
    if (b.hp > 0)   write_n64_s16(rdram, entity + kEntityOffsetHP,   b.hp);
    if (b.fuel > 0) write_n64_s16(rdram, entity + kEntityOffsetFuel, b.fuel);
    // Ammo — restore the full 18-counter table.
    for (int i = 0; i < 18; ++i) {
        write_n64_s16(rdram, kAmmoSlotBase + uint32_t(i * 2), b.ammo[i]);
    }
    // Weapon slots — restore which weapons the player owns. Without
    // this, a bookmark taken before getting a weapon would warp you
    // back but leave the weapon picked up; with it, you get exactly
    // what you had at bookmark time.
    for (int i = 0; i < 8; ++i) {
        write_n64_u8(rdram, kWeaponSlotBase + uint32_t(i),
                     b.weapon_slots[i]);
    }
    // Items bitmask — picked-up artifacts etc. for the current level.
    write_n64_u16(rdram, kItemsBitmask, b.items_bitmask);

    std::fprintf(stderr,
        "[cheats] Teleport slot %d: \"%s\" — warped to "
        "(%.1f, %.1f, %.1f) yaw=0x%04X, restored HP=%d fuel=%d "
        "+18 ammo +8 weapon slots +items 0x%04X. Vehicle was spec %u "
        "(current spec %u — you keep your current vehicle, not "
        "auto-morphed).\n",
        slot, b.label, double(b.fx), double(b.fy), double(b.fz),
        unsigned(uint16_t(b.yaw)), int(b.hp), int(b.fuel),
        unsigned(b.items_bitmask),
        unsigned(b.vehicle_spec),
        unsigned(read_n64_u8(rdram, entity + kEntityOffsetSpecIdx)));
    return true;
}

// Clear a slot — for the Manager's "Delete" button.
void bookmark_clear_slot(int slot) {
    if (slot < 0 || slot >= kBookmarkSlotCount) return;
    {
        std::lock_guard<std::mutex> lk(g_bookmark_mutex);
        g_bookmarks[slot] = Bookmark{};
    }
    bookmarks_save_to_disk();
    std::fprintf(stderr, "[cheats] Bookmark slot %d cleared.\n", slot);
}

// Simple cheats — operate on slot 0 (the "quick" slot).
void custom_bookmark_position(uint8_t* rdram) {
    bookmarks_load_from_disk_if_needed();
    bookmark_capture_slot(rdram, 0, nullptr);
}
void custom_teleport_to_bookmark(uint8_t* rdram) {
    bookmarks_load_from_disk_if_needed();
    bookmark_restore_slot(rdram, 0);
}

// ===========================================================================
// Bookmark disk persistence — bh_bookmarks.json
//
// Auto-saved on every capture/clear. Lazy-loaded on first access.
// Format mirrors our terrain.json / entities.json convention: simple
// hand-written JSON with magic + version header.
// ===========================================================================

// Write a JSON-safe copy of `src` into `dst` (up to `dst_cap-1` chars).
// Escapes \, ", \n. Labels are user-input; treat as untrusted.
void json_escape_into(char* dst, size_t dst_cap, const char* src) {
    size_t w = 0;
    for (size_t i = 0; src[i] != '\0' && w + 2 < dst_cap; ++i) {
        const unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (w + 3 >= dst_cap) break;
            dst[w++] = '\\';
            dst[w++] = char(c);
        } else if (c == '\n') {
            if (w + 3 >= dst_cap) break;
            dst[w++] = '\\';
            dst[w++] = 'n';
        } else if (c < 0x20) {
            // Skip other control chars rather than emit \uXXXX.
        } else {
            dst[w++] = char(c);
        }
    }
    dst[w] = '\0';
}

void bookmarks_save_to_disk() {
    std::array<Bookmark, kBookmarkSlotCount> snap;
    {
        std::lock_guard<std::mutex> lk(g_bookmark_mutex);
        snap = g_bookmarks;
    }
    FILE* f = std::fopen(kBookmarkFilename, "wb");
    if (f == nullptr) {
        std::fprintf(stderr,
            "[cheats] bookmarks: save failed (could not open %s)\n",
            kBookmarkFilename);
        return;
    }
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"magic\": \"BHBOOKMARKS\",\n");
    std::fprintf(f, "  \"version\": 1,\n");
    std::fprintf(f, "  \"slot_count\": %d,\n", kBookmarkSlotCount);
    std::fprintf(f, "  \"slots\": [\n");
    for (int i = 0; i < kBookmarkSlotCount; ++i) {
        const Bookmark& b = snap[i];
        if (!b.valid) {
            std::fprintf(f, "    null%s\n",
                         (i + 1 < kBookmarkSlotCount) ? "," : "");
            continue;
        }
        char esc_label[80];
        json_escape_into(esc_label, sizeof(esc_label), b.label);
        std::fprintf(f,
            "    { \"label\": \"%s\", \"timestamp\": \"%s\","
            " \"level\": %u, \"level_name\": \"%s\","
            " \"x\": %.3f, \"y\": %.3f, \"z\": %.3f,"
            " \"yaw\": %d, \"vehicle_spec\": %u,"
            " \"hp\": %d, \"fuel\": %d,"
            " \"items_bitmask\": %u,"
            " \"weapon_slots\": [",
            esc_label, b.timestamp,
            b.level, level_name(b.level),
            double(b.fx), double(b.fy), double(b.fz),
            int(b.yaw), unsigned(b.vehicle_spec),
            int(b.hp), int(b.fuel),
            unsigned(b.items_bitmask));
        for (int k = 0; k < 8; ++k) {
            std::fprintf(f, "%u%s",
                         unsigned(b.weapon_slots[k]), k < 7 ? ", " : "");
        }
        std::fprintf(f, "], \"ammo\": [");
        for (int k = 0; k < 18; ++k) {
            std::fprintf(f, "%d%s", int(b.ammo[k]), k < 17 ? ", " : "");
        }
        std::fprintf(f, "] }%s\n",
                     (i + 1 < kBookmarkSlotCount) ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
}

// Minimal JSON parser — only handles the exact shape we emit. We
// can't pull in a full JSON lib without a CMakeLists touch (which
// would trigger the cmake-cache-rebuild fragility we hit before).
// So: scan for keys + parse values inline. Tolerant of whitespace,
// strict about field names.
bool parse_int(const char*& p, const char* end, long long& out) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    char* ep = nullptr;
    long long v = std::strtoll(p, &ep, 10);
    if (ep == p) return false;
    p = ep; out = v; return true;
}
bool parse_double(const char*& p, const char* end, double& out) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    char* ep = nullptr;
    double v = std::strtod(p, &ep);
    if (ep == p) return false;
    p = ep; out = v; return true;
}
bool parse_string(const char*& p, const char* end, char* dst, size_t dst_cap) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    if (p >= end || *p != '"') return false;
    ++p;
    size_t w = 0;
    while (p < end && *p != '"' && w + 1 < dst_cap) {
        if (*p == '\\' && p + 1 < end) {
            ++p;
            if (*p == 'n') dst[w++] = '\n';
            else if (*p == '"') dst[w++] = '"';
            else if (*p == '\\') dst[w++] = '\\';
            else dst[w++] = *p;
            ++p;
        } else {
            dst[w++] = *p++;
        }
    }
    if (p >= end || *p != '"') return false;
    ++p;
    dst[w] = '\0';
    return true;
}
// Find the next occurrence of `"key":` in [p, end), advance p just
// past the colon. Returns false on miss.
bool seek_key(const char*& p, const char* end, const char* key) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    while (p < end) {
        const char* hit =
            std::strstr(p, needle.c_str());
        if (hit == nullptr) return false;
        p = hit + needle.size();
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        if (p < end && *p == ':') { ++p; return true; }
    }
    return false;
}

void bookmarks_load_from_disk_if_needed() {
    bool expected = false;
    if (!g_bookmarks_loaded.compare_exchange_strong(expected, true)) return;

    FILE* f = std::fopen(kBookmarkFilename, "rb");
    if (f == nullptr) {
        std::fprintf(stderr,
            "[cheats] bookmarks: no %s on disk (clean slate, %d empty slots)\n",
            kBookmarkFilename, kBookmarkSlotCount);
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) { // 1 MB cap; bookmarks file is tiny
        std::fclose(f);
        std::fprintf(stderr,
            "[cheats] bookmarks: %s size out of range (%ld bytes)\n",
            kBookmarkFilename, sz);
        return;
    }
    std::string body;
    body.resize(size_t(sz));
    std::fread(body.data(), 1, size_t(sz), f);
    std::fclose(f);

    // Validate magic
    if (body.find("\"BHBOOKMARKS\"") == std::string::npos) {
        std::fprintf(stderr,
            "[cheats] bookmarks: %s has wrong magic, ignoring\n",
            kBookmarkFilename);
        return;
    }

    // Walk the slots array. We look for each `{ ... }` block in
    // sequence; null entries (empty slots) are skipped.
    std::array<Bookmark, kBookmarkSlotCount> loaded{};
    const char* p   = body.c_str();
    const char* end = p + body.size();
    if (!seek_key(p, end, "slots")) {
        std::fprintf(stderr,
            "[cheats] bookmarks: %s missing 'slots' array\n",
            kBookmarkFilename);
        return;
    }
    // Skip to first '['
    while (p < end && *p != '[') ++p;
    if (p >= end) return;
    ++p;

    int loaded_count = 0;
    for (int slot = 0; slot < kBookmarkSlotCount && p < end; ++slot) {
        // Skip whitespace + commas
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
                           *p == '\r' || *p == ',')) ++p;
        if (p >= end || *p == ']') break;
        if (*p == 'n') {
            // "null"
            p += 4;
            continue;
        }
        if (*p != '{') {
            // unrecognized; skip char
            ++p;
            continue;
        }
        // Find the end of this object
        const char* obj_start = p;
        int depth = 1;
        const char* q = p + 1;
        while (q < end && depth > 0) {
            if (*q == '{') ++depth;
            else if (*q == '}') --depth;
            ++q;
        }
        const char* obj_end = q;

        // Parse fields inside [obj_start, obj_end).
        Bookmark b;
        b.valid = true;
        const char* sp = obj_start;
        long long iv;
        double dv;

        sp = obj_start;
        if (seek_key(sp, obj_end, "label")) {
            parse_string(sp, obj_end, b.label, sizeof(b.label));
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "timestamp")) {
            parse_string(sp, obj_end, b.timestamp, sizeof(b.timestamp));
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "level") && parse_int(sp, obj_end, iv)) {
            b.level = uint32_t(iv);
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "x") && parse_double(sp, obj_end, dv)) {
            b.fx = float(dv);
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "y") && parse_double(sp, obj_end, dv)) {
            b.fy = float(dv);
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "z") && parse_double(sp, obj_end, dv)) {
            b.fz = float(dv);
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "yaw") && parse_int(sp, obj_end, iv)) {
            b.yaw = int16_t(iv);
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "vehicle_spec") &&
            parse_int(sp, obj_end, iv)) {
            b.vehicle_spec = uint8_t(iv);
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "hp") && parse_int(sp, obj_end, iv)) {
            b.hp = int16_t(iv);
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "fuel") && parse_int(sp, obj_end, iv)) {
            b.fuel = int16_t(iv);
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "ammo")) {
            // ammo: [ ... 18 ints ... ] (v2 format; v1 had 17, the
            // missing 18th entry will read as 0 and harmlessly default)
            while (sp < obj_end && *sp != '[') ++sp;
            if (sp < obj_end) {
                ++sp;
                for (int k = 0; k < 18; ++k) {
                    while (sp < obj_end && (*sp == ' ' || *sp == ',' ||
                                            *sp == '\n' || *sp == '\r' ||
                                            *sp == '\t')) ++sp;
                    if (parse_int(sp, obj_end, iv)) {
                        b.ammo[k] = int16_t(iv);
                    }
                }
            }
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "weapon_slots")) {
            while (sp < obj_end && *sp != '[') ++sp;
            if (sp < obj_end) {
                ++sp;
                for (int k = 0; k < 8; ++k) {
                    while (sp < obj_end && (*sp == ' ' || *sp == ',' ||
                                            *sp == '\n' || *sp == '\r' ||
                                            *sp == '\t')) ++sp;
                    if (parse_int(sp, obj_end, iv)) {
                        b.weapon_slots[k] = uint8_t(iv);
                    }
                }
            }
        }
        sp = obj_start;
        if (seek_key(sp, obj_end, "items_bitmask") &&
            parse_int(sp, obj_end, iv)) {
            b.items_bitmask = uint16_t(iv);
        }

        loaded[slot] = b;
        ++loaded_count;
        p = obj_end;
    }
    {
        std::lock_guard<std::mutex> lk(g_bookmark_mutex);
        g_bookmarks = loaded;
    }
    std::fprintf(stderr,
        "[cheats] bookmarks: loaded %d slot(s) from %s\n",
        loaded_count, kBookmarkFilename);
}

// Toggle the per-tick render-cull bypass. On = every input poll
// writes D_8014FD2A = 0x8000. Off = stop writing; BH recomputes the
// real FOV on its next camera update and resumes normal culling.
void custom_toggle_render_cull_bypass(uint8_t* /*rdram*/) {
    const bool was_on = g_render_cull_bypass_active.exchange(
        !g_render_cull_bypass_active.load(std::memory_order_acquire),
        std::memory_order_acq_rel);
    // Reset the per-tick diagnostic counter so the first 3 ticks after
    // each toggle log their before/after values. Same for the weak-
    // symbol override fire logs so we can confirm both bbox and LOS
    // overrides are actually being called by BH's vehicle render.
    g_cull_tick_log_count.store(0, std::memory_order_release);
    g_cull_override_log_count.store(0, std::memory_order_release);
    g_los_override_log_count.store(0, std::memory_order_release);
    std::fprintf(stderr,
        "[cheats] Render cull bypass %s. %s\n",
        was_on ? "OFF" : "ON",
        was_on ? "Game-side culling resumes next camera-update tick."
               : "All entities will render regardless of distance/frustum. "
                 "Per-tick override active (writes D_8014FD2A=0x8000 + "
                 "D_80157590=1 each input poll). First 3 ticks log "
                 "diagnostics to stderr.");
}

void custom_drain_water(uint8_t* rdram) {
    water_snapshot_if_needed(rdram);
    g_water_target_y.store(kWaterDisabledY, std::memory_order_release);
    g_water_override_active.store(true, std::memory_order_release);
    std::fprintf(stderr,
        "[cheats] Drain water: water Y override ENABLED, "
        "D_80222A70 = %d (re-applied each tick).\n",
        kWaterDisabledY);
}

// Restore: clear the tick-override flag AND write the snapshotted
// original value back so water doesn't sit at the drain value with
// no one to push it back up. Previous version just cleared the flag,
// leaving D_80222A70 at -2000 since BH itself has no reason to write
// it on a normal frame (game-side water Y only changes on state
// transitions, which may not happen for a long time).
void custom_restore_water(uint8_t* rdram) {
    const bool was_active =
        g_water_override_active.exchange(false, std::memory_order_acq_rel);
    if (rdram != nullptr &&
        g_water_snapshot_valid.load(std::memory_order_acquire)) {
        const int32_t orig =
            g_water_original_y.load(std::memory_order_acquire);
        write_n64_u32(rdram, kWaterLevelAddr, uint32_t(orig));
        std::fprintf(stderr,
            "[cheats] Restore water: override %s; wrote original "
            "D_80222A70 = %d back.\n",
            was_active ? "DISABLED" : "(was already off)",
            orig);
    } else {
        std::fprintf(stderr,
            "[cheats] Restore water: override cleared but no snapshot "
            "available (water override was never activated this session, "
            "or game wasn't running when activated). Nothing to write back.\n");
    }
}

// Activation cheat: enable the per-tick re-apply AND fire one
// immediate neutering so the effect is visible the same frame.
void custom_disable_shield_walls(uint8_t* rdram) {
    if (rdram == nullptr) return;
    const uint32_t lvl = read_n64_u32(rdram, kCurrentLevelAddr);
    if (lvl < 1 || lvl > 5) {
        std::fprintf(stderr,
            "[cheats] Disable shield walls: invalid level %u\n", lvl);
        return;
    }
    const bool was_active = g_disable_shields_active.exchange(true);
    apply_shield_disable_for_level(rdram, lvl);
    std::fprintf(stderr,
        "[cheats] Disable shield walls: %s. Walls invisible + "
        "non-blocking + gate visuals hidden for level %u (%s). "
        "Will auto-reapply each tick to survive building exits / "
        "level transitions. Portal data (D_8003E0FC) left intact "
        "for non-visual gameplay events.\n",
        was_active ? "re-applied (already on)" : "ENABLED",
        lvl, level_name(lvl));
}

// ===========================================================================
// Terrain export/import.
//
// Body Harvest's save format only records a beacon ID and player stats —
// it doesn't persist runtime modifications to the tile array. Crater /
// flatten / raise / lower / smooth all evaporate on level reload. To
// keep custom landscapes across saves, we let the user snapshot the
// current level's full 256x256 tile grid to a JSON file and re-apply it
// on demand.
//
// File format (UTF-8, line-broken for human readability):
//   {
//     "magic": "BHTERRAIN",
//     "version": 1,
//     "level": 4,
//     "level_name": "Siberia",
//     "width": 256,
//     "height": 256,
//     "tiles_hex": "0000002100340021..."   // 256*256*4 = 262144 hex chars
//   }
//
// tiles_hex stores each u16 as 4 lowercase hex chars in row-major
// order (z * width + x). All 16 bits are preserved — height, state,
// texture/type — so re-applying produces an identical terrain state.
// ===========================================================================

constexpr uint32_t kTerrainFileVersion = 1;

// Minimal JSON helpers — we only need to read 6 fields and write 7,
// all primitives or quoted strings, so a full JSON library is overkill.
// These cope with the very narrow subset of JSON our own writer emits;
// they will reject anything weird with a clear error.

inline void skip_ws(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
}

bool json_find_key(const std::string& body, const char* key, size_t& out_pos) {
    // Find "key" (with quotes) followed by optional whitespace + colon.
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    size_t p = 0;
    while (p < body.size()) {
        const size_t hit = body.find(needle, p);
        if (hit == std::string::npos) return false;
        size_t q = hit + needle.size();
        while (q < body.size() && (body[q] == ' ' || body[q] == '\t' ||
                                   body[q] == '\n' || body[q] == '\r')) ++q;
        if (q < body.size() && body[q] == ':') {
            ++q;
            while (q < body.size() && (body[q] == ' ' || body[q] == '\t' ||
                                       body[q] == '\n' || body[q] == '\r')) ++q;
            out_pos = q;
            return true;
        }
        p = hit + 1;
    }
    return false;
}

bool json_get_int(const std::string& body, const char* key, long long& out) {
    size_t p;
    if (!json_find_key(body, key, p)) return false;
    const char* start = body.c_str() + p;
    char* endp = nullptr;
    const long long v = std::strtoll(start, &endp, 10);
    if (endp == start) return false;
    out = v;
    return true;
}

bool json_get_string(const std::string& body, const char* key, std::string& out) {
    size_t p;
    if (!json_find_key(body, key, p)) return false;
    if (p >= body.size() || body[p] != '"') return false;
    const size_t start = p + 1;
    const size_t close = body.find('"', start);
    if (close == std::string::npos) return false;
    out.assign(body, start, close - start);
    return true;
}

bool export_terrain_to_file(uint8_t* rdram, HWND parent, std::string& status_out) {
    if (rdram == nullptr) {
        status_out = "Export failed: game not running.";
        return false;
    }
    const uint32_t tiles_virt = read_n64_u32(rdram, kTerrainTilesPtr);
    if (!ram_ptr_valid(tiles_virt)) {
        status_out = "Export failed: no level loaded (terrain pointer is null).";
        return false;
    }
    const uint32_t lvl = read_n64_u32(rdram, kCurrentLevelAddr);
    if (lvl < 1 || lvl > 5) {
        status_out = "Export failed: current level is invalid.";
        return false;
    }

    // Default filename: terrain_<level>_<levelname>.json
    wchar_t initial_name[64];
    {
        const char* nm = level_name(lvl);
        wchar_t nm_w[24] = {0};
        MultiByteToWideChar(CP_UTF8, 0, nm, -1, nm_w, 24);
        swprintf(initial_name, 64, L"terrain_%u_%s.json",
                 unsigned(lvl), nm_w);
    }

    wchar_t path[MAX_PATH];
    std::wmemcpy(path, initial_name, std::wcslen(initial_name) + 1);

    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = parent;
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrFilter  = L"BH terrain (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrDefExt  = L"json";
    ofn.lpstrTitle   = L"Export current level's terrain";
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        status_out = "Export cancelled.";
        return false;
    }

    // Build JSON body. tiles_hex is the bulky part — 262144 chars.
    std::string body;
    body.reserve(512 + 4 * size_t(kTerrainW) * size_t(kTerrainH));
    body += "{\n";
    body += "  \"magic\": \"BHTERRAIN\",\n";
    body += "  \"version\": ";
    body += std::to_string(kTerrainFileVersion);
    body += ",\n";
    body += "  \"level\": ";
    body += std::to_string(lvl);
    body += ",\n";
    body += "  \"level_name\": \"";
    body += level_name(lvl);
    body += "\",\n";
    body += "  \"width\": ";
    body += std::to_string(kTerrainW);
    body += ",\n";
    body += "  \"height\": ";
    body += std::to_string(kTerrainH);
    body += ",\n";
    body += "  \"tiles_hex\": \"";
    {
        char buf[8];
        for (int z = 0; z < kTerrainH; ++z) {
            for (int x = 0; x < kTerrainW; ++x) {
                const uint32_t addr =
                    tiles_virt + uint32_t(z * kTerrainW + x) * 2u;
                const uint16_t v = read_n64_u16(rdram, addr);
                std::snprintf(buf, sizeof(buf), "%04x", unsigned(v));
                body += buf;
            }
        }
    }
    body += "\",\n";

    // Shield walls for the current level (up to 6, raw s16 values).
    // The terrain editor uses these to draw bounding boxes + gate
    // openings on the heightmap overlay. A wall with all-zero fields
    // (or zeroed by the "Disable shield walls" cheat) is treated as
    // inactive by the editor.
    body += "  \"shield_walls\": [\n";
    {
        const uint32_t lvl_base =
            kShieldWallsBase + (lvl - 1u) * kShieldWallsLevelStride;
        char buf[24];
        for (int i = 0; i < kShieldWallsPerLevel; ++i) {
            const uint32_t wall_addr = lvl_base + uint32_t(i) * kShieldWallSize;
            // Wall fields ARE signed s16 in world units. Looking at
            // a Greece dump: wall 3 had values like (-6656, -6656,
            // 6656, 6656) which is a clean central rectangle around
            // the origin. The collision code's `>> 10` is arithmetic
            // (signed) shift so negative wall coords give negative
            // grid indices, which then get offset by +0x820 in the
            // D_8021EA30 array indexing. So world-Z = wall_value /
            // 1 (no scaling), tile = world_value / 256 + 128.
            const int16_t f[12] = {
                read_n64_s16(rdram, wall_addr + 0x00),  // minX
                read_n64_s16(rdram, wall_addr + 0x02),  // minZ
                read_n64_s16(rdram, wall_addr + 0x04),  // maxX
                read_n64_s16(rdram, wall_addr + 0x06),  // maxZ
                read_n64_s16(rdram, wall_addr + 0x08),  // gate1 x0
                read_n64_s16(rdram, wall_addr + 0x0A),  // gate1 z0
                read_n64_s16(rdram, wall_addr + 0x0C),  // gate1 x1
                read_n64_s16(rdram, wall_addr + 0x0E),  // gate1 z1
                read_n64_s16(rdram, wall_addr + 0x10),  // gate2 x0
                read_n64_s16(rdram, wall_addr + 0x12),  // gate2 z0
                read_n64_s16(rdram, wall_addr + 0x14),  // gate2 x1
                read_n64_s16(rdram, wall_addr + 0x16),  // gate2 z1
            };
            body += "    { \"min_x\": ";  body += std::to_string(f[0]);
            body += ", \"min_z\": ";       body += std::to_string(f[1]);
            body += ", \"max_x\": ";       body += std::to_string(f[2]);
            body += ", \"max_z\": ";       body += std::to_string(f[3]);
            body += ", \"gate1\": { \"x0\": ";
            body += std::to_string(f[4]);  body += ", \"z0\": ";
            body += std::to_string(f[5]);  body += ", \"x1\": ";
            body += std::to_string(f[6]);  body += ", \"z1\": ";
            body += std::to_string(f[7]);  body += " }";
            body += ", \"gate2\": { \"x0\": ";
            body += std::to_string(f[8]);  body += ", \"z0\": ";
            body += std::to_string(f[9]);  body += ", \"x1\": ";
            body += std::to_string(f[10]); body += ", \"z1\": ";
            body += std::to_string(f[11]); body += " } }";
            body += (i + 1 < kShieldWallsPerLevel) ? ",\n" : "\n";
            (void)buf;
        }
    }
    body += "  ],\n";

    // Shield-wall gate (portal) positions for the current level.
    // 8 slots; unused tend to be all-zero. NOTE: the decomp's struct
    // comments label offsets (0,2,4) as (X, Z, Y) but the actual game
    // code in `func_800BD688_CC638` uses them as (X, Y, Z):
    //   - offset 0x00: stored X, world_X = stored_X << 8
    //   - offset 0x02: world Y (vertical, no scaling)
    //   - offset 0x04: stored Z, world_Z = stored_Z << 8
    // So X and Z are stored as `world / 256` (compressed); Y is raw.
    // We export the stored (compressed) values verbatim and label
    // them correctly; the editor multiplies to get world coords.
    body += "  \"shield_gates\": [\n";
    {
        const uint32_t lvl_base = kShieldGatesBase +
            (lvl - 1u) * uint32_t(kShieldGatesPerLevel) * kShieldGateSize;
        for (int i = 0; i < kShieldGatesPerLevel; ++i) {
            const uint32_t g = lvl_base + uint32_t(i) * kShieldGateSize;
            const int16_t  x_cmp = read_n64_s16(rdram, g + 0x00); // X / 256
            const int16_t  y     = read_n64_s16(rdram, g + 0x02); // Y raw
            const int16_t  z_cmp = read_n64_s16(rdram, g + 0x04); // Z / 256
            const int8_t   state = int8_t(read_n64_u8(rdram, g + 0x06));
            const uint8_t  unk7  = read_n64_u8(rdram, g + 0x07);
            const uint8_t  unk8  = read_n64_u8(rdram, g + 0x08);
            const uint8_t  type  = read_n64_u8(rdram, g + 0x09);
            body += "    { \"x\": ";       body += std::to_string(x_cmp);
            body += ", \"y\": ";            body += std::to_string(y);
            body += ", \"z\": ";            body += std::to_string(z_cmp);
            body += ", \"state\": ";        body += std::to_string(int(state));
            body += ", \"unk7\": ";         body += std::to_string(unk7);
            body += ", \"unk8\": ";         body += std::to_string(unk8);
            body += ", \"type\": ";         body += std::to_string(type);
            body += " }";
            body += (i + 1 < kShieldGatesPerLevel) ? ",\n" : "\n";
        }
    }
    body += "  ]\n}\n";

    // Write file.
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        status_out = "Export failed: could not open file for writing.";
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, body.data(), DWORD(body.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != body.size()) {
        status_out = "Export failed: write error.";
        return false;
    }

    char st[128];
    std::snprintf(st, sizeof(st),
        "Exported level %u (%s), %u tiles, %u bytes.",
        unsigned(lvl), level_name(lvl),
        unsigned(kTerrainW * kTerrainH), unsigned(body.size()));
    status_out = st;
    std::fprintf(stderr, "[cheats] terrain export -> %ls (%s)\n",
                 path, st);
    return true;
}

bool import_terrain_from_file(uint8_t* rdram, HWND parent,
                              std::string& status_out) {
    if (rdram == nullptr) {
        status_out = "Import failed: game not running.";
        return false;
    }
    const uint32_t tiles_virt = read_n64_u32(rdram, kTerrainTilesPtr);
    if (!ram_ptr_valid(tiles_virt)) {
        status_out = "Import failed: no level loaded (terrain pointer is null).";
        return false;
    }
    const uint32_t lvl = read_n64_u32(rdram, kCurrentLevelAddr);
    if (lvl < 1 || lvl > 5) {
        status_out = "Import failed: current level is invalid.";
        return false;
    }

    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = parent;
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrFilter  = L"BH terrain (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrDefExt  = L"json";
    ofn.lpstrTitle   = L"Import terrain into current level";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        status_out = "Import cancelled.";
        return false;
    }

    // Slurp file.
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        status_out = "Import failed: could not open file.";
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (8 << 20)) { // 8 MiB sanity cap
        CloseHandle(h);
        status_out = "Import failed: file size out of range.";
        return false;
    }
    std::string body;
    body.resize(size_t(sz.QuadPart));
    DWORD got = 0;
    BOOL ok = ReadFile(h, body.data(), DWORD(body.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != body.size()) {
        status_out = "Import failed: read error.";
        return false;
    }

    // Parse + validate.
    std::string magic, level_name_str, tiles_hex;
    long long version = 0, file_level = 0, width = 0, height = 0;
    if (!json_get_string(body, "magic", magic) || magic != "BHTERRAIN") {
        status_out = "Import failed: not a BHTERRAIN file.";
        return false;
    }
    if (!json_get_int(body, "version", version) ||
        version != kTerrainFileVersion) {
        char st[96];
        std::snprintf(st, sizeof(st),
            "Import failed: unsupported version %lld (expected %u).",
            version, unsigned(kTerrainFileVersion));
        status_out = st;
        return false;
    }
    if (!json_get_int(body, "level", file_level)) {
        status_out = "Import failed: missing 'level' field.";
        return false;
    }
    // Level mismatch is no longer a hard fail — let the user opt in
    // to cross-level imports for fun (e.g. painting Greece's hills
    // into Java's jungle). The tile array format is identical across
    // all 5 levels: same 256x256 dims, same bit layout (height 0-5,
    // state 6-9, type 10-15). What CAN go wrong:
    //   - Texture/type bits (10-15) may reference per-level texture
    //     sets that don't exist in the destination, causing missing/
    //     wrong-looking tiles. Visually weird, not crash-prone.
    //   - State bits (6-9) like pit/wall markers interact with
    //     per-level scripts (e.g. Java's water-fall flag is ignored
    //     elsewhere) — collision usually works, scripted triggers
    //     might not fire.
    //   - Level geometry like buildings, beacons, vehicles is NOT
    //     in the tile grid — it's spawned separately. Mismatch can
    //     leave buildings floating in midair or buried in new hills.
    // None of those are corrupting, just amusing. So we confirm
    // rather than block.
    if (uint32_t(file_level) != lvl) {
        wchar_t prompt[512];
        wchar_t file_name_w[24] = {0};
        wchar_t cur_name_w[24]  = {0};
        MultiByteToWideChar(CP_UTF8, 0,
            level_name(uint32_t(file_level)), -1, file_name_w, 24);
        MultiByteToWideChar(CP_UTF8, 0,
            level_name(lvl), -1, cur_name_w, 24);
        swprintf(prompt, 512,
            L"Cross-level import:\n\n"
            L"  File is for level %lld (%ls)\n"
            L"  You are currently in level %u (%ls)\n\n"
            L"The tile grid format is identical across levels, but "
            L"texture/state bits may not all translate cleanly. "
            L"Buildings, beacons, and vehicles are NOT in the terrain "
            L"file, so they may end up floating or buried after the "
            L"import. Crashes are unlikely; weird visuals are likely.\n\n"
            L"Apply anyway?",
            file_level, file_name_w, unsigned(lvl), cur_name_w);
        const int rc = MessageBoxW(parent, prompt,
            L"BH Terrain — Level Mismatch",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (rc != IDYES) {
            char st[160];
            std::snprintf(st, sizeof(st),
                "Import cancelled: cross-level import (file=%lld %s -> "
                "current=%u %s) declined.",
                file_level, level_name(uint32_t(file_level)),
                unsigned(lvl), level_name(lvl));
            status_out = st;
            return false;
        }
        std::fprintf(stderr,
            "[cheats] terrain import: cross-level confirmed "
            "(file=%lld %s -> current=%u %s)\n",
            file_level, level_name(uint32_t(file_level)),
            unsigned(lvl), level_name(lvl));
    }
    if (!json_get_int(body, "width", width)  || width  != kTerrainW ||
        !json_get_int(body, "height", height) || height != kTerrainH) {
        status_out = "Import failed: tile-grid dimensions don't match.";
        return false;
    }
    if (!json_get_string(body, "tiles_hex", tiles_hex)) {
        status_out = "Import failed: missing 'tiles_hex' field.";
        return false;
    }
    const size_t expected = size_t(kTerrainW) * size_t(kTerrainH) * 4u;
    if (tiles_hex.size() != expected) {
        char st[128];
        std::snprintf(st, sizeof(st),
            "Import failed: tiles_hex has %u chars (expected %u).",
            unsigned(tiles_hex.size()), unsigned(expected));
        status_out = st;
        return false;
    }

    // Decode 4 hex chars per tile and write back. Reject malformed
    // hex so a partial corrupt file can't scribble random tiles.
    auto hex_nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const char* hp = tiles_hex.c_str();
    int applied = 0;
    for (int z = 0; z < kTerrainH; ++z) {
        for (int x = 0; x < kTerrainW; ++x) {
            const int n0 = hex_nibble(hp[0]);
            const int n1 = hex_nibble(hp[1]);
            const int n2 = hex_nibble(hp[2]);
            const int n3 = hex_nibble(hp[3]);
            if ((n0 | n1 | n2 | n3) < 0) {
                status_out = "Import failed: bad hex character in tiles.";
                return false;
            }
            const uint16_t v = uint16_t((n0 << 12) | (n1 << 8) |
                                         (n2 << 4)  |  n3);
            const uint32_t addr =
                tiles_virt + uint32_t(z * kTerrainW + x) * 2u;
            write_n64_u16(rdram, addr, v);
            ++applied;
            hp += 4;
        }
    }

    char st[224];
    if (uint32_t(file_level) != lvl) {
        std::snprintf(st, sizeof(st),
            "Imported %d tiles from level %lld (%s) into level %u (%s) "
            "[CROSS-LEVEL]. Walk around to see new mesh.",
            applied,
            file_level, level_name(uint32_t(file_level)),
            unsigned(lvl), level_name(lvl));
    } else {
        std::snprintf(st, sizeof(st),
            "Imported %d tiles into level %u (%s). Walk around to see new mesh.",
            applied, unsigned(lvl), level_name(lvl));
    }
    status_out = st;
    std::fprintf(stderr, "[cheats] terrain import <- %ls (%s)\n", path, st);
    return true;
}

// ===========================================================================
// Entity dump — write the current level's active buildings, vehicles,
// aliens, and save beacons to a separate JSON. Editor overlays markers
// on the heightmap so you can see where stuff lives. Read-only for now;
// editing/spawning back would be a separate import path that respects
// the slot-count limits (128 vehicles, 254 aliens, 255 buildings).
//
// "Active" detection per entity type:
//   - Building: buildingType != 0  (type 0 = unallocated slot)
//   - Vehicle:  specIndex != 0 OR active-flag bit set in unk20
//   - Alien:    specIndex != 0
//   - Beacon:   we just dump all 14 verbatim; level field tells you
//               which ones belong to which level
//
// File: entities_<level>_<name>.json
//
// Format:
//   {
//     "magic": "BHENTITIES", "version": 1,
//     "level": 1, "level_name": "Greece",
//     "buildings": [ { "slot":N, "x":..., "y":..., "z":...,
//                      "type":..., "hp":..., "rotation":...,
//                      "door1":..., "door2":..., "door3":..., "state":... }, ... ],
//     "vehicles":  [ { "slot":..., "spec":..., "x":..., "y":..., "z":...,
//                      "yaw":..., "hp":..., "fuel":..., "flags":... }, ... ],
//     "aliens":    [ { "slot":..., "spec":..., "x":..., "y":..., "z":... }, ... ],
//     "save_beacons": [ { "slot":..., "raw_hex":"AABBCCDDEEFFGGHH" }, ... ]
//   }
// ===========================================================================

bool export_entities_to_file(uint8_t* rdram, HWND parent, std::string& status_out) {
    if (rdram == nullptr) {
        status_out = "Export failed: game not running.";
        return false;
    }
    const uint32_t lvl = read_n64_u32(rdram, kCurrentLevelAddr);
    if (lvl < 1 || lvl > 5) {
        status_out = "Export failed: invalid level (load a save first).";
        return false;
    }

    // Default filename: entities_<level>_<name>.json
    wchar_t initial_name[64];
    {
        wchar_t nm_w[24] = {0};
        MultiByteToWideChar(CP_UTF8, 0, level_name(lvl), -1, nm_w, 24);
        swprintf(initial_name, 64, L"entities_%u_%s.json",
                 unsigned(lvl), nm_w);
    }
    wchar_t path[MAX_PATH];
    std::wmemcpy(path, initial_name, std::wcslen(initial_name) + 1);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = parent;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"BH entities (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle  = L"Export current level's entities";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        status_out = "Export cancelled.";
        return false;
    }

    std::string body;
    body.reserve(64 * 1024); // 64 KB starting cap, will grow if needed
    body += "{\n";
    body += "  \"magic\": \"BHENTITIES\",\n";
    body += "  \"version\": 1,\n";
    body += "  \"level\": ";       body += std::to_string(lvl);  body += ",\n";
    body += "  \"level_name\": \""; body += level_name(lvl);    body += "\",\n";

    // --- Buildings ---------------------------------------------------------
    int buildings_seen = 0;
    body += "  \"buildings\": [\n";
    {
        bool first = true;
        for (int i = 0; i < kBuildingInstanceCount; ++i) {
            const uint32_t b = kBuildingInstancesBase +
                               uint32_t(i) * kBuildingInstanceSize;
            const uint8_t  type = read_n64_u8(rdram, b + 0x06);
            const int16_t  x    = read_n64_s16(rdram, b + 0x00);
            const int16_t  y    = read_n64_s16(rdram, b + 0x02);
            const int16_t  z    = read_n64_s16(rdram, b + 0x04);
            // Skip un-placed slots. Per the struct docs `buildingType
            // == 0` means "no model assigned" — those are the
            // pre-allocated pool slots that pile up at the bottom of
            // the map. We strictly require a real type byte; the
            // coord-zero check was redundant + let through placeholders
            // with a stale type.
            if (type == 0) continue;
            const uint8_t state    = read_n64_u8(rdram, b + 0x0A);
            const uint8_t rotation = read_n64_u8(rdram, b + 0x0B);
            const uint8_t hp       = read_n64_u8(rdram, b + 0x0F);
            const uint8_t door1    = read_n64_u8(rdram, b + 0x12);
            const uint8_t door2    = read_n64_u8(rdram, b + 0x13);
            const uint8_t door3    = read_n64_u8(rdram, b + 0x14);
            if (!first) body += ",\n";
            first = false;
            body += "    { \"slot\": " + std::to_string(i);
            body += ", \"x\": "        + std::to_string(x);
            body += ", \"y\": "        + std::to_string(y);
            body += ", \"z\": "        + std::to_string(z);
            body += ", \"type\": "     + std::to_string(unsigned(type));
            body += ", \"hp\": "       + std::to_string(unsigned(hp));
            body += ", \"rotation\": " + std::to_string(unsigned(rotation));
            body += ", \"state\": "    + std::to_string(unsigned(state));
            body += ", \"door1\": "    + std::to_string(unsigned(door1));
            body += ", \"door2\": "    + std::to_string(unsigned(door2));
            body += ", \"door3\": "    + std::to_string(unsigned(door3));
            body += " }";
            ++buildings_seen;
        }
    }
    body += "\n  ],\n";

    // --- Vehicles ----------------------------------------------------------
    int vehicles_seen = 0;
    body += "  \"vehicles\": [\n";
    {
        bool first = true;
        for (int i = 0; i < kVehicleInstanceCount; ++i) {
            const uint32_t v = kVehicleInstancesBase +
                               uint32_t(i) * kVehicleInstanceSize;
            const uint8_t  spec  = read_n64_u8(rdram, v + kEntityOffsetSpecIdx);
            const uint16_t flags = read_n64_u16(rdram, v + kEntityOffsetFlags);
            // Skip slots that are clearly empty: spec 0 AND no active bit.
            if (spec == 0 && (flags & kVehicleActiveFlagBit) == 0) continue;
            const int16_t x    = read_n64_s16(rdram, v + kEntityOffsetPosX);
            const int16_t y    = read_n64_s16(rdram, v + kEntityOffsetPosY);
            const int16_t z    = read_n64_s16(rdram, v + kEntityOffsetPosZ);
            const int16_t yaw  = read_n64_s16(rdram, v + kEntityOffsetDir);
            const int16_t hp   = read_n64_s16(rdram, v + kEntityOffsetHP);
            const int16_t fuel = read_n64_s16(rdram, v + kEntityOffsetFuel);
            if (!first) body += ",\n";
            first = false;
            body += "    { \"slot\": " + std::to_string(i);
            body += ", \"spec\": "     + std::to_string(unsigned(spec));
            body += ", \"x\": "        + std::to_string(x);
            body += ", \"y\": "        + std::to_string(y);
            body += ", \"z\": "        + std::to_string(z);
            body += ", \"yaw\": "      + std::to_string(yaw);
            body += ", \"hp\": "       + std::to_string(hp);
            body += ", \"fuel\": "     + std::to_string(fuel);
            body += ", \"flags\": "    + std::to_string(unsigned(flags));
            body += " }";
            ++vehicles_seen;
        }
    }
    body += "\n  ],\n";

    // --- Aliens ------------------------------------------------------------
    int aliens_seen = 0;
    body += "  \"aliens\": [\n";
    {
        bool first = true;
        for (int i = 0; i < kAlienInstanceCount; ++i) {
            const uint32_t a = kAlienInstancesBase +
                               uint32_t(i) * kAlienInstanceSize;
            const uint8_t spec = read_n64_u8(rdram, a + 0x1A);
            if (spec == 0) continue;
            const int16_t x   = read_n64_s16(rdram, a + 0x00);
            const int16_t y   = read_n64_s16(rdram, a + 0x02);
            const int16_t z   = read_n64_s16(rdram, a + 0x04);
            const int16_t dir = read_n64_s16(rdram, a + 0x0E);
            if (!first) body += ",\n";
            first = false;
            body += "    { \"slot\": " + std::to_string(i);
            body += ", \"spec\": "     + std::to_string(unsigned(spec));
            body += ", \"x\": "        + std::to_string(x);
            body += ", \"y\": "        + std::to_string(y);
            body += ", \"z\": "        + std::to_string(z);
            body += ", \"yaw\": "      + std::to_string(dir);
            body += " }";
            ++aliens_seen;
        }
    }
    body += "\n  ],\n";

    // --- Save beacons (Unk80148620, 14 entries, 8 bytes each) -------------
    // Struct: s16 unk0, s16 unk2, s16 unk4, s16 unk6 (= level number).
    // IMPORTANT: after func_80116784_125734 runs (buildings.c:380), for
    // beacons whose unk6 matches currentLevel, unk0 is REPLACED with
    // the slot index of the nearest building (which serves as the
    // visual save point), and unk2 is zeroed. So `unk0` after init
    // is NOT a world coord — it's a building-slot index into the
    // BuildingInstance table we just dumped. The editor resolves
    // beacon position by looking up `buildings[unk0]` and using
    // that building's xCoord/zCoord. For beacons whose unk6 doesn't
    // match the current level, unk0 is set to 0x3E8 (1000) as a
    // sentinel.
    body += "  \"save_beacons\": [\n";
    {
        bool first = true;
        const uint32_t saved_id = read_n64_u32(rdram, kSavedBeaconId);
        for (int i = 0; i < 14; ++i) {
            const uint32_t b = kBeaconArrayBase + uint32_t(i) * kBeaconStride;
            const int16_t unk0 = read_n64_s16(rdram, b + 0x00); // building slot OR sentinel 0x3E8
            const int16_t unk2 = read_n64_s16(rdram, b + 0x02); // 0 after init
            const int16_t unk4 = read_n64_s16(rdram, b + 0x04); // unknown
            const int16_t unk6 = read_n64_s16(rdram, b + 0x06); // level (1-5)
            char raw_hex[24];
            std::snprintf(raw_hex, sizeof(raw_hex),
                          "%02x%02x%02x%02x%02x%02x%02x%02x",
                          read_n64_u8(rdram, b + 0),
                          read_n64_u8(rdram, b + 1),
                          read_n64_u8(rdram, b + 2),
                          read_n64_u8(rdram, b + 3),
                          read_n64_u8(rdram, b + 4),
                          read_n64_u8(rdram, b + 5),
                          read_n64_u8(rdram, b + 6),
                          read_n64_u8(rdram, b + 7));
            // Convenience: precompute the resolved world position by
            // dereferencing the building. If unk0 looks like a sentinel
            // (>= kBuildingInstanceCount or == 0x3E8) we leave coords
            // null so the editor knows there's nothing to draw.
            bool resolved = false;
            int16_t res_x = 0, res_z = 0;
            if (uint32_t(unk6) == lvl &&
                unk0 >= 0 && unk0 < kBuildingInstanceCount) {
                const uint32_t bb = kBuildingInstancesBase +
                                    uint32_t(unk0) * kBuildingInstanceSize;
                const uint8_t btype = read_n64_u8(rdram, bb + 0x06);
                if (btype != 0) {
                    res_x = read_n64_s16(rdram, bb + 0x00);
                    res_z = read_n64_s16(rdram, bb + 0x04);
                    resolved = true;
                }
            }
            if (!first) body += ",\n";
            first = false;
            body += "    { \"slot\": " + std::to_string(i);
            body += ", \"building_slot\": " + std::to_string(unk0);
            body += ", \"unk2\": "          + std::to_string(unk2);
            body += ", \"unk4\": "          + std::to_string(unk4);
            body += ", \"level\": "         + std::to_string(unk6);
            if (resolved) {
                body += ", \"resolved_x\": " + std::to_string(res_x);
                body += ", \"resolved_z\": " + std::to_string(res_z);
            } else {
                body += ", \"resolved_x\": null, \"resolved_z\": null";
            }
            body += ", \"raw_hex\": \"";
            body += raw_hex;
            body += "\", \"is_active_save\": ";
            body += (saved_id == uint32_t(i + 1) ? "true" : "false");
            body += " }";
        }
    }
    body += "\n  ],\n";

    // Slot occupancy summary — useful for future spawn cheats to know
    // how many free slots exist before adding entities.
    body += "  \"slot_summary\": {\n";
    body += "    \"buildings\": { \"used\": "
            + std::to_string(buildings_seen)
            + ", \"max\": " + std::to_string(kBuildingInstanceCount) + " },\n";
    body += "    \"vehicles\":  { \"used\": "
            + std::to_string(vehicles_seen)
            + ", \"max\": " + std::to_string(kVehicleInstanceCount) + " },\n";
    body += "    \"aliens\":    { \"used\": "
            + std::to_string(aliens_seen)
            + ", \"max\": " + std::to_string(kAlienInstanceCount) + " }\n";
    body += "  }\n";
    body += "}\n";

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        status_out = "Export failed: could not open file for writing.";
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, body.data(), DWORD(body.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != body.size()) {
        status_out = "Export failed: write error.";
        return false;
    }

    char st[200];
    std::snprintf(st, sizeof(st),
        "Exported %d buildings, %d vehicles, %d aliens, 14 beacons for "
        "level %u (%s). %u bytes written.",
        buildings_seen, vehicles_seen, aliens_seen,
        unsigned(lvl), level_name(lvl), unsigned(body.size()));
    status_out = st;
    std::fprintf(stderr, "[cheats] entity export -> %ls (%s)\n", path, st);
    return true;
}

// "Toggle Noclip Mode" — flips g_noclip_active. The actual movement
// happens in apply_noclip_movement, which is called from
// stub_poll_input every ~60Hz tick.
void custom_toggle_noclip(uint8_t* /*rdram*/) {
    const bool was_on = g_noclip_active.exchange(
        !g_noclip_active.load(std::memory_order_acquire),
        std::memory_order_acq_rel);
    std::fprintf(stderr,
        "[cheats] custom -> Noclip Mode %s "
        "(stick=horizontal, PageUp=up, PageDown=down)\n",
        was_on ? "OFF" : "ON");
}

// IDs for the dialog's child controls. Win32 needs each control to
// have a unique HMENU "ID" that comes back in WM_COMMAND.
constexpr UINT IDC_LIST       = 1001;  // Cheats tab listbox
constexpr UINT IDC_ACTIVATE   = 1002;  // Cheats tab Activate button
constexpr UINT IDC_CLOSE      = 1003;  // always-visible Close button
constexpr UINT IDC_DESC       = 1004;  // Cheats tab description label
constexpr UINT IDC_TABCTRL    = 1005;  // Tab strip (Cheats / Enhancements / Options)
constexpr UINT IDC_ENH_PLACE  = 1006;  // Enhancements tab placeholder STATIC (deprecated, replaced by enh controls)
constexpr UINT IDC_OPT_PLACE  = 1007;  // Options tab placeholder STATIC
// Enhancements tab — aspect ratio + refresh rate + upscale2D combos.
constexpr UINT IDC_ENH_ASPECT_LBL    = 1010;
constexpr UINT IDC_ENH_ASPECT_COMBO  = 1011;
constexpr UINT IDC_ENH_REFRESH_LBL   = 1012;
constexpr UINT IDC_ENH_REFRESH_COMBO = 1013;
constexpr UINT IDC_ENH_UPSCALE_LBL   = 1015;
constexpr UINT IDC_ENH_UPSCALE_COMBO = 1016;
constexpr UINT IDC_ENH_FOG_LBL       = 1017;  // "Fog Distance: 1.0x" updates with slider
constexpr UINT IDC_ENH_FOG_SLIDER    = 1018;  // Win32 trackbar (msctls_trackbar32)
constexpr UINT IDC_ENH_WATER_LBL     = 1019;  // "Water Level Override: ..."
constexpr UINT IDC_ENH_WATER_SLIDER  = 1023;  // trackbar; pos 0 = "off"
constexpr UINT IDC_ENH_WATER_OFF     = 1024;  // "Release override" button
constexpr UINT IDC_ENH_CULL_LBL      = 1025;  // "Entity Render Distance: 1.0x (cull bbox)"
constexpr UINT IDC_ENH_CULL_SLIDER   = 1026;  // exponential mapping pos 0..100 -> 1x..16x
constexpr UINT IDC_ENH_FAR_LBL       = 1027;  // "Far Plane: 1.0x (vanilla)"
constexpr UINT IDC_ENH_FAR_SLIDER    = 1028;  // exponential pos 0..100 -> 1x..16x
constexpr UINT IDC_ENH_FAR_OFF       = 1029;  // "Release override" button
constexpr UINT IDC_ENH_INFO          = 1014;  // helper text below the combos
// Options tab — multiplayer toggle.
constexpr UINT IDC_OPT_MP_ENABLE     = 1020;  // checkbox: enable multiplayer POC
constexpr UINT IDC_OPT_MP_INFO       = 1021;  // helper text below the checkbox
// Options tab — terrain backup buttons.
constexpr UINT IDC_OPT_TERRAIN_LBL    = 1030; // STATIC: "Terrain backup"
constexpr UINT IDC_OPT_TERRAIN_EXPORT = 1031; // button: export current level
constexpr UINT IDC_OPT_TERRAIN_IMPORT = 1032; // button: import from file
constexpr UINT IDC_OPT_TERRAIN_STATUS = 1033; // STATIC: last-op result line
constexpr UINT IDC_OPT_ENTITY_EXPORT  = 1034; // button: export entities (read-only)

// Tab indices match TCM_INSERTITEM insertion order.
constexpr int  TAB_CHEATS       = 0;
constexpr int  TAB_ENHANCEMENTS = 1;
constexpr int  TAB_OPTIONS      = 2;

// Custom messages for the dialog thread.
constexpr UINT WM_BH_SHOW   = WM_USER + 1;
constexpr UINT WM_BH_HIDE   = WM_USER + 2;

// Shared state between F1-detector thread and dialog thread.
std::atomic<HWND>     g_overlay_hwnd{nullptr};
std::atomic<DWORD>    g_overlay_thread_id{0};
std::atomic<bool>     g_overlay_visible{false};
std::atomic<uint8_t*> g_overlay_rdram{nullptr};

// Write a cheat's pattern (reversed) into cheatInputBuffer. The
// matcher walks the buffer such that buffer[0] must equal the
// pattern's LAST char, buffer[1] the second-last, etc. So writing
// the reversed pattern into [0..N-1] presents a match next frame.
void write_cheat_to_buffer(uint8_t* rdram, const char* pattern) {
    if (rdram == nullptr || pattern == nullptr) return;

    const size_t n = std::strlen(pattern);
    if (n == 0 || n > kCheatBufferLen) return;

    // buffer[i] = pattern[n - 1 - i] for i in [0..n-1]
    for (size_t i = 0; i < n; ++i) {
        write_n64_u8(rdram, kCheatInputBuffer + uint32_t(i),
                     uint8_t(pattern[n - 1 - i]));
    }
    // Zero the rest so a previous longer pattern can't still match.
    for (size_t i = n; i < kCheatBufferLen; ++i) {
        write_n64_u8(rdram, kCheatInputBuffer + uint32_t(i), 0);
    }
}

// "Tools" — entries that open their own sub-dialogs rather than
// firing a one-shot effect. Each Tool has an opener function that
// gets called when Activate is clicked on the tool's row.
struct ToolEntry {
    const char* name;
    const char* description;
    void (*open)();
};

void open_vehicle_morpher();     // defined below
void open_alien_morpher();       // defined below
void open_bookmark_manager();    // defined below
void open_weapon_editor();       // defined below

// Weapon-slot ID → display name. IDs verified in-game by the user.
//
// The "slot" terminology in `weaponSlots[8]` is a misnomer; the byte
// actually holds an INVENTORY item ID, not strictly a weapon — slot
// 0x01 is Fuel, slot 0x08 is the Sun Shield, etc. We label them
// "weapons" because that's the WeaponSpecEntry table they cross-
// reference.
//
// === Player weapons (0x00..0x0A) ===
//   On-foot inventory. Pickup-able. Ammo lives in the global table
//   at 0x80048146..0x80048166 indexed by weapon ID.
//
// === Vehicle weapons (0x0B..0x11) ===
//   Mounted weapons. Ammo is per-vehicle (not in the global table),
//   so writing one of these into a player slot won't give a usable
//   weapon — the on-foot player has no vehicle spec to resolve it
//   through. Listed in the dropdown so they appear in editor reads
//   when you bookmark/inspect from inside a vehicle.
//
//   0x0B "Vehicle Weapon 1" — GLOBAL PRIMARY. This is a redirect ID
//        meaning "fire whatever this vehicle's spec says is its main
//        weapon". A tank's main cannon, a jeep's chaingun, the UFO
//        firing a Resonator — all stored as 0x0B in the slot, with
//        the actual behavior determined by the vehicle's spec. Has
//        effectively infinite ammo (the vehicle spec controls it).
//   0x0C "Vehicle Weapon 2" — likely unused / cut secondary slot.
//        User has never seen it in-game; probably intended for a
//        secondary weapon system that never shipped.
//   0x0D..0x11 — ALPHA-1-NATIVE WEAPONS. The five weapons that
//        Alpha 1 (the player's main flying vehicle) carries by
//        default. These DO consume ammo from the global table (the
//        'alfa' built-in cheat refills them all). Other vehicles
//        that "have" these weapons (e.g. UFO's Resonator) reference
//        them indirectly via the 0x0B redirect rather than holding
//        the specific ID — only Alpha 1 stores the actual IDs in
//        its slot.
//
// === Earlier version error ===
//   First pass had phantom entries at 0x0D ("Arme 2") and 0x0E
//   ("Waffe 2") that pushed everything else 2 IDs higher than
//   reality. The source those came from looked like leaked
//   French/German build localization (arme=weapon FR, waffe=weapon
//   DE), but in-game testing proved both IDs are just Chaingun and
//   Fragcannon — the source list had errors that probably trace
//   back to an early-2000s cheat-database cross-contamination.
//   Total weapon count is 18 (0x00..0x11), not 20.
struct WeaponName { uint8_t id; const char* name; bool is_player; };
constexpr WeaponName kWeaponNames[] = {
    // --- Player inventory (pickup-able, global ammo table) ---
    { 0x00, "(empty / none)",        true  },
    { 0x01, "Fuel",                  true  },
    { 0x02, "Pistol",                true  },
    { 0x03, "Shotgun",               true  },
    { 0x04, "Rifle",                 true  },
    { 0x05, "Machine Gun",           true  },
    { 0x06, "Rocket Launcher",       true  },
    { 0x07, "TNT",                   true  },
    { 0x08, "Sun Shield",            true  },
    { 0x09, "Grenades",              true  },
    { 0x0A, "Tri-Spinner",           true  },
    // --- Vehicle slots (ammo via vehicle spec, not the global table) ---
    { 0x0B, "Vehicle Primary (any)", false }, // redirect to vehicle spec, infinite
    { 0x0C, "Vehicle Secondary (unused?)", false }, // never observed in-game
    { 0x0D, "Chaingun (Alpha 1)",    false }, // global ammo slot 13
    { 0x0E, "Fragcannon (Alpha 1)",  false }, // global ammo slot 14
    { 0x0F, "Laser Missiles (Alpha 1)", false }, // global ammo slot 15
    { 0x10, "Resonator (Alpha 1)",   false }, // global ammo slot 16
    { 0x11, "Plasma Bombs (Alpha 1)", false }, // global ammo slot 17 (boundary)
    // === KNOWN Serious Weapons substitutions ===
    // The 'snuffle' (Serious Weapons) cheat doesn't modify slot
    // contents — it swaps the FIRING behavior of equipped weapons:
    //   Pistol       → "wee laser" (laser projectile, low damage)
    //   Shotgun      → "big laser" (laser projectile, big damage)
    //   Machine Gun  → "bad cheat" toast, fires Laser Missiles
    //                  (probably a placeholder name on a dev
    //                  substitution that was never finalized — the
    //                  toast string field wasn't filled in)
    //   Grenades     → Cluster Bomb (same fire behavior as the
    //                  Howitzer vehicle's weapon)
    // These substitutions live in the cheat's firing-side logic,
    // not in the weapon-ID range, which is why they don't appear in
    // this table — selecting one of them via this editor would give
    // you the base weapon (e.g. Machine Gun), and the substitution
    // only kicks in when Serious Weapons is active AND the cheat
    // re-routes the fire call.
};
constexpr int kWeaponNameCount = sizeof(kWeaponNames) / sizeof(kWeaponNames[0]);

constexpr ToolEntry kTools[] = {
    { "Vehicle Morpher / Spawner…",
      "Sub-window with level-aware vehicle picker. 'Morph' rewrites your "
      "current entity (D_80052B34) to the chosen type — pair with EXIT + "
      "RE-ENTER for proper weapons. 'Spawn × N' allocates free vehicleInstances "
      "slots and places vehicles in a ring around the player. PARTIAL spawner: "
      "vehicles render, can be entered, fire once, but don't drive/reload/take "
      "damage (per-frame physics callback D_8015920C not getting hooked up to "
      "our slot — see handoff.md §G.6). For full-functionality 'spawning', "
      "use Morph on an existing vehicle instead.",
      &open_vehicle_morpher },
    { "Alien Morpher / Spawner…",
      "Sub-window with global alien type list and two actions: 'Morph All' "
      "rewrites every alive slot's specIndex to the chosen type (instant "
      "chaos). 'Spawn × N' allocates N free slots via BH's slot list, "
      "copies the template + sets type/HP/flags into each, and places them "
      "in a 192-unit ring around the player (or exactly at player for N=1). "
      "Spawning a type not loaded in the current level may show missing "
      "models. Boss type (0x1B) crashes shortly after spawn — see handoff.md.",
      &open_alien_morpher },
    { "Bookmarks Manager…",
      "10-slot multi-bookmark system: save current player position + "
      "HP/fuel/ammo/weapon-slots/items per slot, teleport back later, "
      "persisted to disk as bh_bookmarks.json. Use 'Save Current to "
      "Slot' to capture, 'Teleport to Slot' to warp. Slot 0 is also "
      "driven by the 'Bookmark position' / 'Teleport to bookmark' "
      "quick-cheats. Safe to fire while running, in vehicles, mid-"
      "combat — pure atomic field writes, no torn-snapshot risk.",
      &open_bookmark_manager },
    { "Weapon Editor…",
      "Edit the player's 8 weapon slots directly: each slot can hold "
      "one of 14 weapon types (0=empty, 1=Pistol, 2=Shotgun, etc.) "
      "plus ammo counts. Write any combination instantly without "
      "playing through pickups. Vehicle-mounted weapons aren't editable "
      "here yet (those live in per-vehicle spec data, not the player "
      "slot table) — flagged as future work in the dialog.",
      &open_weapon_editor },
};

// Listbox layout:
//   rows [0 .. N_BH-1]                       BH cheats from kCheats[]
//   row  [N_BH]                              "── Custom ──" separator
//   rows [N_BH+1 .. N_BH+N_CUSTOM]           custom cheats
//   row  [N_BH+1+N_CUSTOM]                   "── Tools ──" separator
//   rows [N_BH+2+N_CUSTOM .. ...]            tools
//
// Helpers below convert between listbox indices and the underlying
// array indices, hiding the separators from the dispatch logic.

constexpr int kBhCheatCount     = int(std::size(kCheats));
constexpr int kCustomCheatCount = int(std::size(kCustomCheats));
constexpr int kToolCount        = int(std::size(kTools));

constexpr int kCustomSepRow     = kBhCheatCount;
constexpr int kFirstCustomRow   = kBhCheatCount + 1;
constexpr int kToolsSepRow      = kFirstCustomRow + kCustomCheatCount;
constexpr int kFirstToolRow     = kToolsSepRow + 1;
constexpr int kListRowCount     = kFirstToolRow + kToolCount;

enum class RowKind { BhCheat, CustomCheat, Tool, Separator };
struct RowMapping { RowKind kind; int index; };

RowMapping map_list_row(int list_row) {
    if (list_row < kBhCheatCount) {
        return { RowKind::BhCheat, list_row };
    }
    if (list_row == kCustomSepRow || list_row == kToolsSepRow) {
        return { RowKind::Separator, 0 };
    }
    if (list_row >= kFirstCustomRow && list_row < kToolsSepRow) {
        return { RowKind::CustomCheat, list_row - kFirstCustomRow };
    }
    if (list_row >= kFirstToolRow && list_row < kListRowCount) {
        return { RowKind::Tool, list_row - kFirstToolRow };
    }
    return { RowKind::Separator, 0 };
}

// Format the description text shown below the list.
std::string format_description(int list_row) {
    const RowMapping m = map_list_row(list_row);
    switch (m.kind) {
    case RowKind::BhCheat: {
        const CheatInfo& c = kCheats[m.index];
        std::string s;
        s += "Pattern: ";
        s += c.name;
        s += "\r\nEffect:  ";
        s += c.description;
        return s;
    }
    case RowKind::CustomCheat: {
        const CustomCheat& c = kCustomCheats[m.index];
        std::string s;
        s += "Custom: ";
        s += c.name;
        s += "\r\nEffect:  ";
        s += c.description;
        return s;
    }
    case RowKind::Tool: {
        const ToolEntry& t = kTools[m.index];
        std::string s;
        s += "Tool:    ";
        s += t.name;
        s += "\r\nDetails: ";
        s += t.description;
        return s;
    }
    case RowKind::Separator:
    default:
        return "(separator)";
    }
}

// Reflow child controls to fit the current client area. Called from
// WM_CREATE and WM_SIZE so dragging the window border resizes the
// tab strip / listbox / description / buttons proportionally.
void layout_overlay(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;

    constexpr int M = 8;                  // margin
    constexpr int TAB_H = 24;             // tab strip height
    constexpr int BTN_W = 80;
    constexpr int BTN_H = 28;
    constexpr int DESC_H = 56;
    constexpr int GAP = 8;

    // Tab strip across the top.
    HWND tab = GetDlgItem(hwnd, IDC_TABCTRL);
    if (tab) MoveWindow(tab, M, M, W - 2*M, TAB_H, TRUE);

    // Bottom button row.
    const int btn_y = H - M - BTN_H;
    HWND close_btn = GetDlgItem(hwnd, IDC_CLOSE);
    if (close_btn) MoveWindow(close_btn, W - M - BTN_W, btn_y, BTN_W, BTN_H, TRUE);
    HWND act_btn = GetDlgItem(hwnd, IDC_ACTIVATE);
    if (act_btn) MoveWindow(act_btn, W - 2*M - 2*BTN_W, btn_y, BTN_W, BTN_H, TRUE);

    // Description label, just above the buttons.
    const int desc_y = btn_y - GAP - DESC_H;
    HWND desc = GetDlgItem(hwnd, IDC_DESC);
    if (desc) MoveWindow(desc, M, desc_y, W - 2*M, DESC_H, TRUE);

    // Listbox fills the gap between tab and description.
    const int list_y = M + TAB_H + GAP;
    const int list_h = (desc_y - GAP) - list_y;
    HWND list = GetDlgItem(hwnd, IDC_LIST);
    if (list && list_h > 0) MoveWindow(list, M, list_y, W - 2*M, list_h, TRUE);

    // Enhancements + Options placeholders fill the same area as the
    // listbox + description (between tab and button row).
    const int page_y = list_y;
    const int page_h = btn_y - GAP - page_y;
    // NOTE: IDC_OPT_PLACE is a legacy 0-size STATIC kept only so
    // show_tab_controls's GetDlgItem lookup doesn't have to special-case
    // it. We DO NOT grow or show it: it was created after the real
    // Options-tab controls (mp_chk, mp_info, terrain row), so in
    // z-order it sits on top of them. If we make it full-page and
    // visible (as an earlier version of layout_overlay did), it paints
    // its empty grey rectangle over everything else on the tab, hiding
    // all real controls until the user clicks on each one and forces
    // that control to paint itself. Keep it dormant.

    // Options tab — multiplayer checkbox at top, then info block, then
    // terrain backup row at the bottom.
    const int opt_w = W - 2*M - 16;
    // Terrain row: a 3-line wrapped label, two buttons side by side,
    // a status line. Label needs ~60px because the description wraps
    // to 3 lines at typical dialog widths — earlier 20px height was
    // hiding lines 2-3 behind the buttons.
    constexpr int kTerrainLblH    = 60;
    constexpr int kTerrainBtnH    = 28;
    constexpr int kTerrainStatusH = 18;
    constexpr int kTerrainRowGap  = 4;
    // Two button rows now: terrain export/import on top, entity export
    // (full-width) on the bottom. Status line below both.
    const int terrain_row_h = kTerrainLblH + kTerrainRowGap +
                              kTerrainBtnH + kTerrainRowGap +
                              kTerrainBtnH + kTerrainRowGap +
                              kTerrainStatusH + kTerrainRowGap;
    HWND mp_chk = GetDlgItem(hwnd, IDC_OPT_MP_ENABLE);
    if (mp_chk) MoveWindow(mp_chk, M + 8, page_y, opt_w, 24, TRUE);
    const int info_top = page_y + 32;
    const int info_h_avail = page_h - 32 - terrain_row_h - 8;
    HWND mp_info = GetDlgItem(hwnd, IDC_OPT_MP_INFO);
    if (mp_info && info_h_avail > 0) {
        MoveWindow(mp_info, M + 8, info_top, opt_w, info_h_avail, TRUE);
    }
    // Terrain row bottom of Options tab. Stack: label -> 2 button rows -> status.
    const int trow_y    = page_y + page_h - terrain_row_h;
    const int tbtn1_y   = trow_y + kTerrainLblH + kTerrainRowGap;
    const int tbtn2_y   = tbtn1_y + kTerrainBtnH + kTerrainRowGap;
    const int tstatus_y = tbtn2_y + kTerrainBtnH + kTerrainRowGap;
    HWND tlbl = GetDlgItem(hwnd, IDC_OPT_TERRAIN_LBL);
    if (tlbl) MoveWindow(tlbl, M + 8, trow_y, opt_w, kTerrainLblH, TRUE);
    HWND tbtn_e = GetDlgItem(hwnd, IDC_OPT_TERRAIN_EXPORT);
    HWND tbtn_i = GetDlgItem(hwnd, IDC_OPT_TERRAIN_IMPORT);
    const int half_w = (opt_w - 8) / 2;
    if (tbtn_e) MoveWindow(tbtn_e, M + 8,                     tbtn1_y, half_w, kTerrainBtnH, TRUE);
    if (tbtn_i) MoveWindow(tbtn_i, M + 8 + half_w + 8,        tbtn1_y, half_w, kTerrainBtnH, TRUE);
    // Entity export — full-width on the second button row.
    HWND tbtn_ent = GetDlgItem(hwnd, IDC_OPT_ENTITY_EXPORT);
    if (tbtn_ent) MoveWindow(tbtn_ent, M + 8, tbtn2_y, opt_w, kTerrainBtnH, TRUE);
    HWND tstat = GetDlgItem(hwnd, IDC_OPT_TERRAIN_STATUS);
    if (tstat) MoveWindow(tstat, M + 8, tstatus_y, opt_w, kTerrainStatusH, TRUE);

    // Enhancements tab — two rows of (label + combobox) at top of
    // the page area, then a wrapping info STATIC below taking the rest.
    constexpr int LBL_W = 120;
    constexpr int CMB_H = 24;
    constexpr int ROW_H = 36;
    const int enh_x_lbl = M + 8;
    const int enh_x_cmb = enh_x_lbl + LBL_W + 4;
    const int enh_cmb_w = (W - 2*M - 16) - (LBL_W + 4);
    HWND a_lbl = GetDlgItem(hwnd, IDC_ENH_ASPECT_LBL);
    if (a_lbl) MoveWindow(a_lbl, enh_x_lbl, page_y + 4, LBL_W, 18, TRUE);
    HWND a_cmb = GetDlgItem(hwnd, IDC_ENH_ASPECT_COMBO);
    if (a_cmb) MoveWindow(a_cmb, enh_x_cmb, page_y, enh_cmb_w, 220, TRUE);
    HWND r_lbl = GetDlgItem(hwnd, IDC_ENH_REFRESH_LBL);
    if (r_lbl) MoveWindow(r_lbl, enh_x_lbl, page_y + 4 + ROW_H, LBL_W, 18, TRUE);
    HWND r_cmb = GetDlgItem(hwnd, IDC_ENH_REFRESH_COMBO);
    if (r_cmb) MoveWindow(r_cmb, enh_x_cmb, page_y + ROW_H, enh_cmb_w, 220, TRUE);
    HWND u_lbl = GetDlgItem(hwnd, IDC_ENH_UPSCALE_LBL);
    if (u_lbl) MoveWindow(u_lbl, enh_x_lbl, page_y + 4 + 2 * ROW_H, LBL_W, 18, TRUE);
    HWND u_cmb = GetDlgItem(hwnd, IDC_ENH_UPSCALE_COMBO);
    if (u_cmb) MoveWindow(u_cmb, enh_x_cmb, page_y + 2 * ROW_H, enh_cmb_w, 220, TRUE);

    // Fog row spans both columns (label on top, slider below).
    const int fog_y = page_y + 3 * ROW_H;
    HWND f_lbl = GetDlgItem(hwnd, IDC_ENH_FOG_LBL);
    if (f_lbl) MoveWindow(f_lbl, enh_x_lbl, fog_y + 4, 200, 18, TRUE);
    HWND f_sld = GetDlgItem(hwnd, IDC_ENH_FOG_SLIDER);
    const int sld_y = fog_y + 24;
    if (f_sld) MoveWindow(f_sld, enh_x_lbl, sld_y, W - 2*M - 16, 28, TRUE);

    // Water row: label on top, slider + "off" button on the same row.
    const int water_y = sld_y + 32;
    HWND w_lbl = GetDlgItem(hwnd, IDC_ENH_WATER_LBL);
    if (w_lbl) MoveWindow(w_lbl, enh_x_lbl, water_y + 4, W - 2*M - 16, 18, TRUE);
    HWND w_off = GetDlgItem(hwnd, IDC_ENH_WATER_OFF);
    constexpr int OFF_BTN_W = 110;
    if (w_off) MoveWindow(w_off, W - M - 8 - OFF_BTN_W, water_y + 24,
                          OFF_BTN_W, 26, TRUE);
    HWND w_sld = GetDlgItem(hwnd, IDC_ENH_WATER_SLIDER);
    const int w_sld_y = water_y + 24;
    if (w_sld) MoveWindow(w_sld, enh_x_lbl, w_sld_y,
                          (W - 2*M - 16) - OFF_BTN_W - 8, 28, TRUE);

    // Entity render-distance row: label + slider stacked, full width.
    const int cull_y = w_sld_y + 32;
    HWND c_lbl = GetDlgItem(hwnd, IDC_ENH_CULL_LBL);
    if (c_lbl) MoveWindow(c_lbl, enh_x_lbl, cull_y + 4, W - 2*M - 16, 18, TRUE);
    HWND c_sld = GetDlgItem(hwnd, IDC_ENH_CULL_SLIDER);
    const int c_sld_y = cull_y + 24;
    if (c_sld) MoveWindow(c_sld, enh_x_lbl, c_sld_y, W - 2*M - 16, 28, TRUE);

    // Far-plane row: label + slider + release button (same pattern as water).
    const int far_y = c_sld_y + 32;
    HWND f2_lbl = GetDlgItem(hwnd, IDC_ENH_FAR_LBL);
    if (f2_lbl) MoveWindow(f2_lbl, enh_x_lbl, far_y + 4, W - 2*M - 16, 18, TRUE);
    HWND f2_off = GetDlgItem(hwnd, IDC_ENH_FAR_OFF);
    constexpr int FAR_OFF_BTN_W = 110;
    if (f2_off) MoveWindow(f2_off, W - M - 8 - FAR_OFF_BTN_W, far_y + 24,
                           FAR_OFF_BTN_W, 26, TRUE);
    HWND f2_sld = GetDlgItem(hwnd, IDC_ENH_FAR_SLIDER);
    const int f2_sld_y = far_y + 24;
    if (f2_sld) MoveWindow(f2_sld, enh_x_lbl, f2_sld_y,
                           (W - 2*M - 16) - FAR_OFF_BTN_W - 8, 28, TRUE);

    HWND info  = GetDlgItem(hwnd, IDC_ENH_INFO);
    const int info_y = f2_sld_y + 36;
    const int info_h = (page_y + page_h) - info_y;
    if (info && info_h > 0) MoveWindow(info, enh_x_lbl, info_y, W - 2*M - 16, info_h, TRUE);
}

// Show only the controls that belong to a given tab; hide the rest.
// The Close button is shared and stays visible regardless.
void show_tab_controls(HWND hwnd, int active_tab) {
    auto set_vis = [&](UINT id, bool visible) {
        HWND ctrl = GetDlgItem(hwnd, id);
        if (ctrl != nullptr) ShowWindow(ctrl, visible ? SW_SHOW : SW_HIDE);
    };
    const bool cheats = (active_tab == TAB_CHEATS);
    const bool enh    = (active_tab == TAB_ENHANCEMENTS);
    const bool opt    = (active_tab == TAB_OPTIONS);

    // Cheats-tab controls
    set_vis(IDC_LIST,      cheats);
    set_vis(IDC_DESC,      cheats);
    set_vis(IDC_ACTIVATE,  cheats);

    // Enhancements-tab controls
    set_vis(IDC_ENH_ASPECT_LBL,    enh);
    set_vis(IDC_ENH_ASPECT_COMBO,  enh);
    set_vis(IDC_ENH_REFRESH_LBL,   enh);
    set_vis(IDC_ENH_REFRESH_COMBO, enh);
    set_vis(IDC_ENH_UPSCALE_LBL,   enh);
    set_vis(IDC_ENH_UPSCALE_COMBO, enh);
    set_vis(IDC_ENH_FOG_LBL,       enh);
    set_vis(IDC_ENH_FOG_SLIDER,    enh);
    set_vis(IDC_ENH_WATER_LBL,     enh);
    set_vis(IDC_ENH_WATER_SLIDER,  enh);
    set_vis(IDC_ENH_WATER_OFF,     enh);
    set_vis(IDC_ENH_CULL_LBL,      enh);
    set_vis(IDC_ENH_CULL_SLIDER,   enh);
    set_vis(IDC_ENH_FAR_LBL,       enh);
    set_vis(IDC_ENH_FAR_SLIDER,    enh);
    set_vis(IDC_ENH_FAR_OFF,       enh);
    set_vis(IDC_ENH_INFO,          enh);

    // Options-tab controls (mp checkbox/info + terrain row).
    // IDC_OPT_PLACE intentionally NOT shown — see comment in
    // layout_overlay explaining the z-order trap.
    set_vis(IDC_OPT_MP_ENABLE, opt);
    set_vis(IDC_OPT_MP_INFO,   opt);
    set_vis(IDC_OPT_TERRAIN_LBL,    opt);
    set_vis(IDC_OPT_TERRAIN_EXPORT, opt);
    set_vis(IDC_OPT_TERRAIN_IMPORT, opt);
    set_vis(IDC_OPT_ENTITY_EXPORT,  opt);
    set_vis(IDC_OPT_TERRAIN_STATUS, opt);

    // Win32 quirk: ShowWindow(SW_SHOW) on a child that was previously
    // SW_HIDE doesn't always trigger a paint message — the OS marks it
    // "valid" because the parent area hasn't been invalidated since the
    // last paint cycle. The symptom is controls (especially BUTTONs and
    // STATICs) that stay invisible until the user clicks/hovers them
    // and Windows finally paints them in response to the input event.
    //
    // The robust fix is to explicitly tell the OS to invalidate the
    // parent and immediately repaint all children. RDW_ERASE forces
    // the parent's background to repaint underneath, which prevents
    // stale pixels from showing through if a control got smaller.
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW |
                 RDW_ALLCHILDREN);
}

LRESULT CALLBACK overlay_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Tab strip at the top — Cheats / Enhancements / Options.
        HWND tab = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            8, 8, 392, 24, hwnd, (HMENU)(uintptr_t)IDC_TABCTRL,
            GetModuleHandleW(nullptr), nullptr);

        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(L"Cheats");
        SendMessageW(tab, TCM_INSERTITEMW, TAB_CHEATS, (LPARAM)&item);
        item.pszText = const_cast<wchar_t*>(L"Enhancements");
        SendMessageW(tab, TCM_INSERTITEMW, TAB_ENHANCEMENTS, (LPARAM)&item);
        item.pszText = const_cast<wchar_t*>(L"Options");
        SendMessageW(tab, TCM_INSERTITEMW, TAB_OPTIONS, (LPARAM)&item);

        // Cheats tab content — listbox + description + Activate.
        // Y coords shifted down to leave room for the tab strip.
        CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
            8, 40, 384, 248, hwnd, (HMENU)(uintptr_t)IDC_LIST,
            GetModuleHandleW(nullptr), nullptr);

        // SS_NOPREFIX so '&' renders literally (otherwise Win32 treats
        // it as an accelerator-prefix marker and shows the next char
        // underlined with the '&' eaten — e.g. "HP & Fuel" -> "HP _Fuel").
        CreateWindowExW(0, L"STATIC", L"(select a cheat above)",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            8, 296, 384, 56, hwnd, (HMENU)(uintptr_t)IDC_DESC,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"BUTTON", L"Activate",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            220, 360, 80, 28, hwnd, (HMENU)(uintptr_t)IDC_ACTIVATE,
            GetModuleHandleW(nullptr), nullptr);

        // Enhancements tab content — aspect-ratio + refresh-rate combos
        // plus a small info label. Hidden initially; show_tab_controls
        // toggles them when the Enhancements tab is active.
        // Real positions get computed by layout_overlay on WM_SIZE.
        CreateWindowExW(0, L"STATIC", L"Aspect Ratio:",
            WS_CHILD | SS_LEFT | SS_NOPREFIX,
            16, 48, 120, 18, hwnd, (HMENU)(uintptr_t)IDC_ENH_ASPECT_LBL,
            GetModuleHandleW(nullptr), nullptr);
        HWND aspect_combo = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
            140, 44, 220, 220, hwnd, (HMENU)(uintptr_t)IDC_ENH_ASPECT_COMBO,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(aspect_combo, CB_ADDSTRING, 0, (LPARAM)L"4:3 (Original / native)");
        SendMessageW(aspect_combo, CB_ADDSTRING, 0, (LPARAM)L"16:9");
        SendMessageW(aspect_combo, CB_ADDSTRING, 0, (LPARAM)L"16:10");
        SendMessageW(aspect_combo, CB_ADDSTRING, 0, (LPARAM)L"21:9 (Ultrawide)");
        SendMessageW(aspect_combo, CB_ADDSTRING, 0, (LPARAM)L"32:9 (Super-Ultrawide)");
        SendMessageW(aspect_combo, CB_ADDSTRING, 0, (LPARAM)L"Stretch to window");
        SendMessageW(aspect_combo, CB_SETCURSEL, 0, 0);  // default 4:3

        CreateWindowExW(0, L"STATIC", L"Refresh Rate:",
            WS_CHILD | SS_LEFT | SS_NOPREFIX,
            16, 84, 120, 18, hwnd, (HMENU)(uintptr_t)IDC_ENH_REFRESH_LBL,
            GetModuleHandleW(nullptr), nullptr);
        HWND refresh_combo = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
            140, 80, 220, 220, hwnd, (HMENU)(uintptr_t)IDC_ENH_REFRESH_COMBO,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(refresh_combo, CB_ADDSTRING, 0, (LPARAM)L"Native (~30 Hz, game's own rate)");
        SendMessageW(refresh_combo, CB_ADDSTRING, 0, (LPARAM)L"60 Hz");
        SendMessageW(refresh_combo, CB_ADDSTRING, 0, (LPARAM)L"75 Hz");
        SendMessageW(refresh_combo, CB_ADDSTRING, 0, (LPARAM)L"120 Hz");
        SendMessageW(refresh_combo, CB_ADDSTRING, 0, (LPARAM)L"144 Hz");
        SendMessageW(refresh_combo, CB_ADDSTRING, 0, (LPARAM)L"Match display refresh");
        SendMessageW(refresh_combo, CB_SETCURSEL, 0, 0);  // default native

        CreateWindowExW(0, L"STATIC", L"HUD / 2D Upscale:",
            WS_CHILD | SS_LEFT | SS_NOPREFIX,
            16, 120, 120, 18, hwnd, (HMENU)(uintptr_t)IDC_ENH_UPSCALE_LBL,
            GetModuleHandleW(nullptr), nullptr);
        HWND upscale_combo = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
            140, 116, 220, 220, hwnd, (HMENU)(uintptr_t)IDC_ENH_UPSCALE_COMBO,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(upscale_combo, CB_ADDSTRING, 0, (LPARAM)L"Original (chunky N64 2D pixels)");
        SendMessageW(upscale_combo, CB_ADDSTRING, 0, (LPARAM)L"Scaled-only (smoother HUD icons)");
        SendMessageW(upscale_combo, CB_ADDSTRING, 0, (LPARAM)L"All 2D upscaled (max smoothness)");
        SendMessageW(upscale_combo, CB_SETCURSEL, 0, 0);  // default original

        // Fog distance slider. Position 1..100 maps to scale 0.1x..10x.
        // Default position 10 = 1.0x (native).
        CreateWindowExW(0, L"STATIC", L"Fog Distance: 1.0x",
            WS_CHILD | SS_LEFT | SS_NOPREFIX,
            16, 156, 200, 18, hwnd, (HMENU)(uintptr_t)IDC_ENH_FOG_LBL,
            GetModuleHandleW(nullptr), nullptr);
        HWND fog_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | TBS_HORZ | TBS_AUTOTICKS | TBS_TOOLTIPS,
            16, 176, 376, 28, hwnd, (HMENU)(uintptr_t)IDC_ENH_FOG_SLIDER,
            GetModuleHandleW(nullptr), nullptr);
        // Exponential mapping centered on 1.0x. Position N -> scale =
        // pow(2, (N-25)/25.0). Defaults: pos 25 = 1.0x, pos 1 = 0.51x,
        // pos 50 = 2.0x, pos 75 = 4.0x, pos 100 = 8.0x. The useful
        // visual range for BH is 0.5x..2x — most of the slider's
        // resolution lives there with octave-doubling beyond.
        SendMessageW(fog_slider, TBM_SETRANGE, TRUE, MAKELPARAM(1, 100));
        SendMessageW(fog_slider, TBM_SETPAGESIZE, 0, 5);
        SendMessageW(fog_slider, TBM_SETTICFREQ, 25, 0);  // tick at 0.5x, 1x, 2x, 4x, 8x
        SendMessageW(fog_slider, TBM_SETPOS, TRUE, 25);   // default 1.0x

        // Water level slider. Position 0..200 maps linearly to water Y
        // -2000..+2000 (so pos 100 = water Y 0, the typical center).
        // Slider tracks an OVERRIDE — when the user moves it, we set
        // g_water_target_y + flip g_water_override_active on, which
        // makes apply_water_override_tick re-write D_80222A70 every
        // frame. "Release override" button clears the flag so the
        // game's natural water Y resumes.
        HFONT water_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND water_lbl = CreateWindowExW(0, L"STATIC",
            L"Water Level Override: (off)",
            WS_CHILD | SS_LEFT | SS_NOPREFIX,
            16, 212, 376, 18, hwnd, (HMENU)(uintptr_t)IDC_ENH_WATER_LBL,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(water_lbl, WM_SETFONT, (WPARAM)water_font, MAKELPARAM(TRUE, 0));
        HWND water_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | TBS_HORZ | TBS_AUTOTICKS | TBS_TOOLTIPS,
            16, 232, 296, 28, hwnd, (HMENU)(uintptr_t)IDC_ENH_WATER_SLIDER,
            GetModuleHandleW(nullptr), nullptr);
        // Pos 0..200 -> water Y 0..+2000 (10 per step). Negative Y
        // values make the in-game map screen render water over the
        // whole map (visual artifact), so we clamp to non-negative.
        // Default pos 0 = Y 0 (sea level — typical natural value).
        SendMessageW(water_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        SendMessageW(water_slider, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(water_slider, TBM_SETTICFREQ, 50, 0); // tick at Y 0,500,1000,1500,2000
        SendMessageW(water_slider, TBM_SETPOS, TRUE, 0);   // default Y=0
        HWND water_off = CreateWindowExW(0, L"BUTTON",
            L"Release override",
            WS_CHILD | BS_PUSHBUTTON,
            320, 232, 76, 28, hwnd, (HMENU)(uintptr_t)IDC_ENH_WATER_OFF,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(water_off, WM_SETFONT, (WPARAM)water_font, MAKELPARAM(TRUE, 0));

        // Entity render-distance slider (the cull bbox multiplier).
        // Pos 0..100 maps exponentially to 1.0x..16x (each +25 doubles).
        // Drives g_entity_cull_scale, which our weak-symbol override of
        // BH's func_800703B0_7F360 (entity bbox cull) reads. At 1.0x
        // gameplay is BH-identical; at 16x the bbox covers a 240×240
        // tile region (effectively the whole map). Note: terrain
        // streaming still runs at the vanilla 19×19 window, so
        // entities beyond ~9 tiles render "over the void" — useful for
        // testing but the visible improvement is capped until terrain
        // extension lands.
        HWND cull_lbl = CreateWindowExW(0, L"STATIC",
            L"Entity Render Distance: 1.0x (vanilla cull bbox)",
            WS_CHILD | SS_LEFT | SS_NOPREFIX,
            16, 268, 376, 18, hwnd, (HMENU)(uintptr_t)IDC_ENH_CULL_LBL,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(cull_lbl, WM_SETFONT, (WPARAM)water_font, MAKELPARAM(TRUE, 0));
        HWND cull_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | TBS_HORZ | TBS_AUTOTICKS | TBS_TOOLTIPS,
            16, 288, 376, 28, hwnd, (HMENU)(uintptr_t)IDC_ENH_CULL_SLIDER,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(cull_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(cull_slider, TBM_SETPAGESIZE, 0, 5);
        SendMessageW(cull_slider, TBM_SETTICFREQ, 25, 0); // tick at 1x, 2x, 4x, 8x, 16x
        SendMessageW(cull_slider, TBM_SETPOS, TRUE, 0);   // default 1.0x

        // Camera far-plane slider — extends the GPU Z-clip distance.
        // BH's main perspective at 7F220.c:741 uses D_801411A4 as the
        // far plane. Exponential pos 0..100 -> 1x..16x of vanilla.
        // Pairs with a Release Override button for snapshot restore.
        HWND far_lbl = CreateWindowExW(0, L"STATIC",
            L"Far Plane: 1.0x (vanilla — snapshot taken on first slider move)",
            WS_CHILD | SS_LEFT | SS_NOPREFIX,
            16, 324, 376, 18, hwnd, (HMENU)(uintptr_t)IDC_ENH_FAR_LBL,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(far_lbl, WM_SETFONT, (WPARAM)water_font, MAKELPARAM(TRUE, 0));
        HWND far_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | TBS_HORZ | TBS_AUTOTICKS | TBS_TOOLTIPS,
            16, 344, 296, 28, hwnd, (HMENU)(uintptr_t)IDC_ENH_FAR_SLIDER,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(far_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(far_slider, TBM_SETPAGESIZE, 0, 5);
        SendMessageW(far_slider, TBM_SETTICFREQ, 25, 0); // 1x, 2x, 4x, 8x, 16x
        SendMessageW(far_slider, TBM_SETPOS, TRUE, 0);   // default 1.0x
        HWND far_off = CreateWindowExW(0, L"BUTTON",
            L"Release override",
            WS_CHILD | BS_PUSHBUTTON,
            320, 344, 76, 28, hwnd, (HMENU)(uintptr_t)IDC_ENH_FAR_OFF,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(far_off, WM_SETFONT, (WPARAM)water_font, MAKELPARAM(TRUE, 0));

        CreateWindowExW(0, L"STATIC",
            L"BH was designed for 4:3 — wider aspect ratios let you see more of "
            L"the world but the HUD stays anchored at 4:3 coordinates (BH-side "
            L"fix would need actual HUD coord patching). No FOV correction yet.\r\n\r\n"
            L"Higher refresh rates render the same game-state more often (visual "
            L"smoothness); BH's internal tick stays at its original rate. 2D "
            L"upscale smooths HUD icons + sprites; doesn't reposition them.\r\n\r\n"
            L"Fog slider scales how far fog reaches. The visible 'cull line' you "
            L"see at high fog scales is BH's own render-distance limit (the N64 "
            L"engine simply doesn't draw geometry past it). Extending render "
            L"distance is a separate effort — see handoff.md.",
            WS_CHILD | SS_LEFT | SS_NOPREFIX,
            16, 120, 376, 208, hwnd, (HMENU)(uintptr_t)IDC_ENH_INFO,
            GetModuleHandleW(nullptr), nullptr);

        // Legacy placeholder ID still defined for show_tab_controls
        // compatibility — make a 0-size invisible window so GetDlgItem
        // doesn't return nullptr and break the visibility loop.
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD, 0, 0, 0, 0, hwnd, (HMENU)(uintptr_t)IDC_ENH_PLACE,
            GetModuleHandleW(nullptr), nullptr);

        // Options tab — Multiplayer POC toggle + info.
        //
        // Win32 quirk: controls created without an explicit font
        // inherit SYSTEM_FONT (a legacy bitmap font) which can
        // render checkbox + static text invisibly. Send a sane GUI
        // font to each after creation. The existing pushbuttons
        // work without this because Win32 has different defaults
        // for BS_PUSHBUTTON vs BS_AUTOCHECKBOX / STATIC.
        HFONT gui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND mp_chk = CreateWindowExW(0, L"BUTTON",
            L"Enable multiplayer POC (see other instance as ghost vehicle)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            16, 48, 376, 24, hwnd, (HMENU)(uintptr_t)IDC_OPT_MP_ENABLE,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(mp_chk, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND mp_info = CreateWindowExW(0, L"STATIC",
            L"Two-instance loopback demo. Run two bh_app.exe processes on this "
            L"machine; the second one with command line:\r\n\r\n"
            L"    bh_app.exe --port 12346 --peer-port 12345\r\n\r\n"
            L"Enable this checkbox in BOTH instances. Each instance will send "
            L"its player position to the other and render the remote player as "
            L"a 'ghost' Alpha 1 vehicle in its own world. No game-state sync "
            L"yet — each instance is its own independent game.\r\n\r\n"
            L"Defaults: this instance listens on 127.0.0.1:12345 and sends to "
            L"127.0.0.1:12346 — override with --port and --peer-port "
            L"on the command line.",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            16, 80, 376, 248, hwnd, (HMENU)(uintptr_t)IDC_OPT_MP_INFO,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(mp_info, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        // Terrain backup row — label + Export + Import + status line.
        // Layout is done in layout_overlay; initial sizes are throwaway.
        HWND tlbl = CreateWindowExW(0, L"STATIC",
            L"Terrain backup (saves the current level's full 256×256 tile "
            L"grid to a .json file, so crater/raise/lower/smooth edits persist "
            L"across saves):",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            16, 340, 376, 20, hwnd, (HMENU)(uintptr_t)IDC_OPT_TERRAIN_LBL,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(tlbl, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND tbtn_e = CreateWindowExW(0, L"BUTTON",
            L"Export current level…",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            16, 362, 180, 28, hwnd, (HMENU)(uintptr_t)IDC_OPT_TERRAIN_EXPORT,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(tbtn_e, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND tbtn_i = CreateWindowExW(0, L"BUTTON",
            L"Import from file…",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            204, 362, 180, 28, hwnd, (HMENU)(uintptr_t)IDC_OPT_TERRAIN_IMPORT,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(tbtn_i, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        // Entity export — separate JSON listing live buildings, vehicles,
        // aliens, save beacons + slot occupancy. Read-only for now; the
        // editor overlays them on the heightmap so you can see what's
        // where. Edit/import would go through a future spawn-respecting
        // path.
        HWND tbtn_ent = CreateWindowExW(0, L"BUTTON",
            L"Export entities (buildings, vehicles, aliens, beacons)…",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            16, 394, 376, 28, hwnd, (HMENU)(uintptr_t)IDC_OPT_ENTITY_EXPORT,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(tbtn_ent, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND tstat = CreateWindowExW(0, L"STATIC",
            L"(no terrain operation yet this session)",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            16, 394, 376, 18, hwnd, (HMENU)(uintptr_t)IDC_OPT_TERRAIN_STATUS,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(tstat, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        // Legacy IDC_OPT_PLACE — replaced by the checkbox/info above.
        // Keep an invisible 0-size window so show_tab_controls's
        // GetDlgItem lookup still works without churn.
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD, 0, 0, 0, 0, hwnd, (HMENU)(uintptr_t)IDC_OPT_PLACE,
            GetModuleHandleW(nullptr), nullptr);

        // Close button — always visible at the bottom, outside any tab.
        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE,
            312, 360, 80, 28, hwnd, (HMENU)(uintptr_t)IDC_CLOSE,
            GetModuleHandleW(nullptr), nullptr);

        // Make sure the right initial tab is visible.
        show_tab_controls(hwnd, TAB_CHEATS);

        // Do one layout pass so the initial size gets controls
        // positioned via the same code WM_SIZE uses on resize.
        layout_overlay(hwnd);

        // Populate the listbox: BH cheats, separator, custom cheats.
        HWND list = GetDlgItem(hwnd, IDC_LIST);
        auto add_row_a = [&](const char* name_utf8, const char* desc_utf8) {
            wchar_t name_w[64];
            wchar_t desc_w[256];
            MultiByteToWideChar(CP_UTF8, 0, name_utf8, -1, name_w, 32);
            MultiByteToWideChar(CP_UTF8, 0, desc_utf8, -1, desc_w, 200);
            std::wstring row = name_w;
            row += L"  —  ";  // em-dash
            row += desc_w;
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
        };
        for (const CheatInfo& c : kCheats) {
            add_row_a(c.name, c.description);
        }
        SendMessageW(list, LB_ADDSTRING, 0,
            (LPARAM)L"─── Custom (modder) cheats ───");
        for (const CustomCheat& c : kCustomCheats) {
            add_row_a(c.name, c.description);
        }
        SendMessageW(list, LB_ADDSTRING, 0,
            (LPARAM)L"─── Tools (open sub-dialogs) ───");
        for (const ToolEntry& t : kTools) {
            add_row_a(t.name, t.description);
        }
        // Default to the first item selected so Activate works
        // immediately on Enter.
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        SetDlgItemTextA(hwnd, IDC_DESC, format_description(0).c_str());
        return 0;
    }

    case WM_NOTIFY: {
        // Tab control fires TCN_SELCHANGE when the user clicks a tab.
        // We use it to swap which controls are visible.
        LPNMHDR n = (LPNMHDR)lp;
        if (n != nullptr && n->idFrom == IDC_TABCTRL && n->code == TCN_SELCHANGE) {
            const int new_tab = int(SendDlgItemMessageW(hwnd, IDC_TABCTRL,
                                                        TCM_GETCURSEL, 0, 0));
            show_tab_controls(hwnd, new_tab);
            return 0;
        }
        break;
    }

    case WM_HSCROLL: {
        // Trackbars (fog slider) fire WM_HSCROLL on every drag tick
        // and on release. lParam = HWND of the trackbar, wParam low
        // word = scroll request code. We just read the current
        // position and push it as a fog scale.
        HWND scrolled = (HWND)lp;
        if (scrolled == GetDlgItem(hwnd, IDC_ENH_FOG_SLIDER)) {
            const int pos = int(SendMessageW(scrolled, TBM_GETPOS, 0, 0));
            // Exponential: each +25 positions doubles the scale.
            // pos 25 = 1.0x (default), pos 50 = 2.0x, pos 0 = 0.5x, etc.
            const float scale = std::pow(2.0f, (float(pos) - 25.0f) / 25.0f);
            bh::renderer::set_fog_scale(scale);
            // Refresh the label so the user sees the current value.
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Fog Distance: %.2fx", double(scale));
            SetDlgItemTextA(hwnd, IDC_ENH_FOG_LBL, buf);
            return 0;
        }
        if (scrolled == GetDlgItem(hwnd, IDC_ENH_FAR_SLIDER)) {
            // Exponential: pos N -> scale = 2^(N/25)
            // pos 0 = 1x (vanilla), 25 = 2x, 50 = 4x, 75 = 8x, 100 = 16x
            const int pos = int(SendMessageW(scrolled, TBM_GETPOS, 0, 0));
            const float scale = std::pow(2.0f, float(pos) / 25.0f);
            g_far_plane_scale.store(scale, std::memory_order_release);
            g_far_plane_override_active.store(true, std::memory_order_release);
            char buf[128];
            const float orig = g_far_plane_original.load(std::memory_order_acquire);
            if (orig > 0.0f) {
                std::snprintf(buf, sizeof(buf),
                    "Far Plane: %.2fx (%.0f -> %.0f, active)",
                    double(scale), double(orig), double(orig * scale));
            } else {
                std::snprintf(buf, sizeof(buf),
                    "Far Plane: %.2fx (active, snapshot pending first tick)",
                    double(scale));
            }
            SetDlgItemTextA(hwnd, IDC_ENH_FAR_LBL, buf);
            return 0;
        }
        if (scrolled == GetDlgItem(hwnd, IDC_ENH_CULL_SLIDER)) {
            // Exponential mapping: pos N -> scale = 2^(N/25)
            // pos 0 = 1x (vanilla), 25 = 2x, 50 = 4x, 75 = 8x, 100 = 16x
            const int pos = int(SendMessageW(scrolled, TBM_GETPOS, 0, 0));
            const float scale = std::pow(2.0f, float(pos) / 25.0f);
            g_entity_cull_scale.store(scale, std::memory_order_release);
            char buf[96];
            if (pos == 0) {
                std::snprintf(buf, sizeof(buf),
                    "Entity Render Distance: 1.0x (vanilla cull bbox)");
            } else {
                std::snprintf(buf, sizeof(buf),
                    "Entity Render Distance: %.2fx "
                    "(bbox ~%d x %d tiles around camera)",
                    double(scale),
                    int((0x800 + 0x700) * scale / 256),
                    int((0x900 + 0x700) * scale / 256));
            }
            SetDlgItemTextA(hwnd, IDC_ENH_CULL_LBL, buf);
            return 0;
        }
        if (scrolled == GetDlgItem(hwnd, IDC_ENH_WATER_SLIDER)) {
            // Snapshot original water Y on the first move (so Release
            // Override can restore it). Linear mapping pos 0..200 ->
            // water Y 0..+2000 (10 per step). Negative Y values cause
            // a map-screen artifact where the entire map renders as
            // water, so we clamp the slider at 0.
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            water_snapshot_if_needed(rdram);
            const int pos = int(SendMessageW(scrolled, TBM_GETPOS, 0, 0));
            const int water_y = pos * 10; // 0..+2000
            g_water_target_y.store(int32_t(water_y),
                                   std::memory_order_release);
            g_water_override_active.store(true, std::memory_order_release);
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                "Water Level Override: Y = %d (active)", water_y);
            SetDlgItemTextA(hwnd, IDC_ENH_WATER_LBL, buf);
            return 0;
        }
        break;
    }

    case WM_SIZE:
        layout_overlay(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        // Cap how small the user can drag the window — below this and
        // the description label clips so badly the cheat info isn't
        // readable anyway.
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 360;
        mmi->ptMinTrackSize.y = 320;
        return 0;
    }

    case WM_COMMAND: {
        const WORD id   = LOWORD(wp);
        const WORD code = HIWORD(wp);
        if (id == IDC_ENH_ASPECT_COMBO && code == CBN_SELCHANGE) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_ENH_ASPECT_COMBO,
                                                    CB_GETCURSEL, 0, 0));
            bh::renderer::set_aspect_ratio(sel);
            return 0;
        }
        if (id == IDC_ENH_REFRESH_COMBO && code == CBN_SELCHANGE) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_ENH_REFRESH_COMBO,
                                                    CB_GETCURSEL, 0, 0));
            bh::renderer::set_refresh_rate(sel);
            return 0;
        }
        if (id == IDC_ENH_UPSCALE_COMBO && code == CBN_SELCHANGE) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_ENH_UPSCALE_COMBO,
                                                    CB_GETCURSEL, 0, 0));
            bh::renderer::set_upscale_2d(sel);
            return 0;
        }
        if (id == IDC_ENH_FAR_OFF) {
            // Clear the tick override AND restore the snapshotted
            // original far plane. Without the write-back, BH might
            // not recompute the far plane on its own for a while
            // (only on level transitions / camera mode changes).
            g_far_plane_override_active.store(false, std::memory_order_release);
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            if (rdram != nullptr &&
                g_far_plane_snapshot_valid.load(std::memory_order_acquire)) {
                const float orig = g_far_plane_original.load(std::memory_order_acquire);
                std::memcpy(rdram + (kFarPlaneAddr - kVirtBase), &orig, sizeof(orig));
            }
            SetDlgItemTextA(hwnd, IDC_ENH_FAR_LBL,
                            "Far Plane: 1.0x (vanilla — override released)");
            return 0;
        }
        if (id == IDC_ENH_WATER_OFF) {
            // Clear the tick override AND restore the snapshotted
            // pre-override water Y. Without the write-back, water
            // sits frozen at whatever we last forced it to since
            // BH only updates D_80222A70 on level state transitions.
            g_water_override_active.store(false, std::memory_order_release);
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            if (rdram != nullptr &&
                g_water_snapshot_valid.load(std::memory_order_acquire)) {
                const int32_t orig = g_water_original_y.load(std::memory_order_acquire);
                write_n64_u32(rdram, kWaterLevelAddr, uint32_t(orig));
            }
            SetDlgItemTextA(hwnd, IDC_ENH_WATER_LBL,
                            "Water Level Override: (off)");
            return 0;
        }
        if (id == IDC_OPT_MP_ENABLE && code == BN_CLICKED) {
            const LRESULT checked = SendDlgItemMessageW(hwnd, IDC_OPT_MP_ENABLE,
                                                        BM_GETCHECK, 0, 0);
            bh::net::set_enabled(checked == BST_CHECKED);
            return 0;
        }
        if (id == IDC_OPT_TERRAIN_EXPORT) {
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            std::string status;
            export_terrain_to_file(rdram, hwnd, status);
            SetDlgItemTextA(hwnd, IDC_OPT_TERRAIN_STATUS, status.c_str());
            return 0;
        }
        if (id == IDC_OPT_TERRAIN_IMPORT) {
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            std::string status;
            import_terrain_from_file(rdram, hwnd, status);
            SetDlgItemTextA(hwnd, IDC_OPT_TERRAIN_STATUS, status.c_str());
            return 0;
        }
        if (id == IDC_OPT_ENTITY_EXPORT) {
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            std::string status;
            export_entities_to_file(rdram, hwnd, status);
            SetDlgItemTextA(hwnd, IDC_OPT_TERRAIN_STATUS, status.c_str());
            return 0;
        }
        if (id == IDC_LIST && code == LBN_SELCHANGE) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_LIST,
                                                    LB_GETCURSEL, 0, 0));
            SetDlgItemTextA(hwnd, IDC_DESC, format_description(sel).c_str());
            return 0;
        }
        if (id == IDC_ACTIVATE) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_LIST,
                                                    LB_GETCURSEL, 0, 0));
            const RowMapping m = map_list_row(sel);
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            switch (m.kind) {
            case RowKind::BhCheat:
                write_cheat_to_buffer(rdram, kCheats[m.index].name);
                std::fprintf(stderr,
                    "[cheats] menu -> activated BH cheat '%s' "
                    "(wrote reversed pattern to cheatInputBuffer)\n",
                    kCheats[m.index].name);
                break;
            case RowKind::CustomCheat:
                std::fprintf(stderr,
                    "[cheats] menu -> activating custom cheat '%s'\n",
                    kCustomCheats[m.index].name);
                kCustomCheats[m.index].activate(rdram);
                break;
            case RowKind::Tool:
                std::fprintf(stderr,
                    "[cheats] menu -> opening tool '%s'\n",
                    kTools[m.index].name);
                kTools[m.index].open();
                break;
            case RowKind::Separator:
                // Selecting the separator row does nothing.
                break;
            }
            return 0;
        }
        if (id == IDC_CLOSE) {
            ShowWindow(hwnd, SW_HIDE);
            g_overlay_visible.store(false, std::memory_order_release);
            return 0;
        }
        break;
    }

    case WM_BH_SHOW:
        ShowWindow(hwnd, SW_SHOWNA);  // SW_SHOWNA = show without stealing focus
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetForegroundWindow(hwnd);  // pull to top for clickability
        return 0;

    case WM_BH_HIDE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        g_overlay_visible.store(false, std::memory_order_release);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// Vehicle Morpher sub-dialog. Lives in its own top-level Win32 window,
// runs on the same overlay thread (single message pump handles both
// windows via the GetMessage loop in overlay_thread_main).
//
// Opens with a list of vehicles valid for the current level, and a
// Morph button that writes the selected spec index to the player's
// vehicle slot. Refresh re-reads the level byte in case the user
// switched levels while the dialog was open.
// ===========================================================================

constexpr UINT IDC_VM_LEVELLBL  = 2001;
constexpr UINT IDC_VM_LIST      = 2002;
constexpr UINT IDC_VM_MORPH     = 2003;
constexpr UINT IDC_VM_REFRESH   = 2004;
constexpr UINT IDC_VM_CLOSE     = 2005;
constexpr UINT IDC_VM_SPAWN     = 2006;
constexpr UINT IDC_VM_COUNT     = 2007;
constexpr UINT IDC_VM_COUNTLBL  = 2008;

std::atomic<HWND> g_morpher_hwnd{nullptr};

// The level whose vehicles are currently shown. Used by Morph to
// know which row → which spec ID (in case row order isn't 1:1 with
// spec IDs, which is true for Comet).
std::atomic<int>  g_morpher_level_index{0};

// Repopulate the level label + listbox from the current RDRAM state.
void morpher_refresh(HWND hwnd) {
    uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
    int level_idx = 0;
    int active_vehicles = 0;
    if (rdram != nullptr) {
        const uint8_t lv = read_n64_u8(rdram, kLevelByte);
        if (lv >= 1 && lv < kLevelCount) {
            level_idx = int(lv);
        }
        // Count active vehicle slots (unk1A != 0). Slot 0 is the
        // player avatar — if on foot it's 0 (Adam), if in a vehicle
        // it's that vehicle's spec. Count slot 0 only when occupied.
        for (int i = 0; i < kVehicleInstanceCount; ++i) {
            const uint8_t spec = read_n64_u8(rdram,
                kVehicleInstancesBase + uint32_t(i) * kVehicleInstanceSize + 0x1A);
            if (spec != 0) active_vehicles++;
        }
    }
    g_morpher_level_index.store(level_idx, std::memory_order_release);

    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
        "Level: %s  (0x%02X)   |   Vehicles: %d / %d  (slot 0 = player avatar)",
        kLevels[level_idx].name, unsigned(level_idx),
        active_vehicles, kVehicleInstanceCount);
    SetDlgItemTextA(hwnd, IDC_VM_LEVELLBL, hdr);

    HWND list = GetDlgItem(hwnd, IDC_VM_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);

    const LevelInfo& li = kLevels[level_idx];
    if (li.vehicles == nullptr || li.vehicle_count == 0) {
        SendMessageW(list, LB_ADDSTRING, 0,
            (LPARAM)L"(no level loaded — start a mission first, then Refresh)");
        return;
    }
    for (size_t i = 0; i < li.vehicle_count; ++i) {
        char row[128];
        std::snprintf(row, sizeof(row), "0x%02X  —  %s",
            unsigned(li.vehicles[i].id), li.vehicles[i].name);
        wchar_t row_w[160];
        MultiByteToWideChar(CP_UTF8, 0, row, -1, row_w, 160);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)row_w);
    }
    SendMessageW(list, LB_SETCURSEL, 0, 0);
}

LRESULT CALLBACK morpher_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            8, 8, 384, 18, hwnd, (HMENU)(uintptr_t)IDC_VM_LEVELLBL,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
            8, 32, 384, 240, hwnd, (HMENU)(uintptr_t)IDC_VM_LIST,
            GetModuleHandleW(nullptr), nullptr);

        // Row: Morph | Spawn | x [Count] | Refresh | Close
        CreateWindowExW(0, L"BUTTON", L"Morph",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            8, 280, 64, 28, hwnd, (HMENU)(uintptr_t)IDC_VM_MORPH,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"BUTTON", L"Spawn",
            WS_CHILD | WS_VISIBLE,
            76, 280, 56, 28, hwnd, (HMENU)(uintptr_t)IDC_VM_SPAWN,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"STATIC", L"x",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            136, 285, 10, 18, hwnd, (HMENU)(uintptr_t)IDC_VM_COUNTLBL,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT,
            148, 282, 32, 24, hwnd, (HMENU)(uintptr_t)IDC_VM_COUNT,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"BUTTON", L"Refresh",
            WS_CHILD | WS_VISIBLE,
            188, 280, 64, 28, hwnd, (HMENU)(uintptr_t)IDC_VM_REFRESH,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE,
            256, 280, 64, 28, hwnd, (HMENU)(uintptr_t)IDC_VM_CLOSE,
            GetModuleHandleW(nullptr), nullptr);

        morpher_refresh(hwnd);
        return 0;
    }

    case WM_COMMAND: {
        const WORD id = LOWORD(wp);
        if (id == IDC_VM_MORPH) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_VM_LIST,
                                                    LB_GETCURSEL, 0, 0));
            const int level_idx = g_morpher_level_index.load(std::memory_order_acquire);
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            const LevelInfo& li = kLevels[level_idx];
            if (rdram != nullptr && li.vehicles != nullptr &&
                sel >= 0 && size_t(sel) < li.vehicle_count) {
                const uint8_t new_id = li.vehicles[sel].id;

                // Write through D_80052B34 (current-entity pointer) so the
                // morph targets whatever the player is actually controlling.
                // When on foot, this hits vehicleInstances[0] (Adam slot) and
                // wraps the player in a vehicle. When in a vehicle, it hits
                // THAT slot and morphs the driven vehicle in place. The
                // GameShark code's hardcoded 0x8004DCEA only targets slot 0
                // (no-op while driving) — using the live pointer is strictly
                // better.
                const uint32_t entity_virt = read_n64_u32(rdram, kCurrentEntityPtr);
                if (ram_ptr_valid(entity_virt)) {
                    write_n64_u8(rdram, entity_virt + kEntityOffsetSpecIdx, new_id);
                    std::fprintf(stderr,
                        "[cheats] morpher -> wrote spec=0x%02X (%s) to entity 0x%08X (level=%s)\n",
                        unsigned(new_id), li.vehicles[sel].name,
                        entity_virt, li.name);
                } else {
                    std::fprintf(stderr,
                        "[cheats] morpher: D_80052B34 = 0x%08X (no current entity); morph skipped\n",
                        entity_virt);
                }
            } else {
                std::fprintf(stderr,
                    "[cheats] morpher: Morph skipped (no level loaded or invalid selection)\n");
            }
            morpher_refresh(hwnd);  // morph may have changed slot 0's count contribution
            return 0;
        }
        if (id == IDC_VM_SPAWN) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_VM_LIST,
                                                    LB_GETCURSEL, 0, 0));
            const int level_idx = g_morpher_level_index.load(std::memory_order_acquire);
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            const LevelInfo& li = kLevels[level_idx];
            if (rdram == nullptr || li.vehicles == nullptr ||
                sel < 0 || size_t(sel) >= li.vehicle_count) {
                std::fprintf(stderr,
                    "[cheats] vehicle spawner: Spawn skipped (no level loaded or invalid selection)\n");
                return 0;
            }

            BOOL ok = FALSE;
            int count = int(GetDlgItemInt(hwnd, IDC_VM_COUNT, &ok, FALSE));
            if (!ok || count < 1) count = 1;
            if (count > kVehicleInstanceCount - 1) count = kVehicleInstanceCount - 1;

            // Read player position once. Spawn each vehicle in a 192-unit
            // ring around the player (or exactly at player for count=1).
            const uint32_t entity_virt = read_n64_u32(rdram, kCurrentEntityPtr);
            float fx = 0.0f, fy = 0.0f, fz = 0.0f;
            bool have_pos = ram_ptr_valid(entity_virt);
            if (have_pos) {
                fx = read_n64_f32(rdram, entity_virt + kEntityOffsetCacheX);
                fy = read_n64_f32(rdram, entity_virt + kEntityOffsetCacheY);
                fz = read_n64_f32(rdram, entity_virt + kEntityOffsetCacheZ);
            }
            constexpr float kRingRadius = 192.0f;

            const uint8_t veh_type = li.vehicles[sel].id;
            int spawned = 0, failed = 0;
            for (int i = 0; i < count; ++i) {
                const int slot = direct_spawn_vehicle(rdram, veh_type);
                if (slot < 0) { failed++; break; }
                if (have_pos) {
                    float ox = 0.0f, oz = 0.0f;
                    if (count > 1) {
                        const float angle = float(i) * (2.0f * 3.14159265f / float(count));
                        ox = std::cos(angle) * kRingRadius;
                        oz = std::sin(angle) * kRingRadius;
                    }
                    const float sx = fx + ox;
                    const float sz = fz + oz;
                    const uint32_t v = kVehicleInstancesBase
                                     + uint32_t(slot) * kVehicleInstanceSize;
                    write_n64_f32(rdram, v + 0x4C, sx);
                    write_n64_f32(rdram, v + 0x50, fy);
                    write_n64_f32(rdram, v + 0x54, sz);
                    write_n64_s16(rdram, v + 0x00, int16_t(sx));
                    write_n64_s16(rdram, v + 0x02, int16_t(fy));
                    write_n64_s16(rdram, v + 0x04, int16_t(sz));
                }
                spawned++;
            }

            std::fprintf(stderr,
                "[cheats] vehicle spawner -> 0x%02X (%s, %s) x %d: %d spawned, %d failed\n",
                unsigned(veh_type), li.vehicles[sel].name, li.name,
                count, spawned, failed);
            morpher_refresh(hwnd);  // update the "Vehicles: N / 128" counter
            return 0;
        }
        if (id == IDC_VM_REFRESH) {
            morpher_refresh(hwnd);
            return 0;
        }
        if (id == IDC_VM_CLOSE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Called from the main dialog's Activate handler. Lazy-creates the
// sub-dialog window on first open and shows it; subsequent opens
// re-show (and refresh) the existing window.
void open_vehicle_morpher_impl() {
    HWND hwnd = g_morpher_hwnd.load(std::memory_order_acquire);
    if (hwnd == nullptr) {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = morpher_wnd_proc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BHVehicleMorpher";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        // RegisterClass returns 0 on repeat registration — ignore that.
        RegisterClassW(&wc);

        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            L"BHVehicleMorpher",
            L"BH — Vehicle Morpher",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            150, 150, 416, 360,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        if (hwnd == nullptr) {
            std::fprintf(stderr,
                "[cheats] morpher: CreateWindowEx failed (err=%lu)\n",
                GetLastError());
            return;
        }
        g_morpher_hwnd.store(hwnd, std::memory_order_release);
    }
    morpher_refresh(hwnd);
    ShowWindow(hwnd, SW_SHOWNA);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(hwnd);
}

void open_vehicle_morpher() {
    // The Tool's `open` is invoked from the main overlay window's
    // WM_COMMAND on the overlay thread — direct call is fine.
    open_vehicle_morpher_impl();
}

// ===========================================================================
// Alien Morpher sub-dialog. Mass-replaces every alive alien slot's
// specIndex with the chosen type. Simpler than the vehicle morpher
// (no per-level table, no current-entity lookup) — just walk the
// alienInstances array and rewrite.
// ===========================================================================

constexpr UINT IDC_AM_LEVELLBL = 3001;
constexpr UINT IDC_AM_LIST     = 3002;
constexpr UINT IDC_AM_MORPHALL = 3003;
constexpr UINT IDC_AM_REFRESH  = 3004;
constexpr UINT IDC_AM_CLOSE    = 3005;
constexpr UINT IDC_AM_SPAWN    = 3006;
constexpr UINT IDC_AM_COUNT    = 3007;
constexpr UINT IDC_AM_COUNTLBL = 3008;

// Replicate func_8007956C_8851C from
// body-harvest-decompilation/src.us/overlay_gameplay/outside/884C0.c
// in host-side RDRAM writes. Allocates a free slot from BH's
// own slot-list (D_8014D308), copies the standard alien template
// into it, sets specIndex + hitPoints + spec-derived flag bits,
// and increments the active counter. Returns the slot index on
// success or -1 on failure / table full.
//
// Skipped vs the original:
//  - Recursive "spawn helper" call for harvester (0x19) / boss
//    (0x1B) types — those normally spawn a companion entity at
//    type 0. Doing the recursion would need func_80079510_884C0
//    too. Spawning harvesters/bosses without the helper may look
//    odd but shouldn't crash.
//  - Level-5 piranha (0x14) special-case.
//
// After spawn, caller should write player position into the new
// alien's pos fields so it materializes at the player rather than
// at the template's default (0, 0, 0).
int direct_spawn_alien(uint8_t* rdram, uint8_t alien_type) {
    if (rdram == nullptr) return -1;

    uint8_t activeCount = read_n64_u8(rdram, kAlienActiveCount);
    if (activeCount == 0xFFu) return -1;
    if (activeCount >= 0xFEu && (alien_type == 0x19 || alien_type == 0x1B)) return -1;

    uint8_t slotIndex = read_n64_u8(rdram, kAlienSlotList + uint32_t(activeCount));
    uint32_t alien_virt = kAlienInstancesBase + uint32_t(slotIndex) * kAlienInstanceSize;

    // If the slot's flag bits 0x600 are set the slot is in transition
    // (dying / despawning). Try the next slot.
    const uint32_t cur_flags = read_n64_u32(rdram, alien_virt + 0x20);
    if (cur_flags & 0x600) {
        if (activeCount == 0xFEu) return -1;
        slotIndex = read_n64_u8(rdram, kAlienSlotList + uint32_t(activeCount + 1u));
        alien_virt = kAlienInstancesBase + uint32_t(slotIndex) * kAlienInstanceSize;
    }

    // Copy the 0x50-byte template. Aligned word copy is host-native
    // byte order, which matches MEM_W storage. memcpy is fine.
    {
        const uint32_t template_host = kAlienTemplate - kVirtBase;
        const uint32_t alien_host    = alien_virt    - kVirtBase;
        std::memcpy(rdram + alien_host, rdram + template_host, kAlienInstanceSize);
    }

    // specIndex (offset 0x1A, u8 — XOR-3 byte access)
    write_n64_u8(rdram, alien_virt + 0x1A, alien_type);

    // hitPoints from spec (offset 0x3A within spec, s16) -> alien.unk1C
    const uint32_t spec_virt = kAlienSpecsBase + uint32_t(alien_type) * kAlienSpecSize;
    const int16_t hp = read_n64_s16(rdram, spec_virt + kAlienSpecHpOffset);
    write_n64_s16(rdram, alien_virt + 0x1C, hp);

    // Set spec-derived flag bits on alien.unk20 (aligned u32)
    const uint32_t specFlags = read_n64_u32(rdram, spec_virt + kAlienSpecFlagsOffset);
    uint32_t newFlags = read_n64_u32(rdram, alien_virt + 0x20);
    if (specFlags & 0x400u)    newFlags |= 0x8000100u;
    if (specFlags & 0x2000u)   newFlags |= 0x40000u;
    if (specFlags & 0x800000u) newFlags |= 0x400000u;
    write_n64_u32(rdram, alien_virt + 0x20, newFlags);

    // Increment active counter.
    write_n64_u8(rdram, kAlienActiveCount, uint8_t(activeCount + 1));

    return int(slotIndex);
}

// Direct-RAM vehicle spawner. BH doesn't appear to have a clean
// generic "spawn vehicle" function in the decompiled source —
// vehicle placement happens via level-data parsing at level load.
// So instead of replicating an internal function, we use a copy-
// from-template approach: find a real existing vehicle in the
// level (any slot with unk1A != 0 that isn't the player's), copy
// it into a free slot, then overwrite the destination's type and
// position. The "real existing vehicle" source gets us the right
// flag patterns for "render as a normal world vehicle" — copying
// from slot 0 (the Adam template) seems to inherit "on-foot
// player avatar" render flags that hide the spawn until BH's next
// player-exit refresh pass.
//
// Fallback to slot 0 only if no real vehicle exists in the level
// (rare — most levels have some pre-placed vehicles from start).
//
// Returns slot index on success, -1 if no free slot was found.
int direct_spawn_vehicle(uint8_t* rdram, uint8_t veh_type) {
    if (rdram == nullptr) return -1;

    // Find a free slot. Always copy from slot 0 (the on-foot
    // player avatar) — it's the cleanest baseline: no AI driver
    // state, no enemy-controller fields. We follow up with the
    // spec-driven reset (zeros stale state, rederives HP/fuel/
    // flags) and the active-flag + scene rebuild trigger, so
    // slot 0's "Adam render flags" don't keep the slot invisible.
    int free_slot = -1;
    for (int i = 1; i < kVehicleInstanceCount; ++i) {
        const uint8_t spec = read_n64_u8(rdram,
            kVehicleInstancesBase + uint32_t(i) * kVehicleInstanceSize + 0x1A);
        if (spec == 0) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) return -1;
    const int src_slot = 0;

    // Copy src slot into free slot. Aligned word copy = host-native
    // byte order (MEM_W convention), so memcpy works directly.
    const uint32_t src_host = (kVehicleInstancesBase
                              + uint32_t(src_slot) * kVehicleInstanceSize)
                              - kVirtBase;
    const uint32_t dst_host = (kVehicleInstancesBase
                              + uint32_t(free_slot) * kVehicleInstanceSize)
                              - kVirtBase;
    std::memcpy(rdram + dst_host, rdram + src_host, kVehicleInstanceSize);

    // Set vehicle type on the destination slot (offset 0x1A, u8 — XOR-3).
    const uint32_t dst_virt = kVehicleInstancesBase
                            + uint32_t(free_slot) * kVehicleInstanceSize;
    write_n64_u8(rdram, dst_virt + 0x1A, veh_type);

    // Run the spec-driven reset: BH's func_800FAE84_109E34 zeros
    // all per-instance AI/movement state and re-derives unk20,
    // hitPoints, fuel from the spec. Without this, our template
    // copy leaves stale AI fields (counters, targets) that override
    // player control on the next frame — symptom: engine sound
    // plays briefly on entry then stops, vehicle can't be driven.
    {
        const uint32_t spec_virt = kVehicleSpecsBase
                                 + uint32_t(veh_type) * kVehicleSpecSize;
        const uint32_t spec_flags = read_n64_u32(rdram, spec_virt + kVehicleSpecFlagsOff);
        const uint16_t spec_hp    = read_n64_u16(rdram, spec_virt + kVehicleSpecHpOff);
        const uint8_t  spec_fuel  = read_n64_u8 (rdram, spec_virt + kVehicleSpecFuelOff);

        // Zero positional / orientation / velocity / counter fields.
        // Matches the func_800FAE84 assignment list field-for-field.
        write_n64_s16(rdram, dst_virt + 0x00, 0);  // X
        write_n64_s16(rdram, dst_virt + 0x02, 0);  // Y
        write_n64_s16(rdram, dst_virt + 0x04, 0);  // Z
        write_n64_s16(rdram, dst_virt + 0x06, 0);  // X rot
        write_n64_s16(rdram, dst_virt + 0x08, 0);  // Y rot
        write_n64_s16(rdram, dst_virt + 0x0A, 0);  // Z rot
        write_n64_s16(rdram, dst_virt + 0x0C, int16_t(-2));  // unkC
        write_n64_s16(rdram, dst_virt + 0x0E, 0);  // direction
        write_n64_s16(rdram, dst_virt + 0x10, 0);  // elevation
        write_n64_s16(rdram, dst_virt + 0x12, 0);  // speed
        write_n64_s16(rdram, dst_virt + 0x14, 0);
        write_n64_s16(rdram, dst_virt + 0x16, 0);
        write_n64_s16(rdram, dst_virt + 0x18, 0);
        // Derive flags + HP + fuel from spec.
        // unk20 is u16; spec->unk4C is u32 — write low 16 bits and
        // OR in the active-vehicle flag.
        const uint16_t init_flags = uint16_t(spec_flags & 0xFFFFu)
                                  | uint16_t(kVehicleActiveFlagBit);
        write_n64_u16(rdram, dst_virt + 0x20, init_flags);
        write_n64_s16(rdram, dst_virt + 0x1C, int16_t(spec_hp));  // HP
        write_n64_s16(rdram, dst_virt + 0x1E, 0);
        write_n64_s16(rdram, dst_virt + 0x22, 0);
        write_n64_s16(rdram, dst_virt + 0x24, 0);
        write_n64_s16(rdram, dst_virt + 0x26, 0);
        write_n64_s16(rdram, dst_virt + 0x28, 0);
        write_n64_s16(rdram, dst_virt + 0x2A, 0);
        write_n64_s16(rdram, dst_virt + 0x2E, int16_t(0xFA));
        write_n64_f32(rdram, dst_virt + 0x30, 0.0f);     // velX
        write_n64_f32(rdram, dst_virt + 0x34, 0.0f);     // velY
        write_n64_f32(rdram, dst_virt + 0x38, 0.0f);     // velZ
        write_n64_s16(rdram, dst_virt + 0x3C, int16_t(int(spec_fuel) << 8)); // fuel
        write_n64_s16(rdram, dst_virt + 0x40, 0);
        write_n64_s16(rdram, dst_virt + 0x42, 0);
        // unk46 &= 0xFFC0 — preserve low 6 bits, zero the rest. As u16.
        const uint16_t pre46 = read_n64_u16(rdram, dst_virt + 0x46);
        write_n64_u16(rdram, dst_virt + 0x46, uint16_t(pre46 & 0xFFC0u));
        write_n64_f32(rdram, dst_virt + 0x4C, 0.0f);
        write_n64_f32(rdram, dst_virt + 0x50, 0.0f);
        write_n64_f32(rdram, dst_virt + 0x54, 0.0f);
        write_n64_f32(rdram, dst_virt + 0x58, 0.0f);
        // D_80158C58[slot_idx] = 0.0f
        write_n64_f32(rdram,
                      kVehiclePerSlotFloats + uint32_t(free_slot) * 4u,
                      0.0f);
    }

    // Mark scene flags as "vehicle-list rebuild needed". Next time
    // BH's logic checks bit 0x2000 in D_80159320 it'll run
    // func_800FAD10_109CC0 which walks all slots and rebuilds the
    // D_80158E80 active-vehicle index list to include our new slot.
    const uint32_t sceneFlags = read_n64_u32(rdram, kSceneFlagsAddr);
    write_n64_u32(rdram, kSceneFlagsAddr,
                  sceneFlags | kSceneFlagsRebuildBit);

    return free_slot;
}

std::atomic<HWND> g_alien_morpher_hwnd{nullptr};

void alien_morpher_refresh(HWND hwnd) {
    uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
    int level_idx = 0;
    int alive_count = 0;
    if (rdram != nullptr) {
        const uint8_t lv = read_n64_u8(rdram, kLevelByte);
        if (lv >= 1 && lv < kLevelCount) level_idx = int(lv);
        // Count alive aliens for the status line.
        for (int i = 0; i < kAlienInstanceCount; ++i) {
            const uint32_t spec_virt = kAlienInstancesBase
                                      + uint32_t(i) * kAlienInstanceSize
                                      + 0x1A;
            if (read_n64_u8(rdram, spec_virt) >= kAlienAliveMinSpec) {
                alive_count++;
            }
        }
    }

    char hdr[128];
    std::snprintf(hdr, sizeof(hdr),
        "Level: %s   |   Alive aliens: %d / %d",
        kLevels[level_idx].name, alive_count, kAlienInstanceCount);
    SetDlgItemTextA(hwnd, IDC_AM_LEVELLBL, hdr);

    HWND list = GetDlgItem(hwnd, IDC_AM_LIST);
    if (SendMessageW(list, LB_GETCOUNT, 0, 0) == 0) {
        // Populate once on first refresh.
        for (const AlienTypeEntry& t : kAlienTypes) {
            char row[128];
            std::snprintf(row, sizeof(row), "0x%02X  —  %s",
                unsigned(t.id), t.name);
            wchar_t row_w[160];
            MultiByteToWideChar(CP_UTF8, 0, row, -1, row_w, 160);
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)row_w);
        }
        SendMessageW(list, LB_SETCURSEL, 13, 0);  // default to Harvester (index 13 in kAlienTypes)
    }
}

LRESULT CALLBACK alien_morpher_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            8, 8, 384, 18, hwnd, (HMENU)(uintptr_t)IDC_AM_LEVELLBL,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
            8, 32, 384, 240, hwnd, (HMENU)(uintptr_t)IDC_AM_LIST,
            GetModuleHandleW(nullptr), nullptr);

        // Row 1: Morph All | Spawn | x [Count] | Refresh | Close
        CreateWindowExW(0, L"BUTTON", L"Morph All",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            8, 280, 72, 28, hwnd, (HMENU)(uintptr_t)IDC_AM_MORPHALL,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"BUTTON", L"Spawn",
            WS_CHILD | WS_VISIBLE,
            84, 280, 56, 28, hwnd, (HMENU)(uintptr_t)IDC_AM_SPAWN,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"STATIC", L"x",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            144, 285, 10, 18, hwnd, (HMENU)(uintptr_t)IDC_AM_COUNTLBL,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT,
            156, 282, 32, 24, hwnd, (HMENU)(uintptr_t)IDC_AM_COUNT,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"BUTTON", L"Refresh",
            WS_CHILD | WS_VISIBLE,
            196, 280, 64, 28, hwnd, (HMENU)(uintptr_t)IDC_AM_REFRESH,
            GetModuleHandleW(nullptr), nullptr);

        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE,
            264, 280, 64, 28, hwnd, (HMENU)(uintptr_t)IDC_AM_CLOSE,
            GetModuleHandleW(nullptr), nullptr);

        alien_morpher_refresh(hwnd);
        return 0;
    }

    case WM_COMMAND: {
        const WORD id = LOWORD(wp);
        if (id == IDC_AM_MORPHALL) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_AM_LIST,
                                                    LB_GETCURSEL, 0, 0));
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            if (rdram != nullptr && sel >= 0 && size_t(sel) < std::size(kAlienTypes)) {
                const uint8_t new_id = kAlienTypes[sel].id;
                int morphed = 0;
                for (int i = 0; i < kAlienInstanceCount; ++i) {
                    const uint32_t spec_virt = kAlienInstancesBase
                                              + uint32_t(i) * kAlienInstanceSize
                                              + 0x1A;
                    if (read_n64_u8(rdram, spec_virt) >= kAlienAliveMinSpec) {
                        write_n64_u8(rdram, spec_virt, new_id);
                        morphed++;
                    }
                }
                std::fprintf(stderr,
                    "[cheats] alien morpher -> mass replace to 0x%02X (%s): "
                    "%d slots rewritten\n",
                    unsigned(new_id), kAlienTypes[sel].name, morphed);
                alien_morpher_refresh(hwnd);
            } else {
                std::fprintf(stderr,
                    "[cheats] alien morpher: Morph All skipped (rdram=%p, sel=%d)\n",
                    (void*)rdram, sel);
            }
            return 0;
        }
        if (id == IDC_AM_SPAWN) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_AM_LIST,
                                                    LB_GETCURSEL, 0, 0));
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            if (rdram == nullptr || sel < 0 || size_t(sel) >= std::size(kAlienTypes)) {
                return 0;
            }
            const uint8_t new_id = kAlienTypes[sel].id;

            // Read the count from the edit field. Clamp to [1, 254] —
            // 254 is the alien table size, but BH's slot allocator
            // will return -1 well before then if existing aliens fill
            // the table.
            BOOL ok = FALSE;
            int count = int(GetDlgItemInt(hwnd, IDC_AM_COUNT, &ok, FALSE));
            if (!ok || count < 1) count = 1;
            if (count > 254) count = 254;

            // Read player position once — all spawns this batch get
            // placed around it. f32 cache is authoritative; s16 raw
            // is derived from it.
            const uint32_t entity_virt = read_n64_u32(rdram, kCurrentEntityPtr);
            float fx = 0.0f, fy = 0.0f, fz = 0.0f;
            bool have_pos = ram_ptr_valid(entity_virt);
            if (have_pos) {
                fx = read_n64_f32(rdram, entity_virt + kEntityOffsetCacheX);
                fy = read_n64_f32(rdram, entity_virt + kEntityOffsetCacheY);
                fz = read_n64_f32(rdram, entity_virt + kEntityOffsetCacheZ);
            }

            // For count == 1 spawn exactly at player. For count > 1
            // spread in a ring at radius 192 world units so aliens
            // don't all stack on the same point (BH's collision
            // resolution would push them apart anyway, but it's
            // cleaner if we space them).
            constexpr float kRingRadius = 192.0f;

            int spawned = 0, failed = 0;
            for (int i = 0; i < count; ++i) {
                const int slot = direct_spawn_alien(rdram, new_id);
                if (slot < 0) {
                    failed++;
                    break;  // table full — stop trying
                }
                if (have_pos) {
                    float ox = 0.0f, oz = 0.0f;
                    if (count > 1) {
                        const float angle = float(i) * (2.0f * 3.14159265f / float(count));
                        ox = std::cos(angle) * kRingRadius;
                        oz = std::sin(angle) * kRingRadius;
                    }
                    const float spawn_x = fx + ox;
                    const float spawn_z = fz + oz;
                    const uint32_t alien_virt = kAlienInstancesBase
                                              + uint32_t(slot) * kAlienInstanceSize;
                    write_n64_f32(rdram, alien_virt + 0x4C, spawn_x);
                    write_n64_f32(rdram, alien_virt + 0x50, fy);
                    write_n64_f32(rdram, alien_virt + 0x54, spawn_z);
                    write_n64_s16(rdram, alien_virt + 0x00, int16_t(spawn_x));
                    write_n64_s16(rdram, alien_virt + 0x02, int16_t(fy));
                    write_n64_s16(rdram, alien_virt + 0x04, int16_t(spawn_z));
                }
                spawned++;
            }

            std::fprintf(stderr,
                "[cheats] alien spawner -> 0x%02X (%s) x %d requested: "
                "%d spawned, %d failed (slot table %s)\n",
                unsigned(new_id), kAlienTypes[sel].name, count,
                spawned, failed,
                failed > 0 ? "filled mid-batch" : "ok");

            alien_morpher_refresh(hwnd);
            return 0;
        }
        if (id == IDC_AM_REFRESH) {
            alien_morpher_refresh(hwnd);
            return 0;
        }
        if (id == IDC_AM_CLOSE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void open_alien_morpher_impl() {
    HWND hwnd = g_alien_morpher_hwnd.load(std::memory_order_acquire);
    if (hwnd == nullptr) {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = alien_morpher_wnd_proc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BHAlienMorpher";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);

        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            L"BHAlienMorpher",
            L"BH — Alien Morpher (mass replace)",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            200, 200, 416, 360,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        if (hwnd == nullptr) {
            std::fprintf(stderr,
                "[cheats] alien morpher: CreateWindowEx failed (err=%lu)\n",
                GetLastError());
            return;
        }
        g_alien_morpher_hwnd.store(hwnd, std::memory_order_release);
    }
    alien_morpher_refresh(hwnd);
    ShowWindow(hwnd, SW_SHOWNA);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(hwnd);
}

void open_alien_morpher() {
    open_alien_morpher_impl();
}

// ===========================================================================
// Bookmarks Manager sub-dialog. 10 slots, each shows label + level +
// timestamp. Buttons: Save Current, Teleport, Clear, Close.
// Disk persistence is auto on every change (see bookmarks_save_to_disk).
// ===========================================================================

constexpr UINT IDC_BM_LIST     = 3001;  // listbox
constexpr UINT IDC_BM_DETAIL   = 3002;  // STATIC showing slot details
constexpr UINT IDC_BM_SAVE     = 3003;  // "Save Current to Slot"
constexpr UINT IDC_BM_LOAD     = 3004;  // "Teleport to Slot"
constexpr UINT IDC_BM_CLEAR    = 3005;  // "Clear Slot"
constexpr UINT IDC_BM_CLOSE    = 3006;
constexpr UINT IDC_BM_REFRESH  = 3007;  // "Refresh from disk"

std::atomic<HWND> g_bm_hwnd{nullptr};

// Refresh both the listbox (10 lines) and the detail STATIC for the
// currently-selected slot. Reads slot data under the mutex.
void bookmark_manager_refresh(HWND hwnd) {
    bookmarks_load_from_disk_if_needed();
    std::array<Bookmark, kBookmarkSlotCount> snap;
    {
        std::lock_guard<std::mutex> lk(g_bookmark_mutex);
        snap = g_bookmarks;
    }

    HWND list = GetDlgItem(hwnd, IDC_BM_LIST);
    if (list != nullptr) {
        const int prev_sel = int(SendMessageW(list, LB_GETCURSEL, 0, 0));
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < kBookmarkSlotCount; ++i) {
            char row[96];
            if (snap[i].valid) {
                std::snprintf(row, sizeof(row),
                    "Slot %d - %s [%s]",
                    i, snap[i].label, level_name(snap[i].level));
            } else {
                std::snprintf(row, sizeof(row), "Slot %d - (empty)", i);
            }
            wchar_t row_w[128];
            MultiByteToWideChar(CP_UTF8, 0, row, -1, row_w, 128);
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)row_w);
        }
        const int sel = (prev_sel >= 0 && prev_sel < kBookmarkSlotCount)
                        ? prev_sel : 0;
        SendMessageW(list, LB_SETCURSEL, sel, 0);
    }

    const int sel = int(SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (sel >= 0 && sel < kBookmarkSlotCount) {
        const Bookmark& b = snap[sel];
        char detail[512];
        if (b.valid) {
            std::snprintf(detail, sizeof(detail),
                "Slot %d  -  %s\r\n"
                "Saved:    %s\r\n"
                "Level:    %u (%s)\r\n"
                "Position: (%.1f, %.1f, %.1f)\r\n"
                "Yaw:      0x%04X    Vehicle: spec %u\r\n"
                "HP: %d    Fuel: %d    Items: 0x%04X\r\n"
                "Weapons: %u/%u/%u/%u/%u/%u/%u/%u\r\n"
                "Ammo:    %d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d",
                sel, b.label, b.timestamp,
                b.level, level_name(b.level),
                double(b.fx), double(b.fy), double(b.fz),
                unsigned(uint16_t(b.yaw)),
                unsigned(b.vehicle_spec),
                int(b.hp), int(b.fuel),
                unsigned(b.items_bitmask),
                unsigned(b.weapon_slots[0]), unsigned(b.weapon_slots[1]),
                unsigned(b.weapon_slots[2]), unsigned(b.weapon_slots[3]),
                unsigned(b.weapon_slots[4]), unsigned(b.weapon_slots[5]),
                unsigned(b.weapon_slots[6]), unsigned(b.weapon_slots[7]),
                int(b.ammo[0]),  int(b.ammo[1]),  int(b.ammo[2]),
                int(b.ammo[3]),  int(b.ammo[4]),  int(b.ammo[5]),
                int(b.ammo[6]),  int(b.ammo[7]),  int(b.ammo[8]),
                int(b.ammo[9]),  int(b.ammo[10]), int(b.ammo[11]),
                int(b.ammo[12]), int(b.ammo[13]), int(b.ammo[14]),
                int(b.ammo[15]), int(b.ammo[16]), int(b.ammo[17]));
        } else {
            std::snprintf(detail, sizeof(detail),
                "Slot %d is empty.\r\n\r\n"
                "Click 'Save Current to Slot' to capture the player's "
                "current state into this slot.",
                sel);
        }
        SetDlgItemTextA(hwnd, IDC_BM_DETAIL, detail);
    }
}

LRESULT CALLBACK bookmark_manager_wnd_proc(HWND hwnd, UINT msg,
                                           WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT gui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
            LBS_HASSTRINGS,
            8, 8, 240, 220, hwnd, (HMENU)(uintptr_t)IDC_BM_LIST,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(list, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND detail = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            256, 8, 296, 220, hwnd, (HMENU)(uintptr_t)IDC_BM_DETAIL,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(detail, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND b_save = CreateWindowExW(0, L"BUTTON", L"Save Current to Slot",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            8, 240, 140, 28, hwnd, (HMENU)(uintptr_t)IDC_BM_SAVE,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b_save, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND b_load = CreateWindowExW(0, L"BUTTON", L"Teleport to Slot",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            156, 240, 140, 28, hwnd, (HMENU)(uintptr_t)IDC_BM_LOAD,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b_load, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND b_clear = CreateWindowExW(0, L"BUTTON", L"Clear Slot",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            304, 240, 100, 28, hwnd, (HMENU)(uintptr_t)IDC_BM_CLEAR,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b_clear, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND b_refresh = CreateWindowExW(0, L"BUTTON", L"Reload from disk",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            412, 240, 60, 28, hwnd, (HMENU)(uintptr_t)IDC_BM_REFRESH,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b_refresh, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        HWND b_close = CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            480, 240, 72, 28, hwnd, (HMENU)(uintptr_t)IDC_BM_CLOSE,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b_close, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        return 0;
    }

    case WM_COMMAND: {
        const WORD id   = LOWORD(wp);
        const WORD code = HIWORD(wp);
        if (id == IDC_BM_LIST && code == LBN_SELCHANGE) {
            bookmark_manager_refresh(hwnd);
            return 0;
        }
        if (id == IDC_BM_SAVE) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_BM_LIST,
                                                    LB_GETCURSEL, 0, 0));
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            if (sel >= 0 && sel < kBookmarkSlotCount) {
                bookmark_capture_slot(rdram, sel, nullptr);
                bookmark_manager_refresh(hwnd);
            }
            return 0;
        }
        if (id == IDC_BM_LOAD) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_BM_LIST,
                                                    LB_GETCURSEL, 0, 0));
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            if (sel >= 0 && sel < kBookmarkSlotCount) {
                bookmark_restore_slot(rdram, sel);
            }
            return 0;
        }
        if (id == IDC_BM_CLEAR) {
            const int sel = int(SendDlgItemMessageW(hwnd, IDC_BM_LIST,
                                                    LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < kBookmarkSlotCount) {
                bookmark_clear_slot(sel);
                bookmark_manager_refresh(hwnd);
            }
            return 0;
        }
        if (id == IDC_BM_REFRESH) {
            // Force a fresh re-read from disk (in case the user edited
            // the JSON in another editor).
            g_bookmarks_loaded.store(false, std::memory_order_release);
            bookmark_manager_refresh(hwnd);
            return 0;
        }
        if (id == IDC_BM_CLOSE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void open_bookmark_manager_impl() {
    HWND hwnd = g_bm_hwnd.load(std::memory_order_acquire);
    if (hwnd == nullptr) {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = bookmark_manager_wnd_proc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BHBookmarkManager";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);

        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            L"BHBookmarkManager",
            L"BH \xE2\x80\x94 Bookmarks Manager",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            200, 200, 580, 304,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        if (hwnd == nullptr) {
            std::fprintf(stderr,
                "[cheats] bookmark manager: CreateWindowEx failed (err=%lu)\n",
                GetLastError());
            return;
        }
        g_bm_hwnd.store(hwnd, std::memory_order_release);
    }
    bookmark_manager_refresh(hwnd);
    ShowWindow(hwnd, SW_SHOWNA);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(hwnd);
}

void open_bookmark_manager() {
    open_bookmark_manager_impl();
}

// ===========================================================================
// Weapon Editor sub-dialog. 8 player weapon slots, each with a dropdown
// of weapon types. Ammo column shown alongside. "Read" pulls current
// state from RAM; "Apply" writes the dropdown selections + ammo back.
// Vehicle weapon editing isn't here (vehicle weapons live in per-spec
// data, not the player slot table) — noted in the dialog.
// ===========================================================================

constexpr UINT IDC_WE_HEADER     = 4001;
constexpr UINT IDC_WE_NOTE       = 4002;
constexpr UINT IDC_WE_READ       = 4003;
constexpr UINT IDC_WE_APPLY      = 4004;
constexpr UINT IDC_WE_CLOSE      = 4005;
// 8 weapon-slot rows. Each row: a STATIC label "Slot N", a COMBOBOX
// with weapon names, an EDIT for ammo (well — slot ammo is global by
// weapon ID, not per-slot, so the EDIT shows ammo for whichever
// weapon is currently selected in the combo).
constexpr UINT IDC_WE_SLOT_LBL_BASE  = 4100;
constexpr UINT IDC_WE_SLOT_COMBO_BASE = 4108;
constexpr UINT IDC_WE_SLOT_AMMO_BASE  = 4116;
// Items bitmask — 4 checkboxes for the most common bits (rest left
// as a hex edit). Skip checkboxes for now, just edit the bitmask.
constexpr UINT IDC_WE_ITEMS_LBL  = 4150;
constexpr UINT IDC_WE_ITEMS_EDIT = 4151;

std::atomic<HWND> g_we_hwnd{nullptr};

// Push current RAM state into the dialog controls.
void weapon_editor_refresh(HWND hwnd) {
    uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
    if (rdram == nullptr) return;

    for (int slot = 0; slot < 8; ++slot) {
        const uint8_t cur_id = read_n64_u8(rdram,
            kWeaponSlotBase + uint32_t(slot));
        // Find which combo index this ID corresponds to.
        HWND combo = GetDlgItem(hwnd, IDC_WE_SLOT_COMBO_BASE + slot);
        if (combo == nullptr) continue;
        int idx = 0;
        for (int j = 0; j < kWeaponNameCount; ++j) {
            if (kWeaponNames[j].id == cur_id) { idx = j; break; }
        }
        SendMessageW(combo, CB_SETCURSEL, idx, 0);

        // Ammo display rules:
        //   - ID 0 (empty): no ammo, disabled
        //   - 0x0B (vehicle primary redirect): infinite per spec, "(spec)"
        //   - 0x0C (vehicle secondary, unused): "(unused)"
        //   - 0x01..0x0A (player) + 0x0D..0x11 (Alpha 1 mounted):
        //     editable, indexed into the global ammo table.
        // The Alpha 1 weapons (0x0D-0x11) DO consume from the global
        // ammo table (the 'alfa' built-in cheat refills them) — they
        // were incorrectly grouped with the vehicle-redirect IDs in
        // an earlier version of this code.
        HWND ammo_edit = GetDlgItem(hwnd, IDC_WE_SLOT_AMMO_BASE + slot);
        if (ammo_edit) {
            if (cur_id == 0) {
                SetWindowTextA(ammo_edit, "0");
                EnableWindow(ammo_edit, FALSE);
            } else if (cur_id == 0x0B) {
                SetWindowTextA(ammo_edit, "(spec)");
                EnableWindow(ammo_edit, FALSE);
            } else if (cur_id == 0x0C) {
                SetWindowTextA(ammo_edit, "(unused)");
                EnableWindow(ammo_edit, FALSE);
            } else if (cur_id <= 0x11) {
                const int16_t ammo = read_n64_s16(rdram,
                    kAmmoSlotBase + uint32_t(cur_id) * 2u);
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", int(ammo));
                SetWindowTextA(ammo_edit, buf);
                EnableWindow(ammo_edit, TRUE);
            } else {
                // Out of known range
                SetWindowTextA(ammo_edit, "?");
                EnableWindow(ammo_edit, FALSE);
            }
        }
    }

    const uint16_t items = read_n64_u16(rdram, kItemsBitmask);
    char itembuf[16];
    std::snprintf(itembuf, sizeof(itembuf), "0x%04X", unsigned(items));
    SetDlgItemTextA(hwnd, IDC_WE_ITEMS_EDIT, itembuf);
}

// Pull dialog state and write back to RAM.
void weapon_editor_apply(HWND hwnd) {
    uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
    if (rdram == nullptr) {
        std::fprintf(stderr,
            "[cheats] weapon editor: apply skipped (game not running)\n");
        return;
    }
    int weapons_changed = 0, ammo_changed = 0;
    for (int slot = 0; slot < 8; ++slot) {
        HWND combo = GetDlgItem(hwnd, IDC_WE_SLOT_COMBO_BASE + slot);
        if (combo == nullptr) continue;
        const int idx = int(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        if (idx < 0 || idx >= kWeaponNameCount) continue;
        const uint8_t new_id = kWeaponNames[idx].id;
        const uint8_t cur_id = read_n64_u8(rdram,
            kWeaponSlotBase + uint32_t(slot));
        if (new_id != cur_id) {
            write_n64_u8(rdram, kWeaponSlotBase + uint32_t(slot), new_id);
            ++weapons_changed;
        }
        // Ammo edit — meaningful for player weapons (0x01..0x0A) AND
        // Alpha 1 mounted weapons (0x0D..0x11) which both consume
        // from the global table. Skip empty (0) and the redirect/
        // unused slots 0x0B and 0x0C.
        const bool has_global_ammo =
            (new_id >= 0x01 && new_id <= 0x0A) ||
            (new_id >= 0x0D && new_id <= 0x11);
        if (has_global_ammo) {
            HWND ammo_edit = GetDlgItem(hwnd, IDC_WE_SLOT_AMMO_BASE + slot);
            char buf[16] = {0};
            if (ammo_edit && GetWindowTextA(ammo_edit, buf, 16) > 0) {
                const int v = std::atoi(buf);
                const int16_t new_ammo = int16_t(v);
                const int16_t cur_ammo = read_n64_s16(rdram,
                    kAmmoSlotBase + uint32_t(new_id) * 2u);
                if (new_ammo != cur_ammo) {
                    write_n64_s16(rdram,
                        kAmmoSlotBase + uint32_t(new_id) * 2u, new_ammo);
                    ++ammo_changed;
                }
            }
        }
    }
    // Items bitmask — accept 0xHHHH or plain decimal.
    char itembuf[32] = {0};
    if (GetDlgItemTextA(hwnd, IDC_WE_ITEMS_EDIT, itembuf, 32) > 0) {
        const long v = std::strtol(itembuf, nullptr,
                                   (itembuf[0] == '0' &&
                                    (itembuf[1] == 'x' || itembuf[1] == 'X'))
                                   ? 16 : 0);
        const uint16_t new_items = uint16_t(v);
        write_n64_u16(rdram, kItemsBitmask, new_items);
    }
    std::fprintf(stderr,
        "[cheats] weapon editor: applied — %d weapon slot(s) changed, "
        "%d ammo count(s) changed, items bitmask set.\n",
        weapons_changed, ammo_changed);
    weapon_editor_refresh(hwnd);
}

LRESULT CALLBACK weapon_editor_wnd_proc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT gui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND header = CreateWindowExW(0, L"STATIC",
            L"Player weapon slots (8). Each slot holds one weapon "
            L"type + its ammo. Pick a weapon, set ammo, click Apply.",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            8, 8, 480, 32, hwnd, (HMENU)(uintptr_t)IDC_WE_HEADER,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(header, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        // 8 rows of (label + combo + ammo edit).
        for (int slot = 0; slot < 8; ++slot) {
            const int y = 48 + slot * 28;

            wchar_t lbl_w[24];
            swprintf(lbl_w, 24, L"Slot %d", slot);
            HWND lbl = CreateWindowExW(0, L"STATIC", lbl_w,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                8, y + 4, 50, 18, hwnd,
                (HMENU)(uintptr_t)(IDC_WE_SLOT_LBL_BASE + slot),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(lbl, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

            HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                64, y, 220, 200, hwnd,
                (HMENU)(uintptr_t)(IDC_WE_SLOT_COMBO_BASE + slot),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(combo, WM_SETFONT,
                         (WPARAM)gui_font, MAKELPARAM(TRUE, 0));
            for (int j = 0; j < kWeaponNameCount; ++j) {
                wchar_t entry_w[64];
                char entry_a[64];
                std::snprintf(entry_a, sizeof(entry_a),
                    "%2u — %s", unsigned(kWeaponNames[j].id),
                    kWeaponNames[j].name);
                MultiByteToWideChar(CP_UTF8, 0, entry_a, -1, entry_w, 64);
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)entry_w);
            }

            HWND ammo_lbl = CreateWindowExW(0, L"STATIC", L"Ammo:",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                292, y + 4, 38, 18, hwnd, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ammo_lbl, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));
            // No ES_NUMBER — we need to display "(vehicle)" for
            // weapon IDs whose ammo lives elsewhere (per-vehicle).
            // Apply path parses with atoi, which returns 0 for that
            // string and we skip writes for non-player weapons anyway.
            HWND ammo_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                WS_CHILD | WS_VISIBLE | ES_LEFT,
                332, y, 70, 22, hwnd,
                (HMENU)(uintptr_t)(IDC_WE_SLOT_AMMO_BASE + slot),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ammo_edit, WM_SETFONT,
                         (WPARAM)gui_font, MAKELPARAM(TRUE, 0));
        }

        // Items row.
        const int items_y = 48 + 8 * 28 + 8;
        HWND items_lbl = CreateWindowExW(0, L"STATIC",
            L"Items bitmask (hex 0xHHHH or decimal):",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            8, items_y + 4, 230, 18, hwnd,
            (HMENU)(uintptr_t)IDC_WE_ITEMS_LBL,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(items_lbl, WM_SETFONT,
                     (WPARAM)gui_font, MAKELPARAM(TRUE, 0));
        HWND items_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0x0000",
            WS_CHILD | WS_VISIBLE | ES_LEFT,
            244, items_y, 100, 22, hwnd,
            (HMENU)(uintptr_t)IDC_WE_ITEMS_EDIT,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(items_edit, WM_SETFONT,
                     (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        // Footer note.
        HWND note = CreateWindowExW(0, L"STATIC",
            L"Player weapons (0x00-0x0A) — pickup-able, editable ammo.\r\n"
            L"0x0B = vehicle's primary weapon redirect (resolves to "
            L"tank cannon / chaingun / etc. based on vehicle spec; "
            L"infinite ammo).  0x0C looks unused.\r\n"
            L"0x0D-0x11 = Alpha 1's mounted weapons; use the global "
            L"ammo table but on-foot you can't fire them.",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            8, items_y + 32, 480, 64, hwnd,
            (HMENU)(uintptr_t)IDC_WE_NOTE,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(note, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        // Buttons (positioned below the taller multi-line footer note).
        const int btn_y = items_y + 100;
        HWND b_read = CreateWindowExW(0, L"BUTTON", L"Read from game",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            8, btn_y, 140, 28, hwnd, (HMENU)(uintptr_t)IDC_WE_READ,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b_read, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));
        HWND b_apply = CreateWindowExW(0, L"BUTTON", L"Apply to game",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            156, btn_y, 140, 28, hwnd, (HMENU)(uintptr_t)IDC_WE_APPLY,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b_apply, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));
        HWND b_close = CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            340, btn_y, 100, 28, hwnd, (HMENU)(uintptr_t)IDC_WE_CLOSE,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b_close, WM_SETFONT, (WPARAM)gui_font, MAKELPARAM(TRUE, 0));

        return 0;
    }

    case WM_COMMAND: {
        const WORD id = LOWORD(wp);
        const WORD code = HIWORD(wp);
        if (id == IDC_WE_READ) {
            weapon_editor_refresh(hwnd);
            return 0;
        }
        if (id == IDC_WE_APPLY) {
            weapon_editor_apply(hwnd);
            return 0;
        }
        if (id == IDC_WE_CLOSE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        // When user changes combo selection, auto-refresh the ammo
        // field for that slot to show the ammo count for that weapon.
        if (code == CBN_SELCHANGE &&
            id >= IDC_WE_SLOT_COMBO_BASE &&
            id <  IDC_WE_SLOT_COMBO_BASE + 8) {
            const int slot = id - IDC_WE_SLOT_COMBO_BASE;
            uint8_t* rdram = g_overlay_rdram.load(std::memory_order_acquire);
            if (rdram != nullptr) {
                const int idx = int(SendDlgItemMessageW(
                    hwnd, id, CB_GETCURSEL, 0, 0));
                if (idx >= 0 && idx < kWeaponNameCount) {
                    const uint8_t weap_id = kWeaponNames[idx].id;
                    HWND ammo_edit = GetDlgItem(hwnd,
                        IDC_WE_SLOT_AMMO_BASE + slot);
                    if (weap_id == 0) {
                        SetWindowTextA(ammo_edit, "0");
                        EnableWindow(ammo_edit, FALSE);
                    } else if (weap_id == 0x0B) {
                        SetWindowTextA(ammo_edit, "(spec)");
                        EnableWindow(ammo_edit, FALSE);
                    } else if (weap_id == 0x0C) {
                        SetWindowTextA(ammo_edit, "(unused)");
                        EnableWindow(ammo_edit, FALSE);
                    } else if (weap_id <= 0x11) {
                        const int16_t ammo = read_n64_s16(rdram,
                            kAmmoSlotBase + uint32_t(weap_id) * 2u);
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "%d", int(ammo));
                        SetWindowTextA(ammo_edit, buf);
                        EnableWindow(ammo_edit, TRUE);
                    } else {
                        SetWindowTextA(ammo_edit, "?");
                        EnableWindow(ammo_edit, FALSE);
                    }
                }
            }
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void open_weapon_editor_impl() {
    HWND hwnd = g_we_hwnd.load(std::memory_order_acquire);
    if (hwnd == nullptr) {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = weapon_editor_wnd_proc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BHWeaponEditor";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);

        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            L"BHWeaponEditor",
            L"BH \xE2\x80\x94 Weapon Editor",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            200, 200, 472, 452,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        if (hwnd == nullptr) {
            std::fprintf(stderr,
                "[cheats] weapon editor: CreateWindowEx failed (err=%lu)\n",
                GetLastError());
            return;
        }
        g_we_hwnd.store(hwnd, std::memory_order_release);
    }
    weapon_editor_refresh(hwnd);
    ShowWindow(hwnd, SW_SHOWNA);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(hwnd);
}

void open_weapon_editor() {
    open_weapon_editor_impl();
}

void overlay_thread_main() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc   = overlay_wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BHCheatOverlay";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"BHCheatOverlay",
        L"Body Harvest — Cheats (F1 to toggle)",
        // WS_THICKFRAME makes the window resizable by dragging its
        // border. layout_overlay() reflows the child controls on
        // every WM_SIZE so descriptions and the cheats list grow
        // to fit when the user enlarges the window.
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        100, 100, 480, 480,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (hwnd == nullptr) {
        std::fprintf(stderr, "[cheats] overlay CreateWindowEx failed (err=%lu)\n",
                     GetLastError());
        return;
    }

    g_overlay_hwnd.store(hwnd, std::memory_order_release);
    g_overlay_thread_id.store(GetCurrentThreadId(), std::memory_order_release);

    std::fprintf(stderr, "[cheats] overlay window ready — press F1 in-game to toggle\n");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // IsDialogMessage gives us Tab cycling + Enter-on-default-button
        // for free on the listbox + buttons.
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

} // namespace

namespace bh::cheats {

// Start the overlay thread once at app boot. Idempotent (later calls
// are no-ops). Safe to call before rdram is captured; the overlay
// reads g_overlay_rdram lazily at Activate time.
void start_overlay_thread() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) {
        return;
    }
    std::thread(overlay_thread_main).detach();
}

// Called from stub_poll_input each tick — re-applies the shield-
// disable patches for whatever level is currently loaded. BH restores
// the shield wall source table from ROM on building entry/exit AND
// on level transitions; without this tick the disable cheat would
// silently un-stick the moment the player went indoors and came back
// out. Re-application is idempotent (writes the same disabled state
// each frame) and cheap (~40 s16 writes per call), so we just do it
// every tick instead of trying to detect the reload event.
//
// Once-active, stays active for the session — there's no toggle-off
// because we don't snapshot the original wall data. A future
// enhancement could add a "Re-enable shields" companion cheat that
// reloads the per-level original wall table from a ROM-image copy.
void apply_disable_shields_tick(uint8_t* rdram) {
    if (!g_disable_shields_active.load(std::memory_order_acquire)) return;
    if (rdram == nullptr) return;
    const uint32_t lvl = read_n64_u32(rdram, kCurrentLevelAddr);
    if (lvl < 1 || lvl > 5) return; // title screen / menu — wait
    apply_shield_disable_for_level(rdram, lvl);
}

// Per-tick save-state I/O. Runs on the input thread, which we treat
// as a frame boundary (BH's game tick is nominally between input
// polls). Single-shot — `exchange(false)` clears the flag so the
// operation runs exactly once per cheat fire.
//
// File format v2 (binary, little-endian) — see comments at the
// constants for known limitations and rationale for each field:
//   u32 magic         = 0x42485353 ('BHSS')
//   u32 version       = 2
//   u32 level         = currentLevel (1-5, or 0 if on title screen)
//   u32 gameplay_mode = D_80052ADC (0=menu, 1-9 in-game states, etc.)
//   u32 reserved[4]   = 0
//   u8  rdram[kRdramSize]   -- raw 8 MB N64 RAM
//
// Header is 32 bytes. Total file size: 32 + 8388608 = 8388640 bytes.
//
// Load rejects mismatched (level, gameplay_mode) — that prevents the
// menu↔gameplay crash class. Same-context loads still risk the
// running-while-saved tear (see comments at the flag declarations).
void apply_save_state_tick(uint8_t* rdram) {
    if (rdram == nullptr) return;

    if (g_save_state_pending.exchange(false, std::memory_order_acq_rel)) {
        FILE* f = std::fopen("bh_savestate.bin", "wb");
        if (f == nullptr) {
            std::fprintf(stderr,
                "[cheats] Save State: fopen failed (write mode)\n");
            return;
        }
        const uint32_t lvl  = read_n64_u32(rdram, kCurrentLevelAddr);
        const uint32_t mode = read_n64_u32(rdram, kGameplayModeAddr);
        const uint32_t hdr[8] = {
            kSaveStateMagic, kSaveStateVersion,
            lvl, mode,
            0u, 0u, 0u, 0u
        };
        std::fwrite(hdr,   sizeof(hdr),       1, f);
        std::fwrite(rdram, 1, kRdramSize,        f);
        std::fclose(f);
        std::fprintf(stderr,
            "[cheats] Save State: wrote bh_savestate.bin "
            "(level=%u (%s), gameplay_mode=%u, %u bytes RDRAM)\n",
            lvl, level_name(lvl), mode, kRdramSize);
    }

    if (g_load_state_pending.exchange(false, std::memory_order_acq_rel)) {
        FILE* f = std::fopen("bh_savestate.bin", "rb");
        if (f == nullptr) {
            std::fprintf(stderr,
                "[cheats] Load State: bh_savestate.bin not found "
                "(save one first)\n");
            return;
        }
        uint32_t hdr[8] = {0};
        if (std::fread(hdr, sizeof(hdr), 1, f) != 1) {
            std::fclose(f);
            std::fprintf(stderr,
                "[cheats] Load State: header read failed (truncated file?)\n");
            return;
        }
        if (hdr[0] != kSaveStateMagic) {
            std::fclose(f);
            std::fprintf(stderr,
                "[cheats] Load State: bad magic 0x%08X (expected 0x%08X) "
                "— file is not a BHSS save state\n",
                hdr[0], kSaveStateMagic);
            return;
        }
        if (hdr[1] != kSaveStateVersion) {
            std::fclose(f);
            std::fprintf(stderr,
                "[cheats] Load State: version %u not supported "
                "(expected %u). v1 files lack context info and are "
                "no longer loadable — re-save with this build.\n",
                hdr[1], kSaveStateVersion);
            return;
        }
        const uint32_t cur_lvl  = read_n64_u32(rdram, kCurrentLevelAddr);
        const uint32_t cur_mode = read_n64_u32(rdram, kGameplayModeAddr);
        if (hdr[2] != cur_lvl) {
            std::fclose(f);
            std::fprintf(stderr,
                "[cheats] Load State: REJECTED — file is for level %u (%s), "
                "you're in level %u (%s). Loading would crash because BH's "
                "level overlay code differs (function pointers in the saved "
                "RAM target the wrong code). Switch to level %u first, then "
                "retry.\n",
                hdr[2], level_name(hdr[2]),
                cur_lvl, level_name(cur_lvl),
                hdr[2]);
            return;
        }
        if (hdr[3] != cur_mode) {
            std::fclose(f);
            std::fprintf(stderr,
                "[cheats] Load State: REJECTED — file's gameplay_mode=%u "
                "but current gameplay_mode=%u. Loading would mismatch BH's "
                "active state machine (menu vs in-game, cutscene vs play, "
                "etc.) and crash. Get to the same gameplay context as when "
                "you saved, then retry.\n",
                hdr[3], cur_mode);
            return;
        }
        const size_t got =
            std::fread(rdram, 1, kRdramSize, f);
        std::fclose(f);
        if (got != kRdramSize) {
            std::fprintf(stderr,
                "[cheats] Load State: only read %zu / %u bytes — file "
                "may be truncated. RAM partially overwritten; expect "
                "instability.\n",
                got, kRdramSize);
            return;
        }
        std::fprintf(stderr,
            "[cheats] Load State: restored %u bytes of RDRAM from "
            "bh_savestate.bin (level=%u, gameplay_mode=%u). NOTE: if "
            "you were moving when you saved, expect a possible crash "
            "from torn-snapshot inconsistency.\n",
            kRdramSize, hdr[2], hdr[3]);
    }
}

// Per-tick water-Y override. Cheap (~1 write); only runs if active.
// Re-application defeats per-level scripts that try to change water Y
// (Java boss arena drops water on boss spawn, etc.) — without this
// tick the user's slider value would get reset every state change.
void apply_water_override_tick(uint8_t* rdram) {
    if (!g_water_override_active.load(std::memory_order_acquire)) return;
    if (rdram == nullptr) return;
    const int32_t target = g_water_target_y.load(std::memory_order_acquire);
    write_n64_u32(rdram, kWaterLevelAddr, uint32_t(target));
}

// Per-tick far-plane override. Snapshots BH's original far-plane
// value on first activation (so Release can restore it), then writes
// `original * scale` to D_801411A4 each input poll. The address holds
// an f32; we use plain memcpy via the rdram pointer since aligned
// 32-bit accesses don't need the recomp's halfword byteswap.
void apply_far_plane_override_tick(uint8_t* rdram) {
    if (!g_far_plane_override_active.load(std::memory_order_acquire)) return;
    if (rdram == nullptr) return;

    // First time: capture the BH-side current far plane so Release
    // can write it back.
    if (!g_far_plane_snapshot_valid.load(std::memory_order_acquire)) {
        float orig;
        std::memcpy(&orig, rdram + (kFarPlaneAddr - kVirtBase), sizeof(orig));
        g_far_plane_original.store(orig, std::memory_order_release);
        g_far_plane_snapshot_valid.store(true, std::memory_order_release);
        std::fprintf(stderr,
            "[cheats] Far plane snapshot: original D_801411A4 = %.1f "
            "(will be restored on Release).\n", double(orig));
    }

    const float orig  = g_far_plane_original.load(std::memory_order_acquire);
    const float scale = g_far_plane_scale.load(std::memory_order_acquire);
    const float target = orig * scale;
    std::memcpy(rdram + (kFarPlaneAddr - kVirtBase), &target, sizeof(target));
}

// Per-tick render-cull bypass. Writes BOTH bypass conditions each
// input poll so the master cull function takes its render-everything
// early return:
//   - D_8014FD2A (FOV) = 0x8000 — primary "render everything" sentinel
//   - D_80157590 (camera status) = 1 — secondary "!= 0" bypass that
//     stomps the race against BH's per-frame FOV recompute. The "1"
//     is a value none of D_80157590's switch/== branches care about
//     in BF9C0/7F220/F6A50; only the `!= 0` checks fire, which is
//     exactly what we want (render bypass on, normal camera state
//     elsewhere). If you notice weird camera behavior or alternate
//     rendering paths kicking in unexpectedly, that's the side effect
//     to look for.
// Logs first 3 tick fires with the current values it sees so we can
// confirm wiring + check the race condition during testing.
void apply_render_cull_bypass_tick(uint8_t* rdram) {
    if (!g_render_cull_bypass_active.load(std::memory_order_acquire)) return;
    if (rdram == nullptr) return;

    // Diagnostic: print the first 3 tick fires so we can see whether
    // the bypass is taking effect and what BH had set the values to.
    const int n = g_cull_tick_log_count.fetch_add(1, std::memory_order_acq_rel);
    if (n < 3) {
        const uint16_t fov_before    = read_n64_u16(rdram, kCullFovAddr);
        const uint32_t status_before = read_n64_u32(rdram, kCullStatusAddr);
        std::fprintf(stderr,
            "[cheats] cull bypass tick #%d: pre-write D_8014FD2A=0x%04X "
            "D_80157590=0x%08X — overriding to FOV=0x8000, status=1\n",
            n, unsigned(fov_before), unsigned(status_before));
    }

    write_n64_u16(rdram, kCullFovAddr, kCullFovBypass);
    write_n64_u32(rdram, kCullStatusAddr, 1u);
}

// Called from stub_poll_input on the input thread when noclip mode
// is active. Overwrites the player's position with stick + keyboard
// input each tick, with no boundary clamping. Velocity is zeroed
// each frame so the game's physics can't carry inertia, and the
// AIRBORNE flag is forced so gravity / collision don't snap us
// back to the ground.
//
// Called unconditionally — early-outs internally if noclip is off.
// (read_n64_f32 is now defined in the anon-namespace helpers up top
// so the spawner and noclip can share it.)
void apply_noclip_movement(uint8_t* rdram, float stick_x, float stick_y) {
    if (!g_noclip_active.load(std::memory_order_acquire)) return;
    if (rdram == nullptr) return;

    const uint32_t entity_virt = read_n64_u32(rdram, kCurrentEntityPtr);
    constexpr uint32_t kRamHi = 0x80800000u;
    if (entity_virt < kVirtBase || entity_virt >= kRamHi) {
        // Player has no controlled entity right now (cutscene,
        // level transition) — skip this frame.
        return;
    }

    // Horizontal speed matches zwander's: stick (normalized -1..1)
    // scaled to roughly stick*4 in N64-stick units (-80..80), so
    // about 320 world units per frame at full stick.
    constexpr float kHSpeed = 320.0f;
    constexpr float kVSpeed = 200.0f;

    // Read the AUTHORITATIVE f32 cached position. The s16 fields
    // are derived (truncated) from these by BH's setX/setY/setZ
    // helpers, so reading the f32 cache gives us the true current
    // position even after sub-unit drift.
    const float fx = read_n64_f32(rdram, entity_virt + kEntityOffsetCacheX);
    const float fy = read_n64_f32(rdram, entity_virt + kEntityOffsetCacheY);
    const float fz = read_n64_f32(rdram, entity_virt + kEntityOffsetCacheZ);

    // Horizontal from stick — rotated by the player's facing
    // direction so stick-forward = "the way you're looking", not
    // always world-north. Without this, looking around the world
    // de-syncs from movement and makes noclip exploration awkward.
    //
    // BH's world axes: north = -Z, east = +X (deduced from zwander
    // which writes `x += stick_x*4; z -= stick_y*4`). Direction angle
    // is s16 with full 360° spanning [-32768, 32767], so 1 unit ≈
    // 2π / 65536 radians. Assume angle 0 = facing north (-Z) and
    // increases clockwise (toward east, +X); flip signs in the
    // rotation if observed behavior says otherwise.
    const int16_t dir_s16 = read_n64_s16(rdram, entity_virt + kEntityOffsetDir);
    constexpr float kRadPerUnit = 2.0f * 3.14159265358979323846f / 65536.0f;
    const float theta = float(dir_s16) * kRadPerUnit;
    const float cos_t = std::cos(theta);
    const float sin_t = std::sin(theta);
    // Observed convention (corrected after testing):
    //   theta=0     -> facing +X (east),   forward = (+1,  0)
    //   theta=π/2   -> facing +Z (south),  forward = ( 0, +1)
    //   theta=-0x4000 (zwander's lock) -> -90° = facing north (-Z)
    // -> forward = (  cos θ, sin θ )
    // right = 90° clockwise from forward = ( -sin θ, cos θ )
    const float forward_x =  cos_t;
    const float forward_z =  sin_t;
    const float right_x   = -sin_t;
    const float right_z   =  cos_t;
    const float local_dx  = (stick_y * forward_x + stick_x * right_x) * kHSpeed;
    const float local_dz  = (stick_y * forward_z + stick_x * right_z) * kHSpeed;
    float new_fx = fx + local_dx;
    float new_fz = fz + local_dz;

    // Vertical from keyboard. GetAsyncKeyState's high bit indicates
    // the key is currently down. PageUp = ascend, PageDown = descend.
    float new_fy = fy;
    if ((GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0) new_fy += kVSpeed;
    if ((GetAsyncKeyState(VK_NEXT)  & 0x8000) != 0) new_fy -= kVSpeed;

    // Clamp to s16 range so the truncated-to-s16 sister field
    // doesn't wrap weirdly. BH's playable map is ~±27648 but s16
    // gives us ±32767 of headroom, which is plenty to escape the
    // outer shield wall without overflow.
    auto clamp_to_s16_range = [](float v) -> float {
        if (v < -32768.0f) return -32768.0f;
        if (v >  32767.0f) return  32767.0f;
        return v;
    };
    new_fx = clamp_to_s16_range(new_fx);
    new_fy = clamp_to_s16_range(new_fy);
    new_fz = clamp_to_s16_range(new_fz);

    // Write BOTH halves — f32 cache (camera/physics read these) AND
    // s16 raw (some code paths read these directly). Matches what
    // BH's own setX/setY/setZ helpers do internally.
    write_n64_f32(rdram, entity_virt + kEntityOffsetCacheX, new_fx);
    write_n64_f32(rdram, entity_virt + kEntityOffsetCacheY, new_fy);
    write_n64_f32(rdram, entity_virt + kEntityOffsetCacheZ, new_fz);
    write_n64_s16(rdram, entity_virt + kEntityOffsetPosX, int16_t(new_fx));
    write_n64_s16(rdram, entity_virt + kEntityOffsetPosY, int16_t(new_fy));
    write_n64_s16(rdram, entity_virt + kEntityOffsetPosZ, int16_t(new_fz));

    // Zero out velocity so BH's physics doesn't add inertia.
    write_n64_f32(rdram, entity_virt + kEntityOffsetVelX, 0.0f);
    write_n64_f32(rdram, entity_virt + kEntityOffsetVelY, 0.0f);
    write_n64_f32(rdram, entity_virt + kEntityOffsetVelZ, 0.0f);

    // Force AIRBORNE so gravity / ground collision don't snap us.
    const uint16_t flags = read_n64_u16(rdram, entity_virt + kEntityOffsetFlags);
    write_n64_u16(rdram, entity_virt + kEntityOffsetFlags,
                  flags | kVehicleFlagAirborne);
}

// Called from stub_poll_input on the input thread. Polls F1 with a
// rising-edge detector and posts WM_BH_SHOW/WM_BH_HIDE to the
// overlay thread. Also caches the live rdram pointer so the dialog
// thread can write to RDRAM on Activate.
void poll_overlay_hotkey(uint8_t* rdram) {
    static bool s_f1_prev = false;

    g_overlay_rdram.store(rdram, std::memory_order_release);

    // VK_F1 high bit indicates the key is currently down. We don't
    // need foreground filtering — Win32 GetAsyncKeyState reads
    // physical key state regardless of focused window, which is
    // what we want (overlay toggles even when the game has focus).
    const bool f1_now = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    const bool rising_edge = f1_now && !s_f1_prev;
    s_f1_prev = f1_now;

    if (!rising_edge) return;

    const bool was_visible = g_overlay_visible.exchange(!g_overlay_visible.load(
        std::memory_order_acquire), std::memory_order_acq_rel);
    HWND hwnd = g_overlay_hwnd.load(std::memory_order_acquire);
    if (hwnd == nullptr) {
        std::fprintf(stderr,
            "[cheats] F1 pressed but overlay window not yet created\n");
        return;
    }
    PostMessageW(hwnd, was_visible ? WM_BH_HIDE : WM_BH_SHOW, 0, 0);
}

} // namespace bh::cheats

// Phase 16 — expose the alien spawner so the multiplayer POC
// (bh_net.cpp) can reuse it for the ghost entity instead of the
// vehicle-based ghost we shipped first. Thin wrapper around the
// static direct_spawn_alien() in the anon namespace above.
namespace bh::spawn {
    int alien_simple(uint8_t* rdram, uint8_t alien_type) {
        return direct_spawn_alien(rdram, alien_type);
    }
}

// ===========================================================================
// Weak-symbol override of BH's entity-bbox cull function
// `func_800703B0_7F360` (decompiled in body-harvest-decompilation/.../
// 7F220.c:59). The original returns 1 if the entity at (arg0, arg1) is
// inside a ~15-tile bbox around (D_80149434, D_80149436):
//
//   return arg0 >= cam_x - 0x800 &&    // ~8 tiles left
//          arg1 >= cam_z - 0x900 &&    // ~9 tiles forward
//          arg0 <= cam_x + 0x700 &&    // ~7 tiles right
//          arg1 <= cam_z + 0x700;      // ~7 tiles back
//
// This is the actual entity render-distance cap — the previous bypass
// cheat targeted the frustum-test function (`func_800B93AC_C835C`),
// which we confirmed via diagnostics IS being bypassed but produces
// no visible effect because the bbox cull above runs FIRST and rejects
// distant entities before the frustum test ever sees them.
//
// Approach: define a strong-symbol version with the same name. The
// recompiled version (in bh-recomp/RecompiledFuncs/funcs_48.c:3373) is
// emitted with `RECOMP_FUNC` which expands under clang to
// `extern inline __attribute__((weak,noinline))`. Our strong (default
// linkage) version wins at link time, replacing the original.
//
// Behavior:
//   - If bypass cheat is OFF: replicate the original bbox math exactly
//     (1:1 same gameplay).
//   - If bypass cheat is ON: return 1 unconditionally (always render).
// Logs the first 3 calls per toggle-on so we can confirm the override
// is actually being invoked by BH's render loop.
extern "C" {
void func_800703B0_7F360(uint8_t* rdram, recomp_context* ctx) {
    // a0 = arg0 = entity X (sign-extended s16 in low 16 bits of r4)
    // a1 = arg1 = entity Z
    const int16_t entity_x = (int16_t)(ctx->r4 & 0xFFFF);
    const int16_t entity_z = (int16_t)(ctx->r5 & 0xFFFF);

    if (g_render_cull_bypass_active.load(std::memory_order_acquire)) {
        const int n = g_cull_override_log_count.fetch_add(
            1, std::memory_order_acq_rel);
        if (n < 3) {
            std::fprintf(stderr,
                "[cheats] cull override fire #%d (bypass ON): entity "
                "at (%d, %d) — forcing return 1.\n",
                n, int(entity_x), int(entity_z));
        }
        ctx->r2 = 1;
        return;
    }

    // Bypass OFF — apply BH's original bbox math but with the
    // dimensions scaled by `g_entity_cull_scale` (1.0 = vanilla).
    // Camera-center coords come from D_80149434 (s16 X) and D_80149436
    // (s16 Z). The anon-namespace helpers ARE visible here since both
    // this function and the anon namespace are at file scope in the
    // same translation unit.
    constexpr uint32_t kCamCenterXVirt = 0x80149434;
    constexpr uint32_t kCamCenterZVirt = 0x80149436;
    constexpr uint32_t kVirtBaseLocal  = 0x80000000u;

    // Halfword read with the recomp's XOR-2 byteswap convention.
    auto rd_s16 = [&](uint32_t virt) -> int16_t {
        int16_t v;
        std::memcpy(&v, rdram + ((virt - kVirtBaseLocal) ^ 2u), sizeof(v));
        return v;
    };
    const int16_t cam_x = rd_s16(kCamCenterXVirt);
    const int16_t cam_z = rd_s16(kCamCenterZVirt);

    // Vanilla BH constants: left=0x800, top=0x900, right=0x700,
    // bottom=0x700. Scale each by g_entity_cull_scale, clamped to
    // 32-bit positive so we don't overflow s16 world coords when the
    // slider is cranked. Use int32 throughout the compare so the bbox
    // edges can validly extend outside the s16 world range.
    const float scale_f = g_entity_cull_scale.load(std::memory_order_acquire);
    const float clamped = scale_f < 1.0f ? 1.0f
                        : scale_f > 1.0e6f ? 1.0e6f
                        : scale_f;
    const int32_t left   = int32_t(0x800 * clamped);
    const int32_t top    = int32_t(0x900 * clamped);
    const int32_t right  = int32_t(0x700 * clamped);
    const int32_t bottom = int32_t(0x700 * clamped);

    const bool inside =
        int32_t(entity_x) >= (int32_t(cam_x) - left) &&
        int32_t(entity_z) >= (int32_t(cam_z) - top) &&
        (int32_t(cam_x) + right) >= int32_t(entity_x) &&
        (int32_t(cam_z) + bottom) >= int32_t(entity_z);
    ctx->r2 = inside ? 1u : 0u;
}

// Weak-symbol override of `func_800E95BC_F856C` (terrain LOS
// occlusion test, decomp F7870.c:80). This is the REAL culprit for
// the "vehicles vanish at terrain horizon" symptom: BH's vehicle
// render function ray-marches from camera to each entity, sampling
// terrain heights along the path. If the LOS dips below terrain at
// any sample → return 1 → skip render. For distant entities the long
// ray almost always finds *something* (even a small bump or buildings'
// terrain mark) tall enough to "occlude", killing the render at
// roughly the visible horizon distance.
//
// Our override:
//   - Bypass ON (cheat OR slider > 1x): return 0 (never occluded).
//     Every vehicle renders regardless of LOS.
//   - Bypass OFF: replicate the original ray-march so vanilla
//     occlusion is preserved.
//
// The original function's signature (from decomp F7870.c:80):
//   s32 func(s32 arg0, s32 arg1, s32 arg2);
// where arg0 = entity X, arg1 = terrain_y (output of
// func_800B84D0_C7480 >> 8) + 5, arg2 = entity Z. Camera position is
// read from D_80052B2C (unk0=X, unk4=Y, unk8=Z).
// (Already inside the extern "C" block opened above for the bbox cull.)
void func_800E95BC_F856C(uint8_t* rdram, recomp_context* ctx) {
    // Bypass path: tied to the entity cull controls. When the cheat
    // toggle is on OR the cull-scale slider is greater than vanilla,
    // skip occlusion entirely. Otherwise replicate BH's ray-march.
    const bool bypass_active =
        g_render_cull_bypass_active.load(std::memory_order_acquire) ||
        g_entity_cull_scale.load(std::memory_order_acquire) > 1.001f;
    if (bypass_active) {
        // Log first 3 fires per toggle ON so we can confirm BH's
        // vehicle render function is actually calling THIS override
        // (vs the original recomp version, or something else entirely).
        const int n = g_los_override_log_count.fetch_add(
            1, std::memory_order_acq_rel);
        if (n < 3) {
            std::fprintf(stderr,
                "[cheats] LOS override fire #%d (bypass ON): args=(%d, %d, %d) "
                "— forcing return 0 (not occluded).\n",
                n,
                int(int32_t(ctx->r4)),
                int(int32_t(ctx->r5)),
                int(int32_t(ctx->r6)));
        }
        ctx->r2 = 0; // not occluded — render this entity
        return;
    }

    // Bypass OFF — replicate the ray-march. Faithful port of the C
    // version at body-harvest-decompilation/.../F7870.c:80.
    const int32_t arg0 = int32_t(ctx->r4);  // entity X
    const int32_t arg1 = int32_t(ctx->r5);  // terrain Y baseline
    int32_t       arg2 = int32_t(ctx->r6);  // entity Z

    constexpr uint32_t kCamStruct = 0x80052B2C; // ptr to camera struct
    constexpr uint32_t kVirtBaseLocal = 0x80000000u;

    auto rd_u32 = [&](uint32_t virt) -> uint32_t {
        uint32_t v;
        std::memcpy(&v, rdram + (virt - kVirtBaseLocal), sizeof(v));
        return v;
    };
    auto rd_s16_xor2 = [&](uint32_t virt) -> int16_t {
        int16_t v;
        std::memcpy(&v, rdram + ((virt - kVirtBaseLocal) ^ 2u), sizeof(v));
        return v;
    };
    auto rd_s32 = [&](uint32_t virt) -> int32_t {
        int32_t v;
        std::memcpy(&v, rdram + (virt - kVirtBaseLocal), sizeof(v));
        return v;
    };

    const uint32_t cam_struct_virt = rd_u32(kCamStruct);
    int32_t baseX = int32_t(rd_s16_xor2(cam_struct_virt + 0x00)) << 8;
    int32_t baseY = int32_t(rd_s16_xor2(cam_struct_virt + 0x04)) << 8;
    int32_t baseZ = int32_t(rd_s16_xor2(cam_struct_virt + 0x08)) << 8;

    int32_t deltaX = (arg0 << 8) - baseX;
    int32_t absDeltaX = deltaX < 0 ? -deltaX : deltaX;
    int32_t deltaZ = (arg2 << 8) - baseZ;
    int32_t absDeltaZ = deltaZ < 0 ? -deltaZ : deltaZ;

    int32_t sp30 = 0, sp34 = 0, sp2C = 0;
    if (absDeltaZ < absDeltaX) {
        arg2 = absDeltaX >> 8;
        if (arg2 != 0) {
            sp30 = (((arg1 << 8) - baseY) << 8) / arg2;
            sp34 = (deltaX < 0 ? -0x100 : 0x100) << 8;
            sp2C = (deltaZ << 8) / arg2;
        }
    } else {
        arg2 = absDeltaZ >> 8;
        if (arg2 != 0) {
            sp34 = (deltaX << 8) / arg2;
            sp30 = (((arg1 << 8) - baseY) << 8) / arg2;
            sp2C = (deltaZ < 0 ? -0x100 : 0x100) << 8;
        }
    }

    arg2 >>= 8;
    if (arg2 != 0) {
        arg2--;
        // Call into BH's recompiled func_800B84D0_C7480 (terrain height
        // sampler) for each step. We could re-implement the bilinear
        // interpolation in C, but the simpler path is to call the
        // recomp function via its ctx-based ABI. To keep this self-
        // contained we replicate just enough: read tile heights with
        // bilinear interp, same as the ASM.
        for (;;) {
            baseZ += sp2C;
            baseX += sp34;
            baseY += sp30;

            const int16_t sx = int16_t(baseX >> 8);
            const int16_t sz = int16_t(baseZ >> 8);
            // Match BH's func_800B84D0_C7480 ASM:
            // bilinear over 4 corner tile-heights * 8192 (8-bit frac
            // X * 32 height-scale). Tile coord = (s8)(s_world >> 8).
            // Simpler: use the same arithmetic as the renderer, but
            // approximate by reading just the corner tile's height.
            // For our purposes the exact value matters less than that
            // the rough magnitude tracks the real one.
            int32_t terrain_h = 0;
            const uint32_t tiles_virt = rd_u32(0x8014F8A0);
            if (tiles_virt >= kVirtBaseLocal &&
                tiles_virt < (kVirtBaseLocal + 0x800000u)) {
                const int tx = (int(sx) >> 8) + 128;
                const int tz = (int(sz) >> 8) + 128;
                if (tx >= 0 && tx < 256 && tz >= 0 && tz < 256) {
                    const uint32_t addr =
                        tiles_virt + uint32_t(tz * 256 + tx) * 2u;
                    int16_t tile;
                    std::memcpy(&tile,
                        rdram + ((addr - kVirtBaseLocal) ^ 2u),
                        sizeof(tile));
                    // Match ASM: ((tile & 0x3F) * 256) << 5 then >>8
                    // ≈ height * 32. Inline:
                    terrain_h = int32_t(uint16_t(tile) & 0x3Fu) * 32;
                }
            }
            // Convert to the >> 8 scale BH compares against (its
            // ASM returns terrain_h * 8192 then `>> 8` = terrain_h * 32
            // which is what we computed above).
            if (baseY < terrain_h) {
                ctx->r2 = 1; // occluded
                return;
            }

            if (arg2 == 0) break;
            arg2--;
        }
    }
    ctx->r2 = 0; // not occluded — render
}
} // extern "C"

