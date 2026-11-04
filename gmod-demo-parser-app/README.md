# gmod-dem-parser

Parses Garry's Mod `.dem` files. Player movement, world entities, engine chat, voice, Lua net traffic.

[![.NET](https://img.shields.io/badge/.NET-10-512BD4?logo=dotnet&logoColor=white)](https://dotnet.microsoft.com/download)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Demo protocol](https://img.shields.io/badge/demo_protocol-3-111)](DOCUMENTATION.md)
[![Network protocol](https://img.shields.io/badge/network_protocol-24-111)](DOCUMENTATION.md)

![gmoddem parsing a demo](https://u.smeris.me/yuyu8/nUracIWi91.gif/raw)

Example result:

![example JSON output](https://u.smeris.me/yuyu8/fEhOjIkI56.png/raw)

**[DOCUMENTATION.md](DOCUMENTATION.md)**

```text
demo.dem
    │
    ▼
 gmoddem
    │
    ├── demo.json          players, objects, models, audio index
    ├── demo.net.jsonl     Lua net, usermessages, prints, game events
    ├── demo.lua/          Lua paths and source (gluapack unpacked when possible)
    └── demo.audio/        one WAV per speech line
```

Parser, not a player. `gmoddem` writes JSON next to the demo plus those sidecars. Voice is 16-bit mono WAV at 48 kHz, named by Steam64 and tick.

| Output | Contents |
|:---|:---|
| Player timelines | origin, view angles, weapon, health, model — one snapshot per change |
| World log | spawns, moves, doors, owners, Source/GMod game events |
| Identity | slot, edict, SteamID / SteamID64 from userinfo |
| Voice | Steam/Opus → WAV |
| Server sounds | Source `svc_Sounds` and Perpheads `ph_soundplay`, with precache-resolved or Lua string-table sound paths |
| Engine chat | `player_say` only. Custom gamemode chat is in the net log. |
| Net dump | Lua `net`, usermessages, prints, console, entity/voice/sound summaries |
| Lua folder | paths from the demo; source from userdata, or unpacked gluapack stubs |

## Try it

The thing you run is **`gmoddem.exe`**. One project: `src/GmodDemo.Cli`. 

[.NET 10 SDK](https://dotnet.microsoft.com/download) to build.

```bat
dotnet test
dotnet publish src\GmodDemo.Cli -c Release -r win-x64 -o dist
```

```bat
dist\gmoddem.exe path\to\demo.dem
```

Pretty JSON next to the demo. Drag a `.dem` onto `gmoddem.exe` does the same. `-o` for another path, `--compact` for minified.

Other commands: `info`, `players`, `window`, `datatables`. `gmoddem --help` is the short list. `gmoddem --help more` is the format-research commands.

<details>
<summary>What <code>demo.json</code> looks like (schema version 4)</summary>

```json
{
  "schemaVersion": 4,
  "header": {
    "magic": "GMODEMO",
    "mapName": "gm_construct",
    "tickRate": 66.67
  },
  "players": [
    {
      "id": 0,
      "slot": 0,
      "entity": 1,
      "name": "Player",
      "steamId64": "76561198000000000",
      "ticks": [
        { "t": 120, "time": 1.8, "x": 256.0, "y": -128.0, "z": 64.0, "yaw": 90, "weapon": "weapon_physgun", "health": 100 }
      ],
      "chat": [
        { "t": 200, "time": 3.0, "text": "hello" }
      ],
      "voice": [
        { "t": 400, "time": 6.0, "duration": 1.2, "file": "demo.audio/76561198000000000_400.wav" }
      ]
    }
  ],
  "objects": [
    { "t": 1, "type": "spawn", "kind": "vehicle", "p": 0, "class": "prop_vehicle_jeep" }
  ],
  "sounds": [
    { "t": 240, "name": "doors/door_latch3.wav", "v": 0.75, "lvl": 70, "p": 95, "pos": [12.5, -3.25, 0] }
  ],
  "models": { "12": "models/props_c17/chair02a.mdl" },
  "playerAnimations": [
    { "time": 10.0, "entity": 65, "event": 0, "data": 0 }
  ],
  "audio": { "pcm": true, "format": "wav", "sampleRate": 48000, "clips": [] },
  "summary": {}
}
```

If `schemaVersion` isn't `4`, the shape changed.

`playerAnimations` preserves recorded `CTEPlayerAnimEvent` events, including
omitted/default primary attack event zero. Entity indices are extracted from the
serialized player handle; these are not additional gunshots or hit effects.
Brush-model `objects` (models named `*N`) retain translation/rotation and parent
identity across visibility re-entry, including map doors and parent trains.
An initial brush position is not filled from a later movement event.
Attached map models retain the same identity/parent metadata, and `visibility`
object rows explicitly preserve recorded `EF_NODRAW` transitions (`hidden` true
or false), including server-enabled models that start disabled in the BSP.

</details>

## Limits


`svc_CreateStringTable` still doesn't walk sequentially after the 2026 net changes. Model paths come from the truncated `dem_stringtables` dump.

Gamemode Lua `net` is saved, not interpreted. neither are their netmessages, ofcourse.


`.lua/` is often just `_paths.txt`. Gluapack demos search registered Steam library folders for the CDN `.bsp` (or `GMOD_GLUAPACK_DIR`); the interactive CLI suggests it if present. Skip leaves `return gluapack()` placeholders. An incomplete final demo command is discarded while complete preceding commands are preserved.

## Tests

```bat
dotnet test
```

Tests that need a real `.dem` skip unless you point them at one:

```bat
setx GMOD_DEMO_PATH "C:\path\to\long.dem"
setx GMOD_DEMO_SHORT_PATH "C:\path\to\short.dem"
```

The optional dropped-plank regression uses `GMOD_DEMO_PLANK_PATH`, pointing to
`perpheads_demo_2026-10-21 12-57-32.dem`. It checks the exact late-added model,
its entity spawn and that all model-table updates decode successfully.

Clothing export tests cover required body slots defined in either shared registry
file, plus the legacy local slot table. With `GMOD_DEMO_PATH` set, they also export
the recording's exact Lua clothing registry (the matching server gluapack must be
available).

The native `gmod-replay-combat-tests --modern-clothing <demo> <gmoddem.exe> <perp-data-root>`
regression uses `perpheads_demo_2026-10-24 21-10-22.dem`. It checks the warehouse
players' actual outfit timelines and coverage of every required body slot at
0, 4 and 30 seconds, including rewind. Before release, also visually check this
scene at 4 seconds with `--camera 40 -13077 1698 108 27 --outline-all`; successful
packet parsing or nonzero clothing counts alone do not prove bodies render.
Retain the IA legacy outfit and modern command-car/passenger checks alongside it.

When the engine's Lua-file table is truncated, known weapon-attachment module
paths are looked up by salted hash in that recording's own gluapack. Attachment
IDs, models, sight positions and magnification still come from executing its
exact scripts in registration order. Unmapped attachment registrations fail
explicitly rather than exporting a partial or empty registry.

## License

[GNU GPL v3](LICENSE). Distribute this, or reuse the source in something you ship, and that has to come with it under the same license. Running `gmoddem` locally doesn't mean you have to publish your demos.

Concentus (Opus) is BSD 3-Clause. The LZMA decoder is public domain. [THIRD_PARTY.md](THIRD_PARTY.md).
