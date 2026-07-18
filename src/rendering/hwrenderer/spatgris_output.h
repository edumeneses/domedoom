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

// Dome yaw-lock state, pushed each frame by the cubemap renderer. When the
// lock is active the world image is frozen at lockYawDeg and the gun orbits
// to curYawDeg; SpatGRIS azimuths follow the same rule — world sounds are
// computed against the locked heading (stay fixed on the dome) and the
// player's own sounds / the stereo-bed pair follow the gun.
void SpatGRIS_SetGunYaw(bool lockActive, float lockYawDeg, float curYawDeg);

// Route a 2D/UI sound into the stereo bed (SpatGRIS channels 1/2). Returns
// true when the sound was taken — the caller should mute its stereo copy.
bool SpatGRIS_Start2DSound(uint32_t alSrc, uint32_t alBuf, float gain);

// Clear all stereo-mix mutes and restore OpenAL gains — called when
// spatialization is turned off mid-game so live sounds become audible again.
void SpatGRIS_UnmuteAll();
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
