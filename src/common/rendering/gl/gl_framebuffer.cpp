/*
** gl_framebuffer.cpp
** Implementation of the non-hardware specific parts of the
** OpenGL frame buffer
**
**---------------------------------------------------------------------------
** Copyright 2010-2020 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/ 

#include "gl_system.h"
#include "v_video.h"
#include "m_png.h"

#include "i_time.h"

#include "gl_interface.h"
#include "gl_framebuffer.h"
#include "gl_renderer.h"
#include "gl_renderbuffers.h"
#include "gl_samplers.h"
#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_skydome.h"
#include "hw_viewpointbuffer.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "gl_shaderprogram.h"
#include "gl_debug.h"
#include "r_videoscale.h"
#include "gl_buffers.h"
#include "gl_postprocessstate.h"
#include "v_draw.h"
#include "printf.h"
#include <cstring>
#include <cstdio>
#include "gl_hwtexture.h"

#include "flatvertices.h"
#include "hw_cvars.h"

EXTERN_CVAR (Bool, vid_vsync)
EXTERN_CVAR(Bool, r_cubemap_debug)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Int, gl_pipeline_depth);

void gl_LoadExtensions();
void gl_PrintStartupLog();

extern bool vid_hdr_active;

namespace OpenGLRenderer
{
	FGLRenderer *GLRenderer;

//==========================================================================
//
//
//
//==========================================================================

OpenGLFrameBuffer::OpenGLFrameBuffer(void *hMonitor, bool fullscreen) : 
	Super(hMonitor, fullscreen) 
{
	// SetVSync needs to be at the very top to workaround a bug in Nvidia's OpenGL driver.
	// If wglSwapIntervalEXT is called after glBindFramebuffer in a frame the setting is not changed!
	Super::SetVSync(vid_vsync);
	FHardwareTexture::InitGlobalState();

	// Make sure all global variables tracking OpenGL context state are reset..
	gl_RenderState.Reset();

	GLRenderer = nullptr;
}

OpenGLFrameBuffer::~OpenGLFrameBuffer()
{
	PPResource::ResetAll();

	if (mVertexData != nullptr) delete mVertexData;
	if (mSkyData != nullptr) delete mSkyData;
	if (mViewpoints != nullptr) delete mViewpoints;
	if (mLights != nullptr) delete mLights;
	if (mBones != nullptr) delete mBones;
	mShadowMap.Reset();

	if (GLRenderer)
	{
		delete GLRenderer;
		GLRenderer = nullptr;
	}
}

//==========================================================================
//
// Initializes the GL renderer
//
//==========================================================================

void OpenGLFrameBuffer::InitializeState()
{
	static bool first=true;

	if (first)
	{
		if (ogl_LoadFunctions() == ogl_LOAD_FAILED)
		{
			I_FatalError("Failed to load OpenGL functions.");
		}
	}

	gl_LoadExtensions();

	mPipelineNbr = clamp(*gl_pipeline_depth, 1, HW_MAX_PIPELINE_BUFFERS);
	mPipelineType = gl_pipeline_depth > 0;

	// Move some state to the framebuffer object for easier access.
	hwcaps = gl.flags;
	glslversion = gl.glslversion;
	uniformblockalignment = gl.uniformblockalignment;
	maxuniformblock = gl.maxuniformblock;
	vendorstring = gl.vendorstring;

	if (first)
	{
		first=false;
		gl_PrintStartupLog();
	}

	glDepthFunc(GL_LESS);

	glEnable(GL_DITHER);
	glDisable(GL_CULL_FACE);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glEnable(GL_POLYGON_OFFSET_LINE);
	glEnable(GL_BLEND);
	glEnable(GL_DEPTH_CLAMP);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LINE_SMOOTH);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	SetViewportRects(nullptr);

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), screen->mPipelineNbr);
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer(screen->mPipelineNbr);
	mLights = new FLightBuffer(screen->mPipelineNbr);
	mBones = new BoneBuffer(screen->mPipelineNbr);
	GLRenderer = new FGLRenderer(this);
	GLRenderer->Initialize(GetWidth(), GetHeight());
	static_cast<GLDataBuffer*>(mLights->GetBuffer())->BindBase();
	static_cast<GLDataBuffer*>(mBones->GetBuffer())->BindBase();

	mDebug = std::make_unique<FGLDebug>();
	mDebug->Update();
}

//==========================================================================
//
// Updates the screen
//
//==========================================================================

void OpenGLFrameBuffer::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();
	GLRenderer->Flush();
	Flush3D.Unclock();

	Swap();
	Super::Update();
}

void OpenGLFrameBuffer::CopyScreenToBuffer(int width, int height, uint8_t* scr)
{
	IntRect bounds;
	bounds.left = 0;
	bounds.top = 0;
	bounds.width = width;
	bounds.height = height;
	GLRenderer->CopyToBackbuffer(&bounds, false);

	// strictly speaking not needed as the glReadPixels should block until the scene is rendered, but this is to safeguard against shitty drivers
	glFinish();
	glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, scr);
}

//===========================================================================
//
// Camera texture rendering
//
//===========================================================================

void OpenGLFrameBuffer::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	GLRenderer->StartOffscreen();
	GLRenderer->BindToFrameBuffer(tex);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = FHardwareTexture::GetTexDimension(tex->GetWidth());
	bounds.height = FHardwareTexture::GetTexDimension(tex->GetHeight());

	renderFunc(bounds);
	GLRenderer->EndOffscreen();

	tex->SetUpdated(true);
	static_cast<OpenGLFrameBuffer*>(screen)->camtexcount++;
}

//===========================================================================
//
// CompositeCubemapFaces
//
// Blits 6 pre-rendered face textures into a horizontal strip inside crossTex
// using glBlitFramebuffer (pure GPU blit, no CPU readback).
//
// Output layout (6144 × 1024):
//   [RIGHT][LEFT][UP][DOWN][FRONT][BACK]
//   col:  0     1    2    3     4     5
//
// Single row → no FB row inversion needed.
//
//===========================================================================

void OpenGLFrameBuffer::CompositeCubemapFaces(FCanvasTexture** faces, int N, FCanvasTexture* crossTex)
{
	// Lazily create the two helper FBOs (read + draw). They persist for the
	// lifetime of the GL context and are harmless to leave allocated.
	static GLuint sReadFBO = 0, sDrawFBO = 0;
	if (!sReadFBO)
	{
		glGenFramebuffers(1, &sReadFBO);
		glGenFramebuffers(1, &sDrawFBO);
	}

	// Ensure the cross GPU texture exists at the right dimensions.
	auto* crossHW = static_cast<FHardwareTexture*>(crossTex->GetHardwareTexture(0, 0));
	crossHW->BindOrCreate(crossTex, 0, 0, 0, 0);
	FHardwareTexture::Unbind(0);

	// Attach cross texture to the draw FBO.
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sDrawFBO);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, crossHW->GetTextureHandle(), 0);

	// glBlitFramebuffer is subject to the scissor test. The scene render
	// (Set3DViewport) leaves the scissor enabled and clamped to a single
	// face-sized viewport, which would clip every blit but the one landing
	// in that region — only one cube face would reach the cross. Disable
	// the scissor for the blits and restore the exact GL state afterwards so
	// gl_RenderState's cached scissor state stays in sync.
	const GLboolean savedScissor = glIsEnabled(GL_SCISSOR_TEST);
	GLint savedScissorBox[4];
	glGetIntegerv(GL_SCISSOR_BOX, savedScissorBox);
	glDisable(GL_SCISSOR_TEST);

	// Horizontal strip layout: [RIGHT][LEFT][UP][DOWN][FRONT][BACK]
	//   col:                      0      1    2    3      4     5
	// Indexed by CubeFaceIndex (FRONT=0, LEFT=1, RIGHT=2, BACK=3, UP=4, DOWN=5).
	// Single row → all fbY = 0.
	static constexpr int kFBX[] = { 4, 1, 0, 5, 2, 3 }; // col per face
	static constexpr int kFBY[] = { 0, 0, 0, 0, 0, 0 }; // all on row 0

	for (int i = 0; i < 6; i++)
	{
		auto* faceHW = static_cast<FHardwareTexture*>(faces[i]->GetHardwareTexture(0, 0));
		GLuint faceTex = faceHW->GetTextureHandle();
		if (!faceTex) continue;

		glBindFramebuffer(GL_READ_FRAMEBUFFER, sReadFBO);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, faceTex, 0);

		if (r_cubemap_debug)
		{
			static int dbgN = 0;
			if ((dbgN % 120) == 0)
			{
				GLenum st = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
				uint8_t px[4] = {0,0,0,0};
				glReadPixels(N / 2, N / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
				fprintf(stderr, "[cubedoom/dbg] face %d tex=%u fbo=%s "
				        "centerRGBA=%u,%u,%u,%u\n",
				        i, faceTex,
				        st == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE",
				        px[0], px[1], px[2], px[3]);
			}
			if (i == 5) ++dbgN;
		}

		const int dx = kFBX[i] * N;
		const int dy = kFBY[i] * N;
		glBlitFramebuffer(0, 0, N, N,
		                  dx, dy, dx + N, dy + N,
		                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	// Restore the scissor state exactly as it was.
	glScissor(savedScissorBox[0], savedScissorBox[1],
	          savedScissorBox[2], savedScissorBox[3]);
	if (savedScissor) glEnable(GL_SCISSOR_TEST);

	crossTex->SetUpdated(true);

	// Restore to default so we don't leave dangling FBO state.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

//===========================================================================
//
// RenderDomemaster
//
// Warps the 6 face textures into a square fisheye domemaster via a single
// fullscreen pass. Samples the 6 face textures directly (no cube map) using
// the same projection math as the ossia score domemaster ISF, so the result
// matches the proven shader. invRot is a column-major 3x3 inverse content
// rotation built CPU-side, so the fragment shader does no trig per pixel.
//
//===========================================================================

static const char* kDomeVS = R"GLSL(
#version 330 core
out vec2 vUV;
void main() {
    // id 0->(0,0) 1->(2,0) 2->(0,2): attrib-less fullscreen triangle.
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

static const char* kDomeFS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D facePosX, faceNegX, facePosY, faceNegY, facePosZ, faceNegZ;
uniform mat3  uInvRot;
uniform float uHalfFovRad;
uniform vec2  uFlip;        // per-axis output flip: (+1 or -1, +1 or -1)
uniform sampler2D uHud;     // full 2D HUD; bottom strip = status bar
uniform float uHudEnable;   // >0.5 = composite rim HUD
uniform vec4  uHudParams;   // x=halfArcRad y=band z=strip w=chroma(1/0)
#define UV(c) (((c) * vec2(1.0, 1.0) + 1.0) * 0.5)
void main() {
    vec2 p = (vUV * 2.0 - 1.0) * uFlip;
    float r = length(p);
    if (r > 1.0) { FragColor = vec4(0.0); return; }
    vec2 az = (r > 1e-6) ? p / r : vec2(0.0);
    float polar = r * uHalfFovRad;
    float sp = sin(polar);
    vec3 d = uInvRot * vec3(sp * az.x, sp * az.y, cos(polar));
    vec3 a = abs(d); vec2 sc;
    if (a.x >= a.y && a.x >= a.z) {
        if (d.x > 0.0) { sc = vec2(-d.z,-d.y)/a.x; FragColor = texture(facePosX, UV(sc)); }
        else           { sc = vec2( d.z,-d.y)/a.x; FragColor = texture(faceNegX, UV(sc)); }
    } else if (a.y >= a.z) {
        if (d.y > 0.0) { sc = vec2( d.x, d.z)/a.y; FragColor = texture(facePosY, UV(sc)); }
        else           { sc = vec2( d.x,-d.z)/a.y; FragColor = texture(faceNegY, UV(sc)); }
    } else {
        if (d.z > 0.0) { sc = vec2( d.x,-d.y)/a.z; FragColor = texture(facePosZ, UV(sc)); }
        else           { sc = vec2(-d.x,-d.y)/a.z; FragColor = texture(faceNegZ, UV(sc)); }
    }

    // Rim HUD band along the front (bottom of the flipped output).
    if (uHudEnable > 0.5) {
        float ang = atan(p.y, p.x);
        float dd  = mod(ang - (-1.57079633) + 3.14159265, 6.28318531) - 3.14159265;
        float half = uHudParams.x, band = uHudParams.y;
        if (r >= 1.0 - band && abs(dd) <= half) {
            float u  = dd / half * 0.5 + 0.5;
            float vv = (r - (1.0 - band)) / band;       // 0 inner .. 1 rim
            vec4 h = texture(uHud, vec2(u, vv * uHudParams.z));
            bool keyed = (uHudParams.w > 0.5) &&
                         (h.g > h.r * 1.15 && h.g > h.b * 1.15 && h.g > 0.2);
            if (h.a > 0.01 && !keyed) FragColor = vec4(h.rgb, 1.0);
        }
    }
}
)GLSL";

static GLuint CompileDomeShader(GLenum type, const char* src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		fprintf(stderr, "[cubedoom] dome shader compile failed: %s\n", log);
	}
	return s;
}

void OpenGLFrameBuffer::RenderDomemaster(FCanvasTexture** faces, int N,
                                         FCanvasTexture* domeTex, int domeSize,
                                         const DomemasterParams& params)
{
	static GLuint sFBO = 0, sProg = 0, sVAO = 0;
	static GLint  uInvRot = -1, uHalfFov = -1, uFlip = -1, uHudEnable = -1, uHudParams = -1;
	if (!sFBO)
	{
		glGenFramebuffers(1, &sFBO);
		glGenVertexArrays(1, &sVAO);               // empty VAO for attrib-less draw
		GLuint vs = CompileDomeShader(GL_VERTEX_SHADER,   kDomeVS);
		GLuint fs = CompileDomeShader(GL_FRAGMENT_SHADER, kDomeFS);
		sProg = glCreateProgram();
		glAttachShader(sProg, vs); glAttachShader(sProg, fs);
		glLinkProgram(sProg);
		GLint linked = 0; glGetProgramiv(sProg, GL_LINK_STATUS, &linked);
		if (!linked)
		{
			char log[1024]; glGetProgramInfoLog(sProg, sizeof(log), nullptr, log);
			fprintf(stderr, "[cubedoom] dome shader link failed: %s\n", log);
		}
		glDeleteShader(vs); glDeleteShader(fs);
		glUseProgram(sProg);
		const char* names[6] = {"facePosX","faceNegX","facePosY","faceNegY","facePosZ","faceNegZ"};
		for (int i = 0; i < 6; i++) glUniform1i(glGetUniformLocation(sProg, names[i]), i);
		glUniform1i(glGetUniformLocation(sProg, "uHud"), 6);
		uInvRot    = glGetUniformLocation(sProg, "uInvRot");
		uHalfFov   = glGetUniformLocation(sProg, "uHalfFovRad");
		uFlip      = glGetUniformLocation(sProg, "uFlip");
		uHudEnable = glGetUniformLocation(sProg, "uHudEnable");
		uHudParams = glGetUniformLocation(sProg, "uHudParams");
		glUseProgram(0);
	}

	auto* domeHW = static_cast<FHardwareTexture*>(domeTex->GetHardwareTexture(0, 0));
	domeHW->BindOrCreate(domeTex, 0, 0, 0, 0);
	FHardwareTexture::Unbind(0);

	glBindFramebuffer(GL_FRAMEBUFFER, sFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, domeHW->GetTextureHandle(), 0);

	// Save the GL state we touch (mirror CompositeCubemapFaces discipline).
	// Stencil test (portal clipping) and face culling are left enabled by the
	// scene render; a glBlitFramebuffer ignores both but a shader draw does not,
	// so a failing stencil func or a culled triangle would render all black.
	const GLboolean savedScissor = glIsEnabled(GL_SCISSOR_TEST);
	const GLboolean savedDepth   = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean savedBlend   = glIsEnabled(GL_BLEND);
	const GLboolean savedStencil = glIsEnabled(GL_STENCIL_TEST);
	const GLboolean savedCull    = glIsEnabled(GL_CULL_FACE);
	GLint savedVP[4]; glGetIntegerv(GL_VIEWPORT, savedVP);
	glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);

	glViewport(0, 0, domeSize, domeSize);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	if (r_cubemap_debug)
	{
		static int dbgN = 0;
		if ((dbgN++ % 120) == 0)
		{
			GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			fprintf(stderr, "[cubedoom/dbg] dome FBO %s, %dx%d\n",
			        st == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE",
			        domeSize, domeSize);
		}
	}

	// CubeFaceIndex {FRONT,LEFT,RIGHT,BACK,UP,DOWN} -> sampler unit
	// {posX(R)=0,negX(L)=1,posY(U)=2,negY(D)=3,posZ(F)=4,negZ(B)=5}.
	// Same permutation as kFBX in CompositeCubemapFaces.
	static const int kUnitForFace[6] = { 4, 1, 0, 5, 2, 3 };
	for (int f = 0; f < 6; f++)
	{
		auto* hw = static_cast<FHardwareTexture*>(faces[f]->GetHardwareTexture(0, 0));
		glActiveTexture(GL_TEXTURE0 + kUnitForFace[f]);
		// Unbind any sampler object the engine left on this unit — those expect
		// mipmaps and would make the (mip-less) face texture incomplete -> black.
		glBindSampler(kUnitForFace[f], 0);
		glBindTexture(GL_TEXTURE_2D, hw->GetTextureHandle());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	// HUD texture on unit 6 (for the rim band).
	const bool hudOn = params.hudTex != nullptr;
	if (hudOn)
	{
		auto* hudHW = static_cast<FHardwareTexture*>(params.hudTex->GetHardwareTexture(0, 0));
		glActiveTexture(GL_TEXTURE0 + 6);
		glBindSampler(6, 0);
		glBindTexture(GL_TEXTURE_2D, hudHW->GetTextureHandle());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	glUseProgram(sProg);
	glUniformMatrix3fv(uInvRot, 1, GL_FALSE, params.invRot);
	glUniform1f(uHalfFov, params.fovDeg * (3.14159265359f / 360.0f));
	glUniform2f(uFlip, params.flipH ? -1.0f : 1.0f, params.flipV ? -1.0f : 1.0f);
	glUniform1f(uHudEnable, hudOn ? 1.0f : 0.0f);
	glUniform4f(uHudParams, params.hudArcDeg * (3.14159265359f / 360.0f),
	            params.hudBand, params.hudStrip, params.hudChroma ? 1.0f : 0.0f);

	glBindVertexArray(sVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glUseProgram(0);

	if (r_cubemap_debug)
	{
		static int dbgN2 = 0;
		if ((dbgN2++ % 120) == 0)
		{
			GLenum err = glGetError();
			uint8_t c[4] = {0,0,0,0};
			glReadPixels(domeSize/2, domeSize/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
			GLint curProg = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &curProg);
			fprintf(stderr, "[cubedoom/dbg] dome draw: prog=%u glErr=0x%x "
			        "centerRGBA=%u,%u,%u,%u\n",
			        sProg, (unsigned)err, c[0], c[1], c[2], c[3]);
		}
	}

	// Restore the GL state we changed.
	glActiveTexture(GL_TEXTURE0);
	glViewport(savedVP[0], savedVP[1], savedVP[2], savedVP[3]);
	if (savedScissor) glEnable(GL_SCISSOR_TEST);
	if (savedDepth)   glEnable(GL_DEPTH_TEST);
	if (savedBlend)   glEnable(GL_BLEND);
	if (savedStencil) glEnable(GL_STENCIL_TEST);
	if (savedCull)    glEnable(GL_CULL_FACE);

	domeTex->SetUpdated(true);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

//===========================================================================
//
// ReadCubemapCrossPixels
//
// Async GPU→CPU readback using double-PBO (Pixel Buffer Objects).
//
// Frame N   : issue glGetTexImage into PBO[ping]  (non-blocking, queued on GPU)
// Frame N   : map  PBO[pong] and memcpy into buf  (pong was filled in frame N-1)
// Frame N+1 : swap ping/pong and repeat
//
// First call returns without filling buf (pong not yet populated). The caller
// (CubemapRenderer) skips PipeWire push on the first frame; subsequent frames
// arrive with 1-frame latency and zero GPU stall.
//
// Row order: GL stores textures bottom-up; we preserve that order here.
// PipeWireOutput::PushFrame() performs the top-down flip for consumers.
//
//===========================================================================

void OpenGLFrameBuffer::ReadCubemapCrossPixels(FCanvasTexture* crossTex,
                                               uint8_t* buf, int w, int h)
{
	static GLuint sPBOs[2]    = {};
	static int    sPing       = 0;
	static bool   sFirstCall  = true;

	const size_t pixelBytes = (size_t)w * h * 4;

	// Lazy PBO creation.
	if (!sPBOs[0])
	{
		glGenBuffers(2, sPBOs);
		for (int i = 0; i < 2; i++)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, sPBOs[i]);
			glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)pixelBytes,
			             nullptr, GL_STREAM_READ);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}

	auto* crossHW = static_cast<FHardwareTexture*>(crossTex->GetHardwareTexture(0, 0));
	GLuint texId  = crossHW->GetTextureHandle();
	if (!texId) return;

	const int pong = 1 - sPing;

	// Issue async read from cross texture into the ping PBO.
	// With GL_PIXEL_PACK_BUFFER bound, the pointer argument is treated as a
	// byte offset into the PBO — 0 means "write at the start of the buffer".
	glBindBuffer(GL_PIXEL_PACK_BUFFER, sPBOs[sPing]);
	{
		GLint prevTex = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
		glBindTexture(GL_TEXTURE_2D, texId);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
		              reinterpret_cast<void*>(0));
		glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
	}

	// Map the pong PBO (filled in the previous frame) and copy out.
	if (!sFirstCall)
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, sPBOs[pong]);
		void* ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
		if (ptr)
		{
			std::memcpy(buf, ptr, pixelBytes);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	sPing      = pong;
	sFirstCall = false;
}

//===========================================================================
//
// ExportCubemapCrossAsDmaBuf
//
// Exports the cross texture as a DMA-BUF fd for zero-copy PipeWire delivery.
//
// Requires:
//   • An active EGL context  (eglGetCurrentDisplay() returns non-null)
//   • EGL_KHR_image_base      (eglCreateImageKHR / eglDestroyImageKHR)
//   • EGL_KHR_gl_texture_2D_image (EGL_GL_TEXTURE_2D_KHR target)
//   • EGL_MESA_image_dma_buf_export
//
// All function pointers are resolved at runtime via eglGetProcAddress so that
// the build has no compile-time or link-time dependency on specific EGL
// extension headers.
//
// Returns a valid file descriptor (>= 0) on success; the caller owns it and
// must not close it while the PipeWire stream is active.  Returns -1 if any
// requirement is not met (GLX context, missing extension, export failure).
//
//===========================================================================

#include <dlfcn.h>

int OpenGLFrameBuffer::ExportCubemapCrossAsDmaBuf(FCanvasTexture* crossTex,
                                                   int* outStride)
{
	// EGL opaque types — we avoid including EGL headers to keep the build
	// portable between EGL and GLX configurations.
	using EGLDisplay    = void*;
	using EGLContext    = void*;
	using EGLImageKHR   = void*;
	using EGLClientBuffer = void*;
	using EGLenum       = unsigned int;
	using EGLBoolean    = unsigned int;
	using EGLint        = int;
	using EGLuint64KHR  = uint64_t;

	constexpr EGLDisplay EGL_NO_DISPLAY_  = nullptr;
	constexpr EGLContext EGL_NO_CONTEXT_  = nullptr;
	constexpr EGLenum    EGL_GL_TEXTURE_2D_KHR_ = 0x30B1u;

	// Runtime-resolve core EGL functions via dlsym (no link-time dependency).
	using FnGetDisplay = EGLDisplay (*)();
	using FnGetContext = EGLContext (*)();
	using FnGetProc    = void* (*)(const char*);

	auto getDisplayFn = (FnGetDisplay)dlsym(RTLD_DEFAULT, "eglGetCurrentDisplay");
	auto getContextFn = (EGLContext (*)())dlsym(RTLD_DEFAULT, "eglGetCurrentContext");
	auto getProcFn    = (FnGetProc)dlsym(RTLD_DEFAULT, "eglGetProcAddress");

	if (!getDisplayFn || !getContextFn || !getProcFn) return -1;

	EGLDisplay dpy = getDisplayFn();
	EGLContext  ctx = getContextFn();
	if (dpy == EGL_NO_DISPLAY_ || ctx == EGL_NO_CONTEXT_) return -1;

	// Resolve extension entry points.
	using FnCreateImage  = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum,
	                                       EGLClientBuffer, const EGLint*);
	using FnDestroyImage = EGLBoolean  (*)(EGLDisplay, EGLImageKHR);
	using FnDmaBufQuery  = EGLBoolean  (*)(EGLDisplay, EGLImageKHR,
	                                       int*, int*, EGLuint64KHR*);
	using FnDmaBufExport = EGLBoolean  (*)(EGLDisplay, EGLImageKHR,
	                                       int*, EGLint*, EGLint*);

	auto createImage  = (FnCreateImage) getProcFn("eglCreateImageKHR");
	auto destroyImage = (FnDestroyImage)getProcFn("eglDestroyImageKHR");
	auto dmaBufQuery  = (FnDmaBufQuery) getProcFn("eglExportDMABUFImageQueryMESA");
	auto dmaBufExport = (FnDmaBufExport)getProcFn("eglExportDMABUFImageMESA");

	if (!createImage || !destroyImage || !dmaBufQuery || !dmaBufExport)
	{
		fprintf(stderr, "[cubedoom/egl] DMA-BUF export extensions not available"
		                " — falling back to CPU readback\n");
		return -1;
	}

	// Ensure the cross texture has a backing GL id.
	auto* crossHW = static_cast<FHardwareTexture*>(crossTex->GetHardwareTexture(0, 0));
	GLuint texId  = crossHW->GetTextureHandle();
	if (!texId)
	{
		fprintf(stderr, "[cubedoom/egl] cross texture has no GL id yet\n");
		return -1;
	}

	// Wrap the GL texture in an EGLImage.
	EGLImageKHR img = createImage(dpy, ctx, EGL_GL_TEXTURE_2D_KHR_,
	                              reinterpret_cast<EGLClientBuffer>(
	                                  static_cast<uintptr_t>(texId)),
	                              nullptr);
	if (!img)
	{
		fprintf(stderr, "[cubedoom/egl] eglCreateImageKHR failed\n");
		return -1;
	}

	// Query plane count: we only handle single-plane (RGBA) textures.
	int fourcc = 0, nplanes = 0;
	EGLuint64KHR modifier = 0;
	if (!dmaBufQuery(dpy, img, &fourcc, &nplanes, &modifier) || nplanes != 1)
	{
		fprintf(stderr, "[cubedoom/egl] DMA-BUF query failed or nplanes=%d\n", nplanes);
		destroyImage(dpy, img);
		return -1;
	}

	// Export the DRM buffer fd, stride, and offset.
	int  fd     = -1;
	int  stride =  0;
	int  offset =  0;
	if (!dmaBufExport(dpy, img, &fd, &stride, &offset))
	{
		fprintf(stderr, "[cubedoom/egl] eglExportDMABUFImageMESA failed\n");
		destroyImage(dpy, img);
		return -1;
	}

	// Destroy the EGLImage wrapper — the DRM bo and the fd remain valid as
	// long as the GL texture exists (which is the whole game session).
	destroyImage(dpy, img);

	fprintf(stderr, "[cubedoom/egl] exported cross texture as DMA-BUF"
	                " fd=%d stride=%d offset=%d fourcc=0x%x\n",
	        fd, stride, offset, (unsigned)fourcc);

	*outStride = stride;
	return fd;
}

//===========================================================================
//
//
//
//===========================================================================

const char* OpenGLFrameBuffer::DeviceName() const 
{
	return gl.modelstring;
}

//==========================================================================
//
// Swap the buffers
//
//==========================================================================

CVAR(Bool, gl_finishbeforeswap, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);

void OpenGLFrameBuffer::Swap()
{
	bool swapbefore = gl_finishbeforeswap && camtexcount == 0;
	Finish.Reset();
	Finish.Clock();
	if (gl_pipeline_depth < 1)
	{
		if (swapbefore) glFinish();
		FPSLimit();
		SwapBuffers();
		if (!swapbefore) glFinish();
	}
	else
	{
		mVertexData->DropSync();

		FPSLimit();
		SwapBuffers();

		mVertexData->NextPipelineBuffer();
		mVertexData->WaitSync();

		RenderState()->SetVertexBuffer(screen->mVertexData); // Needed for Raze because it does not reset it
	}
	Finish.Unclock();
	camtexcount = 0;
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
	mDebug->Update();
}

//==========================================================================
//
// Enable/disable vertical sync
//
//==========================================================================

void OpenGLFrameBuffer::SetVSync(bool vsync)
{
	// Switch to the default frame buffer because some drivers associate the vsync state with the bound FB object.
	GLint oldDrawFramebufferBinding = 0, oldReadFramebufferBinding = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebufferBinding);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebufferBinding);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	Super::SetVSync(vsync);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFramebufferBinding);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFramebufferBinding);
}

//===========================================================================
//
//
//===========================================================================

void OpenGLFrameBuffer::SetTextureFilterMode()
{
	if (GLRenderer != nullptr && GLRenderer->mSamplerManager != nullptr) GLRenderer->mSamplerManager->SetTextureFilterMode();
}

IHardwareTexture *OpenGLFrameBuffer::CreateHardwareTexture(int numchannels) 
{ 
	return new FHardwareTexture(numchannels);
}

void OpenGLFrameBuffer::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	int numLayers = mat->NumLayers();
	MaterialLayerInfo* layer;
	auto base = static_cast<FHardwareTexture*>(mat->GetLayer(0, translation, &layer));

	if (base->BindOrCreate(layer->layerTexture, 0, CLAMP_NONE, translation, layer->scaleFlags))
	{
		for (int i = 1; i < numLayers; i++)
		{
			auto systex = static_cast<FHardwareTexture*>(mat->GetLayer(i, 0, &layer));
			systex->BindOrCreate(layer->layerTexture, i, CLAMP_NONE, 0, layer->scaleFlags);
		}
	}
	// unbind everything. 
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
}

IVertexBuffer *OpenGLFrameBuffer::CreateVertexBuffer()
{ 
	return new GLVertexBuffer; 
}

IIndexBuffer *OpenGLFrameBuffer::CreateIndexBuffer()
{ 
	return new GLIndexBuffer; 
}

IDataBuffer *OpenGLFrameBuffer::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return new GLDataBuffer(bindingpoint, ssbo);
}

void OpenGLFrameBuffer::BlurScene(float amount)
{
	GLRenderer->BlurScene(amount);
}

void OpenGLFrameBuffer::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GLint activeTex = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
		glActiveTexture(GL_TEXTURE0 + 17);

		if (GLRenderer->mLightMapID == 0)
			glGenTextures(1, (GLuint*)&GLRenderer->mLightMapID);

		glBindTexture(GL_TEXTURE_2D_ARRAY, GLRenderer->mLightMapID);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB16F, LMTextureSize, LMTextureSize, LMTextureCount, 0, GL_RGB, GL_HALF_FLOAT, &LMTextureData[0]);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

		glActiveTexture(activeTex);

		LMTextureData.Reset(); // We no longer need this, release the memory
	}
}

void OpenGLFrameBuffer::SetViewportRects(IntRect *bounds)
{
	Super::SetViewportRects(bounds);
	if (!bounds)
	{
		auto vrmode = VRMode::GetVRMode(true);
		vrmode->AdjustViewport(this);
	}
}

void OpenGLFrameBuffer::UpdatePalette()
{
	if (GLRenderer)
		GLRenderer->ClearTonemapPalette();
}

FRenderState* OpenGLFrameBuffer::RenderState()
{
	return &gl_RenderState;
}

void OpenGLFrameBuffer::AmbientOccludeScene(float m5)
{
	gl_RenderState.EnableDrawBuffers(1);
	GLRenderer->AmbientOccludeScene(m5);
	glViewport(screen->mSceneViewport.left, mSceneViewport.top, mSceneViewport.width, mSceneViewport.height);
	GLRenderer->mBuffers->BindSceneFB(true);
	gl_RenderState.EnableDrawBuffers(gl_RenderState.GetPassDrawBufferCount());
	gl_RenderState.Apply();
}

void OpenGLFrameBuffer::FirstEye()
{
	GLRenderer->mBuffers->CurrentEye() = 0;  // always begin at zero, in case eye count changed
}

void OpenGLFrameBuffer::NextEye(int eyecount)
{
	GLRenderer->mBuffers->NextEye(eyecount);
}

void OpenGLFrameBuffer::SetSceneRenderTarget(bool useSSAO)
{
	GLRenderer->mBuffers->BindSceneFB(useSSAO);
}

void OpenGLFrameBuffer::UpdateShadowMap()
{
	if (mShadowMap.PerformUpdate())
	{
		FGLDebug::PushGroup("ShadowMap");

		FGLPostProcessState savedState;

		static_cast<GLDataBuffer*>(screen->mShadowMap.mLightList)->BindBase();
		static_cast<GLDataBuffer*>(screen->mShadowMap.mNodesBuffer)->BindBase();
		static_cast<GLDataBuffer*>(screen->mShadowMap.mLinesBuffer)->BindBase();

		GLRenderer->mBuffers->BindShadowMapFB();

		GLRenderer->mShadowMapShader->Bind();
		GLRenderer->mShadowMapShader->Uniforms->ShadowmapQuality = gl_shadowmap_quality;
		GLRenderer->mShadowMapShader->Uniforms->NodesCount = screen->mShadowMap.NodesCount();
		GLRenderer->mShadowMapShader->Uniforms.SetData();
		static_cast<GLDataBuffer*>(GLRenderer->mShadowMapShader->Uniforms.GetBuffer())->BindBase();

		glViewport(0, 0, gl_shadowmap_quality, 1024);
		GLRenderer->RenderScreenQuad();

		const auto& viewport = screen->mScreenViewport;
		glViewport(viewport.left, viewport.top, viewport.width, viewport.height);

		GLRenderer->mBuffers->BindShadowMapTexture(16);
		FGLDebug::PopGroup();
		screen->mShadowMap.FinishUpdate();
	}
}

void OpenGLFrameBuffer::WaitForCommands(bool finish)
{
	glFinish();
}

void OpenGLFrameBuffer::SetSaveBuffers(bool yes)
{
	if (!GLRenderer) return;
	if (yes) GLRenderer->mBuffers = GLRenderer->mSaveBuffers;
	else GLRenderer->mBuffers = GLRenderer->mScreenBuffers;
}

//===========================================================================
//
// 
//
//===========================================================================

void OpenGLFrameBuffer::BeginFrame()
{
	SetViewportRects(nullptr);
	mViewpoints->Clear();
	if (GLRenderer != nullptr)
		GLRenderer->BeginFrame();
}

//===========================================================================
// 
//	Takes a screenshot
//
//===========================================================================

TArray<uint8_t> OpenGLFrameBuffer::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	const auto &viewport = mOutputLetterbox;

	// Grab what is in the back buffer.
	// We cannot rely on SCREENWIDTH/HEIGHT here because the output may have been scaled.
	TArray<uint8_t> pixels;
	pixels.Resize(viewport.width * viewport.height * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(viewport.left, viewport.top, viewport.width, viewport.height, GL_RGB, GL_UNSIGNED_BYTE, &pixels[0]);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);

	// Copy to screenshot buffer:
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);

	float rcpWidth = 1.0f / w;
	float rcpHeight = 1.0f / h;
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			float u = (x + 0.5f) * rcpWidth;
			float v = (y + 0.5f) * rcpHeight;
			int sx = u * viewport.width;
			int sy = v * viewport.height;
			int sindex = (sx + sy * viewport.width) * 3;
			int dindex = (x + (h - y - 1) * w) * 3;
			ScreenshotBuffer[dindex] = pixels[sindex];
			ScreenshotBuffer[dindex + 1] = pixels[sindex + 1];
			ScreenshotBuffer[dindex + 2] = pixels[sindex + 2];
		}
	}

	pitch = w * 3;
	color_type = SS_RGB;

	// Screenshot should not use gamma correction if it was already applied to rendered image
	gamma = 1;
	if (vid_hdr_active && vid_fullscreen)
		gamma *= 2.2f;
	return ScreenshotBuffer;
}

//===========================================================================
// 
// 2D drawing
//
//===========================================================================

void OpenGLFrameBuffer::Draw2D()
{
	if (GLRenderer != nullptr)
	{
		GLRenderer->mBuffers->BindCurrentFB();
		::Draw2D(twod, gl_RenderState);
	}
}

void OpenGLFrameBuffer::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) GLRenderer->mBuffers->BlitSceneToTexture(); // Copy the resulting scene to the current post process texture
	GLRenderer->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

bool OpenGLFrameBuffer::CompileNextShader()
{
	return GLRenderer->mShaderManager->CompileNextShader();
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeStartScreen
//
// Called before the current screen has started rendering. This needs to
// save what was drawn the previous frame so that it can be animated into
// what gets drawn this frame.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeStartScreen()
{
	const auto &viewport = screen->mScreenViewport;

	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(nullptr, viewport.width, viewport.height, 0, false, "WipeStartScreen");
	glFinish();
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);

	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeEndScreen
//
// The screen we want to animate to has just been drawn.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeEndScreen()
{
	GLRenderer->Flush();
	const auto &viewport = screen->mScreenViewport;
	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(NULL, viewport.width, viewport.height, 0, false, "WipeEndScreen");
	glFinish();
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);
	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

}
