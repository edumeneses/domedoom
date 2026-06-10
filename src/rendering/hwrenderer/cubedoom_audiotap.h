#pragma once
#include <cstddef>

// Audio tap installed by Sh4lt output — called from OpenALSoundStream::Process()
// after each PCM batch fill.  nullptr = disabled.
using OALAudioTapFn = void (*)(const void* data, size_t bytes,
                                int samplerate, int channels, bool isFloat);

extern OALAudioTapFn g_oalAudioTap;
