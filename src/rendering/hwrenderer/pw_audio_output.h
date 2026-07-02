#pragma once

// Manages a single N-channel S16 PipeWire audio stream for SpatGRIS.
// Appears as one device ("domedoom [spat]") with N mono output ports in
// qpwgraph. Call Init() at startup; AllocSlot/FreeSlot per active source.

#ifdef HAVE_PIPEWIRE

#include <pipewire/pipewire.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class PipeWireAudioOutput
{
public:
    static constexpr int OUT_RATE  = 48000;
    static constexpr int MAX_SLOTS = 64;

    ~PipeWireAudioOutput();

    // Create a single N-channel stream. Returns false on failure.
    bool Init(int numSlots);
    void Shutdown();

    // Load PCM for slot and begin streaming.  Source PCM may be 8 or 16 bit,
    // mono or stereo, any sample rate.  Resampled to OUT_RATE mono S16.
    void AllocSlot(int slot, const void* pcm, size_t bytes,
                   int srcRate, int bits, int srcChannels);

    // Silence this slot.
    void FreeSlot(int slot);

    bool IsRunning() const { return mRunning; }
    int  NumSlots()  const { return mNumSlots; }

private:
    struct Slot
    {
        std::mutex            pcmLock;
        std::vector<int16_t>  pcm;
        std::atomic<uint32_t> readPos{0};
        std::atomic<bool>     active{false};
    };

    static void OnProcess(void* data);
    static void OnStateChanged(void*, enum pw_stream_state,
                               enum pw_stream_state, const char*);

    pw_thread_loop* mLoop     = nullptr;
    pw_stream*      mStream   = nullptr;
    Slot            mSlots[MAX_SLOTS];
    int             mNumSlots = 0;
    bool            mRunning  = false;
};

#else // !HAVE_PIPEWIRE

class PipeWireAudioOutput
{
public:
    bool Init(int)                                              { return false; }
    void Shutdown()                                             {}
    void AllocSlot(int, const void*, size_t, int, int, int)    {}
    void FreeSlot(int)                                          {}
    bool IsRunning() const                                      { return false; }
    int  NumSlots()  const                                      { return 0; }
};

#endif // HAVE_PIPEWIRE
