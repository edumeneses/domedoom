# DomeDoom — Fulldome Edition

DomeDoom is a ZDoom-family fork that renders the scene as a 6-face cubemap and
either streams the raw cubemap strip or warps it into a square fisheye
**domemaster** or a 2:1 **equirectangular** panorama, feeding a fulldome
planetarium pipeline (PipeWire, Sh4lt, NDI) for the Satosphère dome at the
Société des Arts Technologiques (SAT). It also sends per-source 3D positions to
SpatGRIS over OSC for object-based dome audio.

**Lineage:** DomeDoom started as a fork of [GZDoom](https://github.com/ZDoom/gzdoom)
(master, late 2025). In July 2026 the project switched its upstream to
[UZDoom](https://github.com/UZDoom/UZDoom) — the community continuation of
GZDoom by its long-time development team — and now tracks UZDoom trunk.
Mod/WAD compatibility is unchanged (UZDoom is a direct continuation of the
same engine).

Both render backends are supported: the cubemap compositing, readback, and
domemaster warp run on **OpenGL** and **Vulkan** (Vulkan uses `vkCmdBlitImage`
plus an image→staging-buffer readback; zero-copy DMA-BUF export remains an
OpenGL-only optimization). Local builds produce a `domedoom` binary
(`ZDOOM_EXE_NAME` default).

---

## Fulldome Configuration

All options live in the `r_cubemap_*` namespace and can be set in the console
or in a startup config file. Most are also exposed in-game under
**Options → Fulldome Output**.

### Video / streaming

| CVar | Default | Description |
|------|---------|-------------|
| `r_cubemap` | `false` | Enable cubemap rendering pipeline |
| `r_cubemap_mode` | `1` | Output projection: `0` = 6-face strip (6144x1024), `1` = square fisheye domemaster (2048x2048), `2` = equirectangular panorama (4096x2048). Needs restart — re-inits PipeWire + readback buffers. Replaces the old `r_cubemap_domemaster` bool. |
| `r_cubemap_pipewire` | `true` | PipeWire DMA-BUF output |
| `r_cubemap_sh4lt` | `false` | Sh4lt video output |
| `r_cubemap_sh4lt_label` | `"domedoom"` | Sh4lt video stream label |
| `r_cubemap_sh4lt_audio` | `false` | Sh4lt audio tap output |
| `r_cubemap_sh4lt_audio_label` | `"domedoom-audio"` | Sh4lt audio stream label |
| `r_cubemap_ndi` | `false` | NDI video output |
| `r_cubemap_ndi_label` | `"DomeDoom"` | NDI source name |
| `r_cubemap_debug` | `false` | Debug logging |

### Domemaster warp / orientation

Only active when `r_cubemap_mode` is `1` (domemaster). The warp assembles the
six cube faces into a fisheye; orientation and flips are exposed live because
OpenGL and Vulkan differ in NDC and texture origin, so tuned values vary per
machine/backend. The flip and face-swap toggles also apply to the
equirectangular warp (`r_cubemap_mode 2`), which has its own rotation:

| CVar | Default | Description |
|------|---------|-------------|
| `r_cubemap_equi_yaw` | `0` | Equirect content yaw, degrees |
| `r_cubemap_equi_pitch` | `0` | Equirect content pitch, degrees |
| `r_cubemap_equi_roll` | `0` | Equirect content roll, degrees |
| `r_cubemap_equi_flip_h` | `false` | Flip equirect output horizontally |
| `r_cubemap_equi_flip_v` | `true` | Flip equirect output vertically (correct for Vulkan) |

| CVar | Default | Description |
|------|---------|-------------|
| `r_cubemap_dome_fov` | `270` | Fisheye field of view, degrees |
| `r_cubemap_dome_yaw` | `180` | Orientation yaw, degrees |
| `r_cubemap_dome_pitch` | `90` | Orientation pitch, degrees |
| `r_cubemap_dome_roll` | `180` | Orientation roll, degrees |
| `r_cubemap_dome_flip_h` | `false` | Flip output horizontally |
| `r_cubemap_dome_flip_v` | `false` | Flip output vertically |
| `r_cubemap_dome_flip_ud` | `false` | Swap ceiling/floor |
| `r_cubemap_dome_swap_ud` | `true` | Swap up/down faces |
| `r_cubemap_dome_lock_yaw` | `false` | Lock the scene to a fixed dome heading. The domemaster output is counter-rotated by the player's yaw change (latched on enable), so the projected world stays still while the weapon orbits around the dome to show the player's aim. Domemaster and equirect. |

### HUD — status bar & menu

Each projection places the status bar differently so it stays readable:

- **Domemaster** — the status bar is warped into a band along the front rim,
  auto-following the forward view. The band shape is controlled by the
  arc/band/strip/offset/crop/flip parameters below (all shader-driven).
- **Equirectangular** — the status bar is baked onto the **front face**, where
  the weapon is, so it warps through the panorama *with the gun* instead of
  sitting flat at the bottom. Only `r_cubemap_dome_hud`,
  `r_cubemap_dome_hud_scale`, and `r_cubemap_dome_hud_crop` apply here; the
  arc/band/strip/offset/flip parameters are domemaster-only.
- **Cubemap strip** — the status bar is baked full-size onto the face selected
  by `r_cubemap_hud_face`.

The 2D **menu / console** (and the no-scene screen: title, intermission) is
baked onto the `r_cubemap_hud_face` face in every projection, so it is live
from launch — before any level loads. On the equirect the menu uses the
selected face; the status bar always stays on the front face with the weapon.

| CVar | Default | Projection | Description |
|------|---------|------------|-------------|
| `r_cubemap_dome_hud` | `true` | all | Enable the status-bar overlay |
| `r_cubemap_hud_face` | `0` (Front) | all | Face the 2D overlays (strip HUD, menu, no-scene screen) bake onto: `0`=Front `1`=Left `2`=Right `3`=Back `4`=Up `5`=Down. Equirect keeps the status bar on Front regardless. |
| `r_cubemap_dome_hud_scale` | `1.0` | equirect | Status-bar size on the front face: `1` = full width, `<1` shrinks it and centres it (uniform, no stretch) |
| `r_cubemap_dome_hud_crop` | `0.275` | equirect + domemaster | Trim each side (0 = full width, 0.49 = almost nothing). Equirect: clips the sides (no stretch); domemaster: crops the band source |
| `r_cubemap_dome_hud_arc` | `45` | domemaster | Arc width of the rim band, degrees |
| `r_cubemap_dome_hud_band` | `0.035` | domemaster | Radial thickness of the band (fraction of radius) |
| `r_cubemap_dome_hud_strip` | `0.035` | domemaster | Source strip height sampled from the HUD |
| `r_cubemap_dome_hud_offset` | `0` | domemaster | Manual angular offset from the forward view, degrees |
| `r_cubemap_dome_hud_flip_h` | `false` | domemaster | Flip the HUD band horizontally |
| `r_cubemap_dome_hud_flip_v` | `true` | domemaster | Flip the HUD band vertically |

NDI is compiled in when the NDI SDK headers are present at build time (CI
installs them); the `libndi.so.6` runtime is loaded via `dlopen` at first use,
so the host running the AppImage must have the NDI 6 runtime installed
(https://ndi.video/download-ndi-sdk/ or the NDI Tools/Redist package).

### SpatGRIS spatial audio

DomeDoom sends per-source 3D positions to [SpatGRIS](https://github.com/GRIS-UdeM/SpatGRIS)
over OSC UDP so the Satosphère's speaker array can do VBAP spatialization of
each Doom sound object independently.

| CVar | Default | Description |
|------|---------|-------------|
| `r_cubemap_spatgris` | `false` | Enable SpatGRIS object-based audio |
| `r_cubemap_spatgris_ip` | `"127.0.0.1"` | SpatGRIS host IP address |
| `r_cubemap_spatgris_port` | `18032` | SpatGRIS OSC UDP port |
| `r_cubemap_spatgris_stereo` | `false` | Stereo-only mode — disables OSC, uses OpenAL mix |
| `r_cubemap_spatgris_sources` | `32` | Source pool size (max 128; set before launch) |
| `r_cubemap_spatgris_mute3d` | `true` | Mute a 3D sound's stereo-mix copy once it plays on its own SpatGRIS channel — the stereo bed then carries only music + menu/UI sounds. Turn off to keep the full mix in stereo as well |
| `r_cubemap_spatgris_bed` | `true` | Reserve SpatGRIS channels 1/2 for the music + menu/UI mix ("stereo bed"), positioned like a pair of stereo speakers: channel 1 at 45° left of the gun, channel 2 at 45° right, at the dome edge. World sounds then use IDs 3+. Restart to re-slot |
| `r_cubemap_spatgris_gun_dist` | `0.5` | SpatGRIS distance for the player's own sounds (gunfire, pickups): 0 = dome centre, 1 = dome edge. They play at the gun's azimuth at this radius |

Turning `r_cubemap_spatgris` off (or `r_cubemap_spatgris_stereo` on) restores
plain stereo output at any time — live muted sounds get their gains back —
so a normal stereo mix for screen recording is always one toggle away.

With `r_cubemap_dome_lock_yaw` the audio follows the dome behaviour: world
sounds keep their azimuth relative to the locked heading (fixed on the dome,
like the image) while the player's own sounds and the stereo-bed pair follow
the orbiting gun.

**OSC message format** (SpatGRIS dome mode, degrees):
```
/spat/serv  "deg"  <source_id>  <azimuth_deg>  <elevation_deg>  <distance>  0.0  0.0
```
- `azimuth_deg`: degrees clockwise from front (0 = ahead, 90 = right, −90 = left)
- `elevation_deg`: degrees above horizon (0 = equator, 90 = zenith)
- `distance`: normalized `[0, 1]` — 0 = center, 1 = dome edge (= 2048 Doom units)

#### Source ID mapping

Slots are allocated dynamically from a pool of IDs 1–N
(`r_cubemap_spatgris_sources`). A slot is claimed when a 3D sound starts and
released when it stops. IDs are reused in FIFO order.

| SpatGRIS ID | Doom sound | Notes |
|-------------|-----------|-------|
| 1 | stereo bed LEFT (music + UI) | reserved while `r_cubemap_spatgris_bed` is on; 45° left of the gun |
| 2 | stereo bed RIGHT (music + UI) | reserved while `r_cubemap_spatgris_bed` is on; 45° right of the gun |
| 3 | first active 3D sound | reassigned each time a new sound claims an empty slot |
| … | … | |
| N | last active 3D sound | N = `r_cubemap_spatgris_sources` (default 32) |

With the bed disabled, 3D sounds start at ID 1.

2D sounds (UI, menus, HUD) use `StartSound`, not `StartSound3D`, and are never
sent to SpatGRIS — they remain in the OpenAL stereo mix.

#### Audio routing

SpatGRIS receives **positions** from DomeDoom via OSC. For **audio**, configure
OpenAL Soft to use the JACK backend (`snd_aldevice` in the console) and route
the JACK output ports into SpatGRIS's input channels, or use the Sh4lt audio
tap for a stereo bed. True per-source JACK audio (one port per active sound) is
a planned future enhancement.

---

## OSC game control

DomeDoom can be driven **in parallel** over OSC/UDP — the normal
keyboard/mouse/joystick input keeps working; OSC is folded into the same
analog input path. Intended for a dome-side controller (touch surface, show
control, etc.) that streams messages to the host running DomeDoom.

Enable it in-game under **Options → OSC Control**, or via CVars:

| CVar | Default | Description |
|------|---------|-------------|
| `osc_control` | `false` | Enable the OSC control receiver |
| `osc_control_port` | `6666` | UDP port to listen on (all interfaces) |
| `osc_control_debug` | `false` | Log received messages to stderr |

Send OSC messages to the host's IP on that port. Every address lives under the
`/domedoom/*` namespace. The single argument is the magnitude in `[0, 1]`
(`f` or `i`); if omitted it defaults to `1.0`. Send `0` to release.

| Address | Action |
|---------|--------|
| `/domedoom/front` `/domedoom/back` | Move forward / backward |
| `/domedoom/strafeleft` `/domedoom/straferight` | Strafe |
| `/domedoom/left` `/domedoom/right` | Turn (yaw) |
| `/domedoom/lookup` `/domedoom/lookdown` | Look (pitch) |
| `/domedoom/moveup` `/domedoom/movedown` | Fly / swim up / down |
| `/domedoom/attack` `/domedoom/altattack` | Fire |
| `/domedoom/use` `/domedoom/jump` `/domedoom/crouch` | Use / jump / crouch |
| `/domedoom/reload` `/domedoom/zoom` `/domedoom/run` | Reload / zoom / run modifier |
| `/domedoom/rotate` `<deg>` | **Absolute** view heading on the dome, `0–360°` |

`/domedoom/rotate` turns DoomGuy to an absolute compass heading — the natural
way to point the action at a given spot on the dome. All other controls are
relative and mirror the joystick axes/buttons.

**For the intended dome behaviour, enable `r_cubemap_dome_lock_yaw`** (Options →
Fulldome Output → *Lock dome heading*). With it on, the scene stays fixed on the
dome while turning and `/domedoom/rotate` orbit the weapon around the dome to
show where the player is aiming (the output is counter-rotated by the player's
yaw so the world does not spin). With it off, turning yaws the entire dome
(normal Doom behaviour, useful on a flat window).

Axes and buttons **auto-release ~250 ms** after their last message, so a
stalled sender never leaves DoomGuy running. A live controller streaming at
≥10 Hz holds the input; a single message acts as a brief pulse.

A dependency-free test sender lives at `tools/osc_test_sender.py`:

```
tools/osc_test_sender.py 127.0.0.1 6666 /domedoom/front 1.0
tools/osc_test_sender.py 127.0.0.1 6666 /domedoom/rotate 90
```

---

## Notes & known issues

**Recent changes** — equirectangular output mode (`r_cubemap_mode 2`) with a
size/crop-controllable status bar baked onto the front face; the menu, console,
and title/intermission screens are baked onto the `r_cubemap_hud_face` face so
the stream is live from launch, not only in-level; parallel OSC/UDP game
control (`/domedoom/*`). Earlier fixes: sprites no longer split across cube-face
seams (billboards forced to face the camera); the GL dome pass fully
saves/restores GL state (fixes a GL freeze); clean shutdown no longer crashes
with "pure virtual method called"; FluidSynth "Not a RIFF file" soundfont spam
silenced.

**AppImage / NDI runtime** — NDI is compiled in when the SDK headers are
present at build time (CI installs them), but `libndi.so.6` is *not* bundled;
it is loaded via `dlopen` at first use, so the host running the AppImage must
have the NDI 6 runtime installed
(https://ndi.video/download-ndi-sdk/ or the NDI Tools/Redist package).
OSC (SpatGRIS positions) uses raw UDP with no external library.

---

# About the engine (UZDoom)

DomeDoom's engine is [UZDoom](https://github.com/UZDoom/UZDoom), a
modder-friendly OpenGL and Vulkan source port based on the DOOM engine and the
community continuation of GZDoom.

Copyright (c) 1998-2025 ZDoom + GZDoom teams, and contributors
Copyright (c) 2025-2026 UZDoom maintainers and contributors

Doom Source (c) 1997 id Software, Raven Software, and contributors

Please see license files for individual contributor licenses

### Source code licensed under the GPL v3
##### https://www.gnu.org/licenses/quick-guide-gplv3.en.html

# Resources
- https://github.com/UZDoom/UZDoom - UZDoom (engine upstream)
- https://zdoom.org/ - ZDoom home page
- https://zdoom.org/wiki/ - Wiki (applies to UZDoom/GZDoom alike)
- https://forum.zdoom.org/ - Forum
