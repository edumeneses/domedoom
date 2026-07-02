# DomeDoom — Fulldome Edition

DomeDoom is a GZDoom fork that renders the scene as a 6-face cubemap and
streams it to a fulldome planetarium pipeline (PipeWire, Sh4lt, NDI) for the
Satosphère dome at the Société des Arts Technologiques (SAT).

## TODO

Tracked work for DomeDoom (check the boxes as items land):

- [x] **Vulkan backend support for the cubemap pipeline.**
  `CompositeCubemapFaces` and `ReadCubemapCrossPixels` are now implemented
  for the Vulkan backend (`VulkanRenderDevice`), using `vkCmdBlitImage` to
  assemble the strip and an image→staging-buffer copy for readback, so
  PipeWire/Sh4lt/NDI work on the default (Vulkan) backend. Canvas-texture
  images gained `TRANSFER_SRC|DST` usage to allow this. (DMA-BUF export
  `ExportCubemapCrossAsDmaBuf` remains OpenGL-only; Vulkan uses the CPU
  readback path. Zero-copy DMA-BUF export on Vulkan is a future optimization.)
- [x] **Rename the locally built executable from `gzdoom` to `domedoom`.**
  The default `ZDOOM_EXE_NAME` is now `domedoom`, so local builds produce a
  `domedoom` binary (CI already set this explicitly).
- [ ] **Fix AppImage build for NDI and OSC/SpatGRIS.**
  NDI fails at runtime because `libndi.so.6` is not bundled (intentionally —
  loaded via `dlopen`) but the AppImage may land on a system without the NDI
  runtime; document the install requirement more prominently or bundle the
  redistributable `.so`. OSC (SpatGRIS positions) should work in principle
  (raw UDP, no external library) but needs verification on a clean AppImage
  host — check whether the `r_cubemap_spatgris` CVAR survives the AppImage
  startup config path. Reference: ossia score's AppImage CI produces a working
  OSC + NDI bundle worth diffing against.

---

## Fulldome Configuration

All options live in the `r_cubemap_*` namespace and can be set in the console
or in a startup config file.

### Video / streaming

| CVar | Default | Description |
|------|---------|-------------|
| `r_cubemap` | `false` | Enable cubemap rendering pipeline |
| `r_cubemap_pipewire` | `true` | PipeWire DMA-BUF output |
| `r_cubemap_sh4lt` | `false` | Sh4lt video output |
| `r_cubemap_sh4lt_label` | `"domedoom"` | Sh4lt video stream label |
| `r_cubemap_sh4lt_audio` | `false` | Sh4lt audio tap output |
| `r_cubemap_sh4lt_audio_label` | `"domedoom-audio"` | Sh4lt audio stream label |
| `r_cubemap_ndi` | `false` | NDI video output |
| `r_cubemap_ndi_label` | `"DomeDoom"` | NDI source name |
| `r_cubemap_debug` | `false` | Debug logging |

All of the above are also exposed in-game under **Options → Fulldome Output**.

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
