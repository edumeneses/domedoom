//
// pw_output.cpp  —  PipeWire video output for CubeDoom
//
// Implements two delivery modes:
//
//   CPU  (SPA_DATA_MemPtr):
//     Game thread calls PushFrame() which row-flips the GL bottom-up image
//     into an internal double-buffer.  The PW process callback memcpy's that
//     buffer into the PW-allocated MemPtr buffer and queues it.
//
//   DMA-BUF (SPA_DATA_DmaBuf):
//     A single DRM buffer fd (exported via eglExportDMABUFImageMESA) is handed
//     to PW at stream connect time.  Each process callback just updates chunk
//     metadata and requeues; no pixel copy happens.  Linux DRM implicit sync
//     ensures the GPU write completes before the consumer reads.
//

#include "pw_output.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <cstdio>
#include <cstring>

// ---- one-time PW init -------------------------------------------------------

static bool sPwInitDone = false;
static void EnsurePwInit()
{
    if (!sPwInitDone)
    {
        pw_init(nullptr, nullptr);
        sPwInitDone = true;
    }
}

// ---- PW stream callbacks ----------------------------------------------------

void PipeWireOutput::OnStateChanged(void* data,
                                     pw_stream_state /*old*/,
                                     pw_stream_state  state,
                                     const char*      error)
{
    auto* self = static_cast<PipeWireOutput*>(data);
    switch (state)
    {
    case PW_STREAM_STATE_STREAMING:
        self->mRunning.store(true, std::memory_order_relaxed);
        break;
    case PW_STREAM_STATE_ERROR:
        fprintf(stderr, "[cubedoom/pw] stream error: %s\n", error ? error : "?");
        self->mRunning.store(false, std::memory_order_relaxed);
        break;
    default:
        self->mRunning.store(false, std::memory_order_relaxed);
        break;
    }
}

void PipeWireOutput::OnAddBuffer(void* data, pw_buffer* buf)
{
    auto* self = static_cast<PipeWireOutput*>(data);
    if (!self->mDmaBufMode || self->mDmaFd < 0) return;

    // We manage this buffer's memory: point it at our exported DRM fd.
    spa_data& d  = buf->buffer->datas[0];
    d.type       = SPA_DATA_DmaBuf;
    d.flags      = SPA_DATA_FLAG_READABLE;
    d.fd         = (int64_t)self->mDmaFd;
    d.data       = nullptr;
    d.mapoffset  = 0;
    d.maxsize    = (uint32_t)((size_t)self->mStride * self->mHeight);
}

void PipeWireOutput::OnProcess(void* data)
{
    auto* self = static_cast<PipeWireOutput*>(data);

    pw_buffer* pwbuf = pw_stream_dequeue_buffer(self->mStream);
    if (!pwbuf) return;

    spa_buffer* spabuf = pwbuf->buffer;
    spa_data&   d      = spabuf->datas[0];

    if (self->mDmaBufMode)
    {
        // DMA-BUF: only queue when the game has issued new GL commands.
        if (!self->mDmaBufReady.exchange(false, std::memory_order_acquire))
        {
            // No new frame yet — return the buffer without queueing so PW
            // tries again on the next driver tick.
            // (pw_stream_queue_buffer must still be called to avoid stall)
            d.chunk->offset = 0;
            d.chunk->size   = 0;  // empty chunk signals "no new data"
            d.chunk->stride = 0;
            pw_stream_queue_buffer(self->mStream, pwbuf);
            return;
        }

        d.chunk->offset = 0;
        d.chunk->size   = (uint32_t)((size_t)self->mStride * self->mHeight);
        d.chunk->stride = (int32_t)self->mStride;
    }
    else
    {
        // CPU mode: copy from the game-thread double-buffer.
        if (!d.data)
        {
            pw_stream_queue_buffer(self->mStream, pwbuf);
            return;
        }

        std::unique_lock<std::mutex> lock(self->mMutex);
        if (!self->mFrameReady)
        {
            lock.unlock();
            pw_stream_queue_buffer(self->mStream, pwbuf);
            return;
        }
        std::memcpy(d.data, self->mFrameBuf.data(),
                    (size_t)self->mStride * self->mHeight);
        self->mFrameReady = false;
        lock.unlock();

        d.chunk->offset = 0;
        d.chunk->size   = (uint32_t)((size_t)self->mStride * self->mHeight);
        d.chunk->stride = (int32_t)self->mStride;
    }

    pw_stream_queue_buffer(self->mStream, pwbuf);
}

// ---- public API -------------------------------------------------------------

bool PipeWireOutput::InitCPU(int width, int height)
{
    mWidth      = width;
    mHeight     = height;
    mStride     = width * 4;  // RGBA8
    mDmaBufMode = false;
    mFrameBuf.resize((size_t)mStride * mHeight);
    return Connect(false, 2);  // 2 MemPtr buffers for double-buffering
}

bool PipeWireOutput::InitDmaBuf(int dmaFd, int width, int height, int stride)
{
    mWidth      = width;
    mHeight     = height;
    mStride     = stride;
    mDmaFd      = dmaFd;
    mDmaBufMode = true;
    return Connect(true, 1);  // single DMA-BUF buffer
}

bool PipeWireOutput::Connect(bool dmaBufMode, int nBufs)
{
    EnsurePwInit();

    mLoop = pw_thread_loop_new("cubedoom-video", nullptr);
    if (!mLoop)
    {
        fprintf(stderr, "[cubedoom/pw] pw_thread_loop_new failed\n");
        return false;
    }

    static const pw_stream_events kEvents = {
        .version       = PW_VERSION_STREAM_EVENTS,
        .state_changed = OnStateChanged,
        .add_buffer    = OnAddBuffer,
        .process       = OnProcess,
    };

    mStream = pw_stream_new_simple(
        pw_thread_loop_get_loop(mLoop),
        "cubedoom-fulldome",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,     "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE,     "Camera",
            nullptr),
        &kEvents,
        this);

    if (!mStream)
    {
        fprintf(stderr, "[cubedoom/pw] pw_stream_new_simple failed\n");
        pw_thread_loop_destroy(mLoop);
        mLoop = nullptr;
        return false;
    }

    // Build format + buffer params in a stack pod builder.
    uint8_t pbuf[2048];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pbuf, sizeof(pbuf));

    spa_video_info_raw vi = {};
    vi.format          = SPA_VIDEO_FORMAT_RGBA;
    vi.size.width      = (uint32_t)mWidth;
    vi.size.height     = (uint32_t)mHeight;
    vi.framerate.num   = 60;
    vi.framerate.denom = 1;

    const spa_pod* params[2];
    params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &vi);

    const uint32_t dataTypeMask = dmaBufMode
        ? (1u << SPA_DATA_DmaBuf)
        : (1u << SPA_DATA_MemPtr);

    params[1] = (spa_pod*)spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers,  SPA_POD_CHOICE_RANGE_Int(nBufs, 1, nBufs),
        SPA_PARAM_BUFFERS_size,     SPA_POD_Int((int)(mStride * mHeight)),
        SPA_PARAM_BUFFERS_stride,   SPA_POD_Int(mStride),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((int)dataTypeMask));

    pw_stream_flags flags = PW_STREAM_FLAG_DRIVER;
    if (dmaBufMode)
        flags = (pw_stream_flags)(flags | PW_STREAM_FLAG_ALLOC_BUFFERS);
    else
        flags = (pw_stream_flags)(flags | PW_STREAM_FLAG_MAP_BUFFERS);

    int ret = pw_stream_connect(mStream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                flags, params, 2);
    if (ret < 0)
    {
        fprintf(stderr, "[cubedoom/pw] pw_stream_connect: %d\n", ret);
        pw_stream_destroy(mStream);
        mStream = nullptr;
        pw_thread_loop_destroy(mLoop);
        mLoop = nullptr;
        return false;
    }

    pw_thread_loop_start(mLoop);
    fprintf(stderr, "[cubedoom/pw] stream started (%s, %dx%d)\n",
            dmaBufMode ? "DMA-BUF" : "CPU", mWidth, mHeight);
    return true;
}

void PipeWireOutput::PushFrame(const uint8_t* pixels, int srcStride)
{
    // Row-flip: GL gives bottom-up, PW consumers expect top-down.
    const int rowBytes = mWidth * 4;
    std::lock_guard<std::mutex> lock(mMutex);
    for (int y = 0; y < mHeight; y++)
    {
        const uint8_t* src = pixels + (size_t)(mHeight - 1 - y) * srcStride;
        uint8_t*       dst = mFrameBuf.data() + (size_t)y * mStride;
        std::memcpy(dst, src, rowBytes);
    }
    mFrameReady = true;
}

void PipeWireOutput::QueueDmaBufFrame()
{
    // Signal the PW process callback that a new frame is available.
    // GL commands have been submitted; DRM implicit sync handles ordering.
    mDmaBufReady.store(true, std::memory_order_release);
}

void PipeWireOutput::Shutdown()
{
    if (mLoop) pw_thread_loop_stop(mLoop);
    if (mStream)
    {
        pw_stream_disconnect(mStream);
        pw_stream_destroy(mStream);
        mStream = nullptr;
    }
    if (mLoop)
    {
        pw_thread_loop_destroy(mLoop);
        mLoop = nullptr;
    }
    mRunning.store(false, std::memory_order_relaxed);
}

PipeWireOutput::~PipeWireOutput()
{
    Shutdown();
}
