#include "ndi_output.h"

#ifdef HAVE_NDI

#include <Processing.NDI.Lib.h>

#include <cstdio>
#include <cstring>
#include <vector>

// NDIlib_initialize()/NDIlib_destroy() are process-global and reference
// nothing per-sender. Track a simple use count so multiple senders (or
// re-inits) don't tear the library down underneath each other.
static int gNdiInitCount = 0;

static bool NdiLibInit()
{
    if (gNdiInitCount == 0)
    {
        if (!NDIlib_initialize())
        {
            fprintf(stderr, "[cubedoom/ndi] NDIlib_initialize failed "
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
        NDIlib_destroy();
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
    desc.p_ndi_name = label.c_str();
    desc.p_groups   = nullptr;
    desc.clock_video = true;   // rate-limit sends to the submit rate
    desc.clock_audio = false;

    impl->sender = NDIlib_send_create(&desc);
    if (!impl->sender)
    {
        fprintf(stderr, "[cubedoom/ndi] NDIlib_send_create failed (label=%s)\n",
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
    // alpha channel for RGBA frames — receivers (e.g. ossia score) would
    // composite the whole image as fully transparent and show black. RGBX
    // tells NDI to ignore alpha and treat every pixel as opaque.
    frame.FourCC               = NDIlib_FourCC_type_RGBX;
    frame.frame_rate_N         = 60000;
    frame.frame_rate_D         = 1000;
    frame.picture_aspect_ratio = 0.0f;            // square pixels
    frame.frame_format_type    = NDIlib_frame_format_type_progressive;
    frame.timecode             = NDIlib_send_timecode_synthesize;
    frame.p_data               = mImpl->flipped.data();
    frame.line_stride_in_bytes = rowBytes;
    frame.p_metadata           = nullptr;
    frame.timestamp            = 0;

    NDIlib_send_send_video_v2(mImpl->sender, &frame);
}

void NdiVideoOutput::Shutdown()
{
    mRunning = false;
    if (mImpl)
    {
        if (mImpl->sender)
        {
            NDIlib_send_destroy(mImpl->sender);
            NdiLibShutdown();
        }
        delete mImpl;
        mImpl = nullptr;
    }
}

NdiVideoOutput::~NdiVideoOutput() { Shutdown(); }

#endif // HAVE_NDI
