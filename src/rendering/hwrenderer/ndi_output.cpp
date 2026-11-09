#include "ndi_output.h"

#ifdef HAVE_NDI

#include <Processing.NDI.Lib.h>

#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

// ── Runtime-loaded NDI vtable ─────────────────────────────────────────────
// dlopen libndi.so.6 on first Init() so the AppImage works on any machine
// with the NDI runtime installed, without bundling or link-time dependency.
static void*            gNdiHandle = nullptr;  // (void*)-1 = failed
static const NDIlib_v6* gNdiLib    = nullptr;

static bool NdiRuntimeLoad()
{
    if (gNdiLib)                    return true;
    if (gNdiHandle == (void*)-1)    return false;

    // Helper: prepend a directory to LD_LIBRARY_PATH so that NDI's own internal
    // dlopen() calls (for its deps) find libraries in the same dir.  AppImages
    // override LD_LIBRARY_PATH; without this, loading /usr/local/lib/libndi.so.6
    // by absolute path succeeds but NDI's dep loads fail.
    auto prependLibPath = [](const char* dir) {
        const char* cur = getenv("LD_LIBRARY_PATH");
        std::string nv = dir;
        if (cur && cur[0]) { nv += ':'; nv += cur; }
        setenv("LD_LIBRARY_PATH", nv.c_str(), 1);
    };

    // Try NDI_RUNTIME_DIR_V6 env first (user-specified install location).
    const char* envDir = getenv("NDI_RUNTIME_DIR_V6");
    if (envDir) {
        prependLibPath(envDir);
        std::string p = std::string(envDir) + "/" + NDILIB_LIBRARY_NAME;
        gNdiHandle = dlopen(p.c_str(), RTLD_LOCAL | RTLD_LAZY);
        if (gNdiHandle)
            fprintf(stderr, "[domedoom/ndi] loaded from NDI_RUNTIME_DIR_V6: %s\n", p.c_str());
        else
            fprintf(stderr, "[domedoom/ndi] NDI_RUNTIME_DIR_V6 set but dlopen failed: %s\n", dlerror());
    }

    // Fallback: common system install paths.  Prepend each dir to LD_LIBRARY_PATH
    // before the dlopen so NDI's internal deps resolve from the same location.
    static const char* kFallbackDirs[] = {
        "/usr/local/lib",
        "/usr/lib",
        "/usr/lib/x86_64-linux-gnu",
        nullptr
    };
    for (const char** dir = kFallbackDirs; *dir && !gNdiHandle; ++dir) {
        prependLibPath(*dir);
        std::string p = std::string(*dir) + "/" + NDILIB_LIBRARY_NAME;
        gNdiHandle = dlopen(p.c_str(), RTLD_LOCAL | RTLD_LAZY);
        if (gNdiHandle)
            fprintf(stderr, "[domedoom/ndi] loaded from %s\n", p.c_str());
        else
            fprintf(stderr, "[domedoom/ndi] not at %s: %s\n", p.c_str(), dlerror());
    }

    // Last resort: plain name via ldconfig / remaining LD_LIBRARY_PATH.
    if (!gNdiHandle) {
        gNdiHandle = dlopen(NDILIB_LIBRARY_NAME, RTLD_LOCAL | RTLD_LAZY);
        if (gNdiHandle)
            fprintf(stderr, "[domedoom/ndi] loaded via ldconfig: %s\n", NDILIB_LIBRARY_NAME);
    }

    if (!gNdiHandle) {
        fprintf(stderr, "[domedoom/ndi] could not load %s — "
                        "install NDI runtime from http://ndi.link/NDIRedistV6\n",
                NDILIB_LIBRARY_NAME);
        gNdiHandle = (void*)-1;
        return false;
    }

    using pfnLoad = const NDIlib_v6* (*)();
    auto load_fn = (pfnLoad)dlsym(gNdiHandle, "NDIlib_v6_load");
    if (!load_fn) {
        fprintf(stderr, "[domedoom/ndi] NDIlib_v6_load not found in %s\n",
                NDILIB_LIBRARY_NAME);
        dlclose(gNdiHandle);
        gNdiHandle = (void*)-1;
        return false;
    }

    gNdiLib = load_fn();
    if (!gNdiLib) {
        fprintf(stderr, "[domedoom/ndi] NDIlib_v6_load() returned null\n");
        dlclose(gNdiHandle);
        gNdiHandle = (void*)-1;
        return false;
    }

    return true;
}

// ── Init-count guard (process-global) ─────────────────────────────────────
static int gNdiInitCount = 0;

static bool NdiLibInit()
{
    if (gNdiInitCount == 0)
    {
        if (!NdiRuntimeLoad())
            return false;
        if (!gNdiLib->initialize())
        {
            fprintf(stderr, "[domedoom/ndi] NDIlib initialize failed "
                            "(unsupported CPU?)\n");
            return false;
        }
    }
    ++gNdiInitCount;
    return true;
}

static void NdiLibShutdown()
{
    if (gNdiInitCount > 0 && --gNdiInitCount == 0)
        gNdiLib->destroy();
}

struct NdiVideoOutput::Impl {
    NDIlib_send_instance_t sender = nullptr;
    // Two flip buffers, alternated: send_send_video_async_v2 returns before NDI
    // is done reading the frame, and only guarantees it is finished with it once
    // the NEXT async send returns. Writing into the buffer still in flight would
    // tear the frame being transmitted.
    std::vector<uint8_t>   flipped[2];
    int                    flipIndex = 0;

    // Frame rate announced to receivers, measured from the PushFrame cadence.
    // The sender is not clocked (see Init), so this is the only timing hint a
    // receiver gets; declaring a fixed 60 while the 4096 dome renders at
    // ~20-30 fps would misstate the stream.
    std::chrono::steady_clock::time_point lastPush{};
    bool                   havePush    = false;
    double                 avgInterval = 0.0;   // EMA of seconds between pushes
    int                    declaredFps = 0;     // 0 = not measured yet

    // Returns the frame rate to declare for this push, updating the cadence
    // estimate. Re-announces only when the measured rate drifts more than 10%
    // so receivers don't see a format change on every frame-time wobble.
    int UpdateFrameRate()
    {
        const auto now = std::chrono::steady_clock::now();
        if (havePush)
        {
            const double dt = std::chrono::duration<double>(now - lastPush).count();
            // Skip hitches (level loads, console, window drags) so one long
            // frame doesn't drag the average down.
            if (dt > 0.0 && dt < 0.5)
                avgInterval = avgInterval > 0.0 ? avgInterval * 0.9 + dt * 0.1 : dt;
        }
        lastPush = now;
        havePush = true;

        if (avgInterval > 0.0)
        {
            const double fps = 1.0 / avgInterval;
            if (declaredFps == 0 || std::fabs(fps - declaredFps) > declaredFps * 0.1)
            {
                declaredFps = std::clamp((int)std::lround(fps), 1, 240);
                fprintf(stderr, "[domedoom/ndi] declared frame rate: %d fps\n",
                        declaredFps);
            }
        }
        return declaredFps > 0 ? declaredFps : 30;
    }
};

bool NdiVideoOutput::Init(const std::string& label, int width, int height)
{
    Shutdown();

    if (!NdiLibInit())
        return false;

    auto* impl = new Impl();
    impl->flipped[0].resize((size_t)width * height * 4);
    impl->flipped[1].resize((size_t)width * height * 4);

    NDIlib_send_create_t desc;
    desc.p_ndi_name  = label.c_str();
    desc.p_groups    = nullptr;
    // Unclocked: the game loop already paces frames. With clock_video=true NDI
    // rate-limits sends against the declared frame rate, adding wait time on
    // top of the render/readback latency the dome already carries.
    desc.clock_video = false;
    desc.clock_audio = false;

    impl->sender = gNdiLib->send_create(&desc);
    if (!impl->sender)
    {
        fprintf(stderr, "[domedoom/ndi] send_create failed (label=%s)\n",
                label.c_str());
        delete impl;
        NdiLibShutdown();
        return false;
    }

    mImpl    = impl;
    mWidth   = width;
    mHeight  = height;
    mRunning = true;
    fprintf(stderr, "[domedoom/ndi] sender ready: %s (%dx%d)\n",
            label.c_str(), width, height);
    return true;
}

void NdiVideoOutput::PushFrame(const uint8_t* pixels, int srcStride)
{
    if (!mRunning || !mImpl) return;

    const int rowBytes = mWidth * 4;
    uint8_t*  dstBuf   = mImpl->flipped[mImpl->flipIndex].data();
    // Row-flip: GL bottom-up -> NDI top-down.
    for (int y = 0; y < mHeight; ++y)
    {
        const uint8_t* src = pixels + (size_t)(mHeight - 1 - y) * srcStride;
        memcpy(dstBuf + (size_t)y * rowBytes, src, rowBytes);
    }

    NDIlib_video_frame_v2_t frame;
    frame.xres                 = mWidth;
    frame.yres                 = mHeight;
    // RGBX, not RGBA: the cubemap FBO leaves alpha at 0, and NDI honors the
    // alpha channel for RGBA frames — receivers would show black. RGBX tells
    // NDI to ignore alpha and treat every pixel as opaque.
    frame.FourCC               = NDIlib_FourCC_type_RGBX;
    frame.frame_rate_N         = mImpl->UpdateFrameRate() * 1000;
    frame.frame_rate_D         = 1000;
    frame.picture_aspect_ratio = 0.0f;
    frame.frame_format_type    = NDIlib_frame_format_type_progressive;
    frame.timecode             = NDIlib_send_timecode_synthesize;
    frame.p_data               = dstBuf;
    frame.line_stride_in_bytes = rowBytes;
    frame.p_metadata           = nullptr;
    frame.timestamp            = 0;

    // Async: the blocking send copies/transmits a 64 MB domemaster frame on the
    // render thread (~32 ms at 4096x4096, the single largest cost in the dome
    // pipeline). The async variant hands the buffer over and returns; NDI is
    // done with it by the time the next async send returns, hence the two
    // alternating buffers above.
    gNdiLib->send_send_video_async_v2(mImpl->sender, &frame);
    mImpl->flipIndex ^= 1;
}

void NdiVideoOutput::Shutdown()
{
    mRunning = false;
    if (mImpl)
    {
        if (mImpl->sender)
        {
            // Flush the in-flight async frame before the flip buffers die.
            gNdiLib->send_send_video_async_v2(mImpl->sender, nullptr);
            gNdiLib->send_destroy(mImpl->sender);
            NdiLibShutdown();
        }
        delete mImpl;
        mImpl = nullptr;
    }
}

NdiVideoOutput::~NdiVideoOutput() { Shutdown(); }

#endif // HAVE_NDI
