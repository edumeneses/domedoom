#pragma once

// Manages N mono S16 PipeWire audio streams — one per SpatGRIS slot.
// All streams share a single pw_thread_loop.
//
// Usage:
//   Init(32)                  — create 32 idle streams (output silence)
//   AllocSlot(slot, pcm, ...) — load PCM for slot, start playing
//   FreeSlot(slot)            — return slot to silence
//   Shutdown()                — destroy everything

#ifdef HAVE_PIPEWIRE

#include <pipewire/pipewire.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class PipeWireAudioOutput
{
public:
    static constexpr int OUT_RATE     = 48000;
    static constexpr int OUT_CHANNELS = 1;
    static constexpr int MAX_SLOTS    = 128;

    ~PipeWireAudioOutput();

    // Create numSlots streams.  Returns false on any PipeWire failure.
    bool Init(int numSlots, const char* namePrefix = "CubeDoom-Src");
    void Shutdown();

    // Load PCM for slot and begin streaming.
    // Source PCM may be 8 or 16 bit, mono or stereo, any sample rate.
    // It is resampled to OUT_RATE mono S16 internally.
    void AllocSlot(int slot, const void* pcm, size_t bytes,
                   int srcRate, int bits, int srcChannels);

    // Silence slot and discard its PCM.
    void FreeSlot(int slot);

    bool IsRunning() const { return mRunning; }
    int  NumSlots()  const { return mNumSlots; }

private:
    struct Slot
    {
        pw_stream*            stream   = nullptr;
        std::mutex            pcmLock;
        std::vector<int16_t>  pcm;
        std::atomic<uint32_t> readPos{0};
        std::atomic<bool>     active{false};
        int                   index    = 0;

        static void OnProcess(void* data);
        static void OnStateChanged(void*, enum pw_stream_state,
                                   enum pw_stream_state, const char*);
    };

    pw_thread_loop* mLoop     = nullptr;
    Slot            mSlots[MAX_SLOTS];
    int             mNumSlots = 0;
    bool            mRunning  = false;
};

#else // !HAVE_PIPEWIRE

class PipeWireAudioOutput
{
public:
    bool Init(int, const char* = nullptr)                                { return false; }
    void Shutdown()                                                      {}
    void AllocSlot(int, const void*, size_t, int, int, int)             {}
    void FreeSlot(int)                                                   {}
    bool IsRunning() const                                               { return false; }
    int  NumSlots()  const                                               { return 0; }
};

#endif // HAVE_PIPEWIRE
