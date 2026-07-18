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
// Returns true when the sound's PCM was routed to a per-source PipeWire
// channel AND r_cubemap_spatgris_mute3d is set — the caller should then mute
// the source in the OpenAL stereo mix so it plays only on its SpatGRIS
// channel. False = keep the stereo copy (stereo mode, no free slot, no PCM,
// per-source audio not running, or muting disabled).
bool SpatGRIS_AllocSource(uint32_t alSrc, uint32_t alBuf, float sx, float sy, float sz);
void SpatGRIS_UpdateSource(uint32_t alSrc, float sx, float sy, float sz);
void SpatGRIS_FreeSource(uint32_t alSrc);
// True while alSrc is muted in the stereo mix (AllocSource returned true and
// FreeSource hasn't run) — volume updates must not restore its OpenAL gain.
bool SpatGRIS_SourceSpatialized(uint32_t alSrc);
