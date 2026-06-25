/*
**  Vulkan backend
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include <zvulkan/vulkanobjects.h>

#include <inttypes.h>

#include "v_video.h"
#include "m_png.h"

#include "r_videoscale.h"
#include "i_time.h"
#include "v_text.h"
#include "version.h"
#include "v_draw.h"

#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_cvars.h"
#include "hw_skydome.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"

#include "vk_renderdevice.h"
#include "vk_hwbuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/renderer/vk_streambuffer.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/renderer/vk_raytrace.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_imagetransition.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/textures/vk_framebuffer.h"
#include <zvulkan/vulkanswapchain.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkansurface.h>
#include <zvulkan/vulkancompatibledevice.h>
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_buffer.h"
#include "engineerrors.h"
#include "c_dispatch.h"

FString JitCaptureStackTrace(int framesToSkip, bool includeNativeFrames, int maxFrames = -1);

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)

CVAR(Bool, vk_raytrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Physical device info
static std::vector<VulkanCompatibleDevice> SupportedDevices;
int vkversion;

CUSTOM_CVAR(Bool, vk_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CVAR(Bool, vk_debug_callstack, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, vk_device, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vk_listdevices)
{
	for (size_t i = 0; i < SupportedDevices.size(); i++)
	{
		Printf("#%d - %s\n", (int)i, SupportedDevices[i].Device->Properties.Properties.deviceName);
	}
}

void VulkanError(const char* text)
{
	throw CVulkanError(text);
}

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	bool showcallstack = strstr(typestr, "error") != nullptr;

	if (showcallstack)
		Printf("\n");

	Printf(TEXTCOLOR_RED "[%s] ", typestr);
	Printf(TEXTCOLOR_WHITE "%s\n", msg.c_str());

	if (vk_debug_callstack && showcallstack)
	{
		FString callstack = JitCaptureStackTrace(0, true, 5);
		if (!callstack.IsEmpty())
			Printf("%s\n", callstack.GetChars());
	}
}

VulkanRenderDevice::VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanSurface> surface) :
	Super(hMonitor, fullscreen) 
{
	VulkanDeviceBuilder builder;
	builder.OptionalRayQuery();
	builder.Surface(surface);
	builder.SelectDevice(vk_device);
	SupportedDevices = builder.FindDevices(surface->Instance);
	device = builder.Create(surface->Instance);
}

VulkanRenderDevice::~VulkanRenderDevice()
{
	vkDeviceWaitIdle(device->device); // make sure the GPU is no longer using any objects before RAII tears them down

	delete mVertexData;
	delete mSkyData;
	delete mViewpoints;
	delete mLights;
	delete mBones;
	mShadowMap.Reset();

	if (mDescriptorSetManager)
		mDescriptorSetManager->Deinit();
	if (mTextureManager)
		mTextureManager->Deinit();
	if (mBufferManager)
		mBufferManager->Deinit();
	if (mShaderManager)
		mShaderManager->Deinit();

	mCommands->DeleteFrameObjects();
}

void VulkanRenderDevice::InitializeState()
{
	static bool first = true;
	if (first)
	{
		PrintStartupLog();
		first = false;
	}

	// Use the same names here as OpenGL returns.
	switch (device->PhysicalDevice.Properties.Properties.vendorID)
	{
	case 0x1002: vendorstring = "ATI Technologies Inc.";     break;
	case 0x10DE: vendorstring = "NVIDIA Corporation";  break;
	case 0x8086: vendorstring = "Intel";   break;
	default:     vendorstring = "Unknown"; break;
	}

	hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
	glslversion = 4.50f;
	uniformblockalignment = (unsigned int)device->PhysicalDevice.Properties.Properties.limits.minUniformBufferOffsetAlignment;
	maxuniformblock = device->PhysicalDevice.Properties.Properties.limits.maxUniformBufferRange;

	mCommands.reset(new VkCommandBufferManager(this));

	mSamplerManager.reset(new VkSamplerManager(this));
	mTextureManager.reset(new VkTextureManager(this));
	mFramebufferManager.reset(new VkFramebufferManager(this));
	mBufferManager.reset(new VkBufferManager(this));
	mBufferManager->Init();

	mScreenBuffers.reset(new VkRenderBuffers(this));
	mSaveBuffers.reset(new VkRenderBuffers(this));
	mActiveRenderBuffers = mScreenBuffers.get();

	mPostprocess.reset(new VkPostprocess(this));
	mDescriptorSetManager.reset(new VkDescriptorSetManager(this));
	mRenderPassManager.reset(new VkRenderPassManager(this));
	mRaytrace.reset(new VkRaytrace(this));

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer;
	mLights = new FLightBuffer();
	mBones = new BoneBuffer();

	mShaderManager.reset(new VkShaderManager(this));
	mDescriptorSetManager->Init();
#ifdef __APPLE__
	mRenderState.reset(new VkRenderStateMolten(this));
#else
	mRenderState.reset(new VkRenderState(this));
#endif
}

void VulkanRenderDevice::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();

	GetPostprocess()->SetActiveRenderTarget();

	Draw2D();
	twod->Clear();

	mRenderState->EndRenderPass();
	mRenderState->EndFrame();

	Flush3D.Unclock();

	mCommands->WaitForCommands(true);
	mCommands->UpdateGpuStats();

	Super::Update();
}

bool VulkanRenderDevice::CompileNextShader()
{
	return mShaderManager->CompileNextShader();
}

void VulkanRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	auto BaseLayer = static_cast<VkHardwareTexture*>(tex->GetHardwareTexture(0, 0));

	VkTextureImage *image = BaseLayer->GetImage(tex, 0, 0);
	VkTextureImage *depthStencil = BaseLayer->GetDepthStencil(tex);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(image, depthStencil->View.get(), image->Image->width, image->Image->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = min(tex->GetWidth(), image->Image->width);
	bounds.height = min(tex->GetHeight(), image->Image->height);

	renderFunc(bounds);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples());

	tex->SetUpdated(true);
}

void VulkanRenderDevice::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) mPostprocess->BlitSceneToPostprocess(); // Copy the resulting scene to the current post process texture
	mPostprocess->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

const char* VulkanRenderDevice::DeviceName() const
{
	return device->PhysicalDevice.Properties.Properties.deviceName;
}

void VulkanRenderDevice::SetVSync(bool vsync)
{
	mVSync = vsync;
}

void VulkanRenderDevice::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	MaterialLayerInfo* layer;

	auto systex = static_cast<VkHardwareTexture*>(mat->GetLayer(0, translation, &layer));
	systex->GetImage(layer->layerTexture, translation, layer->scaleFlags);

	int numLayers = mat->NumLayers();
	for (int i = 1; i < numLayers; i++)
	{
		auto syslayer = static_cast<VkHardwareTexture*>(mat->GetLayer(i, 0, &layer));
		syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
	}
}

IHardwareTexture *VulkanRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new VkHardwareTexture(this, numchannels);
}

FMaterial* VulkanRenderDevice::CreateMaterial(FGameTexture* tex, int scaleflags)
{
	return new VkMaterial(this, tex, scaleflags);
}

IVertexBuffer *VulkanRenderDevice::CreateVertexBuffer()
{
	return GetBufferManager()->CreateVertexBuffer();
}

IIndexBuffer *VulkanRenderDevice::CreateIndexBuffer()
{
	return GetBufferManager()->CreateIndexBuffer();
}

IDataBuffer *VulkanRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return GetBufferManager()->CreateDataBuffer(bindingpoint, ssbo, needsresize);
}

void VulkanRenderDevice::SetTextureFilterMode()
{
	if (mSamplerManager)
	{
		mDescriptorSetManager->ResetHWTextureSets();
		mSamplerManager->ResetHWSamplers();
	}
}

void VulkanRenderDevice::StartPrecaching()
{
	// Destroy the texture descriptors to avoid problems with potentially stale textures.
	mDescriptorSetManager->ResetHWTextureSets();
}

void VulkanRenderDevice::BlurScene(float amount)
{
	if (mPostprocess)
		mPostprocess->BlurScene(amount);
}

void VulkanRenderDevice::UpdatePalette()
{
	if (mPostprocess)
		mPostprocess->ClearTonemapPalette();
}

FTexture *VulkanRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");

	return tex;
}

FTexture *VulkanRenderDevice::WipeEndScreen()
{
	GetPostprocess()->SetActiveRenderTarget();
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");

	return tex;
}

void VulkanRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data)
{
	VkTextureImage image;

	// Convert from rgba16f to rgba8 using the GPU:
	image.Image = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.Size(w, h)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	GetPostprocess()->BlitCurrentToImage(&image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// Staging buffer for download
	auto staging = BufferBuilder()
		.Size(w * h * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	// Copy from image to buffer
	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	mCommands->GetDrawCommands()->copyImageToBuffer(image.Image->image, image.Layout, staging->buffer, 1, &region);

	// Submit command buffers and wait for device to finish the work
	mCommands->WaitForCommands(false);

	// Map and convert from rgba8 to rgb8
	uint8_t *dest = (uint8_t*)data;
	uint8_t *pixels = (uint8_t*)staging->Map(0, w * h * 4);
	int dindex = 0;
	for (int y = 0; y < h; y++)
	{
		int sindex = (h - y - 1) * w * 4;
		for (int x = 0; x < w; x++)
		{
			dest[dindex] = pixels[sindex];
			dest[dindex + 1] = pixels[sindex + 1];
			dest[dindex + 2] = pixels[sindex + 2];
			dindex += 3;
			sindex += 4;
		}
	}
	staging->Unmap();
}

//===========================================================================
//
// CubeDoom fulldome cubemap pipeline (Vulkan)
//
// Mirrors the OpenGL implementation in gl_framebuffer.cpp: assemble the six
// 90-degree face textures into a horizontal strip, then read the strip back
// to CPU for the PipeWire / Sh4lt / NDI transports.
//
//===========================================================================

void VulkanRenderDevice::CompositeCubemapFaces(FCanvasTexture** faces, int N, FCanvasTexture* crossTex)
{
	// Horizontal strip layout: [RIGHT][LEFT][UP][DOWN][FRONT][BACK]
	//   col:                      0      1    2    3      4     5
	// Indexed by CubeFaceIndex (FRONT=0, LEFT=1, RIGHT=2, BACK=3, UP=4, DOWN=5).
	static const int kFBX[6] = { 4, 1, 0, 5, 2, 3 };

	auto crossHW = static_cast<VkHardwareTexture*>(crossTex->GetHardwareTexture(0, 0));
	VkTextureImage* cross = crossHW->GetImage(crossTex, 0, 0);

	mRenderState->EndRenderPass();
	auto cmd = mCommands->GetDrawCommands();

	// Cross becomes the blit destination.
	VkImageTransition()
		.AddImage(cross, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
		.Execute(cmd);

	for (int i = 0; i < 6; i++)
	{
		auto faceHW = static_cast<VkHardwareTexture*>(faces[i]->GetHardwareTexture(0, 0));
		VkTextureImage* face = faceHW->GetImage(faces[i], 0, 0);

		VkImageTransition()
			.AddImage(face, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmd);

		const int dx = kFBX[i] * N;

		VkImageBlit blit = {};
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = { N, N, 1 };
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.dstOffsets[0] = { dx, 0, 0 };
		blit.dstOffsets[1] = { dx + N, N, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;

		cmd->blitImage(face->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		               cross->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		               1, &blit, VK_FILTER_NEAREST);

		// Return the face to its normal sampling layout.
		VkImageTransition()
			.AddImage(face, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmd);
	}

	// Leave the cross sampleable; ReadCubemapCrossPixels re-transitions it.
	VkImageTransition()
		.AddImage(cross, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(cmd);

	crossTex->SetUpdated(true);
}

void VulkanRenderDevice::ReadCubemapCrossPixels(FCanvasTexture* crossTex, uint8_t* buf, int w, int h)
{
	auto crossHW = static_cast<VkHardwareTexture*>(crossTex->GetHardwareTexture(0, 0));
	VkTextureImage* cross = crossHW->GetImage(crossTex, 0, 0);

	mRenderState->EndRenderPass();
	auto cmd = mCommands->GetDrawCommands();

	VkImageTransition()
		.AddImage(cross, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.Execute(cmd);

	auto staging = BufferBuilder()
		.Size((size_t)w * h * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("ReadCubemapCrossPixels")
		.Create(device.get());

	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	cmd->copyImageToBuffer(cross->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       staging->buffer, 1, &region);

	// Restore the cross to its sampling layout for the next frame.
	VkImageTransition()
		.AddImage(cross, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(cmd);

	// Submit and wait so the staging buffer is populated. This stalls the
	// frame (like CopyScreenToBuffer) — acceptable for the offscreen dome feed.
	mCommands->WaitForCommands(false);

	// GZDoom renders the scene bottom-up (matching the GL convention), so we
	// preserve that order here; the transports' PushFrame() does the top-down
	// flip. No vertical flip on this copy.
	uint8_t* pixels = (uint8_t*)staging->Map(0, (size_t)w * h * 4);
	memcpy(buf, pixels, (size_t)w * h * 4);
	staging->Unmap();
}

//===========================================================================
//
// RenderDomemaster (Vulkan)
//
// Warps the 6 face textures into a square fisheye domemaster via one
// fullscreen graphics pass. Mirrors the OpenGL RenderDomemaster and uses the
// same projection math as the ossia score domemaster ISF. invRot is a
// column-major 3x3 inverse content rotation built CPU-side.
//
//===========================================================================

static const char* kDomeVertSrc = R"GLSL(
#version 450
layout(location = 0) out vec2 vUV;
void main() {
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	vUV = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

static const char* kDomeFragSrc = R"GLSL(
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 0) uniform sampler2D facePosX;
layout(set = 0, binding = 1) uniform sampler2D faceNegX;
layout(set = 0, binding = 2) uniform sampler2D facePosY;
layout(set = 0, binding = 3) uniform sampler2D faceNegY;
layout(set = 0, binding = 4) uniform sampler2D facePosZ;
layout(set = 0, binding = 5) uniform sampler2D faceNegZ;
layout(set = 0, binding = 6) uniform sampler2D hudTex;
layout(push_constant) uniform PC {
	vec4 rot0; vec4 rot1; vec4 rot2; // columns of invRot (xyz used)
	vec4 params;                     // x=halfFovRad y=flipX z=flipY w=hudEnable
	vec4 hud;                        // x=halfArcRad y=band z=strip w=chroma
} pc;
#define UV(c) (((c) * vec2(1.0, 1.0) + 1.0) * 0.5)
void main() {
	vec2 p = (vUV * 2.0 - 1.0) * vec2(pc.params.y, pc.params.z);
	float r = length(p);
	if (r > 1.0) { FragColor = vec4(0.0); return; }
	vec2 az = (r > 1e-6) ? p / r : vec2(0.0);
	float polar = r * pc.params.x;
	float sp = sin(polar);
	mat3 invRot = mat3(pc.rot0.xyz, pc.rot1.xyz, pc.rot2.xyz);
	vec3 d = invRot * vec3(sp * az.x, sp * az.y, cos(polar));
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
	if (pc.params.w > 0.5) {
		float ang = atan(p.y, p.x);
		float dd  = mod(ang - (-1.57079633) + 3.14159265, 6.28318531) - 3.14159265;
		float halfArc = pc.hud.x, band = pc.hud.y;
		if (r >= 1.0 - band && abs(dd) <= halfArc) {
			float u  = dd / halfArc * 0.5 + 0.5;
			float vv = (r - (1.0 - band)) / band;
			vec4 h = texture(hudTex, vec2(u, vv * pc.hud.z));
			bool keyed = (pc.hud.w > 0.5) &&
			             (h.g > h.r * 1.15 && h.g > h.b * 1.15 && h.g > 0.2);
			if (h.a > 0.01 && !keyed) FragColor = vec4(h.rgb, 1.0);
		}
	}
}
)GLSL";

struct DomePush { float rot0[4]; float rot1[4]; float rot2[4]; float params[4]; float hud[4]; };

void VulkanRenderDevice::InitDomemasterResources(int domeSize)
{
	if (mDomeInit) return;

	mDomeVert = ShaderBuilder()
		.Type(ShaderType::Vertex)
		.AddSource("domemaster.vert", kDomeVertSrc)
		.DebugName("domemaster.vert")
		.Create("domemaster.vert", device.get());
	mDomeFrag = ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource("domemaster.frag", kDomeFragSrc)
		.DebugName("domemaster.frag")
		.Create("domemaster.frag", device.get());

	mDomeSampler = SamplerBuilder()
		.MinFilter(VK_FILTER_LINEAR)
		.MagFilter(VK_FILTER_LINEAR)
		.MipmapMode(VK_SAMPLER_MIPMAP_MODE_NEAREST)
		.AddressMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
		.MaxLod(0.25f)
		.DebugName("DomemasterSampler")
		.Create(device.get());

	DescriptorSetLayoutBuilder slb;
	for (int i = 0; i < 7; i++)   // 6 faces + 1 HUD
		slb.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	mDomeSetLayout = slb.DebugName("DomemasterSetLayout").Create(device.get());

	mDomePipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(mDomeSetLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DomePush))
		.DebugName("DomemasterPipelineLayout")
		.Create(device.get());

	mDomeRenderPass = RenderPassBuilder()
		.AddAttachment(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
		               VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
		               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.AddSubpass()
		.AddSubpassColorAttachmentRef(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT)
		.DebugName("DomemasterRenderPass")
		.Create(device.get());

	GraphicsPipelineBuilder pb;
	pb.Cache(GetRenderPassManager()->GetCache());
	pb.AddVertexShader(mDomeVert.get());
	pb.AddFragmentShader(mDomeFrag.get());
	pb.Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pb.Viewport(0.0f, 0.0f, (float)domeSize, (float)domeSize);
	pb.Scissor(0, 0, domeSize, domeSize);
	pb.Cull(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	pb.DepthStencilEnable(false, false, false);
	pb.RasterizationSamples(VK_SAMPLE_COUNT_1_BIT);
	pb.AddColorBlendAttachment(ColorBlendAttachmentBuilder().Create());
	pb.Layout(mDomePipelineLayout.get());
	pb.RenderPass(mDomeRenderPass.get());
	pb.DebugName("DomemasterPipeline");
	mDomePipeline = pb.Create(device.get());

	mDomeDescPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7)
		.MaxSets(1)
		.DebugName("DomemasterDescPool")
		.Create(device.get());
	mDomeDescSet = mDomeDescPool->allocate(mDomeSetLayout.get());

	mDomeFbSize = domeSize;
	mDomeInit = true;
}

void VulkanRenderDevice::RenderDomemaster(FCanvasTexture** faces, int N,
                                          FCanvasTexture* domeTex, int domeSize,
                                          const DomemasterParams& params)
{
	InitDomemasterResources(domeSize);

	auto domeHW = static_cast<VkHardwareTexture*>(domeTex->GetHardwareTexture(0, 0));
	VkTextureImage* dome = domeHW->GetImage(domeTex, 0, 0);

	if (!mDomeFramebuffer)
	{
		mDomeFramebuffer = FramebufferBuilder()
			.RenderPass(mDomeRenderPass.get())
			.AddAttachment(dome->View.get())
			.Size(domeSize, domeSize)
			.DebugName("DomemasterFramebuffer")
			.Create(device.get());
	}

	mRenderState->EndRenderPass();
	auto cmd = mCommands->GetDrawCommands();

	// Move all faces to a sampleable layout.
	for (int i = 0; i < 6; i++)
	{
		auto faceHW = static_cast<VkHardwareTexture*>(faces[i]->GetHardwareTexture(0, 0));
		VkTextureImage* face = faceHW->GetImage(faces[i], 0, 0);
		VkImageTransition()
			.AddImage(face, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmd);
	}

	// HUD texture -> sampleable (or fall back to a face when disabled, so
	// binding 6 is always valid; the shader gates sampling on params.w).
	const bool hudOn = params.hudTex != nullptr;
	VkTextureImage* hudImg = nullptr;
	if (hudOn)
	{
		auto hudHW = static_cast<VkHardwareTexture*>(params.hudTex->GetHardwareTexture(0, 0));
		hudImg = hudHW->GetImage(params.hudTex, 0, 0);
		VkImageTransition()
			.AddImage(hudImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmd);
	}

	// Bind faces to sampler bindings. binding -> CubeFaceIndex:
	// 0 posX=RIGHT(2), 1 negX=LEFT(1), 2 posY=UP(4), 3 negY=DOWN(5),
	// 4 posZ=FRONT(0), 5 negZ=BACK(3).
	static const int kFaceForBinding[6] = { 2, 1, 4, 5, 0, 3 };
	WriteDescriptors wd;
	VkTextureImage* face0 = nullptr;
	for (int b = 0; b < 6; b++)
	{
		auto faceHW = static_cast<VkHardwareTexture*>(faces[kFaceForBinding[b]]->GetHardwareTexture(0, 0));
		VkTextureImage* face = faceHW->GetImage(faces[kFaceForBinding[b]], 0, 0);
		if (!face0) face0 = face;
		wd.AddCombinedImageSampler(mDomeDescSet.get(), b, face->View.get(),
		                           mDomeSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	wd.AddCombinedImageSampler(mDomeDescSet.get(), 6,
	                           (hudImg ? hudImg : face0)->View.get(),
	                           mDomeSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	wd.Execute(device.get());

	const float* invRot = params.invRot;
	DomePush push = {};
	push.rot0[0] = invRot[0]; push.rot0[1] = invRot[1]; push.rot0[2] = invRot[2];
	push.rot1[0] = invRot[3]; push.rot1[1] = invRot[4]; push.rot1[2] = invRot[5];
	push.rot2[0] = invRot[6]; push.rot2[1] = invRot[7]; push.rot2[2] = invRot[8];
	push.params[0] = params.fovDeg * (3.14159265359f / 360.0f);
	push.params[1] = params.flipH ? -1.0f : 1.0f;
	push.params[2] = params.flipV ? -1.0f : 1.0f;
	push.params[3] = hudOn ? 1.0f : 0.0f;
	push.hud[0] = params.hudArcDeg * (3.14159265359f / 360.0f);
	push.hud[1] = params.hudBand;
	push.hud[2] = params.hudStrip;
	push.hud[3] = params.hudChroma ? 1.0f : 0.0f;

	RenderPassBegin()
		.RenderPass(mDomeRenderPass.get())
		.RenderArea(0, 0, domeSize, domeSize)
		.Framebuffer(mDomeFramebuffer.get())
		.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f)
		.Execute(cmd);

	cmd->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, mDomePipeline.get());
	cmd->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, mDomePipelineLayout.get(), 0, mDomeDescSet.get());
	cmd->pushConstants(mDomePipelineLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	cmd->draw(3, 1, 0, 0);
	cmd->endRenderPass();

	// The render pass left the dome image in SHADER_READ_ONLY; sync the tracked
	// layout so ReadCubemapCrossPixels transitions from the correct old layout.
	dome->Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	domeTex->SetUpdated(true);
}

void VulkanRenderDevice::SetActiveRenderTarget()
{
	mPostprocess->SetActiveRenderTarget();
}

TArray<uint8_t> VulkanRenderDevice::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = w;
	box.height = h;
	mPostprocess->DrawPresentTexture(box, true, true);

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return ScreenshotBuffer;
}

void VulkanRenderDevice::BeginFrame()
{
	SetViewportRects(nullptr);
	mViewpoints->Clear();
	mCommands->BeginFrame();
	mTextureManager->BeginFrame();
	mScreenBuffers->BeginFrame(screen->mScreenViewport.width, screen->mScreenViewport.height, screen->mSceneViewport.width, screen->mSceneViewport.height);
	mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT);
	mRenderState->BeginFrame();
	mDescriptorSetManager->BeginFrame();
}

void VulkanRenderDevice::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GetTextureManager()->SetLightmap(LMTextureSize, LMTextureCount, LMTextureData);
		LMTextureData.Reset(); // We no longer need this, release the memory
	}
}

void VulkanRenderDevice::Draw2D()
{
	::Draw2D(twod, *mRenderState);
}

void VulkanRenderDevice::WaitForCommands(bool finish)
{
	mCommands->WaitForCommands(finish);
}

unsigned int VulkanRenderDevice::GetLightBufferBlockSize() const
{
	return mLights->GetBlockSize();
}

void VulkanRenderDevice::PrintStartupLog()
{
	const auto &props = device->PhysicalDevice.Properties.Properties;

	FString deviceType;
	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_OTHER: deviceType = "other"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "discrete gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "virtual gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "cpu"; break;
	default: deviceType.Format("%d", (int)props.deviceType); break;
	}

	FString apiVersion, driverVersion;
	apiVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	driverVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
	vkversion = VK_API_VERSION_MAJOR(props.apiVersion) * 100 + VK_API_VERSION_MINOR(props.apiVersion);

	Printf("Vulkan device: " TEXTCOLOR_ORANGE "%s\n", props.deviceName);
	Printf("Vulkan device type: %s\n", deviceType.GetChars());
	Printf("Vulkan version: %s (api) %s (driver)\n", apiVersion.GetChars(), driverVersion.GetChars());

	Printf(PRINT_LOG, "Vulkan extensions:");
	for (const VkExtensionProperties &p : device->PhysicalDevice.Extensions)
	{
		Printf(PRINT_LOG, " %s", p.extensionName);
	}
	Printf(PRINT_LOG, "\n");

	const auto &limits = props.limits;
	Printf("Max. texture size: %d\n", limits.maxImageDimension2D);
	Printf("Max. uniform buffer range: %d\n", limits.maxUniformBufferRange);
	Printf("Min. uniform buffer offset alignment: %" PRIu64 "\n", limits.minUniformBufferOffsetAlignment);
}

void VulkanRenderDevice::SetLevelMesh(hwrenderer::LevelMesh* mesh)
{
	mRaytrace->SetLevelMesh(mesh);
}

void VulkanRenderDevice::UpdateShadowMap()
{
	mPostprocess->UpdateShadowMap();
}

void VulkanRenderDevice::SetSaveBuffers(bool yes)
{
	if (yes) mActiveRenderBuffers = mSaveBuffers.get();
	else mActiveRenderBuffers = mScreenBuffers.get();
}

void VulkanRenderDevice::ImageTransitionScene(bool unknown)
{
	mPostprocess->ImageTransitionScene(unknown);
}

FRenderState* VulkanRenderDevice::RenderState()
{
	return mRenderState.get();
}

void VulkanRenderDevice::AmbientOccludeScene(float m5)
{
	mPostprocess->AmbientOccludeScene(m5);
}

void VulkanRenderDevice::SetSceneRenderTarget(bool useSSAO)
{
	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples());
}

bool VulkanRenderDevice::RaytracingEnabled()
{
	return vk_raytrace && device->SupportsExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
}
