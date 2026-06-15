#pragma once
#include <cstdint>

// SpatGRIS object-based audio output.
// Sends per-source positions as OSC /spat/serv deg messages over UDP.
// All functions are game-thread-only — no locking needed.

// Called from the r_cubemap_spatgris CUSTOM_CVAR callback to init/shutdown
// the OSC socket and PipeWire audio stream at CVAR-change time.
void SpatGRIS_InitAudio();
void SpatGRIS_ShutdownAudio();

void SpatGRIS_UpdateListener(float x, float y, float z, float angleRad);
void SpatGRIS_AllocSource(uint32_t alSrc, uint32_t alBuf, float sx, float sy, float sz);
void SpatGRIS_UpdateSource(uint32_t alSrc, float sx, float sy, float sz);
void SpatGRIS_FreeSource(uint32_t alSrc);
