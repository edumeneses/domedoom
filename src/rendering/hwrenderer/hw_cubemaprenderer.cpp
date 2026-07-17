//
// hw_cubemaprenderer.cpp
//
// Renders 6 cubemap faces per frame into FCanvasTexture targets for
// fulldome output via PipeWire. Each face uses a 90° square FOV.
//
// Injection point: RenderView() in hw_entrypoint.cpp, hardware path.
//

#include "hw_cubemaprenderer.h"

#include "d_player.h"
#include "actor.h"
#include "r_utility.h"
#include "v_video.h"
#include "textures.h"
#include "vectors.h"
#include "scene/hw_drawinfo.h"  // RenderViewpoint declaration
#include "c_cvars.h"
#include "v_draw.h"
#include "spatgris_output.h"

#include <cstdio>
#include <cstring>
#include <cmath>

// Output projection: 0 = horizontal cubemap strip (6144x1024),
//                    1 = square fisheye domemaster (2048x2048),
//                    2 = equirectangular panorama (4096x2048).
// NOTE: PipeWire init and the readback PBO size on the first frame; switching
// this at runtime needs a restart to re-init those paths cleanly.
CVAR(Int,    r_cubemap_mode,            CUBE_OUT_DOME,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_dome_fov,        270.f,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_dome_yaw,        180.f,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_dome_pitch,      90.f,           CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_dome_roll,       180.f,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Output image flips. GL and Vulkan need different defaults (NDC + texture
// origin differ), so these are exposed live per machine/backend.
CVAR(Bool,   r_cubemap_dome_flip_h,     false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_dome_flip_v,     false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_dome_flip_ud,    false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_dome_swap_ud,    true,           CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Lock the dome to a fixed world heading: the projected world stops following
// the player's yaw, so turning/rotating moves the player across a static dome
// instead of spinning the whole image. Latched to the player's yaw when first
// enabled. Intended for OSC /domedoom/rotate + left/right dome control.
CVAR(Bool,   r_cubemap_dome_lock_yaw,    false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Rim HUD (domemaster only): status bar drawn as a band along the front rim.
CVAR(Bool,   r_cubemap_dome_hud,        true,           CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_dome_hud_arc,    45.f,           CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_dome_hud_band,   0.035f,         CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_dome_hud_strip,  0.035f,         CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_dome_hud_offset, 0.f,            CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Crop each side of the HUD band (0 = full width, 0.49 = almost nothing).
CVAR(Float,  r_cubemap_dome_hud_crop,   0.275f,         CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_dome_hud_flip_h, false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_dome_hud_flip_v, true,           CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Cube face the 2D overlays (strip-mode HUD, menu, no-scene screen) are baked
// onto. Front suits most setups; rotated dome orientations may want another.
CVAR(Int,    r_cubemap_hud_face,        CUBE_FRONT,     CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Equirectangular content rotation (defaults keep the front view centred).
CVAR(Float,  r_cubemap_equi_yaw,        0.f,            CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_equi_pitch,      0.f,            CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float,  r_cubemap_equi_roll,       0.f,            CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Equirect output flips, separate from the dome's (each projection is tuned
// per machine/backend; flip_v=true is correct for Vulkan on the SAT box).
CVAR(Bool,   r_cubemap_equi_flip_h,     false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_equi_flip_v,     true,           CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool,   r_cubemap_pipewire,        true,           CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_sh4lt,           false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, r_cubemap_sh4lt_label,     "domedoom",     CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_sh4lt_audio,     false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, r_cubemap_sh4lt_audio_label, "domedoom-audio", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_ndi,             false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, r_cubemap_ndi_label,       "DomeDoom",     CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_debug,           false,          0)

CUSTOM_CVAR(Bool, r_cubemap_spatgris, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
    if (self) SpatGRIS_InitAudio();
    else      SpatGRIS_ShutdownAudio();
}
CVAR(String, r_cubemap_spatgris_ip,     "127.0.0.1",   CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int,    r_cubemap_spatgris_port,   18032,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_spatgris_stereo, false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int,    r_cubemap_spatgris_sources,32,             CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Defined in r_utility.cpp, also extern'd in hw_entrypoint.cpp
extern bool NoInterpolateView;

// True while rendering the 6 cube faces. Sprite billboarding reads this to
// face the camera POSITION (view-direction-independent) so billboards line up
// across face seams instead of splitting. See hw_sprites.cpp.
bool gCubemapFaceRender = false;

// -------------------------------------------------------------------------

struct FaceDesc
{
	float       yawDeg;    // offset from player yaw
	float       pitchDeg;  // absolute pitch for this face
	int         crossCol;  // column in cross layout (0..3)
	int         crossRow;  // row in cross layout (0..2)
};

static constexpr FaceDesc kFaces[CUBE_FACE_COUNT] = {
//   yaw    pitch  col  row  — horizontal strip: [R][L][U][D][F][B]
	{   0.f,   0.f,  4,   0 },  // FRONT  → col 4
	{  90.f,   0.f,  1,   0 },  // LEFT   → col 1
	{ -90.f,   0.f,  0,   0 },  // RIGHT  → col 0
	{ 180.f,   0.f,  5,   0 },  // BACK   → col 5
	{   0.f, -90.f,  2,   0 },  // UP     → col 2  (engine: -pitch looks up)
	{   0.f,  90.f,  3,   0 },  // DOWN   → col 3  (engine: +pitch looks down)
};

// -------------------------------------------------------------------------

CubemapRenderer gCubemapRenderer;

CubemapRenderer::~CubemapRenderer()
{
	Shutdown();
}

void CubemapRenderer::Shutdown()
{
	// Stop streaming outputs first.
	if (mAudioTapActive)
	{
		Sh4ltRemoveAudioTap();
		mSh4ltAudio.Shutdown();
		mAudioTapActive = false;
	}
	mNdiVideo.Shutdown();
	mSh4ltVideo.Shutdown();
	mPWOutput.Shutdown();

	// Free GPU textures while the renderer (screen) is still alive. Deleting a
	// null pointer is a no-op, so this is safe to call more than once.
	for (int i = 0; i < CUBE_FACE_COUNT; i++)
	{
		delete mFaceTex[i];
		mFaceTex[i] = nullptr;
	}
	delete mCrossTex;
	mCrossTex = nullptr;
	delete mDomeTex;
	mDomeTex = nullptr;
	delete mEquiTex;
	mEquiTex = nullptr;
	delete mHudTex;
	mHudTex = nullptr;

	mInitialized = false;
}

// -------------------------------------------------------------------------
// Output selection: cubemap strip vs domemaster vs equirect.

static int OutputMode()
{
	const int m = r_cubemap_mode;
	return (m < CUBE_OUT_STRIP || m > CUBE_OUT_EQUI) ? CUBE_OUT_DOME : m;
}

// Face selected for baked 2D overlays, clamped to a valid index.
static int HudFace()
{
	const int f = r_cubemap_hud_face;
	return (f < 0 || f >= CUBE_FACE_COUNT) ? CUBE_FRONT : f;
}

int CubemapRenderer::OutW() const
{
	switch (OutputMode())
	{
	case CUBE_OUT_DOME: return DOME_SIZE;
	case CUBE_OUT_EQUI: return EQUI_W;
	default:            return CROSS_W;
	}
}

int CubemapRenderer::OutH() const
{
	switch (OutputMode())
	{
	case CUBE_OUT_DOME: return DOME_SIZE;
	case CUBE_OUT_EQUI: return EQUI_H;
	default:            return CROSS_H;
	}
}

FCanvasTexture* CubemapRenderer::OutTex()
{
	switch (OutputMode())
	{
	case CUBE_OUT_DOME: return mDomeTex;
	case CUBE_OUT_EQUI: return mEquiTex;
	default:            return mCrossTex;
	}
}

// Build the inverse content rotation as a column-major 3x3 for
// glUniformMatrix3fv(transpose=GL_FALSE). The shader computes
// local = uInvRot * world and must match the ossia domemaster ISF, where
// inverse_content_rotation_matrix = transpose(makeRotationMatrix(yaw,pitch,roll)).
//
// Let M be the ISF forward rotation (math) matrix. We want G = transpose(M).
// glUniformMatrix3fv column-major means data[col*3+row] = G(row,col) = M(col,row).
static void BuildInvRot(float yawDeg, float pitchDeg, float rollDeg, bool flipPolar, float out[9])
{
	const float d2r = 3.14159265359f / 180.0f;
	float cy = cosf(yawDeg   * d2r), sy = sinf(yawDeg   * d2r);
	float cp = cosf(pitchDeg * d2r), sp = sinf(pitchDeg * d2r);
	float cr = cosf(rollDeg  * d2r), sr = sinf(rollDeg  * d2r);

	// ISF forward matrix M (math, row-major):
	//   M(0,*) = [ cy*cr - sy*cp*sr,  sy*cr + cy*cp*sr,   sp*sr ]
	//   M(1,*) = [-cy*sr - sy*cp*cr, -sy*sr + cy*cp*cr,   sp*cr ]
	//   M(2,*) = [ sy*sp,            -cy*sp,              cp    ]
	// out[col*3+row] = M(col,row):
	out[0] =  cy*cr - sy*cp*sr;  out[1] =  sy*cr + cy*cp*sr;  out[2] =  sp*sr;
	out[3] = -cy*sr - sy*cp*cr;  out[4] = -sy*sr + cy*cp*cr;  out[5] =  sp*cr;
	out[6] =  sy*sp;             out[7] = -cy*sp;             out[8] =  cp;

	// Flip the fisheye polar axis (column 2) so the dome centre points to engine
	// UP, not down. Without this the zenith shows the floor. Folding it here
	// (instead of negating the ray's z in each shader) keeps the azimuth framing
	// and fixes both GL and Vulkan from one place. The equirect projection has
	// no polar axis to fix (its forward ray is +Z, not up), so it skips this.
	if (flipPolar)
	{
		out[6] = -out[6];  out[7] = -out[7];  out[8] = -out[8];
	}
}

// -------------------------------------------------------------------------

void CubemapRenderer::UpdateSh4ltVideo()
{
	const bool want = (bool)r_cubemap_sh4lt;
	const std::string label = *r_cubemap_sh4lt_label;

	if (!want)
	{
		mSh4ltVideo.Shutdown();
		mSh4ltVideoLabel.clear();
		return;
	}

	// Re-init when label, dims (format switch), or writer state changed.
	const int w = OutW(), h = OutH();
	if (!mSh4ltVideo.IsRunning() || label != mSh4ltVideoLabel ||
	    w != mSh4ltW || h != mSh4ltH)
	{
		mSh4ltVideo.Init(label, w, h);
		mSh4ltVideoLabel = label;
		mSh4ltW = w; mSh4ltH = h;
	}
}

void CubemapRenderer::UpdateSh4ltAudio()
{
	const bool want = (bool)r_cubemap_sh4lt_audio;

	if (want && !mAudioTapActive)
	{
		Sh4ltInstallAudioTap(&mSh4ltAudio);
		mAudioTapActive = true;
	}
	else if (!want && mAudioTapActive)
	{
		Sh4ltRemoveAudioTap();
		mSh4ltAudio.Shutdown();
		mAudioTapActive = false;
	}
}

void CubemapRenderer::UpdateNdiVideo()
{
	const bool want = (bool)r_cubemap_ndi;
	const std::string label = *r_cubemap_ndi_label;

	if (!want)
	{
		mNdiVideo.Shutdown();
		mNdiVideoLabel.clear();
		return;
	}

	// Re-init when label, dims (format switch), or sender state changed.
	const int w = OutW(), h = OutH();
	if (!mNdiVideo.IsRunning() || label != mNdiVideoLabel ||
	    w != mNdiW || h != mNdiH)
	{
		mNdiVideo.Init(label, w, h);
		mNdiVideoLabel = label;
		mNdiW = w; mNdiH = h;
	}
}

// -------------------------------------------------------------------------
//
// InitPipeWire
//
// Called once, after the first CompositeCubemapFaces so the cross texture
// has a real GL backing.  Tries DMA-BUF export first; falls back to CPU
// double-PBO readback if EGL / MESA_image_dma_buf_export is unavailable.
//
// -------------------------------------------------------------------------

void CubemapRenderer::InitPipeWire()
{
	mPWAttempted = true;

	const int w = OutW(), h = OutH();

	int stride = 0;
	int dmaFd  = screen->ExportCubemapCrossAsDmaBuf(OutTex(), &stride);

	if (dmaFd >= 0)
	{
		if (!mPWOutput.InitDmaBuf(dmaFd, w, h, stride))
		{
			fprintf(stderr, "[domedoom] PipeWire DMA-BUF stream init failed,"
			                " retrying as CPU\n");
			dmaFd = -1;
		}
	}

	if (dmaFd < 0)
	{
		mPixelBuf.resize((size_t)w * h * 4);
		if (!mPWOutput.InitCPU(w, h))
			fprintf(stderr, "[domedoom] PipeWire CPU stream init failed\n");
	}
}

void CubemapRenderer::Init()
{
	for (int i = 0; i < CUBE_FACE_COUNT; i++)
		mFaceTex[i] = new FCanvasTexture(FACE_SIZE, FACE_SIZE);
	mCrossTex = new FCanvasTexture(CROSS_W, CROSS_H);
	mDomeTex  = new FCanvasTexture(DOME_SIZE, DOME_SIZE);
	mEquiTex  = new FCanvasTexture(EQUI_W, EQUI_H);
	mHudTex   = new FCanvasTexture(HUD_W, HUD_H);
	mInitialized = true;
}

// -------------------------------------------------------------------------
//
// RenderFacesToTextures
//
// Renders all 6 cubemap faces into mFaceTex[]. Camera angles are
// temporarily set to the face direction and restored on return.
//
// Uses mainview=false / toscreen=false so:
//   - No shadow-map collection
//   - No SSAO
//   - No post-processing / tone mapping
//   - No weapon sprites (drawpsprites = false via DM_OFFSCREEN)
//   - FBO is the one set up by screen->RenderTextureView()
//
// -------------------------------------------------------------------------

void CubemapRenderer::RenderFacesToTextures(player_t* player)
{
	if (!mInitialized) Init();

	AActor* camera = player->camera;
	if (!camera) return;

	const DRotator savedAngles = camera->Angles;

	// Capture the player's yaw for the yaw-lock output offset (see
	// CompositeAndStream). The faces still follow the player, so the weapon
	// stays baked at front-centre; the lock is applied later, to the warp.
	mCurViewYaw = savedAngles.Yaw.Degrees();
	if (r_cubemap_dome_lock_yaw)
	{
		if (!mDomeLockValid) { mDomeLockYaw = mCurViewYaw; mDomeLockValid = true; }
	}
	else
	{
		mDomeLockValid = false;
	}

	gCubemapFaceRender = true;   // sprites face camera position, not per-face view
	for (int i = 0; i < CUBE_FACE_COUNT; i++)
	{
		// Override camera direction for this face.
		camera->Angles.Yaw   = savedAngles.Yaw + DAngle::fromDeg(kFaces[i].yawDeg);
		camera->Angles.Pitch = DAngle::fromDeg(kFaces[i].pitchDeg);
		camera->Angles.Roll  = nullAngle;

		// Force no angle interpolation so rotated direction is used exactly.
		NoInterpolateView = true;

		const bool isFront = (i == CUBE_FRONT);
		screen->RenderTextureView(mFaceTex[i], [&](IntRect& bounds)
		{
			FRenderViewpoint facevp;
			RenderViewpoint(facevp, camera, &bounds,
			                90.f,    // FOV — must be 90° for cube faces
			                1.f,     // ratio — square faces
			                1.f,     // fovratio — square
			                false,   // mainview — no SSAO / no scene FBO rebind
			                false,   // toscreen — render to bound FBO, not screen
			                isFront);// drawPSprites — weapons on front face only
		});
	}

	gCubemapFaceRender = false;
	mFacesThisFrame = true;

	// Restore player camera so the game stays unaffected.
	camera->Angles = savedAngles;
}

// -------------------------------------------------------------------------

void CubemapRenderer::BlitHUDToFace(F2DDrawer* drawer, int face)
{
	if (!mInitialized || face < 0 || face >= CUBE_FACE_COUNT ||
	    !mFaceTex[face] || !drawer)
		return;

	// Re-bind the given face's FBO and overlay the 2D HUD (statusbar, etc.) on
	// top of the already-rendered 3D scene.  Draw2D scales the drawer's logical
	// coordinate space (screen resolution) into the face viewport.
	screen->RenderTextureView(mFaceTex[face], [&](IntRect& bounds) {
		Draw2D(drawer, *screen->RenderState(), 0, 0, bounds.width, bounds.height);
	});
}

void CubemapRenderer::BlitHUDToFrontFace(F2DDrawer* drawer)
{
	// Bake onto the selected overlay face (r_cubemap_hud_face, front by default).
	BlitHUDToFace(drawer, HudFace());
}

// -------------------------------------------------------------------------

void CubemapRenderer::BlitHUD(F2DDrawer* drawer)
{
	if (!mInitialized || !drawer) return;

	if (OutputMode() == CUBE_OUT_DOME)
	{
		// Domemaster: render the 2D HUD into its own texture; the warp pass
		// overlays the status bar (the bottom strip) as a band along the dome
		// rim, honouring the shared arc/crop/hide parameters. The status bar
		// fully repaints its strip each frame, so no clear is needed.
		if (r_cubemap_dome_hud && mHudTex)
		{
			screen->RenderTextureView(mHudTex, [&](IntRect& bounds) {
				Draw2D(drawer, *screen->RenderState(), 0, 0, bounds.width, bounds.height);
			});
		}
	}
	else if (OutputMode() == CUBE_OUT_EQUI)
	{
		// Equirect: bake the HUD onto the FRONT face — where the weapon is
		// drawn — so it warps through the panorama with the gun instead of
		// sitting as a flat band at the bottom. Always the front face (not
		// r_cubemap_hud_face) so the HUD stays with the hand/gun; the menu
		// still honours the selected face via BlitMenuToFrontFace.
		if (r_cubemap_dome_hud)
			BlitHUDToFace(drawer, CUBE_FRONT);
	}
	else
	{
		// Strip: bake the HUD straight onto the selected overlay face.
		BlitHUDToFrontFace(drawer);
	}
}

// -------------------------------------------------------------------------

void CubemapRenderer::BlitMenuToFrontFace(F2DDrawer* drawer, int firstCommand)
{
	if (!mInitialized || !mFaceTex[HudFace()] || !drawer) return;
	if (firstCommand >= drawer->DrawCount()) return;   // no menu this frame

	screen->RenderTextureView(mFaceTex[HudFace()], [&](IntRect& bounds) {
		Draw2D(drawer, *screen->RenderState(), 0, 0, bounds.width, bounds.height,
		       firstCommand);
	});
}

// -------------------------------------------------------------------------

void CubemapRenderer::BlitScreenToFrontFace(F2DDrawer* drawer)
{
	if (!mInitialized) Init();
	if (!drawer) return;

	// Blank all faces so stale scene content (or uninitialized texture memory)
	// doesn't surround the 2D screen on the dome/strip; the selected HUD face
	// gets the full 2D screen (title pic, console, menu) on top of the clear.
	F2DDrawer clear;
	clear.Begin(screen->GetWidth(), screen->GetHeight());
	clear.ClearScreen();
	clear.End();

	for (int i = 0; i < CUBE_FACE_COUNT; i++)
	{
		screen->RenderTextureView(mFaceTex[i], [&](IntRect& bounds) {
			Draw2D(&clear, *screen->RenderState(), 0, 0, bounds.width, bounds.height);
			if (i == HudFace())
				Draw2D(drawer, *screen->RenderState(), 0, 0, bounds.width, bounds.height);
		});
	}
}

// -------------------------------------------------------------------------

void CubemapRenderer::CompositeAndStream()
{
	if (!mInitialized) return;

	const int mode = OutputMode();

	// Yaw lock (dome + equirect): counter-rotate the output by the player's
	// yaw change since the reference. The scene is captured in the player's
	// frame, so adding the player yaw to the warp yaw cancels the scene's
	// rotation (world stays fixed on the output) while orbiting the
	// front-face weapon to the aim direction. See mDomeLock* in the header.
	float lockYawOffset = 0.f;
	if (r_cubemap_dome_lock_yaw && mDomeLockValid)
		lockYawOffset = (float)(mCurViewYaw - mDomeLockYaw);

	if (mode == CUBE_OUT_DOME)
	{
		// Warp the 6 faces into a square fisheye domemaster.
		DomemasterParams dp;
		dp.fovDeg = r_cubemap_dome_fov;
		BuildInvRot(r_cubemap_dome_yaw + lockYawOffset,
		            r_cubemap_dome_pitch, r_cubemap_dome_roll,
		            true, dp.invRot);
		dp.flipH = r_cubemap_dome_flip_h;
		dp.flipV = r_cubemap_dome_flip_v;
		dp.flipUpDown = r_cubemap_dome_flip_ud;
		dp.swapUpDownFaces = r_cubemap_dome_swap_ud;
		// Rim HUD only when a scene was rendered — with no level the whole 2D
		// screen is already baked onto the front face and mHudTex is stale.
		dp.hudTex    = (r_cubemap_dome_hud && mHudTex && mFacesThisFrame) ? mHudTex : nullptr;
		dp.hudArcDeg = r_cubemap_dome_hud_arc;
		dp.hudBand   = r_cubemap_dome_hud_band;
		dp.hudStrip  = r_cubemap_dome_hud_strip;
		dp.hudOffsetDeg = r_cubemap_dome_hud_offset;
		dp.hudCrop   = r_cubemap_dome_hud_crop;
		dp.hudFlipH  = r_cubemap_dome_hud_flip_h;
		dp.hudFlipV  = r_cubemap_dome_hud_flip_v;
		screen->RenderDomemaster(mFaceTex, FACE_SIZE, mDomeTex, DOME_SIZE, DOME_SIZE, dp);
	}
	else if (mode == CUBE_OUT_EQUI)
	{
		// Warp the 6 faces into a 2:1 equirectangular panorama. The HUD is
		// baked onto the front face (see BlitHUD), so it warps through the
		// panorama with the weapon — no separate overlay band here. The menu
		// is likewise baked onto a face (see BlitMenuToFrontFace).
		DomemasterParams ep;
		ep.equirect = true;
		BuildInvRot(r_cubemap_equi_yaw + lockYawOffset,
		            r_cubemap_equi_pitch, r_cubemap_equi_roll,
		            false, ep.invRot);
		ep.flipH = r_cubemap_equi_flip_h;
		ep.flipV = r_cubemap_equi_flip_v;
		ep.flipUpDown = r_cubemap_dome_flip_ud;
		ep.swapUpDownFaces = r_cubemap_dome_swap_ud;
		ep.hudTex    = nullptr;   // HUD baked on the front face, not overlaid
		screen->RenderDomemaster(mFaceTex, FACE_SIZE, mEquiTex, EQUI_W, EQUI_H, ep);
	}
	else
	{
		// GPU blit: assemble the 6 face textures into the horizontal strip.
		screen->CompositeCubemapFaces(mFaceTex, FACE_SIZE, mCrossTex);
	}

	// ---- PipeWire init / DMA-BUF path -----------------------------------
	if (r_cubemap_pipewire && !mPWAttempted)
		InitPipeWire();

	const bool pwRunning = r_cubemap_pipewire && mPWOutput.IsRunning();
	const bool pwDmaBuf  = pwRunning && mPWOutput.IsDmaBufMode();
	const bool pwCPU     = pwRunning && !mPWOutput.IsDmaBufMode();

	if (pwDmaBuf)
	{
		// Zero-copy: GL commands already queued; DRM implicit sync handles
		// producer/consumer ordering.
		mPWOutput.QueueDmaBufFrame();
	}

	// ---- Shared CPU readback (PipeWire-CPU + Sh4lt + NDI) ---------------
	// All CPU consumers share ONE readback per frame. The double-PBO in
	// ReadCubemapCrossPixels keeps static ping/pong state and assumes a
	// single call per frame, so we must not call it more than once here.
	UpdateSh4ltVideo();
	UpdateNdiVideo();

	const int outW = OutW(), outH = OutH();

	const bool needPixels = pwCPU || mSh4ltVideo.IsRunning() || mNdiVideo.IsRunning();
	if (needPixels)
	{
		if (mPixelBuf.size() != (size_t)outW * outH * 4)
			mPixelBuf.resize((size_t)outW * outH * 4);

		screen->ReadCubemapCrossPixels(OutTex(), mPixelBuf.data(),
		                               outW, outH);

		// First readback returns nothing (pong PBO not yet populated).
		if (mPBOFirstFrame)
		{
			mPBOFirstFrame = false;
		}
		else
		{
			if (r_cubemap_debug)
			{
				static int  dbgFrame = 0;
				static bool dbgDumped = false;
				if ((dbgFrame++ % 120) == 0)
				{
					const uint8_t* p = mPixelBuf.data();
					const size_t n = (size_t)outW * outH * 4;
					size_t nonzero = 0; uint8_t mx = 0;
					// Sample every 64th byte to keep this cheap.
					for (size_t i = 0; i < n; i += 64)
					{
						if (p[i]) { ++nonzero; if (p[i] > mx) mx = p[i]; }
					}
					fprintf(stderr, "[domedoom/dbg] frame=%d pwCPU=%d pwDMA=%d "
					        "sh4lt=%d ndi=%d  buf: nonzero(sampled)=%zu max=%u\n",
					        dbgFrame, (int)pwCPU, (int)pwDmaBuf,
					        (int)mSh4ltVideo.IsRunning(), (int)mNdiVideo.IsRunning(),
					        nonzero, (unsigned)mx);
				}

				// Dump the readback buffer to a viewable PPM so the render/
				// readback stage can be inspected without any PipeWire/Sh4lt/
				// NDI receiver. Refreshed every 120 frames so late overlays
				// (menu, console) show up. RGBA->RGB, flipped to top-down.
				if (!dbgDumped || (dbgFrame % 120) == 1)
				{
					dbgDumped = true;
					const char* path = "/tmp/domedoom-readback.ppm";
					FILE* f = fopen(path, "wb");
					if (f)
					{
						fprintf(f, "P6\n%d %d\n255\n", outW, outH);
						std::vector<uint8_t> row((size_t)outW * 3);
						for (int y = 0; y < outH; ++y)
						{
							const uint8_t* src =
							    mPixelBuf.data() + (size_t)(outH - 1 - y) * outW * 4;
							for (int x = 0; x < outW; ++x)
							{
								row[x * 3 + 0] = src[x * 4 + 0];
								row[x * 3 + 1] = src[x * 4 + 1];
								row[x * 3 + 2] = src[x * 4 + 2];
							}
							fwrite(row.data(), 1, row.size(), f);
						}
						fclose(f);
						fprintf(stderr, "[domedoom/dbg] wrote %s\n", path);
					}
				}
			}

			if (pwCPU)               mPWOutput.PushFrame(mPixelBuf.data(), outW * 4);
			if (mSh4ltVideo.IsRunning()) mSh4ltVideo.PushFrame(mPixelBuf.data(), outW * 4);
			if (mNdiVideo.IsRunning())   mNdiVideo.PushFrame(mPixelBuf.data(), outW * 4);
		}
	}

	// ---- Sh4lt audio tap ------------------------------------------------
	UpdateSh4ltAudio();

	mFacesThisFrame = false;   // consumed; next frame decides scene vs 2D-only
}
