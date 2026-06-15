#pragma once
#include <cstdint>

// Non-owning view into the PCM side-cache.  Valid until the AL buffer is
// deleted (OAL_EvictSFXPCM is called from UnloadSound).
struct OALPCMView {
    const uint8_t* data;
    size_t         bytes;
    int            sampleRate;
    int            bits;      // 8 or 16  (float32 not cached)
    int            channels;  // 1 or 2
};

// Returns true and fills *out if the buffer has cached PCM data.
bool OAL_GetSFXPCM(uint32_t alBuffer, OALPCMView* out);
