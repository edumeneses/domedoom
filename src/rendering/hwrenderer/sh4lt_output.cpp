#include "sh4lt_output.h"
#include "cubedoom_audiotap.h"

#ifdef HAVE_SH4LT

#include <sh4lt/writer.hpp>
#include <sh4lt/shtype/shtype.hpp>
#include <sh4lt/logger/console.hpp>

#include "c_cvars.h"
EXTERN_CVAR(String, r_cubemap_sh4lt_audio_label)

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

// ---- Video -----------------------------------------------------------------

struct Sh4ltVideoOutput::Impl {
    sh4lt::Writer writer;
    std::vector<uint8_t> flipped;

    Impl(const std::string& label, int w, int h)
        : writer(
            sh4lt::ShType(
                "video/x-raw,format=RGBA,width=" + std::to_string(w) +
                ",height=" + std::to_string(h) + ",framerate=60/1",
                label),
            (size_t)w * h * 4,
            std::make_shared<sh4lt::logger::Console>())
        , flipped((size_t)w * h * 4)
    {}
};

bool Sh4ltVideoOutput::Init(const std::string& label, int width, int height) {
    Shutdown();
    auto* impl = new Impl(label, width, height);
    if (!impl->writer) {
        fprintf(stderr, "[cubedoom/sh4lt] video init failed (label=%s)\n", label.c_str());
        delete impl;
        return false;
    }
    mImpl   = impl;
    mWidth  = width;
    mHeight = height;
    mRunning = true;
    fprintf(stderr, "[cubedoom/sh4lt] video ready: %s (%dx%d)\n",
            label.c_str(), width, height);
    return true;
}

void Sh4ltVideoOutput::PushFrame(const uint8_t* pixels, int srcStride) {
    if (!mRunning || !mImpl) return;
    const int rowBytes = mWidth * 4;
    // Row-flip: GL bottom-up → top-down
    for (int y = 0; y < mHeight; ++y) {
        const uint8_t* src = pixels + (size_t)(mHeight - 1 - y) * srcStride;
        memcpy(mImpl->flipped.data() + (size_t)y * rowBytes, src, rowBytes);
    }
    mImpl->writer.copy_to_shm(mImpl->flipped.data(),
                               mImpl->flipped.size(), -1, -1);
}

void Sh4ltVideoOutput::Shutdown() {
    mRunning = false;
    delete mImpl;
    mImpl = nullptr;
}

Sh4ltVideoOutput::~Sh4ltVideoOutput() { Shutdown(); }

// ---- Audio -----------------------------------------------------------------

struct Sh4ltAudioOutput::Impl {
    sh4lt::Writer writer;
    Impl(const std::string& label, int rate, int ch, bool flt)
        : writer(
            sh4lt::ShType(
                std::string("audio/x-raw,format=") + (flt ? "F32LE" : "S16LE") +
                ",channels=" + std::to_string(ch) +
                ",rate=" + std::to_string(rate),
                label),
            (size_t)rate * ch * (flt ? 4 : 2),   // 1 s ring buffer
            std::make_shared<sh4lt::logger::Console>())
    {}
};

bool Sh4ltAudioOutput::Init(const std::string& label, int samplerate, int channels, bool isFloat) {
    Shutdown();
    auto* impl = new Impl(label, samplerate, channels, isFloat);
    if (!impl->writer) {
        fprintf(stderr, "[cubedoom/sh4lt] audio init failed (label=%s)\n", label.c_str());
        delete impl;
        return false;
    }
    mImpl    = impl;
    mRunning = true;
    fprintf(stderr, "[cubedoom/sh4lt] audio ready: %s (%dHz, %dch, %s)\n",
            label.c_str(), samplerate, channels, isFloat ? "F32" : "S16");
    return true;
}

void Sh4ltAudioOutput::PushSamples(const void* data, size_t bytes) {
    if (!mRunning || !mImpl) return;
    mImpl->writer.copy_to_shm(data, bytes, -1, -1);
}

void Sh4ltAudioOutput::Shutdown() {
    mRunning = false;
    delete mImpl;
    mImpl = nullptr;
}

Sh4ltAudioOutput::~Sh4ltAudioOutput() { Shutdown(); }

// ---- Audio tap -------------------------------------------------------------

static std::atomic<Sh4ltAudioOutput*> gAudioTapOutput{nullptr};
static std::mutex gAudioInitMtx;

static void AudioTapCallback(const void* data, size_t bytes,
                              int rate, int channels, bool isFloat)
{
    auto* out = gAudioTapOutput.load(std::memory_order_relaxed);
    if (!out) return;
    if (!out->IsRunning()) {
        std::lock_guard<std::mutex> lk(gAudioInitMtx);
        if (!out->IsRunning()) {
            // Label is read from CVAR in hw_cubemaprenderer.cpp before tap install.
            // Once init succeeds the writer is alive for the session.
            out->Init(*r_cubemap_sh4lt_audio_label, rate, channels, isFloat);
        }
    }
    out->PushSamples(data, bytes);
}

void Sh4ltInstallAudioTap(Sh4ltAudioOutput* output) {
    gAudioTapOutput.store(output, std::memory_order_relaxed);
    g_oalAudioTap = AudioTapCallback;
}

void Sh4ltRemoveAudioTap() {
    g_oalAudioTap = nullptr;
    gAudioTapOutput.store(nullptr, std::memory_order_relaxed);
}

#endif // HAVE_SH4LT

// g_oalAudioTap lives here regardless of HAVE_SH4LT so oalsound.cpp
// can always reference it without #ifdefs.
OALAudioTapFn g_oalAudioTap = nullptr;
