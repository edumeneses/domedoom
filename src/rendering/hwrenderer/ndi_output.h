#pragma once
#include <cstdint>
#include <string>

// NDI video output for the CubeDoom fulldome pipeline.
// When HAVE_NDI is not defined, all methods are no-ops.

#ifdef HAVE_NDI

class NdiVideoOutput {
public:
    NdiVideoOutput() = default;
    ~NdiVideoOutput();

    // Init/reinit sender. label = NDI source name. Returns true on success.
    bool Init(const std::string& label, int width, int height);
    // Push one frame; row-flips GL bottom-up to NDI top-down.
    void PushFrame(const uint8_t* pixels, int srcStride);
    void Shutdown();
    bool IsRunning() const { return mRunning; }

private:
    struct Impl;
    Impl*  mImpl    = nullptr;
    bool   mRunning = false;
    int    mWidth   = 0;
    int    mHeight  = 0;
};

#else // !HAVE_NDI

class NdiVideoOutput {
public:
    bool Init(const std::string&, int, int) { return false; }
    void PushFrame(const uint8_t*, int)     {}
    void Shutdown()                         {}
    bool IsRunning() const                  { return false; }
};

#endif // HAVE_NDI
