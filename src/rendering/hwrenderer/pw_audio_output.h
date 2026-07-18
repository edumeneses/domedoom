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

    // ---- Stereo bed (slots 0 = left, 1 = right) --------------------------
    // When enabled, slots 0/1 stop acting as one-shot voices and instead play
    // the "stereo bed": a continuous music ring plus short 2D/UI one-shots.
    void EnableBed(bool on);
    bool BedEnabled() const { return mBedEnabled; }

    // Append music/stream audio to the bed ring. Any rate/format/channels;
    // converted to stereo S16 at OUT_RATE, scaled by gain. Called from the
    // music streaming thread.
    void PushBedStream(const void* pcm, size_t bytes, int srcRate,
                       int srcChannels, bool isFloat, float gain);

    // Play a one-shot 2D/UI sound on both bed channels (centred). Same PCM
    // formats as AllocSlot, scaled by gain. Called from the game thread.
    void PlayBedSound(const void* pcm, size_t bytes, int srcRate, int bits,
                      int srcChannels, float gain);

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

    static constexpr int BED_RING_FRAMES = OUT_RATE;  // 1 s of stereo bed
    static constexpr int BED_VOICES      = 8;         // concurrent UI one-shots

    static void OnProcess(void* data);
    static void OnStateChanged(void*, enum pw_stream_state,
                               enum pw_stream_state, const char*);

    pw_thread_loop* mLoop     = nullptr;
    pw_stream*      mStream   = nullptr;
    Slot            mSlots[MAX_SLOTS];
    int             mNumSlots = 0;
    bool            mRunning  = false;

    // Stereo bed state. Ring: single writer (music thread), single reader
    // (PipeWire thread). Voices: game-thread writer, PW-thread reader, per
    // voice locking via the same Slot machinery.
    std::atomic<bool>     mBedEnabled{false};
    std::mutex            mBedWriteLock;              // serialises PushBedStream
    std::vector<int16_t>  mBedRing;                   // interleaved L/R
    std::atomic<uint32_t> mBedWrite{0}, mBedRead{0};  // frame counters (mod ring)
    Slot                  mBedVoices[BED_VOICES];
    int                   mBedVoiceNext = 0;          // round-robin replacement
};

#else // !HAVE_PIPEWIRE

class PipeWireAudioOutput
{
public:
    bool Init(int)                                              { return false; }
    void Shutdown()                                             {}
    void AllocSlot(int, const void*, size_t, int, int, int)    {}
    void FreeSlot(int)                                          {}
    void EnableBed(bool)                                        {}
    bool BedEnabled() const                                     { return false; }
    void PushBedStream(const void*, size_t, int, int, bool, float) {}
    void PlayBedSound(const void*, size_t, int, int, int, float)   {}
    bool IsRunning() const                                      { return false; }
    int  NumSlots()  const                                      { return 0; }
};

#endif // HAVE_PIPEWIRE
