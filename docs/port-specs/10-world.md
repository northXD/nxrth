# Adonai Port Spec — Module `10-world`

**Source (Rust, Mori 2.0.0):** `src/world/mod_impl.rs`, `src/world/constants.rs`, `src/astar.rs`, `src/cursor.rs`
**Target:** C++ (project "Adonai"). This document is the single source of truth. A C++ engineer must be able to reimplement the module from this spec **without** reading the Rust.

Scope of this module:
- Byte-level parser for the world map blob delivered in the `SendMapData` game packet.
- In-memory `World` / `WorldTileMap` / `Tile` / `WorldObject` model + `TileFlags` + `TileType` (per-tile "extra data").
- Incremental tile updates (`update_tile_from_bytes`).
- A* pathfinder over a per-tile collision grid, with a path cache.
- A little-endian binary read cursor (`Cursor`) used by all the above.
- Pixel↔tile coordinate helpers (documented from the callers, since they belong logically to this module).

Everything on the wire is **little-endian** (GT is LE). Strings are UTF-8, decoded lossily (invalid byte sequences → U+FFFD, never an error).

---

## 0. Rename rules (apply everywhere in this module)

Apply these identifier/string substitutions when porting. They do not affect wire format or protocol constants — only project-owned names.

| Rust (Mori) | C++ (Adonai) |
|---|---|
| `Mori` / `mori` (any identifier, path, log tag, window title, user-agent, config file) | `Adonai` / `adonai` |
| `Cloei` / `cloei` (upstream author/repo) | `North` / `north` |

Concrete occurrences spotted **in these four files**:
- `mod_impl.rs` log lines use the tag **`[world]`** (not "mori"), and the World-parse debug tests reference **`world.dat`**. No literal "Mori"/"Cloei" tokens appear inside these four files. Keep the `[world]` tag but note that the *caller* (`bot/core.rs`) logs with `[Bot]` — those `[Bot]` lines belong to another module.
- The two `eprintln!` diagnostics in `mod_impl.rs` (`"[world] tile {idx} ... failed"` and `"[world] WARNING: unknown TileExtraData kind ..."`) should keep a stable, greppable tag; in Adonai use `[world]` (lowercase, unchanged) or your chosen logger category — do **not** introduce a "mori"/"cloei" token.
- No URLs, user-agents, or config filenames live in this module. If your porting pass touches sibling modules, the branded strings (window title "Mori", UA, `mori_*.json`/`mori_debug.log`, dashboard "Cloei") are handled there; this module contributes none.

---

## 1. Dependency mapping (Rust crate → Adonai C++)

This module uses very few crates:

| Rust crate / feature | Used for | Adonai C++ replacement |
|---|---|---|
| `anyhow::{Result, bail}` | error propagation from the cursor/parser | Return `std::optional<T>` / `bool` + out-param, or throw a `std::runtime_error`/custom `ParseError`. Recommended: an exception-free style — parse functions return `bool` and write into an out struct; a truncation sets a "failed" flag. Preserve the **error messages** (e.g. `"<label> truncated at offset <pos> (need <n> more bytes)"`) as log strings. |
| `bitflags::bitflags!` (`TileFlags: u16`) | typed bitset over a `u16` | Plain `uint16_t` + `enum class TileFlag : uint16_t { ... }` constants and `(flags_raw & TileFlag::X) != 0` tests. `from_bits_retain` semantics = "keep all bits, including undefined ones" → just store the raw `uint16_t` and mask when querying. |
| `serde::Serialize` on `TileType` (`#[serde(tag = "type")]`) | JSON emission of tiles to the (removed) web UI | Adonai has **no web server**. If tile state must be surfaced to the Dear ImGui UI or persisted, use `nlohmann/json` and emit a `{"type": "<VariantName>", ...fields}` object (adjacently-**internally** tagged: a `"type"` string field sits alongside the variant's fields at the same object level). Only needed if you keep the UI mirror; the parser itself needs no JSON. |
| `std::collections::{BinaryHeap, HashMap, HashSet}` (A*) | open list / came-from / g-scores / closed set / path cache | `std::priority_queue` (with a comparator giving a **min-heap on f, tie-break min-heap on h**), `std::unordered_map`, `std::unordered_set`. Key types are `(u32,u32)` and `(u32,u32,u32,u32,bool)` tuples — provide a hash (e.g. pack into `uint64_t` for the 2-tuple; a small struct + custom hash for the 5-tuple). |
| `std::fs` (test only) | reads `world.dat` in a `#[cfg(test)]` unit test | Not shipped. Port as an optional dev fixture reader if desired. |

No `libcurl`, ENet, Lua, md5, argon2, tokio/axum, or crossbeam usage appears **in these four files**. (This module is CPU/parse-only; the network/threading wiring lives in `bot/core.rs`, summarized in §8.)

---

## 2. `Cursor` — little-endian binary reader (`cursor.rs`)

A forward-only reader over a borrowed byte buffer. **Every** parser in this module reads through it. Reimplement first.

### 2.1 State

```cpp
struct Cursor {
    const uint8_t* data;   // buffer base   (Rust: &'a [u8])
    size_t         len;    // buffer length
    size_t         pos;    // current read offset, starts at 0
    const char*    label;  // static string used only in error messages
};
```

Rust ctor: `Cursor::new(data, label)` → `pos = 0`.

### 2.2 Methods (exact semantics)

All multi-byte reads are **little-endian**. Every read first calls `need(n)`; on shortfall it must fail (Rust `bail!`) and abort the current parse. In C++, return `false`/`std::nullopt` and set an error, or throw.

| Method | Bytes | Behavior |
|---|---|---|
| `pos() -> usize` | 0 | returns `pos`. |
| `remaining() -> usize` | 0 | returns `len - pos`. |
| `need(n) -> Result<()>` | 0 | **if `pos + n > len` → error** with message `"{label} truncated at offset {pos} (need {n} more bytes)"`. Else Ok. (Note: uses `pos + n`, no overflow guard — `n` is always small/bounded here.) |
| `skip(n)` | n | `need(n)`; `pos += n`. |
| `u8()` | 1 | `need(1)`; read `data[pos]`; `pos += 1`. |
| `u16()` | 2 | `need(2)`; `v = LE_u16(data+pos)`; `pos += 2`. |
| `u32()` | 4 | `need(4)`; `v = LE_u32(data+pos)`; `pos += 4`. |
| `i32()` | 4 | `need(4)`; `v = LE_i32(data+pos)` (two's complement); `pos += 4`. |
| `f32()` | 4 | `need(4)`; `v = LE_f32(data+pos)` (IEEE-754, LE byte order); `pos += 4`. |
| `bytes(len)` | len | `need(len)`; copy `len` bytes out (returns owned `Vec<u8>`); `pos += len`. |
| `plain_string()` | 2 + L | **uint16-length-prefixed UTF-8 string.** `L = u16()`; `need(L)`; decode `data[pos..pos+L]` as UTF-8 **lossily**; `pos += L`. This is the standard Growtopia string wire format used throughout tile extra-data. |
| `string_raw(len)` | len | UTF-8-lossy decode of `len` raw bytes, **no** length prefix; `pos += len`. |
| `xor_string(key, key_start)` | 2 + L | `L = u16()`; `need(L)`; for each byte i in `0..L`: `out[i] = data[pos+i] ^ key[(key_start + i) % key.len()]`; UTF-8-lossy decode; `pos += L`. **Not used by the world parser** (present for other protocol strings) — port for completeness, but the map blob never calls it. |

"UTF-8 lossy" = never fail on bad bytes; replace each invalid sequence with U+FFFD (`std::string` holding the replacement bytes `EF BF BD`).

**C++ note:** implement `LE_u16`/`LE_u32`/`LE_i32`/`LE_f32` by `memcpy` into the target type (do **not** cast the pointer — avoids alignment UB). On a little-endian host that is already correct byte order; if you ever target BE, byte-swap. GT/Adonai targets are LE.

---

## 3. Constants (`world/constants.rs`)

```cpp
static constexpr uint16_t MAP_VERSION_MIN   = 0x19;      // 25
static constexpr uint32_t MAX_TILE_COUNT    = 65026;     // hard cap; tile_count must be < this
static constexpr uint32_t MAX_WORLD_OBJECTS = 0x493E1;   // 300001; object count must be < this

// Tiles whose fg_item_id is in this set carry a trailing CBOR blob after their
// TileExtraData (see §6.4). The parser only skips it (reads u32 size, skips that many bytes).
static constexpr uint16_t CBOR_TILE_IDS[] = {
    15376, 15546, 3548, 14662, 14666,
    8624, 8630, 8636, 8642, 8648, 8654, 8660, 8666, 8672, 8678,
    8684, 8690, 8696, 8702, 8708, 8714,
};  // 21 entries
```

Use a `std::unordered_set<uint16_t>` or a sorted array + `std::binary_search` for membership.

---

## 4. Data model — structs & enums

### 4.1 `World`

```cpp
struct World {
    uint16_t              version;          // map format version (>= MAP_VERSION_MIN)
    uint32_t              flags;            // world flags bitfield (opaque; stored, not interpreted here)
    WorldTileMap          tile_map;
    std::vector<WorldObject> objects;       // dropped items on the ground
    uint32_t              next_object_uid;  // = last_dropped_uid + 1  (see §6.6)
    uint16_t              base_weather;
    uint16_t              current_weather;
};
```

Accessors:

- `get_tile(x, y) -> const Tile*`:
  `idx = y * tile_map.width + x` (compute in `size_t`; Rust uses `checked_mul`/`checked_add` and returns `None` on overflow — in C++ compute in `size_t`/`uint64_t` and bounds-check `idx < tiles.size()`). Return `&tiles[idx]` or `nullptr`.
- `get_tile_mut(x, y) -> Tile*`: same, mutable.

### 4.2 `WorldTileMap`

```cpp
struct WorldTileMap {
    std::string        world_name;   // UTF-8 (lossy) from the length-prefixed name field
    uint32_t           width;
    uint32_t           height;
    std::vector<Tile>  tiles;        // row-major: index = y*width + x, length == tile_count
};
```

### 4.3 `TileFlags` — `uint16_t` bitset

Exact bit values (do not renumber):

```cpp
enum TileFlag : uint16_t {
    HAS_EXTRA_DATA        = 0x0001,
    HAS_PARENT            = 0x0002,
    WAS_SPLICED           = 0x0004,
    WILL_SPAWN_SEEDS_TOO  = 0x0008,
    IS_SEEDLING           = 0x0010,
    FLIPPED_X             = 0x0020,
    IS_ON                 = 0x0040,
    IS_OPEN_TO_PUBLIC     = 0x0080,
    BG_IS_ON              = 0x0100,
    FG_ALT_MODE           = 0x0200,
    IS_WET                = 0x0400,
    GLUED                 = 0x0800,
    ON_FIRE               = 0x1000,
    PAINTED_RED           = 0x2000,
    PAINTED_GREEN         = 0x4000,
    PAINTED_BLUE          = 0x8000,
};
```

Only `HAS_EXTRA_DATA` and `HAS_PARENT` affect parsing. The rest are informational. Store `flags_raw` verbatim (retain undefined bits).

### 4.4 `Tile`

```cpp
struct Tile {
    uint16_t   fg_item_id;    // foreground block item id (0 = empty)
    uint16_t   bg_item_id;    // background item id
    uint16_t   parent_block;  // parent block field (raw)
    uint16_t   flags_raw;     // raw flags word
    // flags == flags_raw reinterpreted as TileFlags; keep raw, mask on query
    uint32_t   x;             // tile column (filled in by the map loop, not from the wire)
    uint32_t   y;             // tile row    (filled in by the map loop, not from the wire)
    TileType   tile_type;     // decoded extra data (default Basic)
};
```

Note: the Rust struct has both `flags: TileFlags` and `flags_raw: u16`. In C++ collapse to a single `uint16_t flags_raw` and a helper `bool has(TileFlag f) const { return flags_raw & f; }`.

### 4.5 `WorldObject`

```cpp
struct WorldObject {
    uint16_t item_id;   // note: ids 5996 and 1626 are remapped to 0 at parse time (§6.6)
    float    x;         // pixel coordinate (world space)
    float    y;         // pixel coordinate
    uint8_t  count;     // stack size
    uint8_t  flags;
    uint32_t uid;       // object unique id
};
```

### 4.6 `TileType` — tagged union of per-tile extra data

This is a Rust `enum` with data. In C++ use `std::variant<...>` **or** a `struct TileType { TileTypeKind kind; ... }` with a discriminant + the relevant fields (a variant-per-kind or a `std::variant` are both fine; a tagged struct with an anonymous union is most faithful). The default/most-common value is **`Basic`**.

Full variant list with **exact field names and types** (order matters only for the wire read in §6.5, not for storage). `Vec<T>` → `std::vector<T>`; `[u8; N]` → `std::array<uint8_t,N>`; `Option<T>` → `std::optional<T>`; `(u32,u32)` → `std::pair<uint32_t,uint32_t>` or a 2-field struct.

```
Basic
Door                       { std::string label; uint8_t flags; }
Sign                       { std::string label; }
Lock                       { uint8_t settings; uint32_t owner_uid; uint32_t access_count;
                             std::vector<uint32_t> access_uids; uint8_t minimum_level; }
Seed                       { uint32_t age; uint8_t item_on_tree; }
Dice                       { uint8_t symbol; }
Provider                   { uint32_t age; }
AchievementBlock           { uint32_t data; uint8_t tile_type; }
HearthMonitor              { uint32_t player_id; std::string player_name; }
Mannequin                  { std::string label; uint8_t unknown_1; uint16_t unknown_2; uint16_t unknown_3;
                             uint16_t hat; uint16_t shirt; uint16_t pants; uint16_t boots; uint16_t face;
                             uint16_t hand; uint16_t back; uint16_t hair; uint16_t neck; }
BunnyEgg                   { uint32_t egg_placed; }
GameGrave                  { uint8_t team; }
GameGenerator              (no fields)
XenoniteCrystal            { uint8_t unknown_1; uint32_t unknown_2; }
PhoneBooth                 { uint16_t hat; uint16_t shirt; uint16_t pants; uint16_t shoes; uint16_t face;
                             uint16_t hand; uint16_t back; uint16_t hair; uint16_t neck; }
Crystal                    { std::vector<uint8_t> crystals; }
CrimeInProgress            { std::string label; uint32_t unknown_1; uint8_t unknown_2; }
Spotlight                  (no fields)
DisplayBlock               { uint32_t item_id; }
VendingMachine             { uint32_t item_id; int32_t price; }
FishTankPort               { uint8_t flags; std::vector<std::pair<uint32_t,uint32_t>> fishes; }   // (item_id, lbs)
SolarCollector             { std::array<uint8_t,5> data; }
Forge                      { uint32_t temperature; }
GivingTree                 { uint8_t harvested; uint16_t age; uint16_t unknown_1; uint8_t decoration_percentage; }
SteamOrgan                 { uint8_t instrument_type; uint32_t note; }
SilkWorm                   { uint8_t flags; std::string name; uint32_t age; uint32_t unknown_1; uint32_t unknown_2;
                             uint8_t can_be_fed; uint32_t food_saturation; uint32_t water_saturation;
                             uint32_t color; uint32_t sick_duration; }
SewingMachine              { std::vector<uint32_t> bolt_ids; }
CountryFlag                { std::string country; }
LobsterTrap                (no fields)
PaintingEasel              { uint32_t item_id; std::string label; }
PetBattleCage              { std::string label; uint32_t base_pet; uint32_t pet_1; uint32_t pet_2; }
PetTrainer                 { std::string label; uint32_t pet_total_count; uint32_t unknown_1;
                             std::vector<uint32_t> pets_id; }
SteamEngine                { uint32_t temperature; }
LockBot                    { uint32_t age; }
WeatherMachine             { uint32_t settings; }
SpiritStorageUnit          { uint32_t ghost_jar_count; }
DataBedrock                (no fields)
Shelf                      { uint32_t top_left_item_id; uint32_t top_right_item_id;
                             uint32_t bottom_left_item_id; uint32_t bottom_right_item_id; }
VipEntrance                { uint8_t unknown_1; uint32_t owner_uid; std::vector<uint32_t> access_uids; }
ChallangeTimer             (no fields)   // (sic: spelled "Challange" in source — keep the identifier)
FishWallMount              { std::string label; uint32_t item_id; uint8_t lb; }
Portrait                   { std::string label; uint32_t unknown_1; uint32_t unknown_2;
                             std::array<uint8_t,5> unknown_3; uint8_t unknown_4; uint16_t unknown_5;
                             uint16_t face; uint16_t hat; uint16_t hair; uint32_t unknown_6;
                             std::optional<std::string> infinity_crown_data; }
WeatherMachine2            { uint32_t unknown_1; uint32_t gravity; uint8_t flags; }
FossilPrepStation          { uint32_t unknown_1; }
DnaExtractor               (no fields)
Howler                     (no fields)
ChemsynthTank              { uint32_t current_chem; uint32_t target_chem; }
StorageBlock               { std::vector<std::pair<uint32_t,uint32_t>> items; }   // (id, amount)
CookingOven                { uint32_t temperature_level; std::vector<std::pair<uint32_t,uint32_t>> ingredients; // (item_id, time_added)
                             uint32_t unknown_1; uint32_t unknown_2; uint32_t unknown_3; }
AudioRack                  { std::string note; uint32_t volume; }
GeigerCharger              { uint32_t unknown_1; }
AdventureBegins            (no fields)
TombRobber                 (no fields)
BalloonOMatic              { uint32_t total_rarity; uint8_t team_type; }
TrainingPort               { uint32_t fish_lb; uint16_t fish_status; uint32_t fish_id; uint32_t fish_total_exp;
                             std::array<uint8_t,8> unknown_1; uint32_t fish_level; uint32_t unknown_2;
                             std::array<uint8_t,5> unknown_3; }
ItemSucker                 { uint32_t item_id_to_suck; uint32_t item_amount; uint16_t flags; uint32_t limit; }
CyBot                      { std::vector<std::pair<uint32_t,uint32_t>> command_datas; // (command_id, is_command_used)
                             uint32_t sync_timer; uint32_t activated; }
GuildItem                  { std::array<uint8_t,17> unknown_1; }
Growscan                   { uint8_t unknown_1; }
ContainmentFieldPowerNode  { uint32_t time; std::vector<uint32_t> linked_nodes; }
SpiritBoard                { uint32_t player_required; std::string unk1; std::string command;
                             std::vector<uint32_t> required_items; }
TesseractManipulator       { uint32_t gems; uint32_t next_update_ms; uint32_t item_id; uint32_t enabled; }
StormyCloud                { uint32_t sting_duration; uint32_t is_solid; uint32_t non_solid_duration; }
TemporaryPlatform          { uint32_t unknown_1; }
SafeVault                  (no fields)
AngelicCountingCloud       { uint32_t state; uint16_t unknown_1; std::optional<uint8_t> ascii_code; }
InfinityWeatherMachine     { uint32_t interval_minutes; std::vector<uint32_t> weather_machine_list; }
PineappleGuzzler           { uint32_t pineapple_count; }
KrakenGalaticBlock         { uint8_t pattern_index; uint32_t unknown_1; uint8_t r; uint8_t g; uint8_t b; }
FriendsEntrance            { uint32_t owner_user_id; std::vector<uint32_t> allowed_friends_userid; }
Unknown                    { uint8_t kind; }   // fallback for unrecognized extra-data kind
```

(The commented-out variants in the Rust — `Mailbox`, `Bulletin`, `DonationBox`, `BigLock`, `GuildBlock` — are **not** implemented; their `kind` bytes fall into the `Unknown` fallback. Do not implement them.)

---

## 5. A* pathfinder types (`astar.rs`)

### 5.1 `AStar`

```cpp
struct AStar {
    uint32_t width  = 0;
    uint32_t height = 0;
    std::vector<uint8_t> grid;   // row-major collision grid, length == width*height, value = collision_type
    // cache key = (from_x, from_y, to_x, to_y, has_access); value = optional path as list of (x,y).
    std::unordered_map<PathKey, std::optional<std::vector<std::pair<uint32_t,uint32_t>>>> path_cache;
    uint32_t cache_hits   = 0;
    uint32_t cache_misses = 0;
};
```

`PathKey` = 5-tuple `(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, bool has_access)`. Provide `operator==` and a hash (e.g. combine the four u32 + the bool).

### 5.2 Open-list node `PathNode`

```cpp
struct PathNode {
    uint32_t f, g, h, x, y;
    // ctor: PathNode(x,y,g,h): f = g + h;   (f computed, others stored)
};
```

**Ordering (critical):** Rust implements `Ord` so that `BinaryHeap` (a max-heap) yields the **lowest f first**, tie-broken by **lowest h first**:

```
cmp(a, b) = compare(b.f, a.f) then_with compare(b.h, a.h)
```

In C++ `std::priority_queue` is a max-heap by default; supply a comparator that makes the queue pop the smallest f (then smallest h). Equivalent comparator `less` for `priority_queue` (returns true if `a` should come out **after** `b`):

```cpp
struct PathNodeGreater {
    bool operator()(const PathNode& a, const PathNode& b) const {
        if (a.f != b.f) return a.f > b.f;   // smaller f = higher priority
        return a.h > b.h;                    // tie-break: smaller h = higher priority
    }
};
std::priority_queue<PathNode, std::vector<PathNode>, PathNodeGreater> open_list;
```

### 5.3 Output node `Node`

```cpp
struct Node {
    uint32_t g = 0, h = 0, f = 0;   // always zero in the returned path (only x,y,collision_type meaningful)
    uint32_t x, y;
    uint8_t  collision_type;        // grid value at (x,y), or 0 if out of range
    // ctor: Node(x, y, collision_type): g=h=f=0
};
```

`find_path` returns `std::optional<std::vector<Node>>`. Each returned `Node` carries the `collision_type` read from the grid at its coordinate (0 if index out of range).

---

## 6. World blob byte layout & parse steps (`World::parse`)

Input: the full `extra_data` payload of the `SendMapData` game packet (a raw `&[u8]`). Parse **sequentially** through one `Cursor` labelled `"world map blob"`. All integers LE.

### 6.1 Top-level layout (`World::parse`)

| Step | Field | Type / size | Notes |
|---|---|---|---|
| 1 | `version` | u16 (2 B) | **if `version < 0x19` → fail** `"map version {version:#x} < minimum 0x19"`. |
| 2 | `flags` | u32 (4 B) | world flags, stored opaque. |
| 3 | tile map | variable | `WorldTileMap::parse` — see §6.2. |
| 4 | objects | variable | `parse_world_objects` — see §6.6. Returns `(objects, last_dropped_uid)`. |
| 5 | `base_weather` | u16 (2 B) | |
| 6 | *(pad)* | skip 2 B | discarded. |
| 7 | `current_weather` | u16 (2 B) | |
| — | `next_object_uid` | (computed) | `= last_dropped_uid + 1`. |

### 6.2 `WorldTileMap::parse(cur, map_version)`

| Step | Field | Type / size | Notes |
|---|---|---|---|
| 1 | `name_len` | u16 (2 B) | length of the world name in bytes. |
| 2 | `world_name` | `name_len` bytes | UTF-8 lossy. |
| 3 | `width` | u32 (4 B) | |
| 4 | `height` | u32 (4 B) | |
| 5 | `tile_count` | u32 (4 B) | number of tiles that follow. |
| 6 | *(pad)* | skip 5 B | discarded. |
| 7 | guard | — | **if `tile_count >= 65026` → fail** `"tile_count {n} >= limit 65026"`. |
| 8 | tiles | `tile_count` × Tile | loop `idx = 0 .. tile_count`. For each tile compute `x = idx % width`, `y = idx / width`, then `Tile::parse(cur, map_version, x, y)`. On a tile parse error, log `"[world] tile {idx} ({x},{y}) failed at pos {pos_before}: {err}"` and propagate the error (abort whole parse). `pos_before` is the cursor position captured **before** the failing tile. |
| 9 | *(pad)* | skip 12 B | discarded (trailer after tile array). |

`tiles` ends up length `tile_count`, row-major (`index = y*width + x`). Reserve capacity `tile_count`.

### 6.3 `Tile::parse(cur, map_version, x, y)`

`map_version` is currently unused by the tile parser (parameter kept for forward-compat). Steps:

| Step | Field | Type / size | Notes |
|---|---|---|---|
| 1 | `fg_item_id` | u16 (2 B) | |
| 2 | `bg_item_id` | u16 (2 B) | |
| 3 | `parent_block` | u16 (2 B) | |
| 4 | `flags_raw` | u16 (2 B) | reinterpret as `TileFlags`. |
| 5 | parent extra | u16 (2 B) **conditional** | **if `flags & HAS_PARENT (0x0002)`**: read and **discard** one u16. This read happens *before* the extra-data block. |
| 6 | `tile_type` | variable **conditional** | default `Basic`. **if `flags & HAS_EXTRA_DATA (0x0001)`**: `kind = u8()`, then `tile_type = parse_tile_extra(cur, kind, fg_item_id)` (§6.5). |
| 7 | CBOR blob | u32 + N bytes **conditional** | **if `fg_item_id` ∈ `CBOR_TILE_IDS`**: `cbor_size = u32()`; `skip(cbor_size)`. Raw CBOR bytes are discarded (not decoded). This is checked **after** extra-data, unconditionally for those ids. |

Field order on the wire is exactly: fg, bg, parent_block, flags, [parent-u16 if HAS_PARENT], [1-byte kind + extra if HAS_EXTRA_DATA], [u32 size + CBOR bytes if fg in CBOR set]. `x`/`y` are supplied by the caller, not read.

### 6.4 CBOR note

The parser never *decodes* CBOR — it only reads a `u32` length and skips that many bytes. Adonai should do the same (skip). The 21 ids are in `CBOR_TILE_IDS` (§3).

### 6.5 `parse_tile_extra(cur, kind, fg_item_id)` — extra-data dispatch

`kind` is the 1-byte discriminant read in Tile step 6. Dispatch on it. Every read below is sequential through the same cursor. **Unlisted `kind` values (including 0, 5, 6, 7, 12, 13, 29, 46, 64, 70, 71, 76, 78, and anything > 81) hit the default branch:** log `"[world] WARNING: unknown TileExtraData kind {kind:#x} at fg_item={fg_item_id}"`, return `TileType::Unknown{kind}`, and consume **no further bytes**. (Edge case: because the byte length of an unknown block is unknown, the cursor is then misaligned and the *next* tile will very likely fail — this is the existing behavior; do not try to "recover" differently.)

`str` below = `plain_string()` (u16 length + UTF-8 bytes). All ints LE.

| kind (dec / hex) | Variant | Fields read, in order |
|---|---|---|
| 1 / 0x01 | Door | `str label`; `u8 flags` |
| 2 / 0x02 | Sign | `str label`; `u32` (always 0xFFFFFFFF — **discard**) |
| 3 / 0x03 | Lock | `u8 settings`; `u32 owner_uid`; `u32 access_count`; `access_count × u32 access_uids`; `u8 minimum_level`; **skip 7**; **if `fg_item_id == 5814` (Guild Lock): skip 16** |
| 4 / 0x04 | Seed | `u32 age`; `u8 item_on_tree` |
| 8 / 0x08 | Dice | `u8 symbol` |
| 9 / 0x09 | Provider | `u32 age`; **if `fg_item_id == 10656`: skip 4** |
| 10 / 0x0A | AchievementBlock | `u32 data`; `u8 tile_type` |
| 11 / 0x0B | HearthMonitor | `u32 player_id`; `str player_name` |
| 14 / 0x0E | Mannequin | `str label`; `u8 unknown_1`; `u16 unknown_2`; `u16 unknown_3`; `u16 hat`; `u16 shirt`; `u16 pants`; `u16 boots`; `u16 face`; `u16 hand`; `u16 back`; `u16 hair`; `u16 neck` |
| 15 / 0x0F | BunnyEgg | `u32 egg_placed` |
| 16 / 0x10 | GameGrave | `u8 team` |
| 17 / 0x11 | GameGenerator | (nothing) |
| 18 / 0x12 | XenoniteCrystal | `u8 unknown_1`; `u32 unknown_2` |
| 19 / 0x13 | PhoneBooth | `u16 hat`; `u16 shirt`; `u16 pants`; `u16 shoes`; `u16 face`; `u16 hand`; `u16 back`; `u16 hair`; `u16 neck` |
| 20 / 0x14 | Crystal | `u16 crystal_count`; `crystal_count × u8 crystals` |
| 21 / 0x15 | CrimeInProgress | `str label`; `u32 unknown_1`; `u8 unknown_2` |
| 22 / 0x16 | Spotlight | (nothing) |
| 23 / 0x17 | DisplayBlock | `u32 item_id` |
| 24 / 0x18 | VendingMachine | `u32 item_id`; `i32 price` (signed) |
| 25 / 0x19 | FishTankPort | `u8 flags`; `u32 fish_count`; loop **`fish_count / 2`** times: `{ u32 fish_item_id; u32 lbs }` → push `(fish_item_id, lbs)` |
| 26 / 0x1A | SolarCollector | `5 × u8` → `data[5]` |
| 27 / 0x1B | Forge | `u32 temperature` |
| 28 / 0x1C | GivingTree | `u8 harvested`; `u16 age`; `u16 unknown_1`; `u8 decoration_percentage` |
| 30 / 0x1E | SteamOrgan | `u8 instrument_type`; `u32 note` |
| 31 / 0x1F | SilkWorm | `u8 flags`; `str name`; `u32 age`; `u32 unknown_1`; `u32 unknown_2`; `u8 can_be_fed`; `u32 food_saturation`; `u32 water_saturation`; `u32 color`; `u32 sick_duration` |
| 32 / 0x20 | SewingMachine | `u32 bolt_len`; `bolt_len × u32 bolt_ids` |
| 33 / 0x21 | CountryFlag | **if `fg_item_id == 3394`: `str country`; else `country = ""` (read nothing)** |
| 34 / 0x22 | LobsterTrap | (nothing) |
| 35 / 0x23 | PaintingEasel | `u32 item_id`; `str label` |
| 36 / 0x24 | PetBattleCage | `str label`; `u32 base_pet`; `u32 pet_1`; `u32 pet_2` |
| 37 / 0x25 | PetTrainer | `str label`; `u32 pet_total_count`; `u32 unknown_1`; `pet_total_count × u32 pets_id` |
| 38 / 0x26 | SteamEngine | `u32 temperature` |
| 39 / 0x27 | LockBot | `u32 age` |
| 40 / 0x28 | WeatherMachine | `u32 settings` |
| 41 / 0x29 | SpiritStorageUnit | `u32 ghost_jar_count` |
| 42 / 0x2A | DataBedrock | **skip 21** (unk1[17] + pad[4]); no stored fields |
| 43 / 0x2B | Shelf | `u32 top_left_item_id`; `u32 top_right_item_id`; `u32 bottom_left_item_id`; `u32 bottom_right_item_id` |
| 44 / 0x2C | VipEntrance | `u8 unknown_1`; `u32 owner_uid`; `u32 access_count`; `access_count × u32 access_uids` |
| 45 / 0x2D | ChallangeTimer | (nothing) |
| 47 / 0x2F | FishWallMount | `str label`; `u32 item_id`; `u8 lb` |
| 48 / 0x30 | Portrait | `str label`; `u32 unknown_1`; `u32 unknown_2`; `5 × u8 → unknown_3[5]`; `u8 unknown_4`; `u16 unknown_5`; `u16 face`; `u16 hat`; `u16 hair`; `u32 unknown_6`; **if `hat == 12958`: `str infinity_crown_data` (Some) else None** |
| 49 / 0x31 | WeatherMachine2 | `u32 unknown_1`; `u32 gravity`; `u8 flags` |
| 50 / 0x32 | FossilPrepStation | `u32 unknown_1` |
| 51 / 0x33 | DnaExtractor | (nothing) |
| 52 / 0x34 | Howler | (nothing) |
| 53 / 0x35 | ChemsynthTank | `u32 current_chem`; `u32 target_chem` |
| 54 / 0x36 | StorageBlock | `u16 data_len`; loop **`data_len / 13`** times: `{ skip 3; u32 id; skip 2; u32 amount }` → push `(id, amount)` |
| 55 / 0x37 | CookingOven | `u32 temperature_level`; `u32 ingredient_count`; loop **`ingredient_count / 2`** times: `{ u32 item_id; u32 time_added }` → push `(item_id, time_added)`; then `u32 unknown_1`; `u32 unknown_2`; `u32 unknown_3` |
| 56 / 0x38 | AudioRack | `str note`; `u32 volume` |
| 57 / 0x39 | GeigerCharger | `u32 unknown_1` |
| 58 / 0x3A | AdventureBegins | (nothing) |
| 59 / 0x3B | TombRobber | (nothing) |
| 60 / 0x3C | BalloonOMatic | `u32 total_rarity`; `u8 team_type` |
| 61 / 0x3D | TrainingPort | `u32 fish_lb`; `u16 fish_status`; `u32 fish_id`; `u32 fish_total_exp`; `8 × u8 → unknown_1[8]`; `u32 fish_level`; `u32 unknown_2`; `5 × u8 → unknown_3[5]` |
| 62 / 0x3E | ItemSucker | `u32 item_id_to_suck`; `u32 item_amount`; `u16 flags`; `u32 limit` |
| 63 / 0x3F | CyBot | `u32 command_data_count`; loop `command_data_count` times: `{ u32 command_id; u32 is_command_used; skip 7 }` → push `(command_id, is_command_used)`; then `u32 sync_timer`; `u32 activated` |
| 65 / 0x41 | GuildItem | `17 × u8 → unknown_1[17]` |
| 66 / 0x42 | Growscan | `u8 unknown_1` |
| 67 / 0x43 | ContainmentFieldPowerNode | `u32 time`; `u32 linked_node_count`; `linked_node_count × u32 linked_nodes` |
| 68 / 0x44 | SpiritBoard | `u32 player_required`; `str unk1`; `str command`; `u32 num_required_items`; `num_required_items × u32 required_items` |
| 69 / 0x45 | TesseractManipulator | `u32 gems`; `u32 next_update_ms`; `u32 item_id`; `u32 enabled` |
| 72 / 0x48 | StormyCloud | `u32 sting_duration`; `u32 is_solid`; `u32 non_solid_duration` |
| 73 / 0x49 | TemporaryPlatform | `u32 unknown_1` |
| 74 / 0x4A | SafeVault | (nothing) |
| 75 / 0x4B | AngelicCountingCloud | `u32 state`; `u16 unknown_1`; **if `state == 2`: `u8 ascii_code` (Some) else None** |
| 77 / 0x4D | InfinityWeatherMachine | `u32 interval_minutes`; `u32 list_size`; `list_size × u32 weather_machine_list` |
| 79 / 0x4F | PineappleGuzzler | `u32 pineapple_count` |
| 80 / 0x50 | KrakenGalaticBlock | `u8 pattern_index`; `u32 unknown_1`; `u8 r`; `u8 g`; `u8 b` |
| 81 / 0x51 | FriendsEntrance | `u32 owner_user_id`; **skip 2**; `u16 num_allowed` (widened to u32); `num_allowed × u32 allowed_friends_userid` |
| default | Unknown | log warning; store `{ kind }`; read nothing else |

**Loop-count gotchas to preserve exactly:**
- FishTankPort uses `fish_count / 2` iterations but each iteration reads **two** u32s (so `fish_count` counts u32 words, not pairs).
- CookingOven uses `ingredient_count / 2` similarly.
- StorageBlock uses `data_len / 13` and each iteration consumes exactly 13 bytes (3 skip + 4 id + 2 skip + 4 amount).
- CyBot: each command entry is 15 bytes (4 + 4 + 7 skip).
- FriendsEntrance reads a **u16** count (after a 2-byte skip), not a u32.
- Sign discards a trailing u32; Lock/Provider/CountryFlag/Portrait/AngelicCountingCloud have `fg_item_id`- or value-conditional extra reads (5814→+16, 10656→+4, 3394→string, hat 12958→string, state 2→u8).

### 6.6 `parse_world_objects(cur) -> (objects, last_dropped_uid)`

| Step | Field | Type / size | Notes |
|---|---|---|---|
| 1 | `count` | u32 (4 B) | number of ground objects. |
| 2 | `last_dropped_uid` | u32 (4 B) | used to seed `World::next_object_uid = last_dropped_uid + 1`. |
| 3 | guard | — | **if `count >= 0x493E1` (300001) → fail** `"world object count {n} >= limit 300001"`. |
| 4 | objects | `count` × record | each record (12 bytes): `u16 item_id`, `f32 x`, `f32 y`, `u8 count`, `u8 flags`, `u32 uid`. |

**Item-id remap (apply per object):** after reading `item_id`, if it is `5996` **or** `1626`, store `item_id = 0`. (These are treated as "empty" markers.) All other ids stored as-is.

Record byte layout (offsets within one 12-byte record):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `item_id` (u16 LE) |
| 2 | 4 | `x` (f32 LE) |
| 6 | 4 | `y` (f32 LE) |
| 10 | 1 | `count` (u8) |
| 11 | 1 | `flags` (u8) |
| 12 | 4 | `uid` (u32 LE) |

(Record total = 16 bytes; the "12 bytes minimum" note in the incremental-update packet §7.3 is a *different* format.)

### 6.7 Failure / edge behavior for `World::parse`

- Any truncation from the cursor aborts the whole parse (return error). Caller (`bot/core.rs`) logs `"[Bot] World parse error: {e}"` and leaves `world = None`.
- Version below `0x19` → error.
- `tile_count >= 65026` or object `count >= 300001` → error (defensive bounds vs. corrupt/malicious blobs).
- Unknown extra-data kinds do **not** abort immediately (they return `Unknown`), but they desync the cursor and usually cause the *next* tile to fail-and-abort. Preserve this.

---

## 7. Incremental tile update — `update_tile_from_bytes(x, y, data)`

Applies a raw `SendTileUpdateData` blob to an existing tile. Returns `std::optional<std::pair<uint16_t,uint16_t>>` = new `(fg, bg)` on success, `nullopt` if too short or the tile is out of range.

Layout of `data` (little-endian): `fg(u16) bg(u16) parent(u16) flags(u16) [kind(u8) extra...]`.

Steps (exact):

1. **If `data.len() < 4` → return `nullopt`.**
2. `fg = LE_u16(data[0..2])`, `bg = LE_u16(data[2..4])`.
3. `tile = get_tile_mut(x, y)`; if `nullptr` → return `nullopt`.
4. `tile.fg_item_id = fg`; `tile.bg_item_id = bg`.
5. **If `data.len() >= 8`:**
   a. `flags_raw = LE_u16(data[6..8])` (note: bytes `[4..6]` = parent are skipped here); set `tile.flags_raw = flags_raw` and recompute flags.
   b. **If `(flags_raw & HAS_EXTRA_DATA)` and `data.len() >= 9`:**
      - `kind = data[8]`.
      - **If `kind == 4` and `data.len() >= 14`:** `age = LE_u32(data[9..13])`, `item_on_tree = data[13]` → `tile.tile_type = Seed{age, item_on_tree}`.
      - **Else:** `tile.tile_type = Basic`.
   c. **Else if `fg == 0`:** `tile.tile_type = Basic`.
6. **Else (`data.len() < 8`) and `fg == 0`:** `tile.tile_type = Basic`.
7. Return `(fg, bg)`.

Only the `Seed` extra-kind is decoded here (the general dispatch of §6.5 is **not** re-run for incremental updates). All other extra-data kinds collapse to `Basic` on update.

**Callers (context, not part of this module):**
- `on_send_tile_update_data` (single tile): `x = pkt.int_x`, `y = pkt.int_y`, `data = pkt.extra_data`; then mirror `(fg,bg)` into shared UI state at `idx = y*width + x`.
- `on_send_tile_update_data_multiple`: `data` = `u32 count`, then per entry `i32 x (4)`, `i32 y (4)`, then the tile blob starting at `+8` (passed to `update_tile_from_bytes`). Each entry is validated with `offset + 12 > data.len() → break`. Note the multiple-update header advances `offset` by parsing x/y but hands the **remainder** (`&data[offset+8..]`) to `update_tile_from_bytes`, which then re-reads from its own offset 0.
- `on_tile_change` (TileChangeRequest echo): if `item_id == 18` (a "punch"/fist id): clear `fg` (set 0 and `Basic`) if fg≠0, else clear `bg`; otherwise set `fg = item_id`. Not via `update_tile_from_bytes`.

---

## 8. Threading & shared state

**Per-bot ownership.** Each bot runs on its own OS thread (in Adonai: `std::thread`). The `World`, the `AStar` instance, and all parsing live **inside** the bot's core object and are touched only by that bot's thread. There is **no** cross-bot sharing of the `World`/`AStar` themselves. Relevant fields on the bot core (`bot/core.rs`):

```
world: Option<World>      // set on SendMapData (World::parse); mutated by tile/object updates
astar: AStar              // reused across find_path calls; rebuilt from tiles each pathfind
```

**Lifecycle on `SendMapData`:** clear players/local-player state, then `World::parse(extra_data)`. On success, `self.world = Some(world)`, and a **snapshot** is copied under a write-lock into a shared `state` (an `RwLock`-guarded struct) that the UI reads: `world_name`, `world_width`, `world_height`, a `Vec<TileInfo>` (fg, bg, flags, tile_type clone), a `Vec<WorldObjectInfo>` (uid, item_id, x, y, count), status → `InGame`. In Adonai this shared mirror is what the Dear ImGui UI thread reads; guard it with a `std::mutex` (or `std::shared_mutex`) — the Rust `RwLock` uses `unwrap_or_else(PoisonError::into_inner)` (i.e. it ignores lock poisoning), so a plain mutex is a faithful port.

**Object add/remove** (dropped items) mutate `world.objects` and bump `world.next_object_uid` (`next_uid = world.next_object_uid; world.next_object_uid += 1;`) — single-threaded within the bot, no atomics needed.

**AStar rebuild is per-call.** Before each pathfind the bot rebuilds the collision grid from the current tiles (see §9.2) and calls `astar.update_from_tiles(...)`. `update_from_tiles`/`update_from_collision_data` clear the path cache when the grid content changes (and reset dimensions when width/height change). This is all single-threaded per bot.

**Fleet-wide shared state (Adonai requirement).** In Mori bots do **not** see each other's worlds. For Adonai's "bots aware of each other" goal, the natural extension is a fleet registry keyed by world name: each bot, after a successful `World::parse`, could publish `{bot_id, world_name, pos}` into a shared, mutex-guarded fleet map so other bots in the *same* world can coordinate. The per-bot `World`/`AStar` should stay thread-local (they are large and mutated constantly); share only lightweight summaries (world name, tile deltas, positions) through your `std::mutex`+`std::condition_variable` queue (the crossbeam-channel replacement). Do **not** share the `AStar.path_cache` across bots — it is keyed by `(from,to,has_access)` specific to one bot's grid.

---

## 9. Coordinate & collision helpers (used with this module)

These live in the callers but are load-bearing for the world/pathfinding port. Reproduce exactly.

### 9.1 Pixel ↔ tile

- **Tile size = 32.0 px** (constant `32.0` throughout).
- Pixel → tile (truncating, used for pathfinding endpoints): `tile = (uint32_t)(pixel / 32.0f)` — truncation toward zero (C++ float→uint cast matches Rust `as u32` for non-negative values).
- Pixel → tile (flooring, used for build/place offset math): `base = (int32_t)std::floor(pixel / 32.0f)`.
- Tile → pixel: `pixel = tile * 32.0f`.
- Linear tile index: `idx = y * width + x` (compute in `size_t`/`uint64_t`; the multi-tile updater uses `u64` to avoid overflow).
- Collect radius: `r_px = clamp(radius_tiles, 1, 5) * 32.0f`; an object at `(ox,oy)` is "near" `(px,py)` if `|px-ox| <= r_px && |py-oy| <= r_px`, ranked by Chebyshev ring `max(|dx|,|dy|)`.

### 9.2 Building the collision grid from tiles

`AStar.update_from_tiles(width, height, tiles)` takes `tiles: &[(u16 fg_item_id, u8 collision_type)]` and copies the `collision_type` of each into `grid` (row-major). The caller builds each tile's `collision_type` as follows:

```
collision_type(tile) =
    if tile.tile_type == Door        -> 0                    // doors always passable
    else if tile.tile_type == Lock   -> 3                    // (only in the item-collect path; see note)
    else:
        ct = items_dat.find_by_id(tile.fg_item_id)?.collision_type
        if not found: (tile.fg_item_id == 0 ? 0 : 1)         // unknown non-empty block = solid
```

- `collision_type` originates from the **items database** (`items.dat`): it is `u8` field at a fixed position in each item record (read as `render_type, is_stripey_wallpaper, collision_type, block_health, ...`). Adonai's items module owns this; the world module just consumes `item.collision_type`.
- **Note the two grid-build variants in the source:** the item-collection path maps `Lock → 3` **and** `Door → 0`; the general `find_path` path maps only `Door → 0` and lets `Lock` fall through to the items-db lookup. Preserve both behaviors where you port those two call sites. (The `Lock → 3` mapping combined with `is_blocked`'s `ct==3 → !has_access` means locked areas are walkable only when the bot has access.)

### 9.3 `has_access()` (drives the `has_access` A* parameter)

```
LOCK_ITEM_IDS = [242, 1796, 2408, 7188, 10410]
For each tile in the world:
    if tile.fg_item_id ∈ LOCK_ITEM_IDS and tile.tile_type is Lock{access_uids,...}:
        if access_uids contains this bot's user_id -> return true
return false
```

---

## 10. A* algorithm (`astar.rs`) — full behavior

### 10.1 Grid maintenance

- `new()` → all zero/empty.
- `reset()`: `width=0; height=0; grid.clear();` and **clear `path_cache` only if it has > 1000 entries** (otherwise keep the cache — it survives resets to preserve hits across small worlds).
- `update_from_collision_data(width, height, data)`: if dimensions changed → `reset()` then set width/height and `grid.reserve_exact(data.len())`; **else** `path_cache.clear()`. Then `grid.clear()` and copy `data` into `grid`.
- `update_from_tiles(width, height, tiles)`: same dimension logic; then `grid.clear()` and push each `tiles[i].collision_type` (the `fg_item_id` is ignored for the grid). **Effect:** every rebuild with unchanged dimensions clears the whole path cache (fresh tiles ⇒ stale paths).
- `update_single_tile(x, y, ct)`: if in-bounds and `idx < grid.len()`: `grid[idx] = ct`, then **invalidate cache entries whose start OR goal is within the 3×3 neighborhood of (x,y)**. Exact Rust predicate retained (removes entries where the condition is true):
  ```
  remove entry (fx,fy,tx,ty,_) if:
     (fx <= x+1 && fx+1 >= x && fy <= y+1 && fy+1 >= y)   // start near the changed tile
   ||(tx <= x+1 && tx+1 >= x && ty <= y+1 && ty+1 >= y)   // goal near the changed tile
  ```
  (These use unsigned arithmetic; `x+1`/`fx+1` can be relied upon since coords are small. Reproduce with the same `<=`/`>=` bounds.)

### 10.2 `is_blocked(index, has_access) -> bool`

```
ct = grid.get(index)            // out-of-range index -> treated as blocked (true)
if index out of range: return true
if ct == 1 || ct == 6: return true          // solid
if ct == 3:            return !has_access    // access-gated (locked)
else:                  return false          // passable (includes 0, 2, 4, 5, 7...)
```

### 10.3 Cost / heuristic

- `movement_cost(from, to)`: `dx=|fx-tx|, dy=|fy-ty|`; **diagonal (`dx==1 && dy==1`) → 14, else → 10.** (Octile costs, 10 = orthogonal step, 14 ≈ 10·√2.)
- `calculate_h(from, to)` (octile heuristic): `dx=|fx-tx|, dy=|fy-ty|`; `h = 14*min(dx,dy) + 10*|dx-dy|`.

### 10.4 `find_path(from_x, from_y, to_x, to_y, has_access) -> Option<Vec<Node>>`

1. **Cache check.** `key = (from_x,from_y,to_x,to_y,has_access)`. If present: `cache_hits++`; if the cached value is `Some(path)`, rebuild `Vec<Node>` by mapping each `(x,y)` → `Node(x, y, grid[y*width+x] or 0 if OOB)`; if cached value is `None`, return `None`. Return immediately.
2. `cache_misses++`.
3. **Bounds:** if `from_x >= width || from_y >= height || to_x >= width || to_y >= height` → cache `None`, return `None`.
4. `start_index = from_y*width + from_x`; `end_index = to_y*width + to_x`.
5. If `is_blocked(start_index, has_access) || is_blocked(end_index, has_access)` → cache `None`, return `None`.
6. **Trivial path:** if `from == to` → path = `[(from_x,from_y)]`; cache `Some(path)`; return single `Node(from_x, from_y, grid[start_index])`.
7. **Search.** `open_list` = the min-heap (§5.2). `came_from: map<(x,y),(x,y)>`, `g_scores: map<(x,y),u32>`, `closed_set: set<(x,y)>` (all reserve ~256). Push `PathNode(from_x, from_y, g=0, h=calculate_h(from,to))`; `g_scores[from] = 0`.
8. Loop while `open_list` non-empty; pop `current` (lowest f, then lowest h):
   - If `current == to`: `path = reconstruct_optimized_path(came_from, current, from)`; cache `Some(path)`; return mapped `Vec<Node>` (each node's collision_type from grid, 0 if OOB).
   - If `current` in `closed_set`: `continue`.
   - Insert `current` into `closed_set`.
   - `process_neighbors(...)` (§10.5).
9. If loop exhausts with no goal: cache `None`, return `None`.

### 10.5 `process_neighbors`

Neighbor directions, **in this exact order** (order affects tie-breaking / determinism):
```
(-1, 0), (1, 0), (0, -1), (0, 1), (-1,-1), (-1, 1), (1,-1), (1, 1)
```
For each `(dx,dy)`:
1. `nx = current.x + dx`, `ny = current.y + dy` (compute signed). If `nx<0 || ny<0 || nx>=width || ny>=height` → skip.
2. If `(nx,ny)` in `closed_set` → skip.
3. `index = ny*width + nx`; if `is_blocked(index, has_access)` → skip.
4. **Diagonal corner-cut prevention:** if `dx!=0 && dy!=0`, both orthogonally adjacent tiles must be passable:
   - `adj1 = (current.x + dx, current.y)`, `adj2 = (current.x, current.y + dy)`.
   - if `is_blocked(adj1_index, has_access) || is_blocked(adj2_index, has_access)` → skip.
5. `tentative_g = current.g + movement_cost(current, neighbor)`.
6. If `g_scores` has `(nx,ny)` and `tentative_g >= existing_g` → skip.
7. `g_scores[(nx,ny)] = tentative_g`; `came_from[(nx,ny)] = (current.x, current.y)`; `h = calculate_h(neighbor, target)`; push `PathNode(nx, ny, tentative_g, h)`.

(Note: nodes are **not** decrease-key'd; duplicates may be pushed. The `closed_set` check at pop time and the `g_scores` guard make stale duplicates harmless — the first pop of a coordinate wins.)

### 10.6 `reconstruct_optimized_path(came_from, current, start) -> Vec<(u32,u32)>`

```
path = []
while current != start:
    path.push(current)
    current = came_from[current]   // if missing -> break
path.push(start)
reverse(path)
return path
```
Result is start→…→goal inclusive.

### 10.7 Misc

- `clear_cache()`: `path_cache.clear(); cache_hits = 0; cache_misses = 0;`
- `cache_stats() -> (hits, misses, hit_rate)`: `total = hits + misses; hit_rate = total>0 ? hits/total*100.0 : 0.0` (percent, f32).

---

## 11. Reference: minimal end-to-end map blob byte map

For quick sanity while porting (offsets are cumulative from blob start; variable regions marked ⇒):

```
0x00  u16   version                 (>= 0x19)
0x02  u32   flags
0x06  u16   name_len (= N)
0x08  N     world_name bytes
      u32   width
      u32   height
      u32   tile_count (< 65026)
      5     (skip)
      ⇒     tile_count × Tile   (each: u16 fg, u16 bg, u16 parent, u16 flags,
                                  [+u16 if HAS_PARENT], [+u8 kind + extra if HAS_EXTRA_DATA],
                                  [+u32 size + CBOR bytes if fg in CBOR_TILE_IDS])
      12    (skip)
      u32   object_count (< 300001)
      u32   last_dropped_uid
      ⇒     object_count × Object (u16 item_id, f32 x, f32 y, u8 count, u8 flags, u32 uid)  [16 B each]
      u16   base_weather
      2     (skip)
      u16   current_weather
--- next_object_uid = last_dropped_uid + 1 (computed, not on wire) ---
```

This is the complete, exact behavior of the `10-world` module. Reproduce byte offsets, skip counts, conditional reads, loop divisors, item-id remaps, flag bit values, A* costs/heuristic/tie-breaking, and cache-invalidation predicates exactly as specified.
