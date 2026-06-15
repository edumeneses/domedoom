//
// pw_audio_output.cpp — single N-channel PipeWire audio stream for SpatGRIS
//
// Creates one "cubedoom [spat]" stream with N mono channels so it appears as
// a single device in qpwgraph/Helvum with N output ports that can be patched
// directly into SpatGRIS input channels.
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
    if (!sPwAudioInitDone) { pw_init(nullptr, nullptr); sPwAudioInitDone = true; }
}

// Convert raw PCM to mono S16, then linear-resample to OUT_RATE.
static std::vector<int16_t> ToMonoS16At48k(const void* pcm, size_t bytes,
                                            int srcRate, int bits, int srcCh)
{
    const size_t bps    = (size_t)(bits / 8);
    const size_t total  = bytes / (bps * (size_t)srcCh);

    std::vector<int16_t> mono(total);
    if (bits == 8)
    {
        const uint8_t* src = static_cast<const uint8_t*>(pcm);
        for (size_t i = 0; i < total; i++) {
            int32_t s = 0;
            for (int c = 0; c < srcCh; c++)
                s += (int32_t)(src[i * srcCh + c]) - 128;
            mono[i] = (int16_t)std::clamp(s * 256 / srcCh, -32768, 32767);
        }
    }
    else
    {
        const int16_t* src = static_cast<const int16_t*>(pcm);
        for (size_t i = 0; i < total; i++) {
            int32_t s = 0;
            for (int c = 0; c < srcCh; c++) s += src[i * srcCh + c];
            mono[i] = (int16_t)(s / srcCh);
        }
    }

    if (srcRate == PipeWireAudioOutput::OUT_RATE)
        return mono;

    const size_t outN = (size_t)((double)total * PipeWireAudioOutput::OUT_RATE / srcRate + 0.5);
    std::vector<int16_t> out(outN);
    for (size_t i = 0; i < outN; i++) {
        double   pos  = (double)i * srcRate / PipeWireAudioOutput::OUT_RATE;
        size_t   idx  = (size_t)pos;
        double   frac = pos - (double)idx;
        int32_t  s0   = (idx     < total) ? mono[idx]     : 0;
        int32_t  s1   = (idx + 1 < total) ? mono[idx + 1] : 0;
        out[i] = (int16_t)(s0 + (int32_t)((s1 - s0) * frac));
    }
    return out;
}

// ---------------------------------------------------------------------------
// PW callbacks

void PipeWireAudioOutput::OnStateChanged(void* /*data*/,
                                          enum pw_stream_state /*old*/,
                                          enum pw_stream_state /*state*/,
                                          const char* /*error*/) {}

void PipeWireAudioOutput::OnProcess(void* data)
{
    auto* self = static_cast<PipeWireAudioOutput*>(data);

    pw_buffer* pwbuf = pw_stream_dequeue_buffer(self->mStream);
    if (!pwbuf) return;

    spa_data& d = pwbuf->buffer->datas[0];
    if (!d.data) { pw_stream_queue_buffer(self->mStream, pwbuf); return; }

    const int      N      = self->mNumSlots;
    const uint32_t frames = d.maxsize / (uint32_t)(N * sizeof(int16_t));
    int16_t*       dst    = static_cast<int16_t*>(d.data);

    for (int s = 0; s < N; s++)
    {
        Slot& slot = self->mSlots[s];
        std::unique_lock<std::mutex> lock(slot.pcmLock, std::try_to_lock);

        if (!lock || !slot.active.load(std::memory_order_acquire))
        {
            for (uint32_t f = 0; f < frames; f++) dst[f * N + s] = 0;
            continue;
        }

        const uint32_t pos   = slot.readPos.load(std::memory_order_relaxed);
        const uint32_t avail = (pos < (uint32_t)slot.pcm.size())
                               ? (uint32_t)slot.pcm.size() - pos : 0u;
        const uint32_t n     = std::min(frames, avail);

        for (uint32_t f = 0; f < n;      f++) dst[f * N + s] = slot.pcm[pos + f];
        for (uint32_t f = n; f < frames; f++) dst[f * N + s] = 0;

        slot.readPos.store(pos + n, std::memory_order_relaxed);
    }

    d.chunk->offset = 0;
    d.chunk->size   = frames * (uint32_t)(N * sizeof(int16_t));
    d.chunk->stride = (int32_t)(N * sizeof(int16_t));
    pw_stream_queue_buffer(self->mStream, pwbuf);
}

// ---------------------------------------------------------------------------
// public API

bool PipeWireAudioOutput::Init(int numSlots)
{
    if (mRunning)  return true;
    if (numSlots < 1 || numSlots > MAX_SLOTS) return false;

    EnsurePwInit();

    mLoop = pw_thread_loop_new("cubedoom-spat", nullptr);
    if (!mLoop) {
        fprintf(stderr, "[cubedoom/pw-audio] pw_thread_loop_new failed\n");
        return false;
    }

    static const pw_stream_events kEvents = {
        .version       = PW_VERSION_STREAM_EVENTS,
        .state_changed = OnStateChanged,
        .process       = OnProcess,
    };

    mStream = pw_stream_new_simple(
        pw_thread_loop_get_loop(mLoop),
        "cubedoom-spat",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,     "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE,     "Game",
            PW_KEY_NODE_NAME,      "cubedoom-spat",
            PW_KEY_NODE_DESCRIPTION, "CubeDoom [spat]",
            nullptr),
        &kEvents, this);

    if (!mStream) {
        fprintf(stderr, "[cubedoom/pw-audio] pw_stream_new_simple failed\n");
        pw_thread_loop_destroy(mLoop);
        mLoop = nullptr;
        return false;
    }

    // Build N-channel audio format param.
    uint8_t pbuf[2048];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pbuf, sizeof(pbuf));

    spa_audio_info_raw ai = {};
    ai.format   = SPA_AUDIO_FORMAT_S16;
    ai.rate     = (uint32_t)OUT_RATE;
    ai.channels = (uint32_t)numSlots;
    for (int i = 0; i < numSlots; i++)
        ai.position[i] = SPA_AUDIO_CHANNEL_AUX0 + (uint32_t)i;

    const int bufBytes = numSlots * 256 * (int)sizeof(int16_t); // 256-frame default
    const spa_pod* params[2];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &ai);
    params[1] = (spa_pod*)spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers,  SPA_POD_CHOICE_RANGE_Int(2, 1, 32),
        SPA_PARAM_BUFFERS_blocks,   SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,     SPA_POD_CHOICE_RANGE_Int(bufBytes, bufBytes / 4, bufBytes * 8),
        SPA_PARAM_BUFFERS_stride,   SPA_POD_Int(numSlots * (int)sizeof(int16_t)));

    int ret = pw_stream_connect(mStream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                (pw_stream_flags)(PW_STREAM_FLAG_DRIVER |
                                                  PW_STREAM_FLAG_MAP_BUFFERS),
                                params, 2);
    if (ret < 0) {
        fprintf(stderr, "[cubedoom/pw-audio] pw_stream_connect: %d\n", ret);
        pw_stream_destroy(mStream);  mStream = nullptr;
        pw_thread_loop_destroy(mLoop); mLoop = nullptr;
        return false;
    }

    mNumSlots = numSlots;
    pw_thread_loop_start(mLoop);
    mRunning = true;
    fprintf(stderr, "[cubedoom/pw-audio] single %d-ch stream \"CubeDoom [spat]\" at %d Hz\n",
            numSlots, OUT_RATE);
    return true;
}

void PipeWireAudioOutput::AllocSlot(int slot, const void* pcm, size_t bytes,
                                    int srcRate, int bits, int srcCh)
{
    if (slot < 0 || slot >= mNumSlots || !pcm || bytes == 0) return;
    if (bits != 8 && bits != 16) return;

    auto newPCM = ToMonoS16At48k(pcm, bytes, srcRate, bits, srcCh);
    Slot& s = mSlots[slot];
    std::lock_guard<std::mutex> lock(s.pcmLock);
    s.active.store(false, std::memory_order_relaxed);
    s.pcm = std::move(newPCM);
    s.readPos.store(0, std::memory_order_relaxed);
    s.active.store(true, std::memory_order_release);
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
    if (mLoop)   pw_thread_loop_stop(mLoop);
    if (mStream) { pw_stream_disconnect(mStream); pw_stream_destroy(mStream); mStream = nullptr; }
    if (mLoop)   { pw_thread_loop_destroy(mLoop); mLoop = nullptr; }
    mRunning  = false;
    mNumSlots = 0;
}

PipeWireAudioOutput::~PipeWireAudioOutput() { Shutdown(); }

#endif // HAVE_PIPEWIRE
