#include "ndi_output.h"

#ifdef HAVE_NDI

#include <Processing.NDI.Lib.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
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

    // Search order: env-specified dir → plain name (LD_LIBRARY_PATH / ldconfig)
    // → explicit common install paths (needed when AppImage overrides LD_LIBRARY_PATH).
    const char* envDir = getenv("NDI_RUNTIME_DIR_V6");
    if (envDir) {
        std::string p = std::string(envDir) + "/" + NDILIB_LIBRARY_NAME;
        gNdiHandle = dlopen(p.c_str(), RTLD_LOCAL | RTLD_LAZY);
    }

    static const char* kFallbacks[] = {
        NDILIB_LIBRARY_NAME,           // plain name — works if ldconfig knows it
        "/usr/local/lib/" NDILIB_LIBRARY_NAME,
        "/usr/lib/" NDILIB_LIBRARY_NAME,
        "/usr/lib/x86_64-linux-gnu/" NDILIB_LIBRARY_NAME,
        nullptr
    };
    for (const char** p = kFallbacks; *p && !gNdiHandle; ++p)
        gNdiHandle = dlopen(*p, RTLD_LOCAL | RTLD_LAZY);

    if (!gNdiHandle) {
        fprintf(stderr, "[cubedoom/ndi] dlopen %s failed: %s\n"
                        "  Install the NDI runtime from http://ndi.link/NDIRedistV6\n",
                NDILIB_LIBRARY_NAME, dlerror());
        gNdiHandle = (void*)-1;
        return false;
    }

    using pfnLoad = const NDIlib_v6* (*)();
    auto load_fn = (pfnLoad)dlsym(gNdiHandle, "NDIlib_v6_load");
    if (!load_fn) {
        fprintf(stderr, "[cubedoom/ndi] NDIlib_v6_load not found in %s\n",
                NDILIB_LIBRARY_NAME);
        dlclose(gNdiHandle);
        gNdiHandle = (void*)-1;
        return false;
    }

    gNdiLib = load_fn();
    if (!gNdiLib) {
        fprintf(stderr, "[cubedoom/ndi] NDIlib_v6_load() returned null\n");
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
            fprintf(stderr, "[cubedoom/ndi] NDIlib initialize failed "
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
    std::vector<uint8_t>   flipped;
};

bool NdiVideoOutput::Init(const std::string& label, int width, int height)
{
    Shutdown();

    if (!NdiLibInit())
        return false;

    auto* impl = new Impl();
    impl->flipped.resize((size_t)width * height * 4);

    NDIlib_send_create_t desc;
    desc.p_ndi_name  = label.c_str();
    desc.p_groups    = nullptr;
    desc.clock_video = true;
    desc.clock_audio = false;

    impl->sender = gNdiLib->send_create(&desc);
    if (!impl->sender)
    {
        fprintf(stderr, "[cubedoom/ndi] send_create failed (label=%s)\n",
                label.c_str());
        delete impl;
        NdiLibShutdown();
        return false;
    }

    mImpl    = impl;
    mWidth   = width;
    mHeight  = height;
    mRunning = true;
    fprintf(stderr, "[cubedoom/ndi] sender ready: %s (%dx%d)\n",
            label.c_str(), width, height);
    return true;
}

void NdiVideoOutput::PushFrame(const uint8_t* pixels, int srcStride)
{
    if (!mRunning || !mImpl) return;

    const int rowBytes = mWidth * 4;
    // Row-flip: GL bottom-up -> NDI top-down.
    for (int y = 0; y < mHeight; ++y)
    {
        const uint8_t* src = pixels + (size_t)(mHeight - 1 - y) * srcStride;
        memcpy(mImpl->flipped.data() + (size_t)y * rowBytes, src, rowBytes);
    }

    NDIlib_video_frame_v2_t frame;
    frame.xres                 = mWidth;
    frame.yres                 = mHeight;
    // RGBX, not RGBA: the cubemap FBO leaves alpha at 0, and NDI honors the
    // alpha channel for RGBA frames — receivers would show black. RGBX tells
    // NDI to ignore alpha and treat every pixel as opaque.
    frame.FourCC               = NDIlib_FourCC_type_RGBX;
    frame.frame_rate_N         = 60000;
    frame.frame_rate_D         = 1000;
    frame.picture_aspect_ratio = 0.0f;
    frame.frame_format_type    = NDIlib_frame_format_type_progressive;
    frame.timecode             = NDIlib_send_timecode_synthesize;
    frame.p_data               = mImpl->flipped.data();
    frame.line_stride_in_bytes = rowBytes;
    frame.p_metadata           = nullptr;
    frame.timestamp            = 0;

    gNdiLib->send_send_video_v2(mImpl->sender, &frame);
}

void NdiVideoOutput::Shutdown()
{
    mRunning = false;
    if (mImpl)
    {
        if (mImpl->sender)
        {
            gNdiLib->send_destroy(mImpl->sender);
            NdiLibShutdown();
        }
        delete mImpl;
        mImpl = nullptr;
    }
}

NdiVideoOutput::~NdiVideoOutput() { Shutdown(); }

#endif // HAVE_NDI
