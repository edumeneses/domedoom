#pragma once
#include <cstddef>

// Audio taps called from OpenALSoundStream::Process() after each PCM batch
// fill (music / movie audio). `gain` is the stream's effective volume
// (MusicVolume * stream volume) — the tapped data is pre-gain. A tap returns
// true to CONSUME the batch: the stream's OpenAL source is then muted so the
// audio plays only through the tap's output. nullptr = disabled.
using OALAudioTapFn = bool (*)(const void* data, size_t bytes,
                                int samplerate, int channels, bool isFloat,
                                float gain);

extern OALAudioTapFn g_oalAudioTap;    // Sh4lt audio mirror (never consumes)
extern OALAudioTapFn g_oalMusicTap;    // SpatGRIS stereo bed (consumes)
