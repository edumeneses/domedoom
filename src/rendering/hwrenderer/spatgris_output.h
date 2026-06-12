#pragma once
#include <cstdint>

// SpatGRIS object-based audio output.
// Sends per-source positions as OSC /spat/serv deg messages over UDP.
// All functions are game-thread-only — no locking needed.

void SpatGRIS_UpdateListener(float x, float y, float z, float angleRad);
void SpatGRIS_AllocSource(uint32_t alSrc, float sx, float sy, float sz);
void SpatGRIS_UpdateSource(uint32_t alSrc, float sx, float sy, float sz);
void SpatGRIS_FreeSource(uint32_t alSrc);
