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

#include <cstdio>
#include <cstring>

CVAR(Bool,   r_cubemap_pipewire,        true,           CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_sh4lt,           false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, r_cubemap_sh4lt_label,     "cubedoom",     CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_sh4lt_audio,     false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, r_cubemap_sh4lt_audio_label, "cubedoom-audio", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_ndi,             false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, r_cubemap_ndi_label,       "CubeDoom",     CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_debug,           false,          0)

CVAR(Bool,   r_cubemap_spatgris,        false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, r_cubemap_spatgris_ip,     "127.0.0.1",   CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int,    r_cubemap_spatgris_port,   18032,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,   r_cubemap_spatgris_stereo, false,          CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int,    r_cubemap_spatgris_sources,32,             CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Defined in r_utility.cpp, also extern'd in hw_entrypoint.cpp
extern bool NoInterpolateView;

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
	{   0.f,  90.f,  2,   0 },  // UP     → col 2
	{   0.f, -90.f,  3,   0 },  // DOWN   → col 3
};

// -------------------------------------------------------------------------

CubemapRenderer gCubemapRenderer;

CubemapRenderer::~CubemapRenderer()
{
	UpdateSh4ltAudio();  // removes tap before Sh4ltAudioOutput dtor runs
	for (int i = 0; i < CUBE_FACE_COUNT; i++)
	{
		delete mFaceTex[i];
		mFaceTex[i] = nullptr;
	}
	delete mCrossTex;
	mCrossTex = nullptr;
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

	// Re-init when label changes or writer died.
	if (!mSh4ltVideo.IsRunning() || label != mSh4ltVideoLabel)
	{
		mSh4ltVideo.Init(label, CROSS_W, CROSS_H);
		mSh4ltVideoLabel = label;
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

	// Re-init when label changes or sender died.
	if (!mNdiVideo.IsRunning() || label != mNdiVideoLabel)
	{
		mNdiVideo.Init(label, CROSS_W, CROSS_H);
		mNdiVideoLabel = label;
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

	int stride = 0;
	int dmaFd  = screen->ExportCubemapCrossAsDmaBuf(mCrossTex, &stride);

	if (dmaFd >= 0)
	{
		if (!mPWOutput.InitDmaBuf(dmaFd, CROSS_W, CROSS_H, stride))
		{
			fprintf(stderr, "[cubedoom] PipeWire DMA-BUF stream init failed,"
			                " retrying as CPU\n");
			dmaFd = -1;
		}
	}

	if (dmaFd < 0)
	{
		mPixelBuf.resize((size_t)CROSS_W * CROSS_H * 4);
		if (!mPWOutput.InitCPU(CROSS_W, CROSS_H))
			fprintf(stderr, "[cubedoom] PipeWire CPU stream init failed\n");
	}
}

void CubemapRenderer::Init()
{
	for (int i = 0; i < CUBE_FACE_COUNT; i++)
		mFaceTex[i] = new FCanvasTexture(FACE_SIZE, FACE_SIZE);
	mCrossTex = new FCanvasTexture(CROSS_W, CROSS_H);
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

	// Restore player camera so the game stays unaffected.
	camera->Angles = savedAngles;

	// GPU blit: assemble the 6 face textures into the cross layout.
	screen->CompositeCubemapFaces(mFaceTex, FACE_SIZE, mCrossTex);

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

	const bool needPixels = pwCPU || mSh4ltVideo.IsRunning() || mNdiVideo.IsRunning();
	if (needPixels)
	{
		if (mPixelBuf.empty())
			mPixelBuf.resize((size_t)CROSS_W * CROSS_H * 4);

		screen->ReadCubemapCrossPixels(mCrossTex, mPixelBuf.data(),
		                               CROSS_W, CROSS_H);

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
					const size_t n = (size_t)CROSS_W * CROSS_H * 4;
					size_t nonzero = 0; uint8_t mx = 0;
					// Sample every 64th byte to keep this cheap.
					for (size_t i = 0; i < n; i += 64)
					{
						if (p[i]) { ++nonzero; if (p[i] > mx) mx = p[i]; }
					}
					fprintf(stderr, "[cubedoom/dbg] frame=%d pwCPU=%d pwDMA=%d "
					        "sh4lt=%d ndi=%d  buf: nonzero(sampled)=%zu max=%u\n",
					        dbgFrame, (int)pwCPU, (int)pwDmaBuf,
					        (int)mSh4ltVideo.IsRunning(), (int)mNdiVideo.IsRunning(),
					        nonzero, (unsigned)mx);
				}

				// One-shot: dump the readback buffer to a viewable PPM so the
				// render/readback stage can be inspected without any PipeWire/
				// Sh4lt/NDI receiver. RGBA->RGB, flipped to top-down.
				if (!dbgDumped)
				{
					dbgDumped = true;
					const char* path = "/tmp/cubedoom-readback.ppm";
					FILE* f = fopen(path, "wb");
					if (f)
					{
						fprintf(f, "P6\n%d %d\n255\n", CROSS_W, CROSS_H);
						std::vector<uint8_t> row((size_t)CROSS_W * 3);
						for (int y = 0; y < CROSS_H; ++y)
						{
							const uint8_t* src =
							    mPixelBuf.data() + (size_t)(CROSS_H - 1 - y) * CROSS_W * 4;
							for (int x = 0; x < CROSS_W; ++x)
							{
								row[x * 3 + 0] = src[x * 4 + 0];
								row[x * 3 + 1] = src[x * 4 + 1];
								row[x * 3 + 2] = src[x * 4 + 2];
							}
							fwrite(row.data(), 1, row.size(), f);
						}
						fclose(f);
						fprintf(stderr, "[cubedoom/dbg] wrote %s\n", path);
					}
				}
			}

			if (pwCPU)               mPWOutput.PushFrame(mPixelBuf.data(), CROSS_W * 4);
			if (mSh4ltVideo.IsRunning()) mSh4ltVideo.PushFrame(mPixelBuf.data(), CROSS_W * 4);
			if (mNdiVideo.IsRunning())   mNdiVideo.PushFrame(mPixelBuf.data(), CROSS_W * 4);
		}
	}

	// ---- Sh4lt audio tap ------------------------------------------------
	UpdateSh4ltAudio();
}
