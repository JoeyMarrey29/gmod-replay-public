# gmod-dem-parser

[README](README.md) is the short version. This is the file formats, JSON fields, and the bits that are still a mess.

Built against **current GMod**: demo protocol **3**, network protocol **24**. Magic is usually `GMODEMO\0`. `HL2DEMO\0` protocol 3 is accepted as variant `Source` at the container layer. Protocol 4 is rejected.

A full `json` pass writes:

| File | What's in it |
|:---|:---|
| `<stem>.json` | Schema version **4** playback document |
| `<stem>.net.jsonl` | One JSON object per logged net / console row |
| `<stem>.lua/` | Recovered client Lua, plus `_paths.txt` |
| `<stem>.audio/` | Voice sidecars (`.wav`, or `.gmodvoice` if decode failed) |

`<stem>` is the JSON filename without the extension. Default is the demo's name.

The playback document also includes recorded temporary `effects`, decoded using
the demo's send tables and decal/effect precaches. Supported exports include
surface decals, effect dispatches, blood sprites and blood streams when recorded.
Malformed temporary-entity messages are reported and rolled back atomically.
PERP `perp_effects` messages supply firearm impact positions, normals, scales and
hit materials. Flesh materials (45, 66 and 70) become `bloodimpact` dispatches,
as do `ph_bloodsplat` melee messages. Other `gdcw_universal_impact` and
`gdcw_universal_penetrate` messages retain their original names and export
`origin`, `normal`, `scale` and `material` (the authored EffectData radius).
Engine dispatches also retain this material field. Truncated messages produce
explicit diagnostics without partial events. The viewer recreates short-lived
blood mist and surface smoke at recorded hits, alongside supported surface
decals and shot-driven muzzle effects. Missing impact effects are not inferred
from gunfire or arbitrary decals. Particle motion and emission positions are
deterministic across seeks. Exporting an effect does not imply every effect type
is rendered.

Delta entity packets include a separate deletion list after `UpdatedEntries`:
each continuation bit is followed by a 13-bit absolute GMod entity index.
These deletions can remove already-dormant entities. Deleted weapons emit a
serial-specific `weaponState` `gone` event; index reuse also retires the previous
weapon serial. Leaving view without deletion does not strip an inventory item.

The historical `perp-data` cache includes `weapon_attachments.json` (cache format
7). Registration executes the recovered shared attachment definitions in the
game's top-level filename order, preserving pack-specific IDs. Definitions
include model, slot, sight type, magnification and normal/alternate aim offsets.
They also include the attachment's `GetSound()` result when present, allowing
suppressed samples to be selected from the matching historical GunSounds registry.
Attachments in the silencer slot without a sound override are not treated as
sound suppressors. Older cached registries rebuild for the new field.
SWEP metadata includes authored rail aim offsets. The native viewer bone-merges
recorded attachments and renders a 512x512 world view into the actual optic lens,
using `90 / (3 * magnification)` degrees, rather than zooming the whole camera.
Client-only alternate-sight input and attachment Lua rendering/animation callbacks
are not inferred from this static registry export, except front/rear iron-sight
attachment sequence selection: `sequence` and `sightSequence` are evaluated from
the original attachment callbacks without and with an optic. The viewer poses
these attachments in both world and first-person views and selects the recorded
weapon's appropriate variant at playback time, including after seeks or removal.
Matched attachment bones stay locked to the weapon; only unmatched hinge bones
inherit the folding animation. Fixed optics retain their original bind-pose import.

Sound precache additions during recording are applied alongside model additions,
so sounds first used after signon retain their recorded file names.
SWEP metadata also exports holiday dolls' `ElfShake` and `ElfFidget` sound pools
from the recording's Lua. For the recorder only, the viewer reconstructs missing
locally predicted doll audio from recorded primary-attack/fidget view-model
activities, including alternate sequences of the same activity. Recorded sounds
take precedence. Reconstructed clips use a deterministic selection from the
original pool and pitch 100: the exact original random clip/pitch is not recorded
and cannot be recovered. Reconstruction is logged as `DOLL_SOUND_RECONSTRUCTED`.

Player entries can include `connectedAt` and `disconnectedAt` in demo seconds.
They describe separate connection sessions, including reconnects by the same
Steam account. Player entities and voice slots are bound chronologically;
disconnects end ownership rather than carrying a former player's identity into
the next occupant's track.

Entity entry/re-entry restores properties from the packet-selected baseline
for that entity index and class, then applies the entry's explicit property
deltas. When no selected entity baseline exists, the class's `instancebaseline`
string-table entry supplies its initial properties, including vehicle skins and
bodygroups. These entries are read from both the full demo string-table dump and
network creation/update messages; explicit entry values (including zero) win.
Baseline promotion preserves both slots independently of the last live
state. This restores weapon script/model identity for the new serial and player
flags/life state after revival, without borrowing another entity's live state.
Players start with alive/standing defaults when those properties are absent
from both the baseline and the entry. Ordinary delta updates and dormant
reappearances retain their existing live state.

Passenger pods can inherit their model from the class baseline. Consumers must
identify `CPropVehiclePrisonerPod` seats by class, not by a missing model; their
recorded origins are local to their parent vehicle.

`weaponState` rows with kind `grenade` retain Lua Int slot 1 (`m_GMOD_int.001`)
for the exact weapon entity serial. This slot is only a grenade state for the
grenade base (normal 1, pin pulled 2, spoon released 3, thrown 4); other SWEPs
reuse it. The viewer interprets it only for grenade view models with pin/throw
sequences, using recorded transitions for first-person actions and the
third-person throwing gesture. Projectile deletion does not imply an explosion;
missing detonation effects and sounds are a known playback limitation.

```text
.dem
  │
  ▼
DemoReader          header + command records (forward-only)
  │
  ├── PlayerDirectory     userinfo → players
  ├── EventCollector      packets → pose / world / voice / net log
  │     ├── NetMessageWalker
  │     ├── EntityWorld + send tables
  │     ├── PrecacheTables
  │     ├── VoiceTrack
  │     └── GameEventList
  │
  ▼
PlaybackDocument    schema 4 JSON
JsonExporter        header + players/objects + models + audio + summary
LuaExporter / NetMessageLog / VoiceTrack.WriteSidecars
```

`DemoReader` never buffers the whole file. Memory is bounded by the largest single command payload (GMod packets can go past 260 KB; reject ceiling is 16 MiB).

`JsonExporter` walks the file once. If you pass `--commands`, those records are streamed into the JSON as they are read so the dump isn't held entirely in RAM.

Needs [.NET 10](https://dotnet.microsoft.com/download) to *build*. Developed and tested on Windows. One program: `src/GmodDemo.Cli` → `gmoddem.exe`. Concentus 2.2.2 does the Opus decode. `src/GmodDemo.Cli/LzmaSdk/` is the 7-Zip LZMA C# decoder (public domain). Demos are not in the repo.

License is [GNU GPL v3](LICENSE). Third-party notices: [THIRD_PARTY.md](THIRD_PARTY.md).

## Building it

From the repo root:

```bat
dotnet test
dotnet publish src\GmodDemo.Cli -c Release -r win-x64 -o dist
```

`dist\` is what you ship: one `gmoddem.exe`. Publish is set up for startup and parse speed (ReadyToRun, no bundle compression), not a tiny download. Zip the folder or point an installer at it. `bin/` / `obj/` / `dist/` are gitignored. Solution is `GmodDemo.sln` (the program plus tests).

## gmoddem

Pass a `.dem` with no command and it runs **`json`**. Drag a demo onto `gmoddem.exe` does the same.

No arguments prints a short usage screen and exits **1**. `-h` / `--help` / `help` print the same screen and exit **0**. `gmoddem --help more` (or `diagnostics`) lists the format-research commands.

Colors when stdout is a real terminal and `NO_COLOR` isn't set. Windows virtual-terminal mode gets turned on automatically. The sibling `.txt` reports are plain UTF-8, no escape codes.

Every command except help writes something next to the demo, draws a progress bar, and prints a `wrote` list plus `done` with elapsed seconds. `json` and `window` write JSON themselves. The others also tee the text report to `<demo>.<command>.txt` (so `info` becomes `demo.info.txt`). If you double-click the exe on Windows and it's the only process on that console, it waits for a key so the summary doesn't vanish.

Pretty-print is the default. `--compact` minifies. `--indent` still works.

| Exit | Meaning |
|:---|:---|
| 0 | Fine |
| 1 | Bad usage, unknown command, `players` found nobody, or a diagnostic found nothing |
| 2 | Not a parsable demo, file missing, I/O error, or `window` couldn't write |
| 3 | Finished, but the summary reported unrecoverable parse errors (`info` / `json`) |

**`gmoddem json <file.dem>`** — same as `gmoddem <file.dem>`. Schema 4 JSON next to the demo.

| Option | What it does |
|:---|:---|
| `-o`, `--out <path>` | JSON path (sidecars use this stem) |
| `--commands` | Also emit `commands[]`. Large. |
| `--no-commands` | Explicit default |
| `--include-raw` | `payloadBase64` on undecoded command payloads |
| `--max-commands <n>` | Cap raw commands (only with `--commands`) |
| `--compact` | Minified JSON |
| `--indent` | Pretty-print (already the default) |
| `--gluapack <file.bsp>` | Unpack from this pack; skip the prompt |
| `--no-gluapack` | Leave packed Lua as `return gluapack()` placeholders |

Sidecars live next to the JSON: `<stem>.net.jsonl`, `<stem>.lua/`, `<stem>.audio/`.

**`info`** — header, command histogram, payload totals, consistency checks (`playback_frames` vs packet count, `playback_ticks` vs last tick, `signon_length` vs measured signon region), and whether `dem_stringtables` hit the 512 KiB truncation size.

**`window <file.dem> [startTick] [count]`** — same JSON idea, but only ticks in `[startTick, startTick + count)`. Defaults: start **0**, count **30**. Writes `<demo>.window.json`. No net/lua/audio sidecars.

**`players`** — compact `id`, slot, entity, engine userID, SteamID64, name. Exit **1** if the list is empty. If the header client name matches a player, that row is called out as the recorder.

**`datatables`** — send-table / server-class counts from `dem_datatables`, a sample of `DT_BaseEntity` props, entity-decode stats (prop-index mode, clean/failed PacketEntities, recorder entity, `originsTrusted`).

| Command | Arguments | What it's for |
|:---|:---|:---|
| `messages` | `<file.dem>` | Net-message type inventory |
| `calibrate` | `<file.dem>` | Score competing bit widths |
| `serverinfo` | `<file.dem>` | `svc_ServerInfo` from the first signon |
| `tables` | `<file.dem>` | String-table names in packets |
| `probe-tables` | `<file.dem>` | CreateStringTable headers in the large signon |
| `dump-signon` | `<file.dem> <signonIndex>` | Hex / ASCII of one signon payload |
| `find` | `<file.dem> <signonIndex> <text>` | Bit-aligned ASCII search in that signon |
| `bits` | `<file.dem> <signonIndex> <bitOffset>` | Bits/fields from a bit offset |
| `solve-tables` | `<file.dem> <signonIndex>` | CreateStringTable layouts vs known name anchors |
| `search` | `<file.dem> <text>` | ASCII search in header, packets, usercmds, tables |
| `gmod-at` | `<file.dem> <tick>` | `svc_GMod_ServerToClient` around a tick |
| `gmod-ids` | `<file.dem>` | Histogram of Lua net name ids (subtype 0) |

## The JSON (schema version 4)

Older PERP recordings use `perpheads_bonemerge` instead of the modern clothing-data store.
These messages are decoded through the server's shared string table and exported as
`clothingData` snapshots with `explicitParts: true`. Their items include the recorded
`modelPath` and zero-based `modelSkin`, alongside the clothing type, clothing skin,
color and data number. Visibility changes, removals and clothing-variable updates
produce new snapshots. The viewer uses those explicit parts without inventing
additional bare body parts. Networked clothing variable order and count come from
that recording's server `sh_clothing_ent.lua`, not a fixed list of positions; a
missing or unsupported layout fails explicitly instead of guessing. Event times
use the recording tick rate, with the viewer applying recorded server tick-interval
corrections to clothing and attachment timelines along with other playback data.
Historical registry exports recover required body slots
from the server's local `slotsLeftToFill` table when `Clothing.RequiredSlots` is absent.

Top-level object:

```json
{
  "schemaVersion": 4,
  "header": {},
  "players": [],
  "objects": [],
  "models": {},
  "audio": {},
  "summary": {}
}
```

With `--commands`, `commands` sits after `header`.

If `schemaVersion` isn't 4, treat it as a different format.

How we got here: 1 was a flat `events[]` log, 2 grouped pose by player, 3 used `.gmodvoice` sidecars, **4** is WAV per speech line plus `players[]` / `objects[]` and no mixed event array.

If the header has zero playback time (recording didn't patch the header on close), tick rate falls back to `66.6666666667`.

### header

| Field | Meaning |
|:---|:---|
| `magic` | `GMODEMO` or `HL2DEMO` |
| `variant` | `GarrysMod` or `Source` |
| `demoProtocol` | Has to be 3 or we don't parse |
| `networkProtocol` | 24 on current GMod |
| `serverName`, `clientName`, `mapName`, `gameDirectory` | Header strings. 260-byte NUL-padded fields. |
| `playbackTime`, `playbackTicks`, `playbackFrames`, `signonLength` | Header totals |
| `tickRate` | `playbackTicks / playbackTime`, or `null` |

### players[]

Each connection session has its own identity entry. A player who reconnects may therefore have multiple entries with the same SteamID; keeping sessions separate prevents slot reuse from relabeling earlier poses or voice. Pose is under `ticks[]`, engine chat under `chat[]`, and voice cues under `voice[]`.

| Field | Meaning |
|:---|:---|
| `id` | Compact index. This is `p` on objects. |
| `slot` | Userinfo slot (0 … maxClients−1) |
| `entity` | `slot + 1` |
| `userId` | Engine userID from `player_info_t` |
| `name`, `steamId`, `steamId64` | Scoreboard / GUID |
| `fakePlayer`, `isHltv` | Only written when true |
| `ticks[]` | Carry-forward snapshot per pose/weapon/health/anim change |
| `chat[]` | Engine `player_say` lines attributed to this player |
| `voice[]` | Speech lines (`file` is relative to the JSON), including their proximity mode |

Players without a GUID still show up if a `CGMOD_Player` edict was seen (slot/entity, null name/SteamID).

Tick fields are all optional except `t` / `time`:

| Field | Meaning |
|:---|:---|
| `t`, `time` | Tick and seconds (`t / tickRate`) |
| `x,y,z` | Origin (entity, or recorder camera when that's what changed) |
| `pitch,yaw,roll` | View angles |
| `weapon` | Active weapon script name |
| `health`, `lifeState`, `flags`, `team` | Send props |
| `model`, `modelIndex`, `skin`, `body` | Appearance. `model` is a `models/…mdl` path when we know it. |
| `sequence`, `cycle`, `playbackRate` | Animation |
| `spawn` / `gone` | Entered or left PVS this tick |

Stamina isn't on the send tables in the current-GMod demos this was checked against, so it isn't in the JSON.

Vehicle motion is **not** written onto player ticks. Cars are `objects` with `kind: vehicle`.

Chat:

| Field | Meaning |
|:---|:---|
| `t`, `time` | Tick / seconds |
| `text` | `player_say` text |
| `senderEntity` | Edict when known |
| `team` | `true` if `teamonly` / `team` is 1 |

Custom gamemode chat (Lua `net`) is `"type":"lua"` in `.net.jsonl`, not here.

Voice on the player:

| Field | Meaning |
|:---|:---|
| `t`, `time` | First packet of the line |
| `tEnd`, `duration` | Last packet tick; WAV length in seconds when decoded |
| `file` | Path relative to the JSON |

### objects[]

World entities and discrete events. `p` is the owning / driving player id when known.

| `type` | Extra fields | When |
|:---|:---|:---|
| `spawn` | `kind` (`vehicle` / `weapon` / `door` / `player` / `entity`), `class`, `name`, `luaClass`, `model`, origin, `owner`, `parent`, `creationId`, `mapCreatedId`, `mapEntity`, `created`, `scale`, `nw0`–`nw3` | Enter PVS |
| `move` | origin, `kind`, `p` | Later motion of a vehicle or owned SENT. Map props keep spawn position only. |
| `door` | origin, `state`, `open` | `prop_door_rotating` `m_eDoorState`. Brush `func_door` has position only. |
| `owner` | `owner`, `name` | Owner handle changed after spawn |
| `remove` | `deleted` | Left PVS / deleted |
| `game` | `id`, `event`, `keys` | `svc_GameEvent` |

`kind: vehicle` plus `p` means this player owns/drives that vehicle. `mapEntity: true` means `m_iMapCreatedID` ≠ 0.

Door `state`: `closed` / `opening` / `open` / `closing` / `ajar`. `open` is a boolean derived from that.

Script names on spawn (`name`, `luaClass`) come from networked props and `networkstring`. The mesh is `model` from `m_nModelIndex`. Many SENTs have no static model string in the demo.

### models

Object whose keys are precache indices (as strings) and values are mesh paths from `dem_stringtables` `modelprecache`. Brush models are `*N`. Index `0` is omitted.

### audio

Always written. `clips` is empty if nobody spoke.

| Field | Meaning |
|:---|:---|
| `codec`, `quality` | From `svc_VoiceInit` when we recovered it |
| `sampleRate` | WAV output is **48000**. Steam's opcode often says 24000 (capture rate). Decoding Opus at 24 kHz aliases into a hiss. That's a bug, not a setting. |
| `pcm` | `true` when every clip is a WAV |
| `format` | `wav`, or `gmodvoice` if decode failed |
| `payload` | `steam-opus` when applicable |
| `clips[]` | One file per utterance (same slot and proximity mode, gap ≤ 20 ticks / ~300 ms at 66.67 tick) |

Clip rows also include speaker identity, `packets`, `bytes`, and `proximity` (`true` for spatial voice). Player `voice[]` omits identity because it's already on the player.

WAVs are RIFF PCM, 1 channel, 16-bit, 48000 Hz. Failed lines become a small `.gmodvoice` container (`GMODVOICE` magic) instead of slapping a fake PCM header on compressed bytes.

File names: `<steamid64>_<startTick>.wav`, or `slotN_<startTick>.wav` if there's no GUID.

### summary

Command counts, payload byte totals, tick range, `lastContentTick`, `contentTickCoverage`, `parsedCleanly`, `stringTablesLikelyTruncated`, consistency flags, `errors[]`, optional `tailRecovery` (an incomplete final command discarded while preserving the complete prefix), and `entities` (prop-index mode, clean/failed PacketEntities, `originsTrusted`, recorder-related camera match counts).

`originsTrusted` is set when the recorder camera origin matches a player entity origin. Until that check passes, pose is not treated as fact — move/look/anim/state events get dropped. If player origins look insane, look here first.

### commands[] (`--commands`)

Each record: `type`, `tick`, `offset`. Packets add camera info, sequence numbers, `payloadBytes`, `decoded: false`. `--include-raw` adds `payloadBase64`. `dem_stringtables` of exactly 524288 bytes sets `likelyTruncated`.

## Net log (`*.net.jsonl`)

One JSON object per line. Body hex/ascii is capped at **2048** bytes (`NetMessageLog.MaxBodyBytes`).

| `type` | Source |
|:---|:---|
| `console` | `dem_consolecmd` and `net_StringCmd` |
| `print` | `svc_Print` |
| `lua` | `svc_GMod_ServerToClient`. Subtype 0: `id` = `net.ReadHeader` (uint16 network-string id), `name` from `networkstring` when known, `hex` / `ascii` body |
| `usermessage` | `svc_UserMessage` (`id`, body) |
| `game` | `svc_GameEvent` (decoded `name` + `keys` when the event list was recovered) |
| `entities` | `svc_PacketEntities` header only (`bits`, `delta`) — not the entity bitstream |
| `tempentities` | Count + bit length |
| `entitymessage` | Entity index + bit length |
| `voice` | Slot + payload size, not Opus bytes |
| `sound` | Bit length + `reliable` |
| `setview`, `fixangle` | View / punch |

`soundfade` and GameUI escape-show console commands still go in this log. They are omitted from the internal `ConsoleEvent` list used for the JSON.

Lua `net` payloads are dumped, not decoded. There's no generic decoder for gamemode `net.Write*` layouts.

## Lua export (`*.lua/`)

`LuaExporter` writes LZMA userdata from `client_lua_files`, and always writes `_paths.txt`. Some servers stub every file as `return gluapack()`. Those are detected from the stub text and from `gluapack_*` convars in the demo.

The `.bsp` pack is **not recorded in the demo**. [gluapack.com](https://gluapack.com/) sends it over FastDL/CDN on purpose (Lua does not go through the net channel). A `.dem` only has the net channel, plus the pack id/key/salt.

When `json` notices gluapack, it **stops and asks**. It searches the registered Steam install and library folders for the pack under `garrysmod/download/data/gluapack/<id>.bsp`; `GMOD_GLUAPACK_DIR` can specify another folder. If found, that path is suggested; you confirm with `y`, pass another `.bsp` path, or `n` to skip. Skip means `.lua/` keeps `return gluapack()` placeholders. `--gluapack <file.bsp>` / `--no-gluapack` skip the prompt. Redirected stdin also skips. `_gluapack.txt` is written when gluapack was seen. A pack that is no longer on disk cannot be reconstructed from the demo or substituted with the current map BSP.

Pack discovery also preserves historical, hex-ID-named packs from these local
download folders in `%LOCALAPPDATA%\GModReplay\Gluapacks`. This user-owned archive
survives game download cleanup and application upgrades; no Lua packs are uploaded
or bundled with releases. `GMOD_GLUAPACK_ARCHIVE_DIR` overrides its location.
Discovery checks the original locations first, then the archive for the exact
recorded ID only. A different/newer pack is never selected automatically. Existing
archive entries are not overwritten. Import failures are reported on stderr as
`GLUAPACK_ARCHIVE_FAILED`; normal local lookup can still proceed. Archive contents
are raw local copies, not a guarantee of compatibility: the parser still must
successfully decode the pack using the recording's keys. Missing packs and demo
format incompatibilities cannot be repaired by substituting unrelated packs.

Clothing decoding independently reads the matching local gluapack, even when Lua
sidecar extraction is skipped. The server's `sh_clothes.lua` specifies the bit
width of `DataNumber` (10 bits in older recordings, 14 in newer ones). The parser
replays clothing messages with that layout before writing `clothingData`, retaining
the original string-table order. This is a server-version difference, not a
32-bit/64-bit client switch. `CLOTHING_LAYOUT` reports the selected width, source,
event count and malformed-message count. If the Lua is unavailable or unsupported,
an explicit warning reports that the current 14-bit layout is being used; historical
clothing may then be unavailable. Truncated sync messages are discarded in full
rather than exporting partially decoded records.

## How a .dem is actually laid out

Verified against current-GMod recordings (`GMODEMO\0`). After the 8 magic bytes, the container is Source demo protocol 3.

- Header is **1072** bytes, little-endian, four 260-byte NUL-padded strings.
- Each command is `uint8` type, `int32` tick, then payload. **No** player-slot byte — that's protocol 4.
- Command ids: `dem_signon`=1 … `dem_stringtables`=8. No `dem_customdata`.
- Signon/packet: one **76-byte** `democmdinfo_t`, seq in/out (`int32` × 2), `int32` length, payload.
- `playback_frames` should equal the `dem_packet` count. `playback_ticks` should be the final command tick.
- `signon_length` is bytes from the end of the header through the last `dem_signon`, including command headers and camera blocks.
- `dem_synctick` is tick 0 and is the only backwards tick.
- Packet payloads up to ~260,826 bytes have shown up in real files. Stock `NET_MAX_PAYLOAD` checks will reject actual GMod demos.
- `dem_stringtables` is written into a **512 KiB** engine buffer and gets truncated when full (exactly 524,288 bytes on the files I captured). Do not treat it as a complete dump. `modelprecache` is early in that dump and usually complete.

`DemoReader.ReadCommands()` can be enumerated **once**. A second pass throws.

## The bitstream

Packets are LSB-first streams of back-to-back messages. **6-bit** type, no length prefix between messages. One wrong field width and the rest of the packet is garbage.

GMod kept Source 2013 message numbering (`net_Tick` = 3, `svc_Print` = 7). GMod's own message is **33** (`svc_GMod_ServerToClient`). Later Valve branches inserted splitscreen messages and shifted everything. Those numbers do not apply here.

| Constant | Value |
|:---|:---|
| Type tag | 6 bits |
| Edict index | **13** bits (8192 edicts, not stock 11) |
| PacketEntities length | **24** bits (stock 20) |
| GMod message length | **20** bits |
| CreateStringTable length | Auto-detected **varint**, **20** or **32** bits, with entry validation |
| Sendprop strings | **9-bit** byte count, then that many bytes (CS:GO-style, not SDK 2013 NUL `ReadString`) |

Use 11-bit edicts and the container still parses. Then every entity is silently wrong. Use 20-bit PacketEntities length and about 6.6% of gameplay packets parse clean. 24 bits is 100% on the demos this was calibrated against.

`svc_GMod_ServerToClient` subtype 0 is Lua `net`: uint16 name id (`net.ReadHeader` / `util.AddNetworkString`), then whatever `net.Write*` put there.

`svc_VoiceData`: `uint8` slot, `uint8` proximity, `uint16` length in **bits**, then that many bits. Payloads are Steam-wrapped Opus (`FF 39 32 46` …), not Speex and not raw PCM.

`svc_GameEventList` is not reached by the sequential walk on these demos. It's recovered by finding bit-aligned `server_spawn` and reading the 6+9+20-bit header immediately before it. Current GMod writes key types as **3 bits** (0–6), not Source SDK 2013's 8-bit `WriteByte`.

Built-in event names the binder knows about (from the [GMod wiki `gameevent` list](https://wiki.facepunch.com/gmod/gameevent)): `player_say`, `player_hurt`, `player_spawn`, `player_activate`, `player_connect`, `player_connect_client`, `player_disconnect`, `player_changename`, `player_info`, `entity_killed`, `break_prop`, `break_breakable`, `server_spawn`, plus the others in `GmodGameEvents`. Addon events still show up in `objects` / the net log with whatever keys were in the demo.

`player_info.index` / connect `index` is EntIndex minus one (the slot). `entity_killed` uses `entindex_killed`.

Lua `GM:*` hooks (`PlayerSay`, `CheckPassword`, `PlayerHurt`, …) are not in the demo. Only networked game events and net messages are.

## Send tables, entities, players

`dem_datatables` flattening follows Valve `GatherProps` (collapsible inline vs flush child) then `SPROP_CHANGES_OFTEN` priority-64 sort. Priority itself is **not** in the bitstream.

Field index: Source 2013 `CDeltaBitsReader` (continuation bit + `ReadUBitVar`). Class-id width = `Log2(classCount)+1`.

EHANDLE: 13-bit edict + 10 serial bits.

Owner attribution uses `m_hOwnerEntity` / `m_hOwner` / vehicle `m_hPlayer` when those classes track owners (`EntityIdentity.TracksOwner`). Named Lua NW keys are not recovered if `networkvars` is empty in the dump. `nw0`–`nw3` are raw `m_GMOD_String*` values.

Prop-index mode is calibrated per demo (`Source2013` vs alternatives). Origins are trusted only after the camera/entity match.

GMod's `m_GMOD_DataTable` uses a custom variant table, not a fixed-width Int64:
a 12-bit entry count, a clear bit, then 12-bit keys and 3-bit variant types.
Nil, float, integer, boolean, vector, angle, entity-handle and string values
are consumed according to their wire type. Its send-prop type is 7 in builds
without Int64 support and 8 in builds with it; the property name disambiguates
type 7. Leaving PVS retains dormant entity state for subsequent deltas;
an explicit deletion discards it. Losing either alignment or dormant state can
drop the remainder of a packet, including placed props and weapon entities.

`CGMOD_Player` flattened props have no Steam/Account fields. Identity comes from `player_info_t`: `name[128]`, `userID` at 128, `guid[33]` at 132 (GMod widened the name from SDK 2013's 32). SteamID64 = `Z*2 + Y + 76561197960265728`.

The engine's `dem_stringtables` dump supplies explicit `userinfo` indexes, including vacant slots. Its entry text is the numeric slot; the Steam display name comes from userdata, not that text. These recorded indexes override packed-run guesses and preserve correct name/entity attachment in older demos.

If truncation prevents reaching `userinfo`, the longest packed run remains a fallback, rejected on a recorder-entity mismatch (header client name → player, entity = slot + 1). That fallback assumes dense occupied slots and cannot reliably recover holes; it is not used when explicit dump entries are available. Players joining after the dump still depend on recovered update framing.

## How a parse pass is wired

Same exe. Types live in namespace `GmodDemo.Parser`. `JsonExporter.Write` is the pipeline `json` runs; this is that walk, not a library you reference.

```csharp
using GmodDemo.Parser;

using var reader = DemoReader.Open(@"C:\path\to\demo.dem");
var players = new PlayerDirectory();
var events = new EventCollector();
events.SetRecorderName(reader.Header.ClientName);

foreach (var command in reader.ReadCommands())
{
    switch (command)
    {
        case DataTablesCommand dataTables:
            events.AddDataTables(dataTables);
            break;
        case PacketCommand packet:
            players.AddPacket(packet.Payload);
            events.AddPacket(packet);
            break;
        case StringTablesCommand stringTables:
            players.AddStringTablesDump(stringTables.Payload);
            events.Precache.AddPayload(stringTables.Payload);
            break;
        case ConsoleCommand console:
            events.AddConsole(console);
            break;
    }
}

var snapshot = events.BindPlayers(players.Snapshot());
// snapshot, events.Events, events.NetLog, events.Voice, events.Precache
```

```text
GmodDemo.sln
src/GmodDemo.Cli/              gmoddem.exe
tests/GmodDemo.Parser.Tests/
```

| Area | Types |
|:---|:---|
| Container | `DemoFormat`, `DemoHeader`, `DemoCommands`, `DemoReader`, `DemoSummary`, `DemoErrors` |
| Bits | `BitReader`, `BitMath`, `Lzss` |
| Messages | `NetMessageType`, `NetMessageWalker`, `NetMessageCalibration`, `NetMessageLog` |
| Classes / props | `SendTables`, `PropDecoder` |
| World | `EntityWorld`, `EntityIdentity` |
| Tables / Lua | `StringTables`, `DemoStringTables`, `PrecacheTables`, `GmodLzma`, `Gluapack`, `LuaExporter` |
| Identity | `PlayerDirectory` |
| Events | `GameEventList`, `GmodGameEvents`, `EventCollector` |
| Voice | `VoiceTrack`, `SteamVoice`, `VoiceDecoder`, `WaveFile` |
| JSON | `PlaybackDocument`, `JsonExporter` |

`LzmaSdk/` is the 7-Zip decoder with nullable disabled in the csproj.

## Tests

```bat
dotnet test
```

| File | What it's covering |
|:---|:---|
| `BitReaderTests.cs` | LSB-first reader |
| `MalformedInputTests.cs` | Synthetic **failure** paths only |
| `SyntheticDemo.cs` | Builders for those paths |
| `RealDemoTests.cs` | Container facts vs a long demo on disk |
| `DataTableTests.cs` | Send tables, PacketEntities, origins, net-log chat |
| `PlaybackTests.cs` | Schema 4 JSON, objects, voice WAVs |
| `VoiceTests.cs` | Steam framing, utterance split, WAV header / ZCR |
| `PlayerDirectoryTests.cs` | SteamID conversion, wide `player_info_t` |
| `ModelPrecacheTests.cs` | `modelprecache` dump |
| `NetLogTests.cs` | `.net.jsonl` / `.lua/` sidecars |
| `GluapackTests.cs` | Stub detection, catalog lookup, RC4 pack |

Real-demo tests skip unless these env vars point at files:

| Env | Meaning |
|:---|:---|
| `GMOD_DEMO_PATH` | Longer `.dem` |
| `GMOD_DEMO_SHORT_PATH` | Shorter `.dem` |

Synthetic fixtures are failure-path builders, not format documentation.

## What's still ugly

**CreateStringTable** payload lengths are detected per table: varint, 20-bit and
32-bit layouts are tried, and candidates must decode their declared entries and
consume the payload (apart from final byte padding). The same reader is used for
network-message framing and table construction, so 32-bit recordings such as
`Build.dem` load directly without a manually converted copy. Incremental table
updates retain their existing 20-bit layout. Identity scanning and the engine's
string-table dump remain fallbacks when signon data is unavailable.

When the dump's `userinfo` table is unavailable, packed-run slots assume a dense 0..n−1 update. Holes after the recorder can still mis-number later SteamIDs in that fallback.

**Lua `net` bodies** are hex/ascii dumps. No generic decoder.

**`players[].chat`** is engine `player_say` only. Empty chat with people talking in-game → search `.net.jsonl` for `"type":"lua"`.

**`.lua/`** is often just `_paths.txt` unless userdata or a gluapack pack was available.

**`dem_usercmd`** is still raw bytes. The recorder camera path is already in that player's `ticks`.

Usermessage ids cannot be named from a demo alone (closed-source registration table).

Protocol 4 is rejected. `HL2DEMO` protocol 3 is container-only as variant `Source`.

If you get `Not a parsable demo`, it's magic / protocol. Need `GMODEMO` or `HL2DEMO` protocol 3. Exit 3 on `json` / `info` means look at `summary.errors` — truncation and leftover bits are recoverable. High-pitched voice means the clip wasn't decoded at 48 kHz. Player origins that look like a car belong in `objects`; if that's still wrong check `summary.entities.originsTrusted`.

Adding a JSON field that changes meaning should bump `PlaybackDocument.SchemaVersion`.

## License

This is [GNU GPL v3](LICENSE) (`GPL-3.0-only`).

Concentus stays BSD 3-Clause. The vendored LZMA SDK decoder stays public domain. Both can sit next to GPLv3; keep their notices when you ship binaries. See [THIRD_PARTY.md](THIRD_PARTY.md).

JSON, JSONL, WAV, and recovered Lua written by the tool are output data, not part of this program's license.

If you copy this source into another program you convey to people, that program has to be GPLv3 too. Running `gmoddem` on a machine you control does not require you to publish your demos or your output files.
