//
// pw_audio_output.cpp — single N-channel PipeWire audio stream for SpatGRIS
//
// Creates one "domedoom [spat]" stream with N mono channels so it appears as
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

static inline int16_t ClampS16(int32_t v)
{
    return (int16_t)std::clamp(v, -32768, 32767);
}

// Convert S16 or float32 stream audio (mono or stereo) to interleaved stereo
// S16 at OUT_RATE, scaled by gain.
static std::vector<int16_t> ToStereoS16At48k(const void* pcm, size_t bytes,
                                              int srcRate, int srcCh,
                                              bool isFloat, float gain)
{
    const size_t bps   = isFloat ? 4 : 2;
    const size_t total = bytes / (bps * (size_t)srcCh);   // frames

    std::vector<int16_t> st(total * 2);
    for (size_t i = 0; i < total; i++) {
        float l, r;
        if (isFloat) {
            const float* src = static_cast<const float*>(pcm);
            l = src[i * srcCh];
            r = src[i * srcCh + (srcCh > 1 ? 1 : 0)];
        } else {
            const int16_t* src = static_cast<const int16_t*>(pcm);
            l = (float)src[i * srcCh];
            r = (float)src[i * srcCh + (srcCh > 1 ? 1 : 0)];
            l /= 32768.f; r /= 32768.f;
        }
        st[i * 2]     = ClampS16((int32_t)(l * gain * 32767.f));
        st[i * 2 + 1] = ClampS16((int32_t)(r * gain * 32767.f));
    }

    if (srcRate == PipeWireAudioOutput::OUT_RATE)
        return st;

    const size_t outN = (size_t)((double)total * PipeWireAudioOutput::OUT_RATE / srcRate + 0.5);
    std::vector<int16_t> out(outN * 2);
    for (size_t i = 0; i < outN; i++) {
        double  pos  = (double)i * srcRate / PipeWireAudioOutput::OUT_RATE;
        size_t  idx  = (size_t)pos;
        double  frac = pos - (double)idx;
        for (int c = 0; c < 2; c++) {
            int32_t s0 = (idx     < total) ? st[idx * 2 + c]       : 0;
            int32_t s1 = (idx + 1 < total) ? st[(idx + 1) * 2 + c] : 0;
            out[i * 2 + c] = (int16_t)(s0 + (int32_t)((s1 - s0) * frac));
        }
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

    const bool bed = self->mBedEnabled.load(std::memory_order_acquire) && N >= 2;

    for (int s = bed ? 2 : 0; s < N; s++)
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

    if (bed)
    {
        // Music ring → channels 0 (left) / 1 (right). Frame counters are
        // free-running; indices wrap at BED_RING_FRAMES.
        const uint32_t wr = self->mBedWrite.load(std::memory_order_acquire);
        uint32_t       rd = self->mBedRead.load(std::memory_order_relaxed);
        uint32_t avail = wr - rd;
        if (avail > (uint32_t)BED_RING_FRAMES)   // writer lapped the reader
        {
            rd    = wr - (uint32_t)BED_RING_FRAMES;
            avail = (uint32_t)BED_RING_FRAMES;
        }
        const uint32_t n = std::min(frames, avail);
        const int16_t* ring = self->mBedRing.data();

        for (uint32_t f = 0; f < frames; f++)
        {
            int16_t l = 0, r = 0;
            if (f < n)
            {
                const uint32_t idx = (rd + f) % (uint32_t)BED_RING_FRAMES;
                l = ring[idx * 2];
                r = ring[idx * 2 + 1];
            }
            dst[f * N + 0] = l;
            dst[f * N + 1] = r;
        }
        self->mBedRead.store(rd + n, std::memory_order_release);

        // 2D/UI one-shot voices, mixed centred into both bed channels.
        for (int v = 0; v < BED_VOICES; v++)
        {
            Slot& voice = self->mBedVoices[v];
            std::unique_lock<std::mutex> lock(voice.pcmLock, std::try_to_lock);
            if (!lock || !voice.active.load(std::memory_order_acquire))
                continue;

            const uint32_t pos   = voice.readPos.load(std::memory_order_relaxed);
            const uint32_t avl   = (pos < (uint32_t)voice.pcm.size())
                                   ? (uint32_t)voice.pcm.size() - pos : 0u;
            const uint32_t take  = std::min(frames, avl);
            for (uint32_t f = 0; f < take; f++)
            {
                const int32_t s = voice.pcm[pos + f];
                dst[f * N + 0] = ClampS16((int32_t)dst[f * N + 0] + s);
                dst[f * N + 1] = ClampS16((int32_t)dst[f * N + 1] + s);
            }
            voice.readPos.store(pos + take, std::memory_order_relaxed);
            if (take == avl)
                voice.active.store(false, std::memory_order_release);
        }
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

    mLoop = pw_thread_loop_new("domedoom-spat", nullptr);
    if (!mLoop) {
        fprintf(stderr, "[domedoom/pw-audio] pw_thread_loop_new failed\n");
        return false;
    }

    static const pw_stream_events kEvents = {
        .version       = PW_VERSION_STREAM_EVENTS,
        .state_changed = OnStateChanged,
        .process       = OnProcess,
    };

    mStream = pw_stream_new_simple(
        pw_thread_loop_get_loop(mLoop),
        "domedoom-spat",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,     "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE,     "Game",
            PW_KEY_NODE_NAME,      "domedoom-spat",
            PW_KEY_NODE_DESCRIPTION, "DomeDoom [spat]",
            nullptr),
        &kEvents, this);

    if (!mStream) {
        fprintf(stderr, "[domedoom/pw-audio] pw_stream_new_simple failed\n");
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
        fprintf(stderr, "[domedoom/pw-audio] pw_stream_connect: %d\n", ret);
        pw_stream_destroy(mStream);  mStream = nullptr;
        pw_thread_loop_destroy(mLoop); mLoop = nullptr;
        return false;
    }

    mNumSlots = numSlots;
    mBedRing.assign((size_t)BED_RING_FRAMES * 2, 0);
    mBedWrite.store(0); mBedRead.store(0);
    pw_thread_loop_start(mLoop);
    mRunning = true;
    fprintf(stderr, "[domedoom/pw-audio] single %d-ch stream \"DomeDoom [spat]\" at %d Hz\n",
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

void PipeWireAudioOutput::EnableBed(bool on)
{
    if (on == mBedEnabled.load(std::memory_order_relaxed)) return;
    if (on)
    {
        // Reset the ring before the PW thread starts reading it.
        mBedRead.store(mBedWrite.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        mBedEnabled.store(true, std::memory_order_release);
    }
    else
    {
        mBedEnabled.store(false, std::memory_order_release);
        for (int v = 0; v < BED_VOICES; v++)
        {
            Slot& voice = mBedVoices[v];
            std::lock_guard<std::mutex> lock(voice.pcmLock);
            voice.active.store(false, std::memory_order_relaxed);
            voice.pcm.clear();
            voice.readPos.store(0, std::memory_order_relaxed);
        }
    }
}

void PipeWireAudioOutput::PushBedStream(const void* pcm, size_t bytes, int srcRate,
                                        int srcChannels, bool isFloat, float gain)
{
    if (!mRunning || !mBedEnabled.load(std::memory_order_acquire)) return;
    if (!pcm || bytes == 0 || srcRate <= 0 || srcChannels < 1) return;

    auto st = ToStereoS16At48k(pcm, bytes, srcRate, srcChannels, isFloat, gain);

    std::lock_guard<std::mutex> lock(mBedWriteLock);
    const uint32_t rd = mBedRead.load(std::memory_order_acquire);
    uint32_t       wr = mBedWrite.load(std::memory_order_relaxed);
    const size_t frames = st.size() / 2;
    for (size_t i = 0; i < frames; i++)
    {
        if (wr - rd >= (uint32_t)BED_RING_FRAMES) break;   // ring full — drop rest
        const uint32_t idx = wr % (uint32_t)BED_RING_FRAMES;
        mBedRing[idx * 2]     = st[i * 2];
        mBedRing[idx * 2 + 1] = st[i * 2 + 1];
        ++wr;
    }
    mBedWrite.store(wr, std::memory_order_release);
}

void PipeWireAudioOutput::PlayBedSound(const void* pcm, size_t bytes, int srcRate,
                                       int bits, int srcChannels, float gain)
{
    if (!mRunning || !mBedEnabled.load(std::memory_order_acquire)) return;
    if (!pcm || bytes == 0 || (bits != 8 && bits != 16)) return;

    auto mono = ToMonoS16At48k(pcm, bytes, srcRate, bits, srcChannels);
    if (gain != 1.f)
        for (auto& s : mono) s = ClampS16((int32_t)(s * gain));

    int v = -1;
    for (int i = 0; i < BED_VOICES; i++)
        if (!mBedVoices[i].active.load(std::memory_order_relaxed)) { v = i; break; }
    if (v < 0)   // all busy — steal round-robin
    {
        v = mBedVoiceNext;
        mBedVoiceNext = (mBedVoiceNext + 1) % BED_VOICES;
    }

    Slot& voice = mBedVoices[v];
    std::lock_guard<std::mutex> lock(voice.pcmLock);
    voice.active.store(false, std::memory_order_relaxed);
    voice.pcm = std::move(mono);
    voice.readPos.store(0, std::memory_order_relaxed);
    voice.active.store(true, std::memory_order_release);
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
