# DomeDoom — Fulldome Edition

DomeDoom is a GZDoom fork that renders the scene as a 6-face cubemap and
either streams the raw cubemap strip or warps it into a square fisheye
**domemaster** image, feeding a fulldome planetarium pipeline (PipeWire,
Sh4lt, NDI) for the Satosphère dome at the Société des Arts Technologiques
(SAT). It also sends per-source 3D positions to SpatGRIS over OSC for
object-based dome audio.

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
| `r_cubemap_domemaster` | `true` | Output square fisheye domemaster instead of the 6-face strip (needs restart — re-inits PipeWire + readback buffers) |
| `r_cubemap_pipewire` | `true` | PipeWire DMA-BUF output |
| `r_cubemap_sh4lt` | `false` | Sh4lt video output |
| `r_cubemap_sh4lt_label` | `"domedoom"` | Sh4lt video stream label |
| `r_cubemap_sh4lt_audio` | `false` | Sh4lt audio tap output |
| `r_cubemap_sh4lt_audio_label` | `"domedoom-audio"` | Sh4lt audio stream label |
| `r_cubemap_ndi` | `false` | NDI video output |
| `r_cubemap_ndi_label` | `"DomeDoom"` | NDI source name |
| `r_cubemap_debug` | `false` | Debug logging |

### Domemaster warp / orientation

Only active when `r_cubemap_domemaster` is `true`. The warp assembles the six
cube faces into a fisheye; orientation and flips are exposed live because
OpenGL and Vulkan differ in NDC and texture origin, so tuned values vary per
machine/backend.

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
| `r_cubemap_dome_lock_yaw` | `false` | Lock the dome to a fixed world heading — the projected world stops following the player, so turning/rotating moves the player *across* a static dome instead of spinning the whole image. Latched to the player's yaw when first enabled. |

### Rim HUD (domemaster only)

The status bar / HUD is drawn as a band along the front rim of the dome so it
stays readable in fisheye output. Auto-follows the forward view.

| CVar | Default | Description |
|------|---------|-------------|
| `r_cubemap_dome_hud` | `true` | Enable the rim HUD band |
| `r_cubemap_dome_hud_arc` | `45` | Arc width of the band, degrees |
| `r_cubemap_dome_hud_band` | `0.035` | Radial thickness of the band (fraction of radius) |
| `r_cubemap_dome_hud_strip` | `0.07` | Source strip height sampled from the HUD |
| `r_cubemap_dome_hud_offset` | `0` | Manual angular offset from the forward view, degrees |
| `r_cubemap_dome_hud_crop` | `0.275` | Crop each side of the band (0 = full width, 0.49 = almost nothing) |
| `r_cubemap_dome_hud_flip_h` | `false` | Flip the HUD band horizontally |
| `r_cubemap_dome_hud_flip_v` | `true` | Flip the HUD band vertically |

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
| 1 | first active 3D sound | reassigned each time a new sound claims an empty slot |
| 2 | second active 3D sound | |
| … | … | |
| N | Nth active 3D sound | N = `r_cubemap_spatgris_sources` (default 32) |

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
Fulldome Output → *Lock dome heading*). With it on, turning and `/domedoom/rotate`
move the player *across a fixed dome image* instead of spinning the whole
projected world — DoomGuy relocates on the dome surface while the background
stays put. With it off, turning yaws the entire dome (normal Doom behaviour).

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

**Recent fixes** — sprites no longer split across cube-face seams (billboards
forced to face the camera); the GL dome pass fully saves/restores GL state
(fixes a GL freeze); clean shutdown no longer crashes with "pure virtual method
called"; FluidSynth "Not a RIFF file" soundfont spam silenced.

**AppImage / NDI runtime** — NDI is compiled in when the SDK headers are
present at build time (CI installs them), but `libndi.so.6` is *not* bundled;
it is loaded via `dlopen` at first use, so the host running the AppImage must
have the NDI 6 runtime installed
(https://ndi.video/download-ndi-sdk/ or the NDI Tools/Redist package).
OSC (SpatGRIS positions) uses raw UDP with no external library.

---

# Welcome to GZDoom!

[![Continuous Integration](https://github.com/ZDoom/gzdoom/actions/workflows/continuous_integration.yml/badge.svg)](https://github.com/ZDoom/gzdoom/actions/workflows/continuous_integration.yml)

## GZDoom is a modder-friendly OpenGL and Vulkan source port based on the DOOM engine

Copyright (c) 1998-2025 ZDoom + GZDoom teams, and contributors

Doom Source (c) 1997 id Software, Raven Software, and contributors

Please see license files for individual contributor licenses

Special thanks to Coraline of the EDGE team for allowing us to use her [README.md](https://github.com/3dfxdev/EDGE/blob/master/README.md) as a template for this one.

### Source code licensed under the GPL v3
##### https://www.gnu.org/licenses/quick-guide-gplv3.en.html
---

## How to build GZDoom

To build GZDoom, please see the [wiki](https://zdoom.org/wiki/) and see the "Programmer's Corner" on the bottom-right corner of the page to build for your platform.

# Resources
- https://zdoom.org/ - Home Page
- https://forum.zdoom.org/ - Forum
- https://zdoom.org/wiki/ - Wiki
- https://dsc.gg/zdoom - Discord Server
- https://docs.google.com/spreadsheets/d/1pvwXEgytkor9SClCiDn4j5AH7FedyXS-ocCbsuQIXDU/edit?usp=sharing - Translation sheet (Google Docs)
