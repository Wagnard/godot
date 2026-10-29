/**************************************************************************/
/*  dlss.h                                                                */
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

#include "servers/rendering/renderer_rd/shaders/effects/motion_vector_decode.glsl.gen.h"

namespace RendererRD {

class DLSSEffect;
struct DLSSCallbackPayload;
class DLSSContext {
public:
	struct Parameters {
		DLSSContext *context;
		Size2i internal_size;
		RID color;
		RID depth;
		RID velocity;
		RID reactive;
		RID exposure;
		RID output;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		bool reverse_depth = true;
		Vector2 jitter;
		float delta_time = 0.0f;
		float sharpness = 0.0f;
		char preset = '?';
		bool reset_accumulation = false;
		Projection reprojection;
		Projection cam_projection;
		Transform3D cam_transform;
		// Previous camera, for Streamline's matrices in the depth buffer's clip space. `reprojection`
		// stays in the decode shader's convention (z in [-1,1], y flipped): do not merge the two.
		Projection prev_cam_projection;
		Transform3D prev_cam_transform;
		bool dlss_g = false;
		int dlss_g_frames = 1; // Generated frames between two rendered ones: 1 = 2x, 2 = 3x, 3 = 4x.
		// Upscale the color's alpha too (transparent viewports). DLSS and DLSS-RR otherwise output RGB only,
		// with an alpha of their own; NVIDIA advises it only when the alpha is needed, as it costs GPU time.
		bool alpha_upscaling = false;

		// DLSS Ray Reconstruction buffers
		bool dlss_rr = false; // Enable DLSS-RR mode instead of regular DLSS
		RID dlss_rr_diffuse_albedo; // Diffuse albedo buffer (RGB)
		RID dlss_rr_specular_albedo; // Specular albedo buffer (RGB)
		RID dlss_rr_normal_roughness; // World-space normal (XYZ) + roughness (W)
		RID dlss_rr_specular_hit_dist; // Specular hit distance (R16F, -1 = sky)
		RID dlss_rr_color_before_transparency; // Noisy color before the transparent pass (optional)
		RID dlss_rr_transparency_layer; // Unshaded transparent effects, premultiplied RGBA, upscaled but not denoised (optional)
	} last_parameters;
	DLSSEffect *last_effect = nullptr;
	bool is_d3d12 = false;
	int delay = 4; // Warmup frames before DLSS evaluates (Vulkan stability workaround).
	// Parameters::alpha_upscaling this context was made for. Streamline reads alphaUpscalingEnabled only when it creates
	// the NGX feature, which a change of that option alone does not do: a change needs a new context.
	bool alpha_upscaling = false;

	virtual ~DLSSContext() {}
};

class DLSSEffect {
public:
	struct Shaders {
		MotionVectorDecodeShaderRD mvec_decode_shader;
		RID mvec_decode_version;
		RID mvec_decode_pipeline;
	} shaders;

	DLSSEffect();
	~DLSSEffect();
	DLSSContext *create_context(Size2i p_internal_size, Size2i p_target_size);
	void upscale(const DLSSContext::Parameters &p_params);
	bool is_ready(DLSSContext *context);

	// Frame generation's HUD-less color, captured by the compositor after the 3D pass. Render thread only;
	// frame_generation_wants_hudless() consumes the DLSS pass's request, so it is asked once per frame.
	static bool frame_generation_wants_hudless();
	static void set_frame_generation_hudless(RID p_hudless, RID p_ui_alpha);
	static RID get_frame_generation_hudless();
	// The frame generator reads the HUD-less color after the next frame has started: the compositor
	// then alternates two of them.
	static bool frame_generation_hudless_double_buffered();

	// Called before the graph is replayed: the DLSS callbacks recorded in the frame take the HUD-less
	// color and UI alpha the compositor set after them. Render thread.
	static void finalize_frame_callbacks();

private:
	void _upscale_internal(RDD::CommandBufferID cmdid, DLSSCallbackPayload &p_payload);
	static void _upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata);
};

} // namespace RendererRD
