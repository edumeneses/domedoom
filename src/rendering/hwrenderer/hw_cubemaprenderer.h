#pragma once

#include "pw_output.h"
#include "sh4lt_output.h"
#include <vector>
#include <cstdint>
#include <string>

struct player_t;
struct sector_t;
class FCanvasTexture;

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

	~CubemapRenderer();

	// Render all 6 faces offscreen then composite them into the cross texture.
	// Camera angles are temporarily overridden per face and restored on return.
	void RenderFacesToTextures(player_t* player);

	FCanvasTexture* FaceTex(int i)  { return mFaceTex[i]; }
	FCanvasTexture* CrossTex()      { return mCrossTex; }

private:
	void Init();
	void InitPipeWire();       // called once after first composite (if enabled)
	void UpdateSh4ltVideo();   // (re)init sh4lt video writer when label changes
	void UpdateSh4ltAudio();   // install/remove audio tap based on CVAR

	FCanvasTexture*       mFaceTex[CUBE_FACE_COUNT] = {};
	FCanvasTexture*       mCrossTex                 = nullptr;
	bool                  mInitialized              = false;

	// PipeWire output — initialised lazily on first frame.
	PipeWireOutput        mPWOutput;
	std::vector<uint8_t>  mPixelBuf;     // CPU readback staging buffer
	bool                  mPWAttempted   = false;
	bool                  mPBOFirstFrame = true;

	// Sh4lt video output
	Sh4ltVideoOutput      mSh4ltVideo;
	std::string           mSh4ltVideoLabel;   // tracks current label to detect changes

	// Sh4lt audio output (driven via OAL tap)
	Sh4ltAudioOutput      mSh4ltAudio;
	bool                  mAudioTapActive = false;
};

extern CubemapRenderer gCubemapRenderer;
