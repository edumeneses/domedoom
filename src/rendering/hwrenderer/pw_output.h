#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>

#ifdef HAVE_PIPEWIRE

#include <pipewire/pipewire.h>

// PipeWire video output stream for CubeDoom fulldome pipeline.
//
// Two modes, selected at Init time:
//
//   CPU  — double-PBO readback → SPA_DATA_MemPtr stream.
//           Call PushFrame() each game frame from the render thread.
//
//   DMA-BUF — EGL texture export → SPA_DATA_DmaBuf stream.
//              No CPU copy. Linux DRM implicit sync handles producer/consumer
//              ordering; just call QueueDmaBufFrame() after GL commands.
//
// PipeWire drives the output (PW_STREAM_FLAG_DRIVER): its thread calls the
// process callback whenever it wants a frame. We respond by dequeuing the
// pre-allocated buffer, updating chunk metadata, and requeueing.

class PipeWireOutput
{
public:
    PipeWireOutput() = default;
    ~PipeWireOutput();

    // CPU mode: PW allocates MemPtr buffers; PushFrame() supplies pixel data.
    bool InitCPU(int width, int height);

    // DMA-BUF mode: we allocate the single DMA-BUF buffer.
    // dmaFd   — exported DRM buffer fd (must remain valid for lifetime of stream)
    // stride  — bytes per row
    bool InitDmaBuf(int dmaFd, int width, int height, int stride);

    // CPU mode only. Copy pixels (bottom-up GL order) into the internal
    // double-buffer with row-flip. Thread-safe; call from game/render thread.
    // srcStride — bytes per row in pixels (normally width*4).
    void PushFrame(const uint8_t* pixels, int srcStride);

    // DMA-BUF mode only. Call after all GL commands that write the cross
    // texture have been submitted (glFlush or glFinish is NOT required — Linux
    // DRM implicit sync handles ordering). Marks the current buffer ready so
    // the next PW process callback will queue it.
    void QueueDmaBufFrame();

    bool IsDmaBufMode() const { return mDmaBufMode; }
    bool IsRunning()    const { return mRunning.load(std::memory_order_relaxed); }

private:
    bool Connect(bool dmaBufMode, int nBufs);
    void Shutdown();

    static void OnStateChanged(void* data, enum pw_stream_state old,
                               enum pw_stream_state state, const char* error);
    static void OnAddBuffer(void* data, struct pw_buffer* buf);
    static void OnProcess(void* data);

    pw_thread_loop* mLoop   = nullptr;
    pw_stream*      mStream = nullptr;

    int  mWidth      = 0;
    int  mHeight     = 0;
    int  mStride     = 0;  // bytes per row (output)
    int  mDmaFd      = -1;
    bool mDmaBufMode = false;

    // CPU mode: double-buffered pixel data protected by mutex.
    std::mutex           mMutex;
    std::vector<uint8_t> mFrameBuf;
    bool                 mFrameReady = false;

    // DMA-BUF mode: set by QueueDmaBufFrame(), cleared by process callback.
    std::atomic<bool>    mDmaBufReady{false};

    std::atomic<bool>    mRunning{false};
};

#else // !HAVE_PIPEWIRE

// No-op stub so CubeDoom builds without libpipewire-0.3-dev. The fulldome
// PipeWire transport is simply unavailable; Sh4lt output is unaffected.
class PipeWireOutput
{
public:
    bool InitCPU(int, int)              { return false; }
    bool InitDmaBuf(int, int, int, int) { return false; }
    void PushFrame(const uint8_t*, int) {}
    void QueueDmaBufFrame()             {}
    bool IsDmaBufMode() const           { return false; }
    bool IsRunning()    const           { return false; }
};

#endif // HAVE_PIPEWIRE
