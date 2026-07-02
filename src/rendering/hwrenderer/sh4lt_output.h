#pragma once
#include <cstdint>
#include <string>

// Sh4lt video and audio output for DomeDoom fulldome pipeline.
// When HAVE_SH4LT is not defined, all methods are no-ops.

#ifdef HAVE_SH4LT

class Sh4ltVideoOutput {
public:
    Sh4ltVideoOutput() = default;
    ~Sh4ltVideoOutput();

    // Init/reinit writer. Returns true on success.
    bool Init(const std::string& label, int width, int height);
    // Push one frame; row-flips GL bottom-up to top-down.
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

class Sh4ltAudioOutput {
public:
    Sh4ltAudioOutput() = default;
    ~Sh4ltAudioOutput();

    bool Init(const std::string& label, int samplerate, int channels, bool isFloat);
    void PushSamples(const void* data, size_t bytes);
    void Shutdown();
    bool IsRunning() const { return mRunning; }

private:
    struct Impl;
    Impl*  mImpl    = nullptr;
    bool   mRunning = false;
};

// Install/remove the audio tap used by oalsound.cpp.
// output pointer is not owned; caller keeps it alive while tap is active.
void Sh4ltInstallAudioTap(Sh4ltAudioOutput* output);
void Sh4ltRemoveAudioTap();

#else // !HAVE_SH4LT

class Sh4ltVideoOutput {
public:
    bool Init(const std::string&, int, int) { return false; }
    void PushFrame(const uint8_t*, int)     {}
    void Shutdown()                         {}
    bool IsRunning() const                  { return false; }
};

class Sh4ltAudioOutput {
public:
    bool Init(const std::string&, int, int, bool) { return false; }
    void PushSamples(const void*, size_t)          {}
    void Shutdown()                                {}
    bool IsRunning() const                         { return false; }
};

inline void Sh4ltInstallAudioTap(Sh4ltAudioOutput*) {}
inline void Sh4ltRemoveAudioTap()                   {}

#endif // HAVE_SH4LT
