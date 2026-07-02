//
// osc_input.cpp
//
// DomeDoom OSC/UDP game control receiver. See osc_input.h for the design.
//

#include "osc_input.h"

#include "c_cvars.h"
#include "d_event.h"     // BT_* button flags
#include "i_time.h"      // I_msTime

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cmath>

// ---------------------------------------------------------------------------
// CVARs
// ---------------------------------------------------------------------------

CVAR(Bool, osc_control,       false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int,  osc_control_port,  6666,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, osc_control_debug, false, 0)

// Axes/buttons auto-release this many ms after their last refresh, so a
// stalled or crashed sender cannot leave DoomGuy stuck moving. A live
// controller streaming at >=10 Hz keeps them held; a single message acts as a
// brief pulse. Send an explicit 0 to release immediately.
static constexpr uint64_t OSC_HOLD_MS = 250;

// ---------------------------------------------------------------------------
// Socket + decoded state
// ---------------------------------------------------------------------------

static int           g_sock     = -1;
static int           g_boundPort = -1;
static OSCInputState g_state;

// Per-control last-refresh timestamps for the watchdog.
struct Stamps {
    uint64_t forward = 0, side = 0, turn = 0, pitch = 0, fly = 0;
    uint64_t attack = 0, altattack = 0, use = 0, jump = 0, crouch = 0,
             reload = 0, zoom = 0, run = 0;
} static g_ts;

bool OSC_Input_Enabled()
{
    return (bool)osc_control;
}

static void CloseSocket()
{
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
    g_boundPort = -1;
    g_state = OSCInputState();
    g_ts = Stamps();
}

// Open (or reopen on a port change) a non-blocking UDP socket bound to all
// interfaces on osc_control_port. Returns true when a socket is ready.
static bool EnsureSocket()
{
    int port = (int)osc_control_port;
    if (port < 1 || port > 65535) port = 6666;

    if (g_sock >= 0 && g_boundPort == port) return true;
    if (g_sock >= 0) CloseSocket();  // port changed — rebind

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { fprintf(stderr, "[osc] socket() failed\n"); return false; }

    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(s, (const sockaddr*)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "[osc] bind() failed on port %d\n", port);
        close(s);
        return false;
    }

    g_sock = s;
    g_boundPort = port;
    fprintf(stderr, "[osc] control receiver listening on udp/%d\n", port);
    return true;
}

// ---------------------------------------------------------------------------
// Minimal OSC parsing (no bundles — plain messages only)
// ---------------------------------------------------------------------------

static int32_t ReadBE32(const uint8_t* p)
{
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] <<  8) |  (uint32_t)p[3]);
}

// Advance past an OSC-string (null-terminated, padded to a 4-byte boundary).
// Returns the offset just past the padding, or -1 on overrun.
static int SkipOscString(const uint8_t* buf, int len, int off)
{
    int i = off;
    while (i < len && buf[i] != 0) ++i;
    if (i >= len) return -1;   // no terminator
    ++i;                       // consume the null
    while (i & 3) { if (i >= len) return -1; ++i; }
    return i;
}

// Decode the first argument as a float. Supports 'f' (float32) and 'i'
// (int32); anything else (or no arg) yields the supplied default.
static float FirstArgFloat(const uint8_t* buf, int len, int tagOff,
                           int argOff, float dflt)
{
    char t = (char)buf[tagOff + 1];   // buf[tagOff] == ','
    if (t == 'f') {
        if (argOff + 4 > len) return dflt;
        int32_t raw = ReadBE32(buf + argOff);
        float f; memcpy(&f, &raw, 4);
        return f;
    }
    if (t == 'i') {
        if (argOff + 4 > len) return dflt;
        return (float)ReadBE32(buf + argOff);
    }
    return dflt;   // no typed arg
}

// Apply one decoded message. `mag` is the argument value (defaulting to 1.0
// when the message carried no numeric arg); `now` stamps the watchdog.
static void ApplyMessage(const char* addr, float mag, uint64_t now)
{
    // Clamp analog magnitude to [0, 1] for the directional addresses.
    float m = mag;
    if (m < 0.f) m = 0.f;
    if (m > 1.f) m = 1.f;

    // Button state: nonzero = pressed, zero = released.
    bool pressed = (mag != 0.f);

    auto setBtn = [&](uint32_t bit, uint64_t& stamp) {
        if (pressed) { g_state.buttons |= bit;  stamp = now; }
        else         { g_state.buttons &= ~bit; stamp = 0;   }
    };

    // --- movement / look axes ------------------------------------------
    if      (!strcmp(addr, "/domedoom/front"))       { g_state.forward =  m; g_ts.forward = pressed ? now : 0; }
    else if (!strcmp(addr, "/domedoom/back"))        { g_state.forward = -m; g_ts.forward = pressed ? now : 0; }
    else if (!strcmp(addr, "/domedoom/straferight")) { g_state.side    =  m; g_ts.side    = pressed ? now : 0; }
    else if (!strcmp(addr, "/domedoom/strafeleft"))  { g_state.side    = -m; g_ts.side    = pressed ? now : 0; }
    else if (!strcmp(addr, "/domedoom/left"))        { g_state.turn    =  m; g_ts.turn    = pressed ? now : 0; }
    else if (!strcmp(addr, "/domedoom/right"))       { g_state.turn    = -m; g_ts.turn    = pressed ? now : 0; }
    else if (!strcmp(addr, "/domedoom/lookup"))      { g_state.pitch   =  m; g_ts.pitch   = pressed ? now : 0; }
    else if (!strcmp(addr, "/domedoom/lookdown"))    { g_state.pitch   = -m; g_ts.pitch   = pressed ? now : 0; }
    else if (!strcmp(addr, "/domedoom/moveup"))      { g_state.fly     =  m; g_ts.fly     = pressed ? now : 0; }
    else if (!strcmp(addr, "/domedoom/movedown"))    { g_state.fly     = -m; g_ts.fly     = pressed ? now : 0; }
    // --- action buttons -------------------------------------------------
    else if (!strcmp(addr, "/domedoom/attack"))      setBtn(BT_ATTACK,    g_ts.attack);
    else if (!strcmp(addr, "/domedoom/altattack"))   setBtn(BT_ALTATTACK, g_ts.altattack);
    else if (!strcmp(addr, "/domedoom/use"))         setBtn(BT_USE,       g_ts.use);
    else if (!strcmp(addr, "/domedoom/jump"))        setBtn(BT_JUMP,      g_ts.jump);
    else if (!strcmp(addr, "/domedoom/crouch"))      setBtn(BT_CROUCH,    g_ts.crouch);
    else if (!strcmp(addr, "/domedoom/reload"))      setBtn(BT_RELOAD,    g_ts.reload);
    else if (!strcmp(addr, "/domedoom/zoom"))        setBtn(BT_ZOOM,      g_ts.zoom);
    else if (!strcmp(addr, "/domedoom/run"))       { g_state.run = pressed; g_ts.run = pressed ? now : 0; }
    // --- absolute dome heading -----------------------------------------
    else if (!strcmp(addr, "/domedoom/rotate"))
    {
        double d = fmod((double)mag, 360.0);
        if (d < 0.0) d += 360.0;
        g_state.rotateDeg  = d;
        g_state.haveRotate = true;
    }
    else if ((bool)osc_control_debug)
    {
        fprintf(stderr, "[osc] ignored address '%s'\n", addr);
    }
}

// Release any control whose watchdog has expired.
static void ExpireStale(uint64_t now)
{
    auto dead = [&](uint64_t stamp) { return stamp != 0 && now - stamp > OSC_HOLD_MS; };

    if (dead(g_ts.forward)) { g_state.forward = 0.f; g_ts.forward = 0; }
    if (dead(g_ts.side))    { g_state.side    = 0.f; g_ts.side    = 0; }
    if (dead(g_ts.turn))    { g_state.turn    = 0.f; g_ts.turn    = 0; }
    if (dead(g_ts.pitch))   { g_state.pitch   = 0.f; g_ts.pitch   = 0; }
    if (dead(g_ts.fly))     { g_state.fly     = 0.f; g_ts.fly     = 0; }

    auto expBtn = [&](uint32_t bit, uint64_t& stamp) {
        if (dead(stamp)) { g_state.buttons &= ~bit; stamp = 0; }
    };
    expBtn(BT_ATTACK,    g_ts.attack);
    expBtn(BT_ALTATTACK, g_ts.altattack);
    expBtn(BT_USE,       g_ts.use);
    expBtn(BT_JUMP,      g_ts.jump);
    expBtn(BT_CROUCH,    g_ts.crouch);
    expBtn(BT_RELOAD,    g_ts.reload);
    expBtn(BT_ZOOM,      g_ts.zoom);
    if (dead(g_ts.run))  { g_state.run = false; g_ts.run = 0; }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void OSC_Input_Tick()
{
    if (!(bool)osc_control)
    {
        if (g_sock >= 0) CloseSocket();
        return;
    }
    if (!EnsureSocket()) return;

    uint64_t now = I_msTime();

    // Drain all pending datagrams (bounded so a flood can't stall the tic).
    uint8_t buf[1024];
    for (int guard = 0; guard < 256; ++guard)
    {
        ssize_t n = recv(g_sock, buf, sizeof(buf), 0);
        if (n <= 0) break;                 // EWOULDBLOCK / no more data
        if (n < 4 || buf[0] != '/') continue;  // not an OSC message we handle

        char addr[128];
        int alen = 0;
        while (alen < (int)n && alen < (int)sizeof(addr) - 1 && buf[alen] != 0)
        {
            addr[alen] = (char)buf[alen];
            ++alen;
        }
        addr[alen] = 0;

        int tagOff = SkipOscString(buf, (int)n, 0);
        float mag = 1.0f;   // default when no numeric arg is supplied
        if (tagOff >= 0 && tagOff < (int)n && buf[tagOff] == ',')
        {
            int argOff = SkipOscString(buf, (int)n, tagOff);
            if (argOff >= 0)
                mag = FirstArgFloat(buf, (int)n, tagOff, argOff, 1.0f);
        }

        if ((bool)osc_control_debug)
            fprintf(stderr, "[osc] %s %g\n", addr, mag);

        ApplyMessage(addr, mag, now);
    }

    ExpireStale(now);
}

const OSCInputState& OSC_Input_State()
{
    return g_state;
}

bool OSC_Input_ConsumeRotate(double* deg)
{
    if (!g_state.haveRotate) return false;
    if (deg) *deg = g_state.rotateDeg;
    g_state.haveRotate = false;
    return true;
}
