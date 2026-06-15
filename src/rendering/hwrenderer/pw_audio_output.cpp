//
// pw_audio_output.cpp — per-source PipeWire mono audio for SpatGRIS
//
// Each SpatGRIS slot gets its own S16 mono PipeWire stream at 48 kHz.
// Source PCM is resampled (linear) and stored locally; the pw process
// callback streams it in real time.  When a slot is idle it sends silence.
//

#include "pw_audio_output.h"

#ifdef HAVE_PIPEWIRE

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// helpers

static bool sPwAudioInitDone = false;
static void EnsurePwInit()
{
    if (!sPwAudioInitDone)
    {
        pw_init(nullptr, nullptr);
        sPwAudioInitDone = true;
    }
}

// Linear resampler: input is mono S16, output is mono S16 at OUT_RATE.
static std::vector<int16_t> Resample(const int16_t* in, size_t inCount, int srcRate)
{
    if (srcRate == PipeWireAudioOutput::OUT_RATE)
        return std::vector<int16_t>(in, in + inCount);

    size_t outCount = (size_t)((double)inCount * PipeWireAudioOutput::OUT_RATE / srcRate + 0.5);
    std::vector<int16_t> out(outCount);
    for (size_t i = 0; i < outCount; i++)
    {
        double pos  = (double)i * srcRate / PipeWireAudioOutput::OUT_RATE;
        size_t idx  = (size_t)pos;
        double frac = pos - (double)idx;
        int32_t s0  = (idx     < inCount) ? in[idx]     : 0;
        int32_t s1  = (idx + 1 < inCount) ? in[idx + 1] : 0;
        out[i] = (int16_t)(s0 + (int32_t)((s1 - s0) * frac));
    }
    return out;
}

// Convert raw PCM bytes to mono S16, then resample.
static std::vector<int16_t> ToMonoS16At48k(const void* pcm, size_t bytes,
                                            int srcRate, int bits, int srcChannels)
{
    const size_t bytesPerSample = (size_t)(bits / 8);
    const size_t totalSamples   = bytes / (bytesPerSample * (size_t)srcChannels);

    // Step 1: convert to S16 mono
    std::vector<int16_t> mono(totalSamples);
    if (bits == 8)
    {
        const uint8_t* src = static_cast<const uint8_t*>(pcm);
        for (size_t i = 0; i < totalSamples; i++)
        {
            int32_t sum = 0;
            for (int c = 0; c < srcChannels; c++)
                sum += (int32_t)(src[i * srcChannels + c]) - 128;
            // 8-bit unsigned → centre at 0, scale to S16 range
            mono[i] = (int16_t)std::clamp(sum * 256 / srcChannels, -32768, 32767);
        }
    }
    else // 16-bit
    {
        const int16_t* src = static_cast<const int16_t*>(pcm);
        for (size_t i = 0; i < totalSamples; i++)
        {
            int32_t sum = 0;
            for (int c = 0; c < srcChannels; c++)
                sum += src[i * srcChannels + c];
            mono[i] = (int16_t)(sum / srcChannels);
        }
    }

    // Step 2: resample to 48 kHz
    return Resample(mono.data(), mono.size(), srcRate);
}

// ---------------------------------------------------------------------------
// PW callbacks

void PipeWireAudioOutput::Slot::OnStateChanged(void* /*data*/,
                                                enum pw_stream_state /*old*/,
                                                enum pw_stream_state /*state*/,
                                                const char* /*error*/)
{
    // Nothing to do — silence is always streamed regardless of state.
}

void PipeWireAudioOutput::Slot::OnProcess(void* data)
{
    auto* slot = static_cast<Slot*>(data);

    pw_buffer* pwbuf = pw_stream_dequeue_buffer(slot->stream);
    if (!pwbuf) return;

    spa_buffer* spabuf = pwbuf->buffer;
    spa_data&   d      = spabuf->datas[0];
    if (!d.data)
    {
        pw_stream_queue_buffer(slot->stream, pwbuf);
        return;
    }

    const uint32_t maxSamples = d.maxsize / sizeof(int16_t);
    int16_t*       dst        = static_cast<int16_t*>(d.data);

    // Try non-blocking lock; on failure output silence (avoids blocking PW thread).
    std::unique_lock<std::mutex> lock(slot->pcmLock, std::try_to_lock);
    if (!lock || !slot->active.load(std::memory_order_acquire))
    {
        std::memset(dst, 0, maxSamples * sizeof(int16_t));
    }
    else
    {
        const uint32_t pos       = slot->readPos.load(std::memory_order_relaxed);
        const uint32_t available = (uint32_t)slot->pcm.size();
        const uint32_t remaining = (pos < available) ? (available - pos) : 0u;
        const uint32_t toCopy    = std::min(maxSamples, remaining);

        if (toCopy > 0)
            std::memcpy(dst, slot->pcm.data() + pos, toCopy * sizeof(int16_t));
        if (toCopy < maxSamples)
            std::memset(dst + toCopy, 0, (maxSamples - toCopy) * sizeof(int16_t));

        slot->readPos.fetch_add(toCopy, std::memory_order_relaxed);
    }

    d.chunk->offset = 0;
    d.chunk->size   = maxSamples * sizeof(int16_t);
    d.chunk->stride = sizeof(int16_t);
    pw_stream_queue_buffer(slot->stream, pwbuf);
}

// ---------------------------------------------------------------------------
// public API

bool PipeWireAudioOutput::Init(int numSlots, const char* namePrefix)
{
    if (mRunning) return true;
    if (numSlots < 1 || numSlots > MAX_SLOTS) return false;

    EnsurePwInit();

    mLoop = pw_thread_loop_new("cubedoom-audio", nullptr);
    if (!mLoop)
    {
        fprintf(stderr, "[cubedoom/pw-audio] pw_thread_loop_new failed\n");
        return false;
    }

    static const pw_stream_events kSlotEvents = {
        .version       = PW_VERSION_STREAM_EVENTS,
        .state_changed = Slot::OnStateChanged,
        .process       = Slot::OnProcess,
    };

    mNumSlots = numSlots;

    for (int i = 0; i < numSlots; i++)
    {
        mSlots[i].index = i;

        char name[64];
        std::snprintf(name, sizeof(name), "%s-%02d", namePrefix, i + 1);

        mSlots[i].stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(mLoop),
            name,
            pw_properties_new(
                PW_KEY_MEDIA_TYPE,     "Audio",
                PW_KEY_MEDIA_CATEGORY, "Playback",
                PW_KEY_MEDIA_ROLE,     "Game",
                PW_KEY_NODE_NAME,      name,
                nullptr),
            &kSlotEvents,
            &mSlots[i]);

        if (!mSlots[i].stream)
        {
            fprintf(stderr, "[cubedoom/pw-audio] pw_stream_new_simple failed for slot %d\n", i);
            Shutdown();
            return false;
        }

        // Build audio format + buffer params.
        uint8_t pbuf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(pbuf, sizeof(pbuf));

        spa_audio_info_raw ai = {};
        ai.format   = SPA_AUDIO_FORMAT_S16;
        ai.rate     = (uint32_t)OUT_RATE;
        ai.channels = (uint32_t)OUT_CHANNELS;
        ai.position[0] = SPA_AUDIO_CHANNEL_MONO;

        const spa_pod* params[2];
        params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &ai);
        params[1] = (spa_pod*)spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers,  SPA_POD_CHOICE_RANGE_Int(2, 1, 32),
            SPA_PARAM_BUFFERS_blocks,   SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size,     SPA_POD_CHOICE_RANGE_Int(4096, 128, 65536),
            SPA_PARAM_BUFFERS_stride,   SPA_POD_Int((int)sizeof(int16_t)));

        int ret = pw_stream_connect(mSlots[i].stream,
                                    PW_DIRECTION_OUTPUT,
                                    PW_ID_ANY,
                                    (pw_stream_flags)(PW_STREAM_FLAG_DRIVER |
                                                      PW_STREAM_FLAG_MAP_BUFFERS),
                                    params, 2);
        if (ret < 0)
        {
            fprintf(stderr, "[cubedoom/pw-audio] pw_stream_connect slot %d: %d\n", i, ret);
            Shutdown();
            return false;
        }
    }

    pw_thread_loop_start(mLoop);
    mRunning = true;
    fprintf(stderr, "[cubedoom/pw-audio] %d mono streams at %d Hz\n", numSlots, OUT_RATE);
    return true;
}

void PipeWireAudioOutput::AllocSlot(int slot, const void* pcm, size_t bytes,
                                    int srcRate, int bits, int srcChannels)
{
    if (slot < 0 || slot >= mNumSlots) return;
    if (!pcm || bytes == 0) return;
    if (bits != 8 && bits != 16) return; // float32 not supported

    auto newPCM = ToMonoS16At48k(pcm, bytes, srcRate, bits, srcChannels);

    Slot& s = mSlots[slot];
    {
        std::lock_guard<std::mutex> lock(s.pcmLock);
        s.active.store(false, std::memory_order_relaxed);
        s.pcm = std::move(newPCM);
        s.readPos.store(0, std::memory_order_relaxed);
        s.active.store(true, std::memory_order_release);
    }
}

void PipeWireAudioOutput::FreeSlot(int slot)
{
    if (slot < 0 || slot >= mNumSlots) return;
    Slot& s = mSlots[slot];
    std::lock_guard<std::mutex> lock(s.pcmLock);
    s.active.store(false, std::memory_order_relaxed);
    s.pcm.clear();
    s.readPos.store(0, std::memory_order_relaxed);
}

void PipeWireAudioOutput::Shutdown()
{
    if (mLoop) pw_thread_loop_stop(mLoop);

    for (int i = 0; i < mNumSlots; i++)
    {
        if (mSlots[i].stream)
        {
            pw_stream_disconnect(mSlots[i].stream);
            pw_stream_destroy(mSlots[i].stream);
            mSlots[i].stream = nullptr;
        }
    }

    if (mLoop)
    {
        pw_thread_loop_destroy(mLoop);
        mLoop = nullptr;
    }

    mRunning  = false;
    mNumSlots = 0;
}

PipeWireAudioOutput::~PipeWireAudioOutput()
{
    Shutdown();
}

#endif // HAVE_PIPEWIRE
