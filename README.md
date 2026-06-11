# CubeDoom — Fulldome Edition

CubeDoom is a GZDoom fork that renders the scene as a 6-face cubemap and
streams it to a fulldome planetarium pipeline (PipeWire, Sh4lt, NDI) for the
Satosphère dome at the Société des Arts Technologiques (SAT).

## TODO

Tracked work for CubeDoom (check the boxes as items land):

- [ ] **Vulkan backend support for the cubemap pipeline.** The cubemap
  composite + readback + DMA-BUF export (`CompositeCubemapFaces`,
  `ReadCubemapCrossPixels`, `ExportCubemapCrossAsDmaBuf`) are implemented
  only in `OpenGLFrameBuffer`; on Vulkan they fall back to the no-op
  virtual defaults, so all fulldome outputs (PipeWire/Sh4lt/NDI) produce a
  black frame. Implement the Vulkan equivalents so the dome works on the
  default (Vulkan) backend without forcing `vid_preferbackend 0`.
- [ ] **Rename the locally built executable from `gzdoom` to `cubedoom`.**
  The CI release sets `-DZDOOM_EXE_NAME=cubedoom`, but a plain local build
  still produces `gzdoom`. Make `cubedoom` the default executable name for
  local builds too.

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
