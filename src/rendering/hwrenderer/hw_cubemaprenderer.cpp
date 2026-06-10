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

#include <cstdio>

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
	for (int i = 0; i < CUBE_FACE_COUNT; i++)
	{
		delete mFaceTex[i];
		mFaceTex[i] = nullptr;
	}
	delete mCrossTex;
	mCrossTex = nullptr;
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

	// First frame after composite: the cross texture now has a GL id.
	// Attempt to open the PipeWire stream (DMA-BUF or CPU fallback).
	if (!mPWAttempted)
		InitPipeWire();

	if (!mPWOutput.IsRunning()) return;

	if (mPWOutput.IsDmaBufMode())
	{
		// Zero-copy path: GL commands are already queued on the GPU.
		// DRM implicit sync ensures the consumer reads after our write
		// completes, so no glFinish() or mutex is needed.
		mPWOutput.QueueDmaBufFrame();
	}
	else
	{
		// CPU path: async PBO readback (1-frame latency, no GPU stall).
		screen->ReadCubemapCrossPixels(mCrossTex, mPixelBuf.data(),
		                               CROSS_W, CROSS_H);
		// Skip the first call: the pong PBO is not yet populated.
		if (mPBOFirstFrame) { mPBOFirstFrame = false; return; }

		mPWOutput.PushFrame(mPixelBuf.data(), CROSS_W * 4);
	}
}
