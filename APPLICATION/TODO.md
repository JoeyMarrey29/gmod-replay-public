# Native DirectX 12 Application TODO

## Current objective

Reach a working native DirectX 12 viewer that allows freecam movement around
the v6.3 map with:

- BSP world geometry
- BSP/static lighting and lightmaps
- World materials and textures from raw VMT/VTF files
- All resolved static props from the BSP
- Static-prop materials and textures from raw VMT/VTF files
- Correct Source coordinate systems and transforms
- Depth-tested 3D rendering

The raw BSP, GMA, VMT, VTF, MDL, VVD, VTX, ANI, and related files remain the
authoritative source. Generated PNG/JSON runtime assets are not required for
the native application.

## Current work list (updated 2026-10-21, fitted optics and inventory lifecycles)

Private repository/release baseline: 0.1.12 adds versioned GitHub releases,
individual device sign-in and opt-in verified installer updates. Maintained
viewer/parser source, extraction tools and tests are tracked; experiments,
backups, game content and recording fixtures are excluded. See the root README
and release notes for access permissions, update behavior and known limits.

Scope brightness follow-up: the active lens now bypasses the weapon shader's
second ambient-light multiplication. A matching Build 21:13.04 render increased
the sampled lens brightness from 55.08 to 74.85 while the sampled surrounding
wall and optic housing remained pixel-identical. This is a lighting correction,
not an exposure boost. Release 0.1.10 packages this correction.

Built release: **0.1.9-test.20261021**, correcting stale crouch after player re-entry,
inverted world textures, idle door angle drift and repeating one-shot door sounds,
while retaining recovered-demo timing, entity decoding and demo-named map selection.
The installer now also includes the verified mask/uniform and driver fixes,
historical fitted weapon attachments, real scope lenses and inventory lifecycle
corrections. Its exact packaged viewer/parser payload passed an isolated hidden
Build/ACOG render, generated the format-4 historical registry from a fresh cache,
and reported the expected 10-degree lens. The user's installed viewer was not replaced.
Setup SHA256: `AF694FC9F268115A4FF5D8EFFB23E5AF86F99DA78E54E28B278851B90B94B406`.

Latest targeted validation: five parser tests passed (one existing fixture test
skipped); native original/recovered Build, September 30 Gun Game, October 8 and
historical registry integration passed, alongside the synthetic combat regressions.
Gun Game's complete export decoded all 89,404 entity packets cleanly with zero failures.

Earlier validation: 91 parser tests passed, 33 skipped; the separate full-recording entity
regression passed against the real truncated demo with zero failed entity packets.
Native paired-demo tests verify the original/recovered duration, base props and
guns; hidden renders verify restored barricades and Janis's pistol. Native
Source-reader recovery passed. The packaged viewer opened that demo, selected
its recorded Paralake v6 map, and generated its matching Lua-pack cache.
Native combat/real-demo integration, BSP face ownership and real-model
fist/melee/melee2/knife animation checks passed. Hidden, muted October 8 renders
verified closed/open elevator leaves at 47:35.8/47:36.4. Missing recorded child
rotations retain the BSP-authored parent-relative rotation; explicit recorded
rotations still take priority. This fixes perpendicular leaves without hardcoded
door angles or guessed geometry.

In progress

- [x] Jonathan Shelby's police uniform appeared white in September 28 at 45:12. The exact historical clothing registry and police model were loaded correctly, but `police_traffic2.vtf` uses ABGR8888 (format 1), which the decoder rejected. Added channel-correct RGBA conversion and support reporting. Synthetic VTF tests cover alpha, mip/frame offsets, native upload, truncation, and existing RGBA/BGRA decoding. A matching-camera hidden render confirms the black uniform and reflective yellow details.
- [x] Halloween hockey mask appeared at the hand, then beside the head. Its vertices use an independent root; Source aligns that root through the first merged bone in the accessory's default animation, not its bind pose. Added animation-aware root correction and head mapping without model-specific offsets. A hidden render confirms the mask covers Kane Stryker's face; glasses retain their placement.
- [x] Model-specific driver placement and angle-only vehicle interpolation. Read `vehicle_feet_passenger0` from each occupied vehicle model rather than applying one universal left-side offset. Driver attachment rotation determines facing; vehicle translation ignores angular-only keys. Synthetic left/right-drive, rebinding and interpolation checks pass, alongside original/recovered real-demo integration. A visible Nissan Leaf render was checked, but does not establish correct seating for every occupant.
- [ ] Passenger seating remains unresolved. Experimental local-seat/pose changes did not pass visual validation and were removed; passenger behavior is unchanged. Hidden passenger entities omit model information in the current export, and missing baseline/model or seat-attachment data needs investigation before changing offsets.
- [ ] Older Steam names/IDs remain unresolved. September 28 contains Steam identities, but the packed-run slot inference disagrees with the recorder's authoritative entity and is discarded. Do not attach those identities to players using guessed slot ordinals.
- [x] Upside-down hallway wall panelling at 31:08: the world vertex upload inverted V even though the BSP UVs and decoded VTF rows already follow DirectX orientation. Removed that inversion, leaving runtime mapping-size scaling, lightmaps, model UVs and decals unchanged. Hidden render at 10760,10488,1892 confirms the panelling is now at floor level.
- [x] Slightly skewed closed doors near 12302,11003,1892 at 31:32: interpolation was blending toward the first movement several minutes in the future. Door angles now hold across gaps greater than 0.2 seconds; continuous movement still interpolates with wrapped pitch/yaw/roll. Both original/recovered demo tests verify nearby doors 2884,2886,2889 remain exactly 180 degrees, and a hidden render confirms the closed leaf aligns with its frame.
- [x] Door sounds repeating after movement: a future stop command incorrectly classified one-shot WAVs as infinite loops (one captured wood movement was extended to 102.64 seconds). Door sounds now use installed WAV loop cues/sampler markers instead. New starts replace existing loops on the same non-auto entity channel; seek restoration replays these replacements in order. Sound control events survive filename filtering. Synthetic checks cover one-shot WAVs, Source cues, sampler loops and malformed metadata.
- [x] Janis incorrectly remained crouched at 21:11. The old check treated a flag from 14:48.48 as authoritative, but later full snapshots omitted it because their selected entity baseline was standing. Player flags now use both packet-selected baseline slots and are exported on re-entry even when no flag delta is sent. Original/recovered native regressions verify the earlier crouch, standing by 15:00 and at 21:11, a separate crouch at 21:44, standing at 21:45, and rewind. Synthetic coverage checks both baseline slots, promotion, omitted flags and explicit zero. Impact placement is unchanged.
- [x] Original `Build.dem` stopped at 15 minutes while its recovered copy reached 41:25: unset JSON tick rates now use the exporter's 66.67 timestamp rate before conversion to the recorded 25 Hz server clock; missing duration comes from complete content ticks. Both files verify at 2485.72 seconds. GMod's `m_GMOD_DataTable` was incorrectly decoded as a fixed-width integer, and leaving PVS incorrectly deleted entity state needed by later deltas. The real demo now has zero failed entity packets (previously 61986); native integration verifies ten placed base props and the L115A3/OTS-33 held guns at 21:11. A hidden render confirms Janis's pistol is visible.
- [x] Recorder's gunfire missing in older demos despite other players' shots working. Server gun-sound messages disabled the muzzle-counter path for the whole demo; the recorder's view-model fallback then skipped shots already in that counter, although they had not played. The recorder's recorded counter shots now play even with server gun sounds, excluding shots already covered by a server sound for that player. Fire sample lookup prefers the server's exact GunSounds registration rather than folder-name matching. Hidden muted playback verified all 12 G36 shots at 5:14-5:17 in October 8 15:38:14, and the L115A3 shot at 46:37.72 in September 28; sample decoding succeeded with zero missing sounds.
- [x] Older demos missing all clothing: September 28 server Lua uses a 10-bit clothing DataNumber, not the 14 bits in current recordings. Parser now reads the demo's own Lua layout and replays clothing messages before export (including when `--no-gluapack` skips Lua sidecars). Not a client architecture rule. Verified the September 28 demo at 0:36: full police uniforms and civilian outfits restored; 528 clothing events, zero malformed messages. Current October 19 clothing JSON is unchanged. Missing historical Lua packs are reported explicitly.
- [x] Double-clicking a player in the scene follows them using the existing follow camera. Single-click still selects, empty clicks/right-click clear selection, and dragging still turns the camera. Uses Windows' double-click timing and movement thresholds. Hidden checks: single-click selects without following, double-click follows the hit player, double-click on empty space does not follow.
- [x] Shareable Windows x64 test installer: `build\installer\GModReplay-Setup-0.1.2-test.20261019-win-x64.exe` (workspace-relative), including historical clothing and recorder gunfire fixes. Per-user installation, shortcuts and uninstaller; includes the viewer, self-contained parser, C++ runtime, menu background, tester guide, licenses and modified parser source. No bundled game archives, demos, personal settings or caches. Verified a clean install loading content/data from GMod, hidden menu render and uninstall. Tester must have GMod/PERP content installed and should have joined PERP after updating. Unsigned test build.
- Packaging: build the Release viewer with the existing CMake command, then run `APPLICATION\installer\Build-Installer.ps1` from the workspace. Requires .NET 10 SDK, Inno Setup 6 and the x64 VC143 redistributable DLLs; compiler/CRT paths can be supplied as parameters. The included tester guide correctly lists tap Shift (not Space) for play/pause.
- [x] Gunshots silent in post-update demos. Cause: the weapon rewrite no longer networks the muzzle-flash counter, so no shots were found. The server's own `ph_bullet_sound` / `ph_bullet_impact_sound` messages (sound index + exact position) are now decoded, and the sound table (`sh_gun_sounds.lua`) is exported per server version as `gun_sounds.json`. Shots play with Perpheads' own falloff ((level / distance)^0.75, heard up to 4000 units, impacts 2000), so distant gunfire is no longer cut short. The 357 now uses its real sound.
- [x] Halloween costumes: the clothing data's model override (costume) replaces the body; the bare body parts are no longer drawn over it, only the worn items (e.g. the witch hat). The server removing a costume (weapon equipped, going on duty) is recorded as new clothing data, so it is followed automatically.
- [x] Costume models T-posed: each body model is now skinned against its own rest pose (re-based onto the animation skeleton), like the engine does. Checked: stock costume, skeleton costume, normal players.
- [x] Application kept running after closing (soundscape kept playing). Fixed: sound is silenced the moment the window closes and stopped before the rest of the teardown, a texture-decoder shutdown race (lost wakeup that could hang forever) is fixed, and a 3-second watchdog ends the process if anything else still stalls. Needs your confirmation.
- [x] Held weapons/items: resolve immutable weapon classes by the exact serialized handle even when learned later; do not hide all normal/passive-hold items. Explicitly empty WorldModel remains hidden. October 8 bottle at 36:24 visually verified in a hidden render. Unavailable models and custom SWEP drawing remain limitations.
- [x] Standing dead players: separate authoritative incapacitation from availability of a usable ragdoll. Load recorded corpse body variants, require mapped physics bones, suppress standing bodies/clothes/weapons and first-person models when dead. October 8 civilian body maps all 15 physics solids; unavailable recorded poses are hidden and logged, not invented.
- [ ] Material loading slower since content is read from the GMod install.
- [x] Game content read in place from your GMod install (Workshop addons, GMod/HL2/CSS VPKs, `garrysmod\download`), no bundled copy needed. Load time unchanged.
- [x] Map selection follows the demo header again, searching installed Workshop addons and local GMod content for that exact BSP name. No newest-version alias or unrelated bundled v6.3 substitution. The chosen BSP supplies its own embedded asset pakfile; server Lua-pack lookup remains keyed to the demo's pack ID.
- [x] Demos missing `dem_stop` or ending inside a final command retain all complete commands and discard only that unfinished tail. The original `.dem` stays unchanged; the parser reports `summary.tailRecovery`. The viewer searches registered Steam libraries for the demo's exact Lua pack, but a deleted historical pack cannot be reconstructed or replaced by the current map.
- [x] PERP data (jobs, weapons, clothing) built by the parser from the server Lua the demo was recorded with (its gluapack), no Python needed. Fixed a gluapack unpack bug that had hidden the NPC jobs and some weapons.
- [x] Automatic refresh on every load, only doing work when something changed: the addon list checks each Workshop `.gma`'s size and date (about 35 ms) and re-reads only new or changed ones; PERP data is kept per server Lua version in `assets\data\perpheads\packs\<id>` and built once (about 0.5 s) the first time a new version is seen. Old demos keep their own item lists. If the demo's gluapack is not downloaded, the shared data (last "Refresh PERP Assets") is used.
- [ ] Delete the bundled `assets\extracted`, `raw`, `content` and `maps` folders once you confirm everything looks right.
- [ ] `police_traffic2.vtf` uses a texture format the decoder does not support yet.
- [ ] Halloween update models (more bones): check they animate correctly. Costume models (skeleton) now checked.

- [x] F10 froze the viewer. Cause: F10 is Windows' "menu bar" key, so releasing it put the window into a modal menu mode that stalled everything. F10 is now handled directly, and a lone Alt no longer triggers that mode either. The 3D view now draws into its own child window, which is removed on return to the menu, so the menu paints on a plain window. Checked hidden: the scene, audio and 3D view shut down and the menu comes up. How it looks on screen needs your confirmation.
- [x] Outlines are coloured by health only (green full, yellow half, red low). If health is unknown it counts as full, or as zero while the player is knocked out or dead (a ragdoll). Job/rank colours are no longer used.
- [x] Left click selects players again. Clicking empty space, or right-clicking anywhere, clears the selection. Dragging still turns the camera.
- [x] Steam names showed "(unnamed)" for everyone except you in some demos (22-02-06, 10-8 16-38-03, 9-30 18-49-56). Cause: the parser checked the name list against a guess of which entity is you (whoever was nearest the camera), and threw every name away when the guess was wrong. It now uses the demo's own record of the local player. 22-02-06 now has 168 of 169 players named.
- [x] Older demos with vacant player slots lost Steam-name bindings despite containing the names. Read the engine's explicit userinfo slots instead of treating occupied records as a dense list; use userdata display names rather than numeric entry text.
- [x] Some sounds were high-pitched or sped up. Cause: Garry's Mod sends sound flags in 12 bits, not the 11 in Valve's SDK, so about 1 in 6 sound messages was misread (garbage pitch such as 0, 64 or 192, which made engine loops jump in pitch). All sound messages now decode exactly. They are also no longer dropped, so about 50% more sounds play.

- [x] Main menu: "Back to main menu" (F10) now stays in the same window instead of opening a new one. Checked by loading a demo twice in one window; how it looks on screen needs your confirmation.
- [x] Console output (the old command-prompt window) now shows in the ` console, and is saved to `build\console.log`.
- [x] Demo browser: a folder tree like VS Code (click or Right/Left to open and close folders; Enter opens a demo), "Recent" at the top. Garry's Mod is found automatically (Steam registry, every Steam library in libraryfolders.vdf, then the usual folders on each drive), then its `garrysmod\data\phdemos`. "Change folder..." picks any folder (remembered); "Find GMod" goes back to the automatic one. Checked in renders; the folder picker needs your confirmation.
- [x] F = free view, R = first/third person (swapped).
- [x] Following someone in a car switches to third person; leaving the car puts back your previous view. Checked in a render.
- [x] Wooden boards (`wood_fence*`, player-placed) not sitting flush. Cause: the prop importer decoded the model's compressed idle rotation (Quaternion48) wrongly, so the board came out skewed. Now flush over the window in renders. Test: demo `22-02-06`, 5:01, entity 5179 at -13212 14806 2085. Needs your confirmation.
- [x] Player selection rework: right-click selects a player (right-click on empty space clears it); the selected player gets an outline, drawn through walls when "Names visible through walls" is on. New "Outline all players" option. Outline colour is health (green full, yellow half, red low) when the demo has it, otherwise the name tag colour. Hold P for the player list; Tab toggles the playback bar; copy camera position moved from P to O. Checked in renders; the live mouse and keys need your confirmation.
- [x] Real time of day next to the timestamp (from the demo's file name, else the file's creation time), 24-hour, also in the timeline hover preview. Checked in renders.
- [x] Freecam slowed to 700 units/s (hold Shift for 3x).
- [x] Playback bar no longer blocks the scene: with it open you can right-click players and drag to turn the camera (third person and free view). Needs your confirmation (live mouse).
- [x] Tapping Shift plays or pauses (holding Shift for speed, or Shift with another key, does not). Needs your confirmation.
- [x] Voice exclusion (hear only / mute a selected player): confirmed working by you. Voice clips were checked against the demo data and each one belongs to the right player.

Next

- [ ] Blood splatters, muzzle flash and bullet holes/decals shown straight away.
- [x] Blood splatters and bullet-hole decals last 60 seconds of demo time, fading over the final 5 seconds. Pause freezes their age; seeking backward restores unexpired decals. This is the chosen replay lifetime, not a claim about PERP's in-game cleanup settings.
- [x] Airborne blood mist from recorded PERP flesh-impact effects (material, origin, normal and scale), melee blood messages and native bloodimpact dispatches. Native deterministic 0.65-second puffs follow demo time and the decal setting. No guessed hits from gunfire; shooter-only predicted effects omitted by the server remain unavailable.
- [x] Recorded primary/secondary player-animation events reach playback independently of gunfire. Third-person primary gestures and view-model model activities restore fist/melee animations without invented shots. Real fist/melee/melee2/knife bone palettes tested for motion, deterministic pause and expiry; October 8 punch at 6:35.56 visually checked. Other-player first-person custom Lua sequence overrides may still differ from the recorder's exact timeline.
- [ ] Iron sights on rifles that show none (use the rifle's own iron sight).

- [ ] Water: dark, reflective, like the real game (sky and time of day are not wanted; that work was removed).
- [x] Punching shows recorded fist swings (including models whose sequence is named range_fists_r rather than range_fist).
- [x] Sliding doors/shutters: BSP brush models are separated from the static world, matched by exact *N model, and follow recorded translation/rotation including parent trains. Attached MDL leaves/cabs replace their static instance by unique model + exact parent-model hierarchy; Source map creation IDs are a fallback when parent model metadata is unavailable. This avoids entity-ordinal drift between installed map copies without positional guesses. Recorded EF_NODRAW overrides authored disabled state. Zero-face controls never borrow neighboring models' faces. Static/unrecorded brushes retain map placement; PVS absence holds the last pose. Native checks cover the parented October 8 office elevator and visibility/rewind boundaries. Moving brush lighting uses ambient model lighting, not baked brush lightmaps.
- [x] Held bottles restored; server WorldModel and hand attachment used even for normal hold type.
- [x] Remaining ph_soundplay decoded from exact server Lua (indexed/raw paths, packed vector, level/pitch/volume, exact bit-length truncation checks). October 8 adds 418 server sounds; no key/unlock samples occur in those messages in this fixture. Recorded latch/key sounds already use Source sounds; no sound is invented from a lock-state change.
- [x] Reconstruct NPC dialogue text/options from the matching Lua pack; track replacement pages and dismissal using recorded GUI input and server events. Exact mouse targets remain unavailable when no cursor position is recorded.
- [ ] Planter orientation (could not reproduce).
- [x] Historical weapon attachment registry and bone-merged fitted models in first/third person. A separate 512x512 lens world pass uses the fitted optic's actual magnification and model reticle, leaving the surrounding scene unzoomed. Removed the full-screen fake scope, guessed sniper IDs and scope_zoom override. Build L115A3/ACOG visually verified at 21:11 and recorded ADS at 21:13.04 (10-degree lens). Alternate sights are client-only in this pack; laser/flashlight effects and magazine animation callbacks remain unsupported.
- [x] Inventory/held-weapon lifecycle: parse the separate dormant-entity deletion list, retire the old serial on index reuse, and reject deleted/no-longer-owned held handles. Full September 30 export has zero failed entity packets. Native checks verify pre-event strip, OTS-33/P99 Gun Game advancement, held class, owner loss, serial isolation and rewind.

Needs your confirmation (in the installed build, not all seen on screen)

- [ ] Additional visual fixtures for other magnified optics; definitions come from each demo's historical registry rather than fixed optic IDs.
- [ ] Weapon raised correctly in first person after sprinting.
- [ ] Ragdoll constant noises gone.
- [ ] Handcuffed, zip-tied and phone gestures.
- [ ] Other emotes.

Done

- [x] Surrender emote (arms up) and "SURRENDERING" / "HANDCUFFED" tags (seen in renders).
- [x] Door blown off by C2 is hidden (seen in renders).
- [x] Doors that leave and re-enter view keep their state.
- [x] Time-of-day / day-night work removed at your request.
- [x] Install the latest build (`GModReplay.exe` 23:44, previous kept as `GModReplay.old.exe`).

## Phase 1 — Workspace and DirectX 12 foundation

- [x] Create the isolated `APPLICATION` workspace.
- [x] Copy the v6.3 BSP and raw GMA archives into `APPLICATION/assets/`.
- [x] Organize existing extracted raw Source files.
- [x] Add DirectXTK12 as a native dependency.
- [x] Create the clean CMake/MSVC application project.
- [x] Create the DirectX 12 device, swap chain, command queue, and fences.
- [x] Add a reusable descriptor-heap and GPU-resource layer.
- [x] Add automated screenshot capture for renderer validation.

## Phase 2 — BSP world importer

- [x] Read and validate the BSP header and lump directory.
- [x] Parse vertices, edges, surfedges, faces, and world models.
- [x] Reconstruct ordered BSP face polygons.
- [x] Triangulate world faces while preserving winding.
- [x] Parse displacement information and displacement vertices.
- [x] Build native world vertex/index buffers.
- [ ] Preserve Source texture coordinates and lightmap coordinates.
- [ ] Calculate or import world bounds.
- [x] Validate world triangle and face counts against the reference parser.

## Phase 3 — BSP lighting and world rendering

- [ ] Parse BSP lighting/lightmap lumps.
- [ ] Associate lightmap samples with world faces.
- [ ] Upload lightmap data to DirectX 12 resources.
- [x] Create the world depth buffer and depth-stencil state.
- [ ] Create the world material pipeline.
- [x] Render an untextured grey BSP correctly in 3D.
- [ ] Add the Source-to-DirectX coordinate conversion.
- [x] Add a proper perspective camera.
- [x] Add freecam movement and mouse look.
- [x] Add frustum and distance culling (static props: per-instance frustum + BSP fade distance).

## Phase 4 — Raw VMT/VTF world materials

- [x] Implement Source KeyValues/VMT parsing, including patch includes.
- [x] Resolve available `$basetexture` paths across extracted content roots.
- [x] Scale world UVs by the runtime base-texture size (engine `GetMappingWidth`), not BSP texdata.
- [x] Blend WorldVertexTransition `$basetexture2` by displacement alpha with `$blendmodulatetexture`.
- [ ] Resolve `$bumpmap` and normal-map paths.
- [x] Parse VTF headers and decode supported top-mip formats.
- [x] Decode supported DXT VTFs to uncompressed RGBA8 before GPU upload.
- [ ] Decode the remaining VTF image formats and upload mip chains.
- [x] Upload decoded and unsupported textures through explicit RGBA8 fallbacks.
- [ ] Apply `$color` and `$alpha`.
- [x] Apply `$translucent`, `$alphatest` (with `$alphatestreference`), and `$additive`.
- [ ] Apply `$nocull` (all geometry is currently drawn double-sided).
- [ ] Add supported `$envmap`/reflection behavior.
- [ ] Implement water material behavior and animation.
- [ ] Render the textured BSP with lightmaps.
- [ ] Validate against the v6.3 raw material set.

## Phase 5 — Static-prop importer

- [x] Parse the BSP game lump.
- [x] Locate and parse `sprp`/`prps` static-prop data.
- [x] Read the static-prop model dictionary.
- [x] Read prop origins, angles, skins, flags, and fade distances.
- [x] Implement extracted-source virtual-file lookup.
- [x] Resolve MDL/VVD/VTX dependencies from the raw content.
- [x] Parse static-prop model geometry.
- [x] Parse bodypart submeshes, skin references, and material directories.
- [x] Build shared prop vertex and index buffers.
- [x] Build prop instance transform buffers.
- [x] Implement Source prop angle transforms using the WebGPU reference math.
- [x] Add prop bounds and visibility culling.
- [x] Render all 16,292 available static-prop instances with their material batches.
- [x] Validate prop counts and material lookup against the WebGPU model cache.

## Phase 6 — Static-prop materials and lighting

- [x] Resolve available prop materials through raw VMT files and MDL `$cdmaterials`.
- [x] Decode available prop VTF diffuse textures and upload as uncompressed RGBA8.
- [ ] Load prop normal maps.
- [x] Apply prop transparency and alpha-test behavior.
- [x] Apply prop additive behavior.
- [ ] Apply static lighting/lightmap behavior where available.
- [x] Render textured static props.
- [ ] Validate the complete textured map-plus-props scene.

## Phase 7 — Native viewer usability

- [ ] Add native window resizing.
- [ ] Add keyboard/mouse freecam controls.
- [x] Add renderer statistics (FPS, frame time, draws, triangles, VRAM in the title bar).
- [ ] Add GPU/CPU frame timing.
- [x] Add visible/culled prop and triangle counts.
- [x] Add error reporting and asset diagnostics.
- [x] Load the scene on a background thread with a cancellable, responsive loading screen.
- [x] Decode textures on worker threads and upload within a per-frame time budget.
- [x] Upload VTF DXT1/3/5 textures as native BC1/BC2/BC3 with their stored mip chains.
- [x] Apply baked BSP lightmaps (atlas, displacements included) to world geometry.
- [x] Light static props from the baked leaf ambient cubes.
- [x] Render the 3D skybox (sky_camera, PVS-selected geometry and props) and the env_skypaint background.
- [x] Apply the sky_camera fog to the 3D skybox and the map sun to static props.
- [ ] Add HDR/bump lightmaps, static-prop vertex lighting (.vhv) and normal maps.
- [x] Draw entity-lump doors, prop_dynamic and brush doors at their spawn pose (static; no door animation, no modelscale).
- [x] Play map ambient_generic emitters and trigger_soundscape soundscapes (XAudio2; M mutes). Sound-script names, demo-driven sounds, occlusion and reverb are not done.
- [x] GMod-style main menu (recent demos, Open Demo, Free Roam); parse the chosen demo with gmoddem on every open.
- [x] Demo playback: players, player-placed props and door open/closed state follow the demo clock (Space, Left/Right, [ ], Tab, F, O, F10 menu).
- [x] Demo players: full body (clothing bonemerge), animation, name labels; NPCs (humans with their clothing, ATMs and pumps); vehicles; weapons; ragdolls; radio HUD; reconstructed NPC dialogue and reply options from the matching Lua pack.
- [x] First-person viewmodel (weapon model, pickup animation, sights/scopes). Scope is still an approximation (see the current work list).
- [ ] Add PIX-friendly debug labels and markers.

## Definition of done for the current objective

- [ ] The native application opens the v6.3 map.
- [ ] The camera can move freely through the map.
- [ ] BSP geometry has correct depth and perspective.
- [x] BSP lighting/lightmaps are visible.
- [ ] BSP materials use raw VMT/VTF assets.
- [x] All available static props appear at their recorded BSP transforms.
- [ ] Static props use their raw VMT/VTF materials.
- [x] Missing model and material assets are reported explicitly.
- [ ] No entity, prop, or material relationships are invented.
- [ ] The result is visually comparable to the existing WebGPU map-plus-props viewer.
