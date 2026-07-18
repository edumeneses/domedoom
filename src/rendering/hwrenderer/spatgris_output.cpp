#include "spatgris_output.h"
#include "pw_audio_output.h"
#include "domedoom_audiotap.h"
#include "c_cvars.h"

#include "../../common/audio/sound/oal_sfxcache.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

EXTERN_CVAR(Bool,   r_cubemap_spatgris)
EXTERN_CVAR(String, r_cubemap_spatgris_ip)
EXTERN_CVAR(Int,    r_cubemap_spatgris_port)
EXTERN_CVAR(Bool,   r_cubemap_spatgris_stereo)
EXTERN_CVAR(Int,    r_cubemap_spatgris_sources)
EXTERN_CVAR(Float,  snd_sfxvolume)

// Mute 3D world sounds in the OpenAL stereo mix once their PCM plays on a
// per-source SpatGRIS channel. Turning it off restores the stereo copies.
CUSTOM_CVAR(Bool, r_cubemap_spatgris_mute3d, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
    if (!self) SpatGRIS_UnmuteAll();
}

// Stereo bed: reserve SpatGRIS channels 1/2 for the music + 2D/UI mix,
// positioned as a stereo speaker pair ±45° around the gun. Replaces the
// local stereo playback of those sounds. Needs a restart to re-slot.
CVAR(Bool,  r_cubemap_spatgris_bed, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// SpatGRIS distance for the player's own sounds (gun shots, pickups): 0 =
// dome centre, 1 = dome edge. They sit at the gun's azimuth at this radius.
CVAR(Float, r_cubemap_spatgris_gun_dist, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Doom units past which a source sits at the dome edge (distance = 1.0).
static constexpr float SPAT_MAX_DIST = 2048.f;

static int          g_sock     = -1;
static sockaddr_in  g_dest     = {};
static float        g_lx = 0.f, g_ly = 0.f, g_lz = 0.f, g_la = 0.f;

// Dome yaw-lock / gun state (SpatGRIS_SetGunYaw). With the lock on, world
// azimuths are computed against the locked heading so they stay fixed on the
// dome like the image; the gun azimuth is where the weapon currently sits.
static bool  g_lockActive = false;
static float g_lockA      = 0.f;   // locked heading, radians (listener space)
static float g_gunAzDeg   = 0.f;   // gun azimuth on the dome, degrees CW from front

// Bed = SpatGRIS IDs 1/2 reserved for the music + 2D/UI stereo pair.
static bool g_bedReserved = false;

// Slot pool: ALuint → SpatGRIS source ID (1-based)
static std::unordered_map<uint32_t, int> g_srcMap;
static bool g_slotFree[128] = {};
static int  g_numSlots = 0;

// AL sources whose stereo-mix copy is muted because their PCM plays on a
// per-source PipeWire channel instead (see SpatGRIS_AllocSource).
static std::unordered_set<uint32_t> g_mutedSrc;

// Per-source PipeWire audio — single N-channel stream, inited at CVAR enable.
static PipeWireAudioOutput g_pwAudio;

static void InitSlots()
{
    g_numSlots = (int)r_cubemap_spatgris_sources;
    if (g_numSlots < 1)   g_numSlots = 1;
    if (g_numSlots > 64)  g_numSlots = 64;  // matches PipeWireAudioOutput::MAX_SLOTS
    for (int i = 0; i < g_numSlots; ++i) g_slotFree[i] = true;

    // Reserve IDs 1/2 for the stereo bed pair; 3D sounds start at ID 3.
    g_bedReserved = r_cubemap_spatgris_bed && g_numSlots > 2;
    if (g_bedReserved) g_slotFree[0] = g_slotFree[1] = false;

    g_srcMap.clear();
}

static bool BedLive()
{
    return g_bedReserved && g_pwAudio.IsRunning() &&
           (bool)r_cubemap_spatgris && !(bool)r_cubemap_spatgris_stereo;
}

static bool EnsureSocket()
{
    if (g_sock >= 0) return true;
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { fprintf(stderr, "[spatgris] socket() failed\n"); return false; }
    fprintf(stderr, "[spatgris] OSC sender ready (%d slots)\n", g_numSlots);
    return true;
}

// Music/stream tap → bed ring (SpatGRIS channels 1/2). Called from the music
// streaming thread; returns true to CONSUME the batch (mutes local playback).
static bool SpatMusicTap(const void* data, size_t bytes, int samplerate,
                         int channels, bool isFloat, float gain)
{
    if (!BedLive()) return false;
    g_pwAudio.PushBedStream(data, bytes, samplerate, channels, isFloat, gain);
    return true;
}

void SpatGRIS_InitAudio()
{
    if (g_pwAudio.IsRunning()) return;
    InitSlots();
    EnsureSocket();
    if (!g_pwAudio.Init(g_numSlots))
        fprintf(stderr, "[spatgris] PipeWire per-source audio failed to init\n");
    if (g_bedReserved && g_pwAudio.IsRunning())
    {
        g_pwAudio.EnableBed(true);
        g_oalMusicTap = SpatMusicTap;
        fprintf(stderr, "[spatgris] stereo bed on channels 1/2 (music + UI)\n");
    }
}

void SpatGRIS_ShutdownAudio()
{
    g_oalMusicTap = nullptr;       // stop consuming music before teardown
    SpatGRIS_UnmuteAll();          // live world/UI sounds back to the stereo mix
    g_pwAudio.EnableBed(false);
    g_pwAudio.Shutdown();
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
    g_srcMap.clear();
    for (int i = 0; i < g_numSlots; ++i) g_slotFree[i] = true;
    g_bedReserved = false;
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

// OSC message: /spat/serv  type=",sifffff"  args: "deg", id, az, el, dist, h_span, v_span
static void SendPosAngles(int spatId, float azDeg, float elDeg, float dist)
{
    while (azDeg >  180.f) azDeg -= 360.f;
    while (azDeg < -180.f) azDeg += 360.f;

    OscBuf p;
    p.str("/spat/serv");
    p.str(",sifffff");
    p.str("deg");
    p.i32(spatId);
    p.f32(azDeg);
    p.f32(elDeg);
    p.f32(dist);
    p.f32(0.f);  // h_span
    p.f32(0.f);  // v_span

    sendto(g_sock, p.b, p.n, 0, (const sockaddr*)&g_dest, sizeof(g_dest));
}

// Sounds this close to the listener (Doom units) are the player's own —
// weapon fire, pickups, pain. They follow the gun on the dome.
static constexpr float SELF_SOUND_DIST = 40.f;

static void SendPos(int spatId, float sx, float sy, float sz)
{
    // Listener-relative vector in the engine's SOUND space: X=east, Y=UP,
    // Z=north (see OpenALSoundRenderer::UpdateListener — its orientation
    // vectors are forward=(cos a, 0, -sin a), up=(0,1,0)). The positions the
    // OpenAL layer hands us are in this space, NOT Doom world x/y/z.
    float dx = sx - g_lx, dy = sy - g_ly, dz = sz - g_lz;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);

    // The player's own sounds sit at the gun's dome azimuth (front when the
    // yaw lock is off) instead of collapsing to the dome centre.
    if (dist < SELF_SOUND_DIST)
    {
        float gd = r_cubemap_spatgris_gun_dist;
        if (gd < 0.f) gd = 0.f;
        if (gd > 1.f) gd = 1.f;
        SendPosAngles(spatId, g_gunAzDeg, 0.f, gd);
        return;
    }

    // With the dome yaw lock active the projected world is frozen at the
    // locked heading, so azimuths are computed against it — world sounds stay
    // fixed on the dome while the player turns, matching the image.
    const float a = g_lockActive ? g_lockA : g_la;

    // Project onto the listener's forward/right axes in the horizontal (X,Z)
    // plane. Forward at angle a: (cos a, -sin a). Right = forward x up =
    // (sin a, cos a).
    float fwd = dx * cosf(a) - dz * sinf(a);
    float rgt = dx * sinf(a) + dz * cosf(a);

    float az    = atan2f(rgt, fwd) * (180.f / (float)M_PI);   // clockwise from front, degrees
    float horiz = sqrtf(fwd*fwd + rgt*rgt);
    float el    = atan2f(dy, horiz) * (180.f / (float)M_PI);  // positive = up, degrees
    float nd    = dist / SPAT_MAX_DIST;
    if (nd > 1.f) nd = 1.f;

    SendPosAngles(spatId, az, el, nd);
}

// ---------------------------------------------------------------------------

void SpatGRIS_UpdateListener(float x, float y, float z, float angleRad)
{
    g_lx = x; g_ly = y; g_lz = z; g_la = angleRad;

    // Keep the stereo-bed pair glued to the gun: left channel (ID 1) 45° to
    // the gun's left, right channel (ID 2) 45° to its right, at the dome edge
    // like a pair of stereo speakers.
    if (BedLive() && g_sock >= 0)
    {
        UpdateDest();
        SendPosAngles(1, g_gunAzDeg - 45.f, 0.f, 1.f);
        SendPosAngles(2, g_gunAzDeg + 45.f, 0.f, 1.f);
    }
}

void SpatGRIS_SetGunYaw(bool lockActive, float lockYawDeg, float curYawDeg)
{
    g_lockActive = lockActive;
    if (!lockActive)
    {
        g_gunAzDeg = 0.f;   // gun fixed at the dome front
        return;
    }
    g_lockA = lockYawDeg * ((float)M_PI / 180.f);

    // Doom yaw grows counter-clockwise (turning left); SpatGRIS azimuth grows
    // clockwise. Turning left moves the gun left on the dome → negative az.
    float az = -(curYawDeg - lockYawDeg);
    while (az >  180.f) az -= 360.f;
    while (az < -180.f) az += 360.f;
    g_gunAzDeg = az;
}

bool SpatGRIS_AllocSource(uint32_t alSrc, uint32_t alBuf, float sx, float sy, float sz)
{
    if (!(bool)r_cubemap_spatgris || (bool)r_cubemap_spatgris_stereo) return false;
    if (g_sock < 0) return false;  // not inited — SpatGRIS_InitAudio not called yet
    UpdateDest();

    int id = -1;
    for (int i = 0; i < g_numSlots; ++i) {
        if (g_slotFree[i]) { id = i + 1; g_slotFree[i] = false; break; }
    }
    if (id < 0) return false;  // all slots busy — drop (sound stays in the stereo mix)

    g_srcMap[alSrc] = id;
    SendPos(id, sx, sy, sz);

    OALPCMView pcm;
    if (g_pwAudio.IsRunning() && OAL_GetSFXPCM(alBuf, &pcm))
    {
        g_pwAudio.AllocSlot(id - 1, pcm.data, pcm.bytes,
                            pcm.sampleRate, pcm.bits, pcm.channels);
        // The sound now plays on its own SpatGRIS channel; mute its stereo-mix
        // copy so the stereo bed carries only music + 2D/UI sounds. Skipped
        // when muting is disabled or the PCM couldn't be tapped — then the
        // stereo copy is the only audio and must stay audible.
        if (r_cubemap_spatgris_mute3d)
        {
            g_mutedSrc.insert(alSrc);
            return true;
        }
    }
    return false;
}

void SpatGRIS_UpdateSource(uint32_t alSrc, float sx, float sy, float sz)
{
    if (!(bool)r_cubemap_spatgris || (bool)r_cubemap_spatgris_stereo) return;
    if (g_sock < 0) return;
    auto it = g_srcMap.find(alSrc);
    if (it == g_srcMap.end()) return;
    SendPos(it->second, sx, sy, sz);
}

bool SpatGRIS_SourceSpatialized(uint32_t alSrc)
{
    return g_mutedSrc.find(alSrc) != g_mutedSrc.end();
}

bool SpatGRIS_Start2DSound(uint32_t alSrc, uint32_t alBuf, float gain)
{
    if (!BedLive()) return false;

    OALPCMView pcm;
    if (!OAL_GetSFXPCM(alBuf, &pcm)) return false;

    g_pwAudio.PlayBedSound(pcm.data, pcm.bytes, pcm.sampleRate,
                           pcm.bits, pcm.channels, gain);
    g_mutedSrc.insert(alSrc);   // keep the local stereo copy silent
    return true;
}

void SpatGRIS_UnmuteAll()
{
    if (g_mutedSrc.empty()) return;
    g_mutedSrc.clear();
    // Re-walk every channel's gain now that nothing is flagged muted.
    snd_sfxvolume->Callback();
}

void SpatGRIS_FreeSource(uint32_t alSrc)
{
    g_mutedSrc.erase(alSrc);
    auto it = g_srcMap.find(alSrc);
    if (it == g_srcMap.end()) return;
    int id = it->second;
    if (id >= 1 && id <= g_numSlots)
    {
        g_slotFree[id - 1] = true;
        if (g_pwAudio.IsRunning())
            g_pwAudio.FreeSlot(id - 1);
    }
    g_srcMap.erase(it);
}
