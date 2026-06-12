#include "spatgris_output.h"
#include "c_cvars.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <unordered_map>

EXTERN_CVAR(Bool,   r_cubemap_spatgris)
EXTERN_CVAR(String, r_cubemap_spatgris_ip)
EXTERN_CVAR(Int,    r_cubemap_spatgris_port)
EXTERN_CVAR(Bool,   r_cubemap_spatgris_stereo)
EXTERN_CVAR(Int,    r_cubemap_spatgris_sources)

// Doom units past which a source sits at the dome edge (distance = 1.0).
static constexpr float SPAT_MAX_DIST = 2048.f;

static int          g_sock     = -1;
static sockaddr_in  g_dest     = {};
static float        g_lx = 0.f, g_ly = 0.f, g_lz = 0.f, g_la = 0.f;

// Slot pool: ALuint → SpatGRIS source ID (1-based)
static std::unordered_map<uint32_t, int> g_srcMap;
static bool g_slotFree[128] = {};
static int  g_numSlots = 0;

static void InitSlots()
{
    g_numSlots = (int)r_cubemap_spatgris_sources;
    if (g_numSlots < 1)   g_numSlots = 1;
    if (g_numSlots > 128) g_numSlots = 128;
    for (int i = 0; i < g_numSlots; ++i) g_slotFree[i] = true;
    g_srcMap.clear();
}

static bool EnsureSocket()
{
    if (g_sock >= 0) return true;
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { fprintf(stderr, "[spatgris] socket() failed\n"); return false; }
    InitSlots();
    fprintf(stderr, "[spatgris] OSC sender ready (%d slots)\n", g_numSlots);
    return true;
}

static void UpdateDest()
{
    memset(&g_dest, 0, sizeof(g_dest));
    g_dest.sin_family = AF_INET;
    g_dest.sin_port   = htons((uint16_t)(int)r_cubemap_spatgris_port);
    inet_pton(AF_INET, *r_cubemap_spatgris_ip, &g_dest.sin_addr);
}

// Minimal OSC packet builder — fixed stack buffer, no heap allocation.
struct OscBuf {
    uint8_t b[128] = {};
    int n = 0;
    void str(const char* s) {
        for (; *s; ++s) b[n++] = (uint8_t)*s;
        b[n++] = 0;
        while (n & 3) b[n++] = 0;
    }
    void i32(int32_t v) {
        b[n++] = (uint8_t)((v >> 24) & 0xFF);
        b[n++] = (uint8_t)((v >> 16) & 0xFF);
        b[n++] = (uint8_t)((v >>  8) & 0xFF);
        b[n++] = (uint8_t)( v        & 0xFF);
    }
    void f32(float v) { uint32_t u; memcpy(&u, &v, 4); i32((int32_t)u); }
};

static void SendPos(int spatId, float sx, float sy, float sz)
{
    // Listener-relative vector in Doom world space (X=east, Y=north, Z=up).
    float dx = sx - g_lx, dy = sy - g_ly, dz = sz - g_lz;

    // Project onto listener's forward/right axes.
    // Forward at angle a: (cos a, sin a). Right: (sin a, -cos a).
    float fwd = dx * cosf(g_la) + dy * sinf(g_la);
    float rgt = dx * sinf(g_la) - dy * cosf(g_la);

    float dist  = sqrtf(dx*dx + dy*dy + dz*dz);
    float az    = atan2f(rgt, fwd) * (180.f / (float)M_PI);   // clockwise from front, degrees
    float horiz = sqrtf(fwd*fwd + rgt*rgt);
    float el    = atan2f(dz, horiz) * (180.f / (float)M_PI);  // positive = up, degrees
    float nd    = dist / SPAT_MAX_DIST;
    if (nd > 1.f) nd = 1.f;

    // OSC message: /spat/serv  type=",sifffff"  args: "deg", id, az, el, dist, h_span, v_span
    OscBuf p;
    p.str("/spat/serv");
    p.str(",sifffff");
    p.str("deg");
    p.i32(spatId);
    p.f32(az);
    p.f32(el);
    p.f32(nd);
    p.f32(0.f);  // h_span
    p.f32(0.f);  // v_span

    sendto(g_sock, p.b, p.n, 0, (const sockaddr*)&g_dest, sizeof(g_dest));
}

// ---------------------------------------------------------------------------

void SpatGRIS_UpdateListener(float x, float y, float z, float angleRad)
{
    g_lx = x; g_ly = y; g_lz = z; g_la = angleRad;
}

void SpatGRIS_AllocSource(uint32_t alSrc, float sx, float sy, float sz)
{
    if (!(bool)r_cubemap_spatgris || (bool)r_cubemap_spatgris_stereo) return;
    if (!EnsureSocket()) return;
    UpdateDest();

    int id = -1;
    for (int i = 0; i < g_numSlots; ++i) {
        if (g_slotFree[i]) { id = i + 1; g_slotFree[i] = false; break; }
    }
    if (id < 0) return;  // all slots busy — drop

    g_srcMap[alSrc] = id;
    SendPos(id, sx, sy, sz);
}

void SpatGRIS_UpdateSource(uint32_t alSrc, float sx, float sy, float sz)
{
    if (!(bool)r_cubemap_spatgris || (bool)r_cubemap_spatgris_stereo) return;
    if (g_sock < 0) return;
    auto it = g_srcMap.find(alSrc);
    if (it == g_srcMap.end()) return;
    SendPos(it->second, sx, sy, sz);
}

void SpatGRIS_FreeSource(uint32_t alSrc)
{
    auto it = g_srcMap.find(alSrc);
    if (it == g_srcMap.end()) return;
    int id = it->second;
    if (id >= 1 && id <= g_numSlots) g_slotFree[id - 1] = true;
    g_srcMap.erase(it);
}
