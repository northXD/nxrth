# Adonai Port Spec — Module 02: Constants & Data

**Source (Mori / Rust):** `constants.rs`, `items.rs`, `save_dat.rs`, `inventory.rs`, `account_devices.rs`
**Shared dependency also specified here:** `cursor.rs` (the byte reader every binary parser in this module uses).
**Target (Adonai / C++):** reimplement without reading the Rust. This document is the single source of truth.

> **Endianness:** every multi-byte integer/float in this module is **little-endian** (matches Growtopia's wire/disk format and x86). Do not use host-order helpers that could differ on big-endian.

---

## 0. Rename rules (apply throughout the port)

Global project rename conventions:

- `Mori` / `mori` → `Adonai` / `adonai` (identifiers, file paths, log prefixes, window titles, user-agents, config filenames).
- `Cloei` / `cloei` (upstream author/repo) → `North` / `north`.

**Concrete occurrences inside THIS module:** none of the five source files (nor `cursor.rs`) contain the literal strings `Mori`, `mori`, `Cloei`, or `cloei`. What the rename touches here is indirect:

| Rust artifact | Nature | Port action |
|---|---|---|
| `crate::cursor::Cursor` | internal module path of the `mori` crate | becomes `adonai::Cursor` (or `adonai/cursor.hpp`) |
| `crate::constants::{...}` | internal module path | becomes `adonai::constants::…` |
| Log line `"[Items] Loaded {} items"` | runtime log | keep `[Items]` prefix (not a Mori token); if Adonai standardizes a global log tag, apply it here |
| Log line `"[Items] Failed to load items.dat: {e} — pathfinding will be inaccurate"` | runtime log | same |
| `items.dat`, `save.dat` filenames | **Growtopia's own** data files | **DO NOT rename** — the game defines these names |
| `data/account_devices.json` | Mori-authored on-disk store | keep name (contains no "mori" token); path may move under an Adonai data dir if the project standardizes one |

No user-agent, window title, or `Cloei` string lives in this module — those belong to other modules.

---

## 1. `cursor.rs` — the byte reader (foundational)

Every parser below is driven by this cursor. Port it first as e.g. `adonai::Cursor`.

### State
```
data  : const uint8_t*  + length (the input buffer, borrowed, not owned)
pos   : size_t          (current read offset, starts at 0)
label : const char*     (a static tag used only in error messages, e.g. "items.dat")
```

### Semantics of every method

| Method | Behavior | Error condition |
|---|---|---|
| `new(data, label)` | pos = 0 | — |
| `pos() -> size_t` | returns current offset | — |
| `remaining() -> size_t` | `len - pos` | — |
| `need(n)` | bounds check | if `pos + n > len` → error: `"{label} truncated at offset {pos} (need {n} more bytes)"` |
| `skip(n)` | `need(n)`, then `pos += n` | propagates `need` |
| `u8() -> uint8_t` | read 1 byte, `pos += 1` | `need(1)` |
| `u16() -> uint16_t` | LE, `pos += 2` | `need(2)` |
| `u32() -> uint32_t` | LE, `pos += 4` | `need(4)` |
| `i32() -> int32_t` | LE, `pos += 4` | `need(4)` |
| `f32() -> float` | LE IEEE-754, `pos += 4` | `need(4)` |
| `bytes(len) -> vector<uint8_t>` | copy `len` bytes, `pos += len` | `need(len)` |
| `plain_string() -> string` | read `u16` length prefix `L`, then `L` bytes decoded UTF-8 (lossy), `pos += L` | `u16()` then `need(L)` |
| `string_raw(len) -> string` | read `len` bytes as UTF-8 (lossy), **no length prefix**, `pos += len` | `need(len)` |
| `xor_string(key, key_start) -> string` | read `u16` length prefix `L`; for `i in 0..L`: `out[i] = data[pos+i] ^ key[(key_start + i) % key.len()]`; decode UTF-8 (lossy); `pos += L` | `u16()` then `need(L)` |

**Notes for C++:**
- "UTF-8 lossy" = replace invalid byte sequences with U+FFFD (`�`). In practice most callers can keep raw bytes; a straight `std::string` copy is acceptable if you don't need the replacement-char behavior, but match lossy decoding if you re-emit these strings anywhere user-facing.
- `plain_string` uses a **`uint16` length prefix** (max 65535). `string_raw` takes an externally-supplied length (used by save.dat keys, which are `u32`-prefixed by the caller).
- All reads are strictly bounds-checked; on underflow the whole parse aborts with the error string. Port this as either exceptions or an `Expected<T>` / status-return — pick one and use it consistently (see §8 Dependency mapping).

---

## 2. `constants.rs` — verbatim constants

These are compile-time constants. Reproduce **exactly** (values are load-bearing for login/protocol handshakes elsewhere). Suggested C++: `constexpr` for scalars, `inline constexpr std::string_view` (or `const char*`) for strings, in `namespace adonai::constants`.

| Rust name | Type | Value (verbatim) |
|---|---|---|
| `PROTOCOL` | `u32` | `226` |
| `GAME_VER` | `&str` | `"5.51"` |
| `FHASH` | `i32` | `-716928004` |
| `DEFAULT_RID` | `&str` | `"025B42980AFB659E0394C846233653FF"` |
| `DEFAULT_MAC` | `&str` | `"74:d4:dd:6c:24:e1"` |
| `DEFAULT_WK` | `&str` | `"788E366E74D2D098398A35C3F6360DDA"` |
| `DEFAULT_HASH` | `&str` | `"381621508"` |
| `DEFAULT_HASH2` | `&str` | `"-332772458"` |
| `DEFAULT_FZ` | `&str` | `"18274296"` |
| `DEFAULT_PLATFORM_ID` | `&str` | `"15,1,0"` |
| `DEFAULT_ZF` | `&str` | `"1597752569"` |

`DEFAULT_STEAM_TOKEN` (`&str`) — space-separated hex bytes, terminated with the literal suffix `.240`. Reproduce **exactly** (single space separators, lowercase hex):

```
14 00 00 00 3c 13 04 25 a4 c5 fd 4c 8c 55 93 72 01 00 10 01 38 b9 12 6a 18 00 00 00 01 00 00 00 05 00 00 00 6d 91 7f 14 12 ca 43 3b 27 31 26 00 02 00 00 00 b8 00 00 00 38 00 00 00 04 00 00 00 8c 55 93 72 01 00 10 01 e4 36 0d 00 c0 47 54 b9 06 64 a8 c0 00 00 00 00 6a 3c 10 6a ea eb 2b 6a 01 00 11 2b 04 00 01 00 e6 41 28 00 00 00 00 00 c2 54 51 5d b7 4e 66 c6 a0 52 02 c9 67 d0 56 bd aa d6 43 6d ab 51 3b 68 b9 3c 10 71 21 37 f7 ca ec 78 eb 8b 5d 83 5a 14 91 87 b2 fa b2 c3 db 69 1e 23 11 d2 35 bd 6b a0 1c 41 fe 83 75 43 51 ce b6 71 6f da be ef 1d 09 9e 04 53 46 7c 41 62 f8 f7 e6 51 ac b8 b1 c4 24 84 a6 94 33 36 52 8c dc 3a a6 d3 80 f6 ba 2d 26 d3 eb ac ea 91 5c 52 17 60 55 f2 52 26 24 d4 c6 b9 4b 52 c5 0e ff 2d 5d.240
```

**Context / rationale baked into the source (2026-07-11):** the game force-enforces client `5.51`; `5.50` gets "UPDATE REQUIRED", so `GAME_VER` **must** be `"5.51"`. `PROTOCOL = 226` (= 175 base + 51 minor) is accepted at the ENet logon. These are used by login/spoofing modules (not by the parsers in this file); only `DEFAULT_RID/MAC/WK/HASH/HASH2/ZF` are consumed within this module (by `account_devices.rs`, §6).

---

## 3. `items.rs` — items.dat binary parser

Parses Growtopia's `items.dat` (the item metadata database). Read-only after load.

### 3.1 Constant
```
XOR_KEY (16 bytes) = "PBG892FXX982ABC*"
  hex: 50 42 47 38 39 32 46 58 58 39 38 32 41 42 43 2A
```
Used to decrypt each item's `name` string.

### 3.2 `ItemInfo` struct — EXACT field order & types

The struct field order below is **also the on-disk read order** for the fixed portion (see parser §3.5). Serde-serializable (used elsewhere to emit JSON); for the port, a plain struct is fine.

```
struct ItemInfo {
    uint32  id
    uint16  flags
    uint8   action_type
    uint8   material
    string  name                 // XOR-decoded (see parser)
    string  texture_file_name    // plain_string
    uint32  texture_hash
    uint8   visual_effect
    uint32  cooking_ingredient
    uint8   texture_x
    uint8   texture_y
    uint8   render_type
    uint8   is_stripey_wallpaper
    uint8   collision_type
    uint8   block_health
    uint32  drop_chance
    uint8   clothing_type
    uint16  rarity
    uint8   max_item
    string  file_name            // plain_string
    uint32  file_hash
    uint32  audio_volume
    string  pet_name             // plain_string
    string  pet_prefix           // plain_string
    string  pet_suffix           // plain_string
    string  pet_ability          // plain_string
    uint8   seed_base_sprite
    uint8   seed_overlay_sprite
    uint8   tree_base_sprite
    uint8   tree_overlay_sprite
    uint32  base_color           // BGRA-packed
    uint32  overlay_color        // BGRA-packed
    uint32  ingredient
    uint32  grow_time
    uint16  is_rayman            // (preceded by a skipped unused u16)
    string  extra_options        // plain_string
    string  texture_path_2       // plain_string
    string  extra_option2        // plain_string
    // version-gated:
    string  punch_option         // v>=11
    string  description          // v>=22
}
```
All fields default to zero / empty string. `Default` is used to construct then fields are filled in read order.

### 3.3 Color helpers

`items.dat` packs colors as **BGRA**, MSB→LSB (i.e. byte 3 = B, byte 2 = G, byte 1 = R, byte 0 = A within the little-endian-loaded `u32`).

```
extract_bgra(color: u32) -> (b, g, r, a):
    b = (color >> 24) & 0xFF
    g = (color >> 16) & 0xFF
    r = (color >> 8)  & 0xFF
    a =  color        & 0xFF
    return (b, g, r, a)   // NOTE: order is (b, g, r, a)

bgra_to_rgb(color: u32) -> u32:
    (b, g, r, _) = extract_bgra(color)
    return (r << 16) | (g << 8) | b    // 0x00RRGGBB, alpha dropped
```
`bgra_to_rgb` produces a `0xRRGGBB` value (used for minimap coloring elsewhere).

### 3.4 `ItemsDat` container

```
struct ItemsDat {
    uint16          version
    vector<ItemInfo> items
}
```

**`parse(data) -> Result<ItemsDat>`**
1. `cur = Cursor(data, "items.dat")`
2. `version = cur.u16()`
3. `item_count = cur.u32()`
4. reserve `item_count`; loop `item_count` times: `items.push(parse_item(cur, version))`
5. return `{ version, items }`

**`find_by_id(id: u32) -> Option<&ItemInfo>`**
- Fast path: index `items[id]`; **if that element exists AND its `.id == id`**, return it.
- Fallback: linear scan for the first item whose `.id == id`.
- Else `None`.
- (In C++: `if (id < items.size() && items[id].id == id) return &items[id];` then `std::find_if`.)

**`find_by_name(name: &str) -> Option<&ItemInfo>`**
- Lowercase `name`; linear scan for first item whose lowercased `.name` equals it. ASCII/`to_lowercase` semantics — use a locale-independent lowercase.

**`load() -> ItemsDat`** (never fails; returns empty DB on error)
1. Read file `"items.dat"` (relative to CWD).
2. On success parse; print `"[Items] Loaded {N} items"`; return DB.
3. On any read/parse error print `"[Items] Failed to load items.dat: {error} — pathfinding will be inaccurate"` and return `{ version: 0, items: [] }`.

### 3.5 `parse_item(cur, version)` — exact step-by-step

Fixed portion (read in this exact order):
1. `id = u32`
2. `flags = u16`
3. `action_type = u8`
4. `material = u8`
5. `name = xor_string(XOR_KEY, key_start = id % 16)` — **the XOR key start offset is the item's own `id` mod 16.** Length is a `u16` prefix (per `xor_string`).
6. `texture_file_name = plain_string`
7. `texture_hash = u32`
8. `visual_effect = u8`
9. `cooking_ingredient = u32`
10. `texture_x = u8`
11. `texture_y = u8`
12. `render_type = u8`
13. `is_stripey_wallpaper = u8`
14. `collision_type = u8`
15. `block_health = u8`
16. `drop_chance = u32`
17. `clothing_type = u8`
18. `rarity = u16`
19. `max_item = u8`
20. `file_name = plain_string`
21. `file_hash = u32`
22. `audio_volume = u32`
23. `pet_name = plain_string`
24. `pet_prefix = plain_string`
25. `pet_suffix = plain_string`
26. `pet_ability = plain_string`
27. `seed_base_sprite = u8`
28. `seed_overlay_sprite = u8`
29. `tree_base_sprite = u8`
30. `tree_overlay_sprite = u8`
31. `base_color = u32`
32. `overlay_color = u32`
33. `ingredient = u32`
34. `grow_time = u32`
35. `skip(2)` — an unused `u16`
36. `is_rayman = u16`
37. `extra_options = plain_string`
38. `texture_path_2 = plain_string`
39. `extra_option2 = plain_string`
40. `skip(80)` — 80 unused bytes

Version-gated tail (evaluate each gate in order; each is an independent `if version >= N`, **cumulative** — higher versions execute all lower gates too):

| Gate | Action |
|---|---|
| `v >= 11` | `punch_option = plain_string` |
| `v >= 12` | `skip(13)` |
| `v >= 13` | `skip(4)` |
| `v >= 14` | `skip(4)` |
| `v >= 15` | `skip(25)`, then `plain_string()` **discarded** |
| `v >= 16` | `plain_string()` **discarded** |
| `v >= 17` | `skip(4)` |
| `v >= 18` | `skip(4)` |
| `v >= 19` | `skip(9)` |
| `v >= 21` | `skip(2)` |
| `v >= 22` | `description = plain_string` |
| `v >= 23` | `skip(4)` |
| `v >= 24` | `skip(1)` |

**Gotchas to preserve exactly:**
- There is **no `v >= 20` gate** — 20 is intentionally absent.
- The two `plain_string()` calls at `v>=15` (the second one) and `v>=16` are read-and-discarded (they advance the cursor but are not stored).
- `skip` values are literal byte counts; do not "optimize" them away.

### 3.6 Threading / usage
`ItemsDat` is loaded once at startup and treated as **immutable, read-only, shared across the whole fleet** (all bots consult the same DB for pathfinding/collision). In C++: build once, share via `std::shared_ptr<const ItemsDat>` (or a global `const`). No locking needed since it never mutates after load.

---

## 4. `save_dat.rs` — save.dat / world .dat variant store

A key/value store serialized in Growtopia's "variant list" style. Used for the client `save.dat` (login/session persistence). Round-trippable (parse → serialize is byte-stable given same entry order).

### 4.1 XOR helper (key `"90210"`)
```
xor_90210(data) -> vector<uint8_t>:
    KEY = "90210"  (5 bytes: 39 30 32 31 30)
    for i in 0..len:  out[i] = data[i] ^ KEY[i % 5]
```
Self-inverse (encode == decode). Applied to the `meta` field (and conceptually `tankid_password`, `parentalpass`).

### 4.2 `VariantValue` (tagged union) + wire type IDs

```
enum VariantValue {
    Float(float)                              // type_id 1
    String(vector<uint8_t>)                   // type_id 2  — RAW BYTES, not guaranteed UTF-8
    Vec2(float x, float y)                    // type_id 3
    Vec3(float x, float y, float z)           // type_id 4
    Uint(uint32)                              // type_id 5
    Rect(float x, float y, float w, float h)  // type_id 8
    Int(int32)                                // type_id 9
}
```
- **`String` holds raw bytes** (`Vec<u8>`), because `meta` and `seed_diary_data` contain binary/XOR-encoded content. In C++ use `std::vector<uint8_t>` (or `std::string` treated as a byte container). Do **not** force UTF-8 here.
- Type IDs **6 and 7 are unused/unsupported**. Parser rejects any type ID other than {1,2,3,4,5,8,9}.
- Equality (`PartialEq`) is used in tests; implement value equality for round-trip verification.

### 4.3 `Entry`
```
struct Entry { string key; VariantValue value; }
```

### 4.4 Seed diary

```
const SEED_DIARY_MAX_ID: uint16 = 16010   // 0x3E8A

struct SeedDiary {
    set<uint16> have    // ordered set
    set<uint16> grown   // ordered set
}
```

On-disk format of the `seed_diary_data` string: packed 16-bit **little-endian** entries, one per "have" item:
```
bits [14:0] = item_id   (0 .. 16010)
bit  [15]   = grown flag
```

**`SeedDiary::parse(data) -> SeedDiary`**
```
i = 0
while i + 1 < data.len():            // requires 2 available bytes for the pair
    lo = data[i]
    hi = data[i+1]
    item_id = lo | ((hi & 0x7F) << 8)
    if item_id <= 16010:
        have.insert(item_id)
        if (hi & 0x80) != 0: grown.insert(item_id)
    i += 2
```
> Loop guard is `i + 1 < len` (strictly less-than). This reads every complete 2-byte pair; a trailing odd byte is ignored. Verified: for `len` even it reads all pairs; the final pair at index `len-2` satisfies `(len-2)+1 < len`.

**`SeedDiary::serialize() -> vector<uint8_t>`**
```
for id in 0 ..= 16010:               // inclusive, ascending
    if have.contains(id):
        lo = id & 0xFF
        hi = ((id >> 8) & 0x7F) | (grown.contains(id) ? 0x80 : 0)
        push lo, push hi
```
Output is ascending by id (relies on `have` being iterated in order — use `std::set`/ordered iteration).

### 4.5 `SaveDat` container

```
struct SaveDat { vector<Entry> entries; }
```

Methods:
- `new()` → empty.
- `set(key, value)` → if an entry with `key` exists, **update its value in place** (position unchanged); else **append** a new entry. (Insertion order is preserved and load-bearing for byte-stable re-serialization.)
- `get(key) -> Option<&VariantValue>` → first entry with matching key.
- `get_meta() -> Option<vector<uint8_t>>` → if `get("meta")` is a `String(b)`, return `xor_90210(b)`; else `None`.
- `set_meta(plain)` → `set("meta", String(xor_90210(plain)))`.
- `get_seed_diary() -> Option<SeedDiary>` → if `get("seed_diary_data")` is `String(b)`, return `SeedDiary::parse(b)`; else `None`.
- `set_seed_diary(diary)` → `set("seed_diary_data", String(diary.serialize()))`.

### 4.6 `SaveDat::serialize() -> vector<uint8_t>` — exact byte layout

```
[ u32 LE = 1 ]                          // magic / version
for each entry (in stored order):
    [ u32 LE type_id ]                  // 1/2/3/4/5/8/9 per §4.2
    [ u32 LE key_len ]                  // key length in bytes (UTF-8)
    [ key bytes ]                       // no NUL terminator
    value payload:
        Float : [ f32 LE ]
        String: [ u32 LE byte_len ][ bytes ]
        Vec2  : [ f32 LE x ][ f32 LE y ]
        Vec3  : [ f32 LE x ][ f32 LE y ][ f32 LE z ]
        Uint  : [ u32 LE ]
        Rect  : [ f32 LE x ][ f32 LE y ][ f32 LE w ][ f32 LE h ]
        Int   : [ i32 LE ]
[ u32 LE = 0 ]                          // terminator
```

### 4.7 `SaveDat::parse(data) -> Result<SaveDat>` — exact steps

1. `cur = Cursor(data, "save.dat")`
2. `magic = cur.u32()`; if `magic != 1` → error `"unexpected magic: {magic}"`.
3. Loop while `cur.remaining() > 0`:
   - `type_id = cur.u32()`
   - if `type_id == 0` → **break** (terminator).
   - `key_len = cur.u32()`
   - `key = cur.string_raw(key_len)` (raw `key_len` bytes → string)
   - decode value by `type_id`:
     - `1` → `Float(cur.f32())`
     - `2` → `len = cur.u32(); String(cur.bytes(len))`
     - `3` → `Vec2(cur.f32(), cur.f32())`
     - `4` → `Vec3(cur.f32(), cur.f32(), cur.f32())`
     - `5` → `Uint(cur.u32())`
     - `8` → `Rect(cur.f32(), cur.f32(), cur.f32(), cur.f32())`
     - `9` → `Int(cur.i32())`
     - otherwise → error `"unknown variant type {type_id} for key {key}"`
   - push `Entry{key, value}`
4. return `{ entries }`

**Round-trip invariant** (must hold): parse → serialize → parse yields identical entry count, keys, and values in order. Preserve `set` in-place-update semantics or you will reorder entries and break byte-stability.

### 4.8 Threading / usage
Pure data structure, **per-bot** (each bot owns its own `save.dat` view). No global state, no interior locking. If a bot is mutated from multiple threads, the owning bot's lock (defined in another module) covers it.

---

## 5. `inventory.rs` — inventory model

Parses the inventory blob the server sends and maintains a live per-bot inventory.

### 5.1 Constant
```
TEMPORARY_ITEM_IDS : uint16[] = { 1424 /*World Key*/, 5640 /*Magplant 5000 Remote*/ }
```

### 5.2 Structs
```
struct InventoryItem {
    uint16 id
    uint8  amount
    uint8  flag
}

struct Inventory {
    uint32                       size          // total inventory capacity (slots)
    uint16                       item_count    // number of distinct item stacks
    map<uint16, InventoryItem>   items         // keyed by item id (unordered ok)
    int32                        gems          // NOT parsed from wire; default 0
}
```
`items` may be a `std::unordered_map<uint16_t, InventoryItem>` (order doesn't matter for logic).

### 5.3 `Inventory::parse(data) -> Result<Inventory>` — exact byte layout
```
cur = Cursor(data, "inventory")
skip(1)                    // one unknown leading byte
size       = cur.u32()
item_count = cur.u16()
repeat item_count times:
    id     = cur.u16()
    amount = cur.u8()
    flag   = cur.u8()
    items[id] = { id, amount, flag }     // duplicate ids overwrite
gems = 0                   // gems come from elsewhere, not this blob
```
Each item record on the wire is **4 bytes**: `u16 id`, `u8 amount`, `u8 flag`.

### 5.4 Mutating / query methods (exact semantics)

- `clear()` → `size = 0; item_count = 0; items.clear()`. (Does **not** touch `gems`.)
- `add_item(id, amount)` → if present: `amount = saturating_add(existing.amount, amount)` (clamp at 255, no wrap). Else insert `{id, amount, flag: 0}` and `item_count += 1`.
- `add_gems(amount)` → **sets** `gems = amount` (despite the name; it is an assignment, not an accumulate).
- `has_item(id, min_amount) -> bool` → `present && amount >= min_amount`, else false.
- `is_active(id) -> bool` → `present && (flag & 1) != 0`, else false.
- `set_active(id, active)` → if present: `active ? flag |= 1 : flag &= ~1`. (No-op if absent.)
- `can_collect(item_id) -> bool`:
  - if `item_id == 112` → `true` (gems always fit).
  - else if present → `existing.amount < 200` (stack cap 200).
  - else → `(item_count as u32) < size` (room for a new stack).
- `sub_item(id, amount)` → if present: if `existing.amount <= amount` → remove the entry and `item_count = saturating_sub(item_count, 1)`; else `existing.amount -= amount`.
- `remove_item(id)` → if the id was present and removed → `item_count = saturating_sub(item_count, 1)`.
- `remove_temp_items() -> bool` → collect all present ids that are in `TEMPORARY_ITEM_IDS`; `remove_item` each; return `true` iff at least one was removed. (Removes World Key 1424 and Magplant Remote 5640.)

**Saturating arithmetic:** `add_item` uses `u8` saturating add (max 255); `item_count` decrements use `u16` saturating sub (floor 0). Match these exactly in C++ (clamp manually).

**Magic constants to preserve:** item id `112` (gems, always collectable), stack cap `200`, temp ids `1424` / `5640`.

### 5.5 Threading / usage
`Inventory` is **per-bot mutable state**, updated as inventory packets arrive and as the bot collects/consumes items. It is guarded by the owning bot's lock (external to this module). No global/static state here.

---

## 6. `account_devices.rs` — per-account device identity store (FLEET-WIDE)

Persists a unique "device identity" (`rid`, `mac`, `wk`, plus `hash`, `hash2`, `zf`) per account so every bot presents as a distinct device. This is **process-wide shared state** — the single most fleet-relevant piece in this module.

### 6.1 Types
```
struct AccountDevice {          // serde JSON <-> struct
    string rid
    string mac
    string wk
    string hash
    string hash2
    string zf
}

struct AccountDeviceStore {     // serde JSON: { "devices": { <key>: AccountDevice, ... } }
    map<string, AccountDevice> devices   // ORDERED (BTreeMap) — key = lowercased username
}
```
JSON on disk (`data/account_devices.json`), pretty-printed:
```json
{
  "devices": {
    "someuser": {
      "rid": "….", "mac": "aa:bb:…", "wk": "….",
      "hash": "…", "hash2": "…", "zf": "…"
    }
  }
}
```
Missing/corrupt file → treated as empty store (`{ "devices": {} }`).

### 6.2 Global lock
```
STORE_LOCK : global Mutex<()>   // lazily initialized (OnceLock)
```
Every public entry point acquires this **process-global** mutex for the whole read-modify-write of the JSON file. In C++: a function-local `static std::mutex` (Meyers singleton) guarding a read→mutate→write critical section. Lock-poisoning maps to an I/O error in Rust; in C++ just hold a `std::lock_guard`.

### 6.3 Helpers (exact behavior)

- `account_key(username) -> Option<string>`: `trim` then `to_ascii_lowercase`; if empty → `None`; else the lowercased key. **All lookups/inserts are by this lowercased, trimmed username.**
- `storage_path() -> path`: `current_dir()` (fallback `"."`) `/ "data" / "account_devices.json"`.
- `read_store(path) -> AccountDeviceStore`: read file → parse JSON; on any error return default (empty). Never throws.
- `write_store(path, store)`: `create_dir_all(parent)`; serialize **pretty JSON**; write file (overwrite).
- `is_default_device(rid, mac, wk) -> bool`: `rid == DEFAULT_RID && mac == DEFAULT_MAC && wk == DEFAULT_WK`, **all three case-insensitive** (ASCII). (From §2: RID `025B4298…`, MAC `74:d4:dd:6c:24:e1`, WK `788E366E…`.)
- `generate_device() -> AccountDevice`: `rid = random_hex32()`, `mac = random_mac()`, `wk = random_hex32()`, `hash = random_i32()`, `hash2 = random_i32()`, `zf = random_i32()` — **fully random identity**.
- `default_device() -> AccountDevice`: `rid = random_hex32()`, `mac = random_mac()`, `wk = random_hex32()`, but `hash = DEFAULT_HASH`, `hash2 = DEFAULT_HASH2`, `zf = DEFAULT_ZF` — random rid/mac/wk but default hash triple.
- `parse_login_token_device(login_token) -> Option<(rid, mac, wk)>`: `trim`, `split('|')`, `trim` each part; require **exactly 4 parts** and parts[1], parts[2], parts[3] all non-empty; return `(parts[1], parts[2], parts[3])`. **parts[0] is ignored.** (Login-token device layout: `field0 | rid | mac | wk`.)

RNG helpers (source uses UUID v4; port with a CSPRNG producing 16 random bytes per call):
- `random_hex32() -> string`: 32 hex chars of a fresh random 16-byte value (UUIDv4 "simple" form), **uppercased**. → e.g. `"A1B2…"` (32 uppercase hex).
- `random_i32() -> string`: take a fresh random 16-byte value; interpret its **first 4 bytes as little-endian `i32`**; stringify (may be negative). C++: `int32_t v; memcpy(&v, bytes, 4);` then `std::to_string(v)`.
- `random_mac() -> string`: fresh random 16-byte value `b`; `first = (b[0] & 0xFE) | 0x02` (clear multicast bit, set locally-administered bit); format lowercase `"%02x:%02x:%02x:%02x:%02x:%02x"` using `first, b[1], b[2], b[3], b[4], b[5]`.

> On UUID version/variant bits: the Rust code generates a full UUIDv4 then slices raw bytes. The v4 version bit is at byte 6 and variant at byte 8 — **outside** the byte ranges used by `random_i32` (bytes 0–3) and `random_mac` (bytes 0–5). So for the port you may simply draw independent uniform random bytes for each helper; the exact UUID bit-fiddling is not observable in the outputs used here. `random_hex32` uses all 16 bytes, but since it's just an opaque device id, plain random hex is equivalent.

### 6.4 `get_or_create(username) -> Result<Option<AccountDevice>>`
1. `key = account_key(username)`; if `None` → return `Ok(None)`.
2. Acquire `STORE_LOCK`.
3. `store = read_store(storage_path())`.
4. If `store.devices[key]` exists **and** it is **not** a default device → return `Ok(Some(clone))`.
   - **Self-heal:** if the existing entry *is* the shared DEFAULT placeholder (all bots would look identical → ban vector), fall through and replace it.
5. `device = generate_device()`; `store.devices[key] = device`; `write_store(...)`; return `Ok(Some(device))`.

### 6.5 `upsert_from_login_token(username, login_token) -> Result<bool>`
Returns `true` if the store was modified/written, `false` otherwise.
1. `key = account_key(username)`; if `None` → `Ok(false)`.
2. `(rid, mac, wk) = parse_login_token_device(login_token)`; if `None` → `Ok(false)`.
3. Acquire `STORE_LOCK`; `store = read_store(...)`.
4. **If the token's device is the DEFAULT placeholder** (`is_default_device(rid,mac,wk)`):
   - if `store` already has `key` → return `Ok(false)` (keep the existing real identity; never overwrite with the shared default).
   - else `store.devices[key] = generate_device()`; write; return `Ok(true)`.
5. Otherwise (a real per-account device in the token):
   - `device = store.devices[key].clone()` if present, else `default_device()`.
   - If any of `device.rid != rid || device.mac != mac || device.wk != wk` → **reset the hash triple**: `device.hash = DEFAULT_HASH; device.hash2 = DEFAULT_HASH2; device.zf = DEFAULT_ZF`.
   - Assign `device.rid = rid; device.mac = mac; device.wk = wk`.
   - `store.devices[key] = device`; write; return `Ok(true)`.

**Design intent to preserve:** a login token carrying only the shared DEFAULT device must never stamp every account with the same `rid/mac/wk` (all bots looking like one device is a ban vector). Real device fields from a token are imported; when they change, the derived hash triple is invalidated back to defaults.

### 6.6 Threading & fleet-wide shared state (important for Adonai)
- The account-device store is **shared by the entire fleet**: all bots read/write the one `data/account_devices.json` under the single global `STORE_LOCK`. This is exactly the "bots must be aware of each other" concern — device identities are coordinated so no two accounts collide on `rid/mac/wk`, and each account keeps a stable identity across runs.
- **C++ port:** implement as a singleton/service with a `std::mutex` guarding an atomic read-modify-write of the JSON file (via `nlohmann/json`). Consider caching the parsed store in memory behind the same mutex to avoid re-reading the file on every call, but keep write-through to disk so a crash doesn't lose identities. Keys are always the trimmed-lowercased username.
- No async; all calls are synchronous and short. Safe to call from any bot thread.

---

## 7. Threading & shared-state summary (whole module)

| Piece | Ownership | Mutability | Sharing / locking |
|---|---|---|---|
| `constants.rs` | compile-time | none | globally visible constants |
| `ItemsDat` (items.rs) | loaded once at startup | immutable after load | **fleet-shared read-only** (`shared_ptr<const>`), no lock |
| `SaveDat` (save_dat.rs) | per-bot | mutable | guarded by owning bot's lock (external) |
| `Inventory` (inventory.rs) | per-bot | mutable | guarded by owning bot's lock (external) |
| `AccountDeviceStore` (account_devices.rs) | **process/fleet-wide** | mutable on disk | **global `STORE_LOCK` mutex** around read-modify-write of `data/account_devices.json` |

---

## 8. Dependency mapping (Rust crate → Adonai C++)

Only these crates appear in this module. (mlua, ureq/reqwest, rusty_enet, md5, argon2, scraper, tokio/axum/tower, crossbeam-channel are **not** used here — no action needed for them in this module.)

| Rust | Used for | Adonai C++ |
|---|---|---|
| `anyhow::{Result, bail}` | error propagation in parsers | pick one project-wide error strategy: exceptions (`throw std::runtime_error(msg)`) **or** an `Expected<T>` / `tl::expected` / status-return. Preserve the exact error message strings (`"{label} truncated at offset {pos} (need {n} more bytes)"`, `"unexpected magic: {magic}"`, `"unknown variant type {t} for key {k}"`). |
| `serde` / `serde_json` (account_devices) | JSON (de)serialize of the device store | **nlohmann/json**. `to_string_pretty` → `dump(indent)`. Tolerant read (bad JSON → empty store). |
| `uuid` (v4) | random device id / mac / i32 | any CSPRNG: `std::random_device` seeding `std::mt19937_64` (or platform RNG) → 16 random bytes per helper. Match `random_hex32` (32 UPPER hex), `random_i32` (first 4 bytes as LE `int32`, `to_string`), `random_mac` (`(b0&0xFE)|0x02`, lowercase `xx:..`). |
| `std::collections::HashMap` (inventory) | `id -> InventoryItem` | `std::unordered_map<uint16_t, InventoryItem>` |
| `std::collections::BTreeSet` (seed diary) | ordered id sets | `std::set<uint16_t>` (ordered iteration is required for serialize) |
| `std::collections::BTreeMap` (device store) | ordered `key -> device` | `std::map<std::string, AccountDevice>` (ordered → stable JSON output) |
| `std::sync::{Mutex, OnceLock}` | global store lock | `std::mutex` + Meyers singleton (`static std::mutex` in a function) / `std::call_once` |
| `std::fs` / `std::path` | file read/write, dir create | `<fstream>` + `<filesystem>` (`std::filesystem::create_directories`) |
| `crate::cursor::Cursor` | byte reader | port as `adonai::Cursor` (§1) |

**No Lua, no HTTP, no ENet, no channels, no web server** are involved in this module.

---

## 9. Constants & magic values checklist (quick reference for the porter)

- Protocol/version: `PROTOCOL=226`, `GAME_VER="5.51"`, `FHASH=-716928004`.
- items XOR key: `"PBG892FXX982ABC*"` (16 bytes); name decode key-start = `id % 16`.
- items version gates: 11,12,13,14,15,16,17,18,19,21,22,23,24 (**no 20**); fixed tail `skip(80)`; earlier `skip(2)` before `is_rayman`.
- color packing: **BGRA** (`extract_bgra` returns `(b,g,r,a)`; `bgra_to_rgb` → `0x00RRGGBB`).
- save.dat: magic `u32=1`, terminator `u32=0`; variant type ids `1/2/3/4/5/8/9` (6,7 unused); `String` = raw `u32`-len bytes.
- save.dat XOR key: `"90210"` (5 bytes), self-inverse, applied to `meta`.
- seed diary: `MAX_ID=16010 (0x3E8A)`, 15-bit id + bit15 grown, LE packed, ascending serialize.
- inventory: 1 skip byte, `u32 size`, `u16 item_count`, then 4-byte records `u16 id / u8 amount / u8 flag`; gems item id `112`; stack cap `200`; temp ids `1424`,`5640`; `add_item` u8 saturating add.
- device store: file `data/account_devices.json`, key = trimmed-lowercased username, global mutex; default device = (`DEFAULT_RID`,`DEFAULT_MAC`,`DEFAULT_WK`,`DEFAULT_HASH`,`DEFAULT_HASH2`,`DEFAULT_ZF`); login-token layout `f0|rid|mac|wk` (parts[0] ignored, exactly 4 parts, last 3 non-empty).
