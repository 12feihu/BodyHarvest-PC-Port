# Body Harvest weapon reference

The most complete published mapping of weapon IDs to weapon types in
Body Harvest, verified in-game. Compiled during the development of
`bh-recomp-toolkit/bh-app/src/bh_cheats.cpp` — the live table in code
is the source of truth (see `kWeaponNames[]` near the top of the
Weapon Editor implementation), this doc is the human-readable export.

## The weapon table

| ID   | Name                 | Type    | Ammo source       |
|------|----------------------|---------|-------------------|
| 0x00 | (empty / none)       | —       | none              |
| 0x01 | Fuel                 | Item    | global table[1]   |
| 0x02 | Pistol               | Player  | global table[2]   |
| 0x03 | Shotgun              | Player  | global table[3]   |
| 0x04 | Rifle                | Player  | global table[4]   |
| 0x05 | Machine Gun          | Player  | global table[5]   |
| 0x06 | Rocket Launcher      | Player  | global table[6]   |
| 0x07 | TNT                  | Player  | global table[7]   |
| 0x08 | Sun Shield           | Item    | global table[8]   |
| 0x09 | Grenades             | Player  | global table[9]   |
| 0x0A | Tri-Spinner          | Player  | global table[10]  |
| 0x0B | Vehicle Primary      | Vehicle | redirected via vehicle spec, effectively infinite |
| 0x0C | Vehicle Secondary    | Vehicle | unused / cut content (never observed in-game) |
| 0x0D | Chaingun             | Alpha 1 | global table[13]  |
| 0x0E | Fragcannon           | Alpha 1 | global table[14]  |
| 0x0F | Laser Missiles       | Alpha 1 | global table[15]  |
| 0x10 | Resonator            | Alpha 1 | global table[16]  |
| 0x11 | Plasma Bombs         | Alpha 1 | global table[17]  |

**Total: 18 weapon/item IDs (0x00..0x11), not 20 as commonly listed.**

## The "Arme 2 / Waffe 2" mystery (a corrected myth)

Most cheat databases online list BH as having 20 weapons, with two
extra entries between Vehicle Weapon 2 and Chaingun:

```
... (incorrect, do not use)
0D - Arme 2
0E - Waffe 2
0F - Chaingun
...
```

These entries don't exist in the US ROM. Empirical in-game testing
shows that **0x0D is actually Chaingun** — the phantom entries push
every subsequent weapon's ID up by 2 in the wrong list.

The likely origin of the error:
- `Arme` is French for "weapon"
- `Waffe` is German for "weapon"

Some early-2000s cheat database merged weapon-name lists from
multiple European ROM regions, and the editor didn't realize that
"Arme 2" and "Vehicle Weapon 2" were the same ID under different
localizations. The error has propagated to almost every BH cheat
list published since.

We have not personally verified the PAL ROM — it's possible the
European release DOES surface these as the actual displayed names
for 0x0D / 0x0E. If you have a PAL ROM and a hex editor, this would
be a fun investigation. But for the US ROM specifically, the weapon
count is 18.

## How "vehicle weapons" work

The 7 weapons in the 0x0B..0x11 range aren't really 7 separate
weapons in the same sense as player weapons. There are two distinct
mechanisms:

### 0x0B "Vehicle Primary" — a redirect

This is a polymorphic weapon ID. When a vehicle holds 0x0B as its
"weapon", the actual gun behavior is defined by the vehicle's spec
data — could be a tank cannon, a jeep chaingun, a UFO Resonator,
whatever. Ammo is effectively infinite (the vehicle's spec controls
ammo cost, and most non-Alpha vehicles set it to 0).

So when you drive a tank and shell aliens, the cannon you're firing
is technically weapon 0x0B + a tank-spec — the same ID 0x0B becomes
a chaingun for a jeep and a Resonator for the America UFO.

### 0x0C "Vehicle Secondary" — apparently cut

Never observed in any played-through vehicle. Likely intended for a
secondary-fire system that never shipped. We list it for
completeness but don't recommend writing it into any slot.

### 0x0D..0x11 — Alpha 1's native arsenal

These are the five weapons that Alpha 1 (the player's main flying
vehicle) carries:

| ID   | Weapon          |
|------|-----------------|
| 0x0D | Chaingun        |
| 0x0E | Fragcannon      |
| 0x0F | Laser Missiles  |
| 0x10 | Resonator       |
| 0x11 | Plasma Bombs    |

Unlike 0x0B, these DO consume ammo from the global ammo table — the
built-in `alfa` cheat ("Welfare Cheat") refills all of them.

Other vehicles that "have" these weapons (e.g. the America UFO firing
a Resonator) reference them indirectly via the 0x0B redirect rather
than holding the specific ID. Only Alpha 1 stores the actual IDs
0x0D-0x11 in its weapon slot table.

## The "Serious Weapons" upgrade system

The `snuffle` built-in cheat ("Serious Weapons Cheat") doesn't modify
the weapon slot contents. Instead, it sets a save-flag that, when the
game is reloaded, swaps the FIRING BEHAVIOR of certain weapons:

| Base weapon | When Serious Weapons is active, fires as | Toast string |
|-------------|------------------------------------------|--------------|
| Pistol      | "Wee Laser" (small laser projectile, low damage) | "wee laser" |
| Shotgun     | "Big Laser" (large laser projectile, high damage) | "big laser" |
| Machine Gun | Laser Missiles (homing) | "bad cheat" (placeholder string!) |
| Grenades    | Cluster Bomb (same firing behavior as the Howitzer vehicle weapon) | (varies) |

The "bad cheat" toast on the upgraded Machine Gun strongly suggests
that ID's display-string field was never finalized — a placeholder
that was supposed to be filled in but slipped through. The
substitution logic itself works (firing produces laser missiles), the
text just wasn't updated.

There may be additional substitutions for other weapons we haven't
mapped. The substitution table lives in cheat-firing code, not in
the weapon ID range, so these "upgraded" weapons can't be reached by
direct slot editing — you have to activate Serious Weapons and have
the base weapon equipped.

## The global ammo table

Located at `0x80048146`, 18 `s16` entries (one per weapon ID
0x00..0x11). The next variable after the table is `humansKilled`
(`s16 D_8004816A`), which is how we confirmed the table is exactly 18
entries and not 17.

| Offset from 0x80048146 | Weapon ID | Weapon |
|--|--|--|
| 0x00 | 0x00 | (none — ammo always 0) |
| 0x02 | 0x01 | Fuel |
| 0x04 | 0x02 | Pistol |
| 0x06 | 0x03 | Shotgun |
| 0x08 | 0x04 | Rifle |
| 0x0A | 0x05 | Machine Gun |
| 0x0C | 0x06 | Rocket Launcher |
| 0x0E | 0x07 | TNT |
| 0x10 | 0x08 | Sun Shield |
| 0x12 | 0x09 | Grenades |
| 0x14 | 0x0A | Tri-Spinner |
| 0x16 | 0x0B | (Vehicle Primary — not used; vehicle ammo lives elsewhere) |
| 0x18 | 0x0C | (Vehicle Secondary — unused) |
| 0x1A | 0x0D | Chaingun (Alpha 1) |
| 0x1C | 0x0E | Fragcannon (Alpha 1) |
| 0x1E | 0x0F | Laser Missiles (Alpha 1) |
| 0x20 | 0x10 | Resonator (Alpha 1) |
| 0x22 | 0x11 | Plasma Bombs (Alpha 1) |

Writing `0x8000` (INT16_MIN) to any of these gives effectively
infinite ammo — the game's decrement-on-fire logic wraps around for
negative counters before depleting them.

## The player weapon slot array

Located at `0x80048138`, 8 `u8` entries — the player carries up to 8
weapons/items at once. Each byte holds a weapon ID from the table
above; `0x00` means the slot is empty.

| Offset | Slot |
|--|--|
| 0x00 | Slot 0 |
| 0x01 | Slot 1 |
| ... | ... |
| 0x07 | Slot 7 |

Note that the slots can hold non-weapon items too — `0x01 Fuel` and
`0x08 Sun Shield` are items, not weapons, and they coexist with
actual weapons in the same 8-slot table. The Weapon Editor in
`bh-app/src/bh_cheats.cpp` exposes this directly.

## The items bitmask

A separate `u16` at `0x8004DC4E` tracks the per-level inventory of
artifacts, the hangar key, and any other mission-progression items.
Each bit is one item. The built-in `useful` cheat sets specific bits
for alien artifacts; the built-in `arsenal` cheat indirectly grants
items by giving every weapon.

Items bitmask is per-level — when you switch levels, the bitmask
reloads to match the new level's collected state. So writing
`0xFFFF` to give yourself "all items" only applies to the level
you're currently in.
