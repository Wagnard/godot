/**************************************************************************/
/*  fsr3_upscaler.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/fsr3_upscaler_reactive.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/motion_vector_decode.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// One viewport's AMD FSR upscaling context (FidelityFX API), and the R8 reactive mask it reads.
class FSR3UpscalerContext {
public:
	void *ffx_context = nullptr;
	bool vulkan = false; // The context's backend: FSRUpscalerVulkan, else FSRUpscalerD3D12.
	Size2i internal_size;
	Size2i target_size;
	RID reactive_mask;
	bool reset = true; // The first dispatch has no history.

	~FSR3UpscalerContext();
};

// AMD FSR 3.1 upscaling through AMD's signed DLLs (Viewport.SCALING_3D_MODE_FSR3), on D3D12 and Vulkan (Windows). The
// DLL picks the newest provider the GPU supports: FSR 3.1 on any GPU, FSR 4 on RDNA 4.
class FSR3UpscalerEffect {
public:
	struct Parameters {
		FSR3UpscalerContext *context = nullptr;
		Size2i internal_size;
		Size2i target_size;
		float sharpness = 0.0f; // 0 (none) to 1 (most).
		RID color; // Linear HDR; its alpha is the reactive mask (see fsr3_upscaler_reactive.glsl).
		RID depth;
		RID velocity; // Godot's velocity buffer: (-1,-1) on pixels without object motion; decoded in place.
		RID output;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f; // Degrees.
		Vector2 jitter; // In pixels.
		float delta_time = 0.0f; // Seconds.
		// The decode shader's convention (see DLSSContext::Parameters::reprojection).
		Projection reprojection;
	};

	// A texture as the driver knows it, resolved on the render thread for a callback that may run on another thread.
	struct ResolvedTexture {
		uint64_t image = 0; // ID3D12Resource* or VkImage.
		uint32_t format = 0; // The driver's format of the view RenderingDevice reads it through.
		uint32_t usage = 0; // The driver's usage flags.
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t layers = 1;
		uint32_t mipmaps = 1;
	};

	FSR3UpscalerEffect();
	~FSR3UpscalerEffect();

	// nullptr when the context cannot be created (the error is printed).
	FSR3UpscalerContext *create_context(Size2i p_internal_size, Size2i p_target_size);
	// Records the motion vector decode and the reactive mask, then the upscale in a driver callback.
	void upscale(const Parameters &p_params);

private:
	MotionVectorDecodeShaderRD mvec_decode_shader;
	RID mvec_decode_version;
	RID mvec_decode_pipeline;

	Fsr3UpscalerReactiveShaderRD reactive_shader;
	RID reactive_version;
	RID reactive_pipeline;

	// What one recorded upscale uses, resolved on the render thread: the callback may run on another thread after the
	// render thread moved on. Owned and freed by the callback.
	struct CallbackPayload {
		void *ffx_context = nullptr;
		bool vulkan = false;
		Parameters params;
		bool reset = false;
		ResolvedTexture color;
		ResolvedTexture depth;
		ResolvedTexture velocity;
		ResolvedTexture reactive;
		ResolvedTexture output;
	};

	static ResolvedTexture _resolve_texture(RID p_texture);

	static void _upscale_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata);
};

} // namespace RendererRD
