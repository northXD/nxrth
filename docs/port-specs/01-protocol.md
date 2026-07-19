# Nxrth Port Spec 01 — `protocol` module

**Source (Mori 2.0.0, Rust):** `src/protocol/packet.rs`, `src/protocol/variant.rs`, `src/protocol/crypto.rs`, `src/protocol/mod.rs`
**Target (Nxrth, C++):** `nxrth::protocol` namespace — suggested files `protocol/packet.hpp/.cpp`, `protocol/variant.hpp/.cpp`, `protocol/crypto.hpp/.cpp`.

This module is the Growtopia (GT) wire codec: outer ENet message framing, the tank/GameUpdatePacket 56-byte struct, VariantList deserialization, and the login crypto (`klv`, GT hash). **GT is little-endian on the wire; every multi-byte integer/float below is LE unless stated otherwise.** This spec is the single source of truth — reimplement in C++ without reading the Rust.

> **Scope note on `crc32`:** the FOCUS mentions `crc32`, but **no `crc32` function exists in any of these four files.** `hash_string` (the GT rotate-left-5 hash) *is* here and is documented below. If Nxrth needs CRC32 it lives in a different Mori module (likely login / item-database) and must be specced separately. Do not invent one here.

---

## 0. Module layout & re-exports

`protocol/mod.rs` is only:
```rust
pub mod crypto;
pub mod packet;
pub mod variant;
```
No logic. In C++ this is just the three headers under `protocol/`. No literal `Mori`/`mori`/`Cloei`/`cloei` string appears anywhere in these four files (see §12 for the namespace/rename implications).

---

## 1. Outer message types (`packet.rs`)

These are the 4-byte LE `u32` message-type prefix on every ENet packet payload (the value at byte offset 0 of the ENet packet data).

| Name | Value | Notes |
|------|-------|-------|
| `MSG_SERVER_HELLO` | `1` | |
| `MSG_TEXT` | `2` | text/action packet, NUL-terminated text |
| `MSG_GAME_MESSAGE` | `3` | game message, NUL-terminated text |
| `MSG_GAME_PACKET` | `4` | tank / GameUpdatePacket (the 56-byte struct) |
| `MSG_TRACK` | `6` | telemetry/track string |
| `MSG_CLIENT_LOG_REQUEST` | `7` | |

**Value `5` is intentionally not defined** (GT's `NET_MESSAGE_ERROR`); it falls through to `Unknown` (§6). Keep these exact numeric values.

C++: `constexpr uint32_t MSG_* = ...;`

---

## 2. `GamePacketType` enum (tank-packet subtype, byte [0] of the 56-byte struct)

`#[repr(u8)]`. Default variant = `State (0x00)`. This is the "0x?? packet type enum" from the FOCUS. Full table (value == the on-wire byte):

| Variant | Byte |
|---------|------|
| `State` | `0x00` (default) |
| `CallFunction` | `0x01` |
| `UpdateStatus` | `0x02` |
| `TileChangeRequest` | `0x03` |
| `SendMapData` | `0x04` |
| `SendTileUpdateData` | `0x05` |
| `SendTileUpdateDataMultiple` | `0x06` |
| `TileActivateRequest` | `0x07` |
| `TileApplyDamage` | `0x08` |
| `SendInventoryState` | `0x09` |
| `ItemActivateRequest` | `0x0A` |
| `ItemActivateObjectRequest` | `0x0B` |
| `SendTileTreeState` | `0x0C` |
| `ModifyItemInventory` | `0x0D` |
| `ItemChangeObject` | `0x0E` |
| `SendLock` | `0x0F` |
| `SendItemDatabaseData` | `0x10` |
| `SendParticleEffect` | `0x11` |
| `SetIconState` | `0x12` |
| `ItemEffect` | `0x13` |
| `SetCharacterState` | `0x14` |
| `PingReply` | `0x15` |
| `PingRequest` | `0x16` |
| `GotPunched` | `0x17` |
| `AppCheckResponse` | `0x18` |
| `AppIntegrityFail` | `0x19` |
| `Disconnect` | `0x1A` |
| `BattleJoin` | `0x1B` |
| `BattleEvent` | `0x1C` |
| `UseDoor` | `0x1D` |
| `SendParental` | `0x1E` |
| `GoneFishin` | `0x1F` |
| `Steam` | `0x20` |
| `PetBattle` | `0x21` |
| `Npc` | `0x22` |
| `Special` | `0x23` |
| `SendParticleEffectV2` | `0x24` |
| `ActiveArrowToItem` | `0x25` |
| `SelectTileIndex` | `0x26` |
| `SendPlayerTributeData` | `0x27` |
| `FtueSetItemToQuickInventory` | `0x28` |
| `PveNpc` | `0x29` |
| `PvpCardBattle` | `0x2A` |
| `PveApplyPlayerDamage` | `0x2B` |
| `PveNpcPositionUpdate` | `0x2C` |
| `SetExtraMods` | `0x2D` |
| `OnStepOnTileMod` | `0x2E` |
| `Unknown(u8)` | any other byte (carries the raw byte) |

**Conversions (must preserve round-trip):**
- `from(u8)`: exact match table above; any byte not `0x00..=0x2E` → `Unknown(byte)`.
- `as_u8()`: inverse; `Unknown(v)` → `v`.

C++ recommendation: an `enum class GamePacketType : uint8_t {...}` covering `0x00..0x2E`, but because you must retain arbitrary unknown bytes, store the subtype as a plain `uint8_t` in the struct and provide `to_enum(uint8_t)` / helper predicates, **or** a small tagged struct `{ uint8_t raw; }`. Do not clamp/normalize unknown bytes — preserve them verbatim so `to_bytes` round-trips.

---

## 3. `PacketFlags` (bitflags over `u32`, at byte offset [12] of the struct)

28 named bits. Values are exact:

| Flag | Bit |
|------|-----|
| `WALK` | `0x0000_0001` |
| `UNK_2` | `0x0000_0002` |
| `SPAWN_RELATED` | `0x0000_0004` |
| `EXTENDED` | `0x0000_0008` |
| `FACING_LEFT` | `0x0000_0010` |
| `STANDING` | `0x0000_0020` |
| `FIRE_DAMAGE` | `0x0000_0040` |
| `JUMP` | `0x0000_0080` |
| `GOT_KILLED` | `0x0000_0100` |
| `PUNCH` | `0x0000_0200` |
| `PLACE` | `0x0000_0400` |
| `TILE_CHANGE` | `0x0000_0800` |
| `GOT_PUNCHED` | `0x0000_1000` |
| `RESPAWN` | `0x0000_2000` |
| `OBJECT_COLLECT` | `0x0000_4000` |
| `TRAMPOLINE` | `0x0000_8000` |
| `DAMAGE` | `0x0001_0000` |
| `SLIDE` | `0x0002_0000` |
| `PARASOL` | `0x0004_0000` |
| `UNK_GRAVITY_RELATED` | `0x0008_0000` |
| `SWIM` | `0x0010_0000` |
| `WALL_HANG` | `0x0020_0000` |
| `POWER_UP_PUNCH_START` | `0x0040_0000` |
| `POWER_UP_PUNCH_END` | `0x0080_0000` |
| `UNK_TILE_CHANGE` | `0x0100_0000` |
| `HAY_CART_RELATED` | `0x0200_0000` |
| `ACID_RELATED_DAMAGE` | `0x0400_0000` |
| `UNK_3` | `0x0800_0000` |
| `ACID_DAMAGE` | `0x1000_0000` |

**Critical behavior:** parsing uses `from_bits_retain` — i.e. **unknown/undefined bits are preserved**, not masked off. The only flag with codec-level meaning in this module is `EXTENDED (0x08)`, which gates the trailing `extra_data`. In C++ just store `flags` as a raw `uint32_t` and test bits with `&`; do not strip unknown bits.

C++: `constexpr uint32_t FLAG_EXTENDED = 0x0000'0008u;` etc., or a `namespace PacketFlags`.

---

## 4. `GameUpdatePacket` — the 56-byte tank struct (MOST CORRECTNESS-CRITICAL)

`GAME_PACKET_SIZE = 56`. Fields (Rust struct field order = wire order):

### 4.1 Exact byte layout (little-endian)

| Offset | Size | Field | C++ type | Rust type |
|-------:|-----:|-------|----------|-----------|
| 0 | 1 | `packet_type` | `uint8_t` | `GamePacketType` (byte) |
| 1 | 1 | `object_type` | `uint8_t` | `u8` |
| 2 | 1 | `jump_count` | `uint8_t` | `u8` |
| 3 | 1 | `animation_type` | `uint8_t` | `u8` |
| 4 | 4 | `net_id` | `uint32_t` | `u32` |
| 8 | 4 | `target_net_id` | `int32_t` | `i32` |
| 12 | 4 | `flags` | `uint32_t` | `PacketFlags` |
| 16 | 4 | `float_variable` | `float` | `f32` |
| 20 | 4 | `value` | `uint32_t` | `u32` |
| 24 | 4 | `vector_x` | `float` | `f32` |
| 28 | 4 | `vector_y` | `float` | `f32` |
| 32 | 4 | `vector_x2` | `float` | `f32` |
| 36 | 4 | `vector_y2` | `float` | `f32` |
| 40 | 4 | `particle_rotation` | `float` | `f32` |
| 44 | 4 | `int_x` | `int32_t` | `i32` |
| 48 | 4 | `int_y` | `int32_t` | `i32` |
| 52 | 4 | `extra_data_size` | `uint32_t` | (derived; not a stored struct field) |
| 56 | N | `extra_data` | `std::vector<uint8_t>` | `Vec<u8>` |

Header is exactly **56 bytes**; total on-wire struct = `56 + extra_data_size`.
`extra_data_size` is **not** a Rust struct field — it is written at [52..56] on serialize and read from [52..56] on parse. `extra_data` (`std::vector<uint8_t>`) holds the trailing blob (e.g. a serialized `VariantList` for `CallFunction` packets, or map/tile blobs).

### 4.2 `from_bytes(data: &[u8]) -> Option<Self>` — parse

Input `data` = the ENet payload **after** the 4-byte outer message type (i.e. what follows `MSG_GAME_PACKET`).

Steps:
1. If `data.len() < 56` → return **None** (parse failure).
2. Read each field at its offset above using LE decoders (`read_u32`, `read_i32`, `read_f32`).
3. `packet_type = GamePacketType::from(data[0])`.
4. `flags = from_bits_retain(read_u32(12))` (keep all bits).
5. `extra_data_size = read_u32(52) as usize` — **always read**, regardless of flags.
6. `extra_data`:
   - If `flags & EXTENDED`: `start=56`, `end=56+extra_data_size`. If `data.len() < end` → return **None**. Else copy `data[56..end]`.
   - Else: `extra_data` = empty.

C++ signature: `std::optional<GameUpdatePacket> GameUpdatePacket::from_bytes(std::span<const uint8_t> data);` (or `const uint8_t* + size_t`, returning `bool` + out-param). Match the two `None` conditions exactly (short header; truncated extended blob).

### 4.3 `to_bytes(&self) -> Vec<u8>` — serialize

1. `extra_data_size = (flags & EXTENDED) ? extra_data.len() : 0`. **Note:** if `EXTENDED` is *not* set, any `extra_data` bytes are ignored and size 0 is written.
2. Allocate `56 + extra_data_size` zeroed bytes.
3. Write fields at their offsets (LE); write `packet_type.as_u8()` at [0].
4. Write `extra_data_size` (as `u32` LE) at [52..56].
5. If `extra_data_size > 0`: copy `extra_data` into [56..].
6. Return buffer.

C++: `std::vector<uint8_t> to_bytes() const;`

### 4.4 Display / logging

Rust `Display`: `GameUpdatePacket { type=<Debug>, net_id=<n>, pos=(x,y), vel=(x2,y2), flags=<Debug> }` with `{:.1}` on the four floats. Nxrth log lines are optional; if you replicate, use one decimal place for pos/vel. (Rename any surrounding `Mori` log tag → `Nxrth`; none appears in the format string itself.)

---

## 5. Packet builders (`packet.rs`)

All produce a `Vec<u8>` ready to hand to ENet (payload including the 4-byte type prefix).

| Fn | Layout produced |
|----|-----------------|
| `make_text_packet(text)` | `[u32 LE = 2 (MSG_TEXT)] [text UTF-8 bytes] [0x00]` — trailing NUL appended |
| `make_game_message_packet(text)` | `[u32 LE = 3 (MSG_GAME_MESSAGE)] [text UTF-8 bytes] [0x00]` |
| `make_game_packet(pkt)` | `[u32 LE = 4 (MSG_GAME_PACKET)] [pkt.to_bytes()]` — **no** trailing NUL |

Note the asymmetry: text/game-message packets get a NUL terminator; the game packet does not (it's binary). C++: `std::vector<uint8_t> make_text_packet(std::string_view);` etc.

---

## 6. `IncomingPacket` dispatch (`packet.rs`)

Rust enum (borrows from the input slice via lifetime `'a`):
```
ServerHello
Text(&str)
GameMessage(&str)
GameUpdate(GameUpdatePacket)
Track(&str)
ClientLogRequest
Unknown { msg_type: u32, data: &[u8] }
```

### `parse(data: &[u8]) -> Option<Self>`

1. If `data.len() < 4` → **None**.
2. `msg_type = read_u32_le(data[0..4])`; `payload = data[4..]`.
3. Dispatch on `msg_type`:
   - `1` (`MSG_SERVER_HELLO`) → `ServerHello`.
   - `2` (`MSG_TEXT`) **or** `3` (`MSG_GAME_MESSAGE`) → decode string (see §6.1); `Text` for 2, `GameMessage` for 3. If UTF-8 decode fails → **None**.
   - `4` (`MSG_GAME_PACKET`) → `GameUpdatePacket::from_bytes(payload)` mapped to `GameUpdate`. Propagates **None** on failure.
   - `6` (`MSG_TRACK`) → decode string (§6.1) → `Track`. UTF-8 failure → **None**.
   - `7` (`MSG_CLIENT_LOG_REQUEST`) → `ClientLogRequest`.
   - anything else → `Unknown { msg_type, data: payload }`.

### 6.1 String terminator rule (IMPORTANT — replicate exactly)

For `Text`/`GameMessage`/`Track`, the string is the payload **up to but not including the first byte that is `0x00` OR `>= 0x80`**:
```
segment = payload.split_at_first(|b| b == 0x00 || b >= 0x80)[0]   // may be the whole payload if no such byte
string  = utf8_decode(segment)   // strict; on invalid UTF-8 the whole parse returns None
```
Rust code: `payload.split(|&b| b == 0 || b >= 0x80).next().unwrap_or(payload)`. So any high-bit byte (`>= 0x80`) also terminates the string, not just NUL. C++: scan for the first `b == 0 || b >= 0x80`, take the prefix, validate/decode as UTF-8. On invalid UTF-8, treat as a parse failure (return no packet) to mirror Rust's strict `from_utf8`.

> Nxrth note: because C++ strings are byte buffers, "UTF-8 validation" can be a lightweight check; but to match Mori's behavior of *dropping* an un-decodable packet, keep a validity check and skip on failure.

---

## 7. VariantList / Variant (`variant.rs`)

The VariantList is the serialized argument list used inside tank packets (typically the `extra_data` of a `CallFunction (0x01)` GameUpdatePacket). **This module only *deserializes*; there is no serialize here.** (Outgoing VariantList building lives in another module.)

### 7.1 `VariantType` (internal `u8` → enum)

| Byte | Type |
|-----:|------|
| `1` | `Float` |
| `2` | `String` |
| `3` | `Vec2` |
| `4` | `Vec3` |
| `5` | `Unsigned` |
| `9` | `Signed` |
| anything else (incl. 0,6,7,8) | `Unknown` |

### 7.2 `Variant` (public value enum)

```
Float(f32)
String(String)
Vec2(f32, f32)
Vec3(f32, f32, f32)
Unsigned(u32)
Signed(i32)
Unknown
```

Accessors:
- `as_string() -> String`:
  - `Float(v)` → Rust `f32::to_string(v)` (shortest round-trip decimal; e.g. `1.0 → "1"`, `1.5 → "1.5"` — **not** C++ default `printf %f`; see note below).
  - `String(v)` → copy.
  - `Vec2(x,y)` → `"{x}, {y}"` (comma+space).
  - `Vec3(x,y,z)` → `"{x}, {y}, {z}"`.
  - `Unsigned(v)` / `Signed(v)` → decimal.
  - `Unknown` → `""`.
- `as_int32() -> i32`: `Signed(v) → v`, else `0`.
- `as_uint32() -> u32`: `Unsigned(v) → v`, else `0`.
- `as_vec2() -> (f32,f32)`: `Vec2(x,y) → (x,y)`, else `(0.0, 0.0)`.

> **Float→string formatting subtlety:** Rust `to_string` on floats emits the shortest string that round-trips (trailing `.0` dropped for integers, e.g. `5.0 → "5"`). If any Nxrth logic compares/uses these strings, match it (e.g. a shortest-round-trip formatter, or strip trailing `.0`/zeros). If `as_string` is only used for logging, exact fidelity is not correctness-critical.

C++ `Variant`: `std::variant<float, std::string, Vec2, Vec3, uint32_t, int32_t, std::monostate>` or a tagged struct with a `VariantType tag` + union-ish payload. Provide the four accessors with the exact fallbacks above.

### 7.3 `VariantList` struct & wire format

Rust: `struct VariantList { variants: Vec<Variant> }` (field private; accessed via `get`).

**Wire format (little-endian):**
```
[0]        count : u8
then repeat `count` times, each entry:
   index  : u8      // the slot/arg index; READ AND IGNORED by the parser (order is positional)
   type   : u8      // VariantType byte (see 7.1)
   payload: depends on type:
        Float(1)    -> f32                (4 bytes)
        String(2)   -> len:u32, then len bytes UTF-8 (via from_utf8_lossy)
        Vec2(3)     -> f32 x, f32 y       (8 bytes)
        Vec3(4)     -> f32 x, f32 y, f32 z(12 bytes)
        Unsigned(5) -> u32                (4 bytes)
        Signed(9)   -> i32                (4 bytes)
        Unknown(*)  -> 0 bytes consumed   (parser emits Unknown and does NOT advance)
```

### 7.4 `deserialize(data: &[u8]) -> Result<VariantList>`

Steps:
1. Create a sequential LE reader (`Cursor`, see §8) over `data`.
2. `count = read_u8()` (as `usize`).
3. Loop `count` times:
   a. `read_u8()` → the index byte, **discarded** (parser is positional).
   b. `type = VariantType::from(read_u8())`.
   c. Read the payload per the table in §7.3. **String** uses `read_u32` length then that many bytes decoded with a *lossy* UTF-8 conversion (invalid sequences → U+FFFD, never errors). **Unknown** reads nothing.
   d. Append to `variants`.
4. Return the list. Any read that runs past the buffer yields the reader's error → the whole `deserialize` returns `Err` (map to `std::nullopt`/`false` in C++).

> **Desync caveat:** if an entry has an unrecognized `type` byte, the parser produces `Unknown` and consumes **no** payload bytes, so every subsequent entry is misaligned. In practice GT never sends unknown types; replicate the behavior (do not try to "skip" an unknown payload — there's no length to skip).

`get(index: usize) -> Option<&Variant>`: bounds-checked index into `variants`. C++: `const Variant* get(size_t) const;` (nullptr if OOB), plus expose `size()`.

C++ signature: `std::optional<VariantList> VariantList::deserialize(std::span<const uint8_t>);`

---

## 8. `Cursor` dependency (sibling module `crate::cursor` — NOT in these files)

`variant.rs` reads via `Cursor::new(data, "variant")` (the `"variant"` is a context/label string for error messages, **not** a Mori/Cloei token). This helper is defined in a separate module and will be specced elsewhere, but the C++ port of VariantList needs a matching **sequential little-endian byte reader** with bounds checking. Required contract (inferred from usage):

| Method used | Behavior required |
|-------------|-------------------|
| `u8() -> Result<u8>` | read 1 byte, advance 1; err if past end |
| `u32() -> Result<u32>` | read 4 bytes **LE**, advance 4 |
| `i32() -> Result<i32>` | read 4 bytes **LE**, advance 4 |
| `f32() -> Result<f32>` | read 4 bytes **LE** (IEEE-754), advance 4 |
| `bytes(len) -> Result<Vec<u8>/&[u8]>` | read `len` bytes, advance `len`; err if short |

Nxrth: implement a `BinaryReader { const uint8_t* p; size_t len; size_t pos; }` with those methods returning `std::optional`/throwing/`bool`+out-param. Endianness = LE. On overrun, signal failure so `deserialize` can bail. (Do not build a full Cursor spec here — that's a separate module; just satisfy this interface.)

---

## 9. Login crypto (`crypto.rs`)

### 9.1 Embedded KLV keys (GT binary constants — DO NOT rename, DO NOT alter)

These are Growtopia's own embedded keys, not Mori/Cloei branding. Copy **verbatim** (lowercase 32-hex strings):
```
KEY1 = "832aac071ffbcfc15bfe1d0a7ad15221"
KEY2 = "709296ddd04fc4074a7b443ecc0799aa"
KEY3 = "623de1e8fff22a2b3e0d7e01593e7c22"
KEY4 = "bb835e5a57e6c88e2449499ca487ced2"
KEY5 = "ea76e4d6009282186063fe9465f2d9ab"
```

### 9.2 `md5u(s: &str) -> String` (internal helper)

`MD5(s.as_bytes())` formatted as **UPPERCASE** hex, 32 chars, no separators (`format!("{:X}", md5::compute(...))`). **Uppercase is mandatory** — it affects every nested hash.

C++: `std::string md5u(std::string_view s)` → hex-encode the 16-byte digest as **uppercase** `A–F`.

### 9.3 `hash_string(s: &str) -> i32` (GT rotate-left-5 hash, NUL-terminated variant)

```
h : u32 = 0x55555555
for each byte b in s.as_bytes(), THEN one extra trailing 0x00 byte:
    h = rotate_left(h, 5) + b        // rotate is a 32-bit bit-rotation; add wraps mod 2^32
return h reinterpreted as i32        // bitwise cast u32 -> i32 (two's complement)
```
- Seed is `0x55555555`.
- `rotate_left(h, 5)` = `(h << 5) | (h >> 27)` on `uint32_t` (use `std::rotl(h,5)` in C++20).
- Addition wraps (natural `uint32_t` overflow).
- Iterate over the string bytes **and then one appended `0x00`** (this is the "NullTerminated" hash mode).
- Return value is the `uint32_t` bit-pattern reinterpreted as `int32_t` (`(int32_t)h`), so it can be negative.

C++: `int32_t hash_string(std::string_view s);`

### 9.4 `compute_klv(game_version, protocol, rid, hash_val: i32) -> String`

Build one concatenated string in this **exact order**, then MD5-uppercase-hex the whole thing:
```
combined =
      md5u(md5u(game_version))                 // game_version hashed TWICE
    + KEY1
    + md5u(md5u(md5u(protocol)))               // protocol hashed THREE times
    + KEY2
    + KEY3                                      // KEY2 and KEY3 are adjacent (no hash between)
    + md5u(md5u(rid))                           // rid hashed TWICE
    + KEY4
    + md5u(md5u( decimal_string(hash_val) ))    // hash_val: i32 -> decimal string, then TWICE
    + KEY5

return md5u(combined)
```
Critical details:
- **Nesting depths differ:** game_version ×2, protocol ×3, rid ×2, hash_val ×2. Get these right.
- Each `md5u` hashes the **ASCII text of the previous uppercase-hex string**, not raw digest bytes. (i.e. `md5u(md5u(x))` = MD5 of the 32-char uppercase-hex string that is `md5u(x)`.)
- `hash_val.to_string()` = **signed decimal** (`std::to_string(int32_t)`, leading `-` if negative). This is the string fed into the double-MD5.
- The five keys sit at fixed positions; `KEY2` immediately precedes `KEY3` with no hash between them.
- Final result is uppercase 32-hex.

C++: `std::string compute_klv(std::string_view game_version, std::string_view protocol, std::string_view rid, int32_t hash_val);`

### 9.5 `random_hex(n: usize) -> String`

`n` characters, each an independent random nibble `0..=15` formatted as **UPPERCASE** hex (`rng.random::<u8>() & 0xF`, `{:X}`). So each char is one uppercase hex digit. C++: draw a random byte per char, mask `& 0xF`, emit uppercase.

### 9.6 `random_mac() -> String`

Format `"02:XX:XX:XX:XX:XX"` where the first octet is the **fixed literal `02`** (locally-administered unicast) and the remaining **5** octets are random `u8` printed as two uppercase hex digits (`{:02X}`). Total 6 octets. C++: `std::snprintf(buf,"02:%02X:%02X:%02X:%02X:%02X", r,r,r,r,r)` with uppercase.

### 9.7 `generate_rid() -> String`

Returns `random_hex(32)` → **32 uppercase hex chars**. ⚠️ The Rust doc-comment claims it is "derived from the current nanosecond timestamp," but the **actual implementation ignores time and just calls `random_hex(32)`**. Port the real behavior (32 random uppercase hex chars); ignore the stale comment.

---

## 10. Complete on-wire summary (for quick reference)

**ENet payload framing (what `make_*`/`parse` handle):**
```
[u32 LE msg_type] [ type-specific body ]
  msg_type=2/3/6 : text bytes + (builders append 0x00; parser reads up to first 0x00 or byte>=0x80)
  msg_type=4     : 56-byte GameUpdatePacket header [+ extra_data if EXTENDED]
  msg_type=1/7   : no body used
```

**GameUpdatePacket (56 bytes) offsets:** see §4.1 table.

**VariantList:** `count:u8` then per entry `index:u8, type:u8, payload`. See §7.3.

---

## 11. Dependency mapping (Rust crate → Nxrth C++)

| Rust (in these files) | Used for | Nxrth C++ |
|-----------------------|----------|------------|
| `bitflags` | `PacketFlags` | plain `constexpr uint32_t` masks; store raw `uint32_t`; **retain unknown bits** (mirror `from_bits_retain`) |
| `anyhow::Result` | `VariantList::deserialize` error | `std::optional<T>` / `bool`+out-param (matches the `Option` style used in `packet.rs`) |
| `crate::cursor::Cursor` | sequential LE reads | a `BinaryReader` (LE, bounds-checked) — see §8; belongs to the separate `cursor` module |
| `md5` | `md5u` (klv/hash) | bundled MD5; hex-encode **uppercase** |
| `rand` (`rand::rng`, `random::<u8>`) | `random_hex`/`random_mac`/`generate_rid` | any decent RNG (`std::mt19937` seeded from `std::random_device`, or platform CSPRNG). No reproducibility requirement — pure randomness for per-bot identity |
| `std::fmt` | `Display` for logging | `std::format`/`operator<<` (optional) |

Module-wide, per the Nxrth stack: no Lua (`mlua`) here, no HTTP (`ureq`/`reqwest`) here, no ENet API here — this module is a **pure codec** consumed by the ENet transport module. (ENet itself → vendored C ENet patched for SOCKS5-UDP, in the transport module, not here.)

---

## 12. Threading & shared state

- **Everything in this module is pure / stateless.** `packet.rs` and `variant.rs` are value types + free functions with no globals, no I/O, no locks. `crypto.rs` functions are pure except `random_*`/`generate_*`, which pull from a thread-local RNG (`rand::rng()`), so they are inherently thread-safe/reentrant.
- **Thread-safety in Nxrth:** make all functions free/`static` and re-entrant. No shared mutable state to guard — no mutex/condvar needed inside this module.
- **Who calls it:** each bot's ENet receive thread calls `IncomingPacket::parse` / `GameUpdatePacket::from_bytes` / `VariantList::deserialize` on its own inbound buffers; each bot's send path calls `make_*_packet` / `GameUpdatePacket::to_bytes`; the login flow calls `compute_klv` / `hash_string` / `generate_rid` / `random_mac` **once per bot** at connect time.
- **Fleet-wide shared state (Nxrth requirement that bots be aware of each other):** this module produces the *decoded* packets that feed the per-bot world/state model; the **shared** fleet state lives one layer up (the world/net-id/inventory model), not here. Two consequences for the port:
  1. Keep `from_bytes`/`deserialize` allocation-light and copy-in (they already return owned data: `extra_data` is copied, strings are owned) so decoded packets can be safely handed across threads to a shared state store without lifetime hazards. (`IncomingPacket` borrows the input slice — in C++ prefer returning owned copies, or ensure the backing buffer outlives consumers, since the fleet store may be on another thread.)
  2. `generate_rid()` / `random_mac()` produce **per-bot identity** — generate once, store on the bot, and register in the shared fleet registry so bots don't collide and can recognize each other's net-ids.

---

## 13. Rename rules applied to this module

Global rules: `Mori`/`mori` → `Nxrth`/`nxrth`; `Cloei`/`cloei` → `North`/`north` (identifiers, paths, log lines, window titles, user-agents, config filenames).

**Concrete occurrences found in these four files:**
- **None** of the literal tokens `Mori`, `mori`, `Cloei`, or `cloei` appear in `packet.rs`, `variant.rs`, `crypto.rs`, or `mod.rs`. No log lines, user-agents, window titles, or config filenames are present here.
- The only surrounding rename is the **crate root namespace**: `crate::cursor::Cursor` — the Rust crate is `mori`, so in C++ everything goes under `namespace nxrth` (e.g. `nxrth::protocol`, `nxrth::cursor`). Any place the crate name leaks (paths, `use crate::...`) becomes `nxrth`.
- The string label `"variant"` in `Cursor::new(data, "variant")` is a parser context label, **not** a brand token — keep as `"variant"`.
- **Do NOT rename** the five KLV keys (`KEY1..KEY5`), the GT message-type numbers, the `GamePacketType` byte values, the `PacketFlags` values, the `hash_string` seed `0x55555555`, or `GAME_PACKET_SIZE = 56` — these are Growtopia protocol constants and must stay byte-exact.

---

## 14. Suggested C++ surface (non-normative sketch)

```cpp
namespace nxrth::protocol {

// §1
inline constexpr uint32_t MSG_SERVER_HELLO      = 1;
inline constexpr uint32_t MSG_TEXT              = 2;
inline constexpr uint32_t MSG_GAME_MESSAGE      = 3;
inline constexpr uint32_t MSG_GAME_PACKET       = 4;
inline constexpr uint32_t MSG_TRACK             = 6;
inline constexpr uint32_t MSG_CLIENT_LOG_REQUEST= 7;

inline constexpr size_t   GAME_PACKET_SIZE      = 56;
inline constexpr uint32_t FLAG_EXTENDED         = 0x0000'0008u; // + the other 27 masks

struct GameUpdatePacket {
    uint8_t  packet_type{};      uint8_t object_type{};
    uint8_t  jump_count{};       uint8_t animation_type{};
    uint32_t net_id{};           int32_t target_net_id{};
    uint32_t flags{};            float   float_variable{};
    uint32_t value{};
    float vector_x{}, vector_y{}, vector_x2{}, vector_y2{}, particle_rotation{};
    int32_t int_x{}, int_y{};
    std::vector<uint8_t> extra_data; // only when flags & FLAG_EXTENDED
    static std::optional<GameUpdatePacket> from_bytes(std::span<const uint8_t>);
    std::vector<uint8_t> to_bytes() const;
};

std::vector<uint8_t> make_text_packet(std::string_view);
std::vector<uint8_t> make_game_message_packet(std::string_view);
std::vector<uint8_t> make_game_packet(const GameUpdatePacket&);

struct Variant { /* tag + float/string/Vec2/Vec3/uint32/int32 */ };
struct VariantList {
    std::vector<Variant> variants;
    static std::optional<VariantList> deserialize(std::span<const uint8_t>);
    const Variant* get(size_t) const;
};

// §9
std::string md5u(std::string_view);                    // UPPERCASE hex
int32_t     hash_string(std::string_view);             // seed 0x55555555, rotl5, +NUL
std::string compute_klv(std::string_view game_version,
                        std::string_view protocol,
                        std::string_view rid,
                        int32_t hash_val);
std::string random_hex(size_t n);                      // n uppercase hex nibbles
std::string random_mac();                              // "02:XX:XX:XX:XX:XX"
std::string generate_rid();                            // == random_hex(32)

} // namespace nxrth::protocol
```

**End of spec 01-protocol.**
