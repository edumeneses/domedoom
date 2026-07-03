#pragma once

#include "pw_output.h"
#include "sh4lt_output.h"
#include "ndi_output.h"
#include <vector>
#include <cstdint>
#include <string>

struct player_t;
struct sector_t;
class FCanvasTexture;
class F2DDrawer;

// Cubemap face order and horizontal-strip layout:
//  [RIGHT][LEFT][UP][DOWN][FRONT][BACK]
//  col:  0     1    2   3      4     5
//
// Face order matches the standard cubemap convention (+X,-X,+Y,-Y,+Z,-Z)
// used by ossia score / most fulldome toolchains.
enum CubeFaceIndex {
	CUBE_FRONT = 0,
	CUBE_LEFT,
	CUBE_RIGHT,
	CUBE_BACK,
	CUBE_UP,
	CUBE_DOWN,
	CUBE_FACE_COUNT
};

class CubemapRenderer
{
public:
	static constexpr int FACE_SIZE  = 1024;
	static constexpr int CROSS_COLS = 6;
	static constexpr int CROSS_ROWS = 1;
	static constexpr int CROSS_W    = FACE_SIZE * CROSS_COLS; // 6144
	static constexpr int CROSS_H    = FACE_SIZE * CROSS_ROWS; // 1024
	static constexpr int DOME_SIZE  = 2048;                  // square domemaster
	static constexpr int HUD_W      = 2048;                  // rim-HUD source tex
	static constexpr int HUD_H      = 2048;                  // square: avoids vertical squish

	~CubemapRenderer();

	// Free GPU textures and stop streaming outputs. MUST be called during engine
	// shutdown while the renderer (screen) is still alive — otherwise the global
	// gCubemapRenderer destructor frees textures after the GL/Vulkan context is
	// gone, calling into a dead renderer ("pure virtual method called"). Idempotent.
	void Shutdown();

	// Render all 6 faces offscreen. Does NOT composite or stream — call
	// BlitHUDToFrontFace + CompositeAndStream after drawing the HUD.
	void RenderFacesToTextures(player_t* player);

	// Composite the 6 face textures into the cross layout, then push to all
	// streaming outputs (PipeWire, Sh4lt, NDI). Call after BlitHUDToFrontFace.
	void CompositeAndStream();

	// Render the contents of `drawer` (HUD/statusbar) into the front face FBO,
	// compositing on top of the already-rendered 3D scene.
	void BlitHUDToFrontFace(F2DDrawer* drawer);

	// Dispatch HUD handling by output mode: domemaster renders the HUD into a
	// dedicated texture (overlaid as a rim band by RenderDomemaster); the cube
	// strip keeps baking it onto the front face.
	void BlitHUD(F2DDrawer* drawer);

	FCanvasTexture* FaceTex(int i)  { return mFaceTex[i]; }
	FCanvasTexture* CrossTex()      { return mCrossTex; }

private:
	void Init();
	void InitPipeWire();       // called once after first composite (if enabled)
	void UpdateSh4ltVideo();   // (re)init sh4lt video writer when label/dims change
	void UpdateSh4ltAudio();   // install/remove audio tap based on CVAR
	void UpdateNdiVideo();     // (re)init NDI sender when label/dims change

	// Selected output (cubemap strip vs domemaster) — depends on r_cubemap_domemaster.
	int             OutW() const;
	int             OutH() const;
	FCanvasTexture* OutTex();

	FCanvasTexture*       mFaceTex[CUBE_FACE_COUNT] = {};
	FCanvasTexture*       mCrossTex                 = nullptr;
	FCanvasTexture*       mDomeTex                  = nullptr;
	FCanvasTexture*       mHudTex                   = nullptr;
	bool                  mInitialized              = false;

	// Dome yaw lock: when r_cubemap_dome_lock_yaw is set, the cube faces are
	// rendered at a fixed world heading (latched here on enable) instead of
	// following the player. Turning/rotating then moves the player across a
	// static dome image rather than spinning the whole projected world.
	double                mDomeLockYaw   = 0.0;
	bool                  mDomeLockValid = false;

	// PipeWire output — initialised lazily on first frame.
	PipeWireOutput        mPWOutput;
	std::vector<uint8_t>  mPixelBuf;     // CPU readback staging buffer
	bool                  mPWAttempted   = false;
	bool                  mPBOFirstFrame = true;

	// Sh4lt video output
	Sh4ltVideoOutput      mSh4ltVideo;
	std::string           mSh4ltVideoLabel;   // tracks current label to detect changes
	int                   mSh4ltW = 0, mSh4ltH = 0;  // tracks dims to detect format switch

	// Sh4lt audio output (driven via OAL tap)
	Sh4ltAudioOutput      mSh4ltAudio;
	bool                  mAudioTapActive = false;

	// NDI video output
	NdiVideoOutput        mNdiVideo;
	std::string           mNdiVideoLabel;   // tracks current label to detect changes
	int                   mNdiW = 0, mNdiH = 0;  // tracks dims to detect format switch
};

extern CubemapRenderer gCubemapRenderer;
