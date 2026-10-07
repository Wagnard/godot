/**************************************************************************/
/*  fsr_frame_generation.h                                                */
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
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// PROTOTYPE (GODOT_FSR_FG): AMD FSR frame generation (D3D12) fed by the 3D pass when DLSS does not
// upscale it: FSR2, TAA, native, MSAA. With DLSS upscaling, DLSSEffect hands FSR its inputs itself.
class FSRFrameGenerationEffect {
public:
	struct Parameters {
		RID depth; // Single-sample (resolved with MSAA).
		RID velocity; // Godot's velocity buffer: (-1,-1) on pixels without object motion; decoded in place.
		Size2i internal_size;
		Vector2 jitter; // In pixels.
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f; // Degrees.
		bool reverse_depth = true;
		float delta_time = 0.0f; // Seconds.
		// The decode shader's convention (see DLSSContext::Parameters::reprojection).
		Projection reprojection;
		Transform3D cam_transform;
	};

	// Render thread, set by the compositor around each viewport's 3D render: the render target
	// shown on the main window, or null. Only that one feeds frame generation.
	static void set_source(RID p_render_target);
	// Whether FSR frame generation runs and p_render_target is its source: the 3D pass must then
	// produce motion vectors and call prepare(). Always false without D3D12.
	static bool is_source(RID p_render_target);
	// The source rendered with Viewport.frame_generation off: frame generation stops.
	static void stop();

	FSRFrameGenerationEffect();
	~FSRFrameGenerationEffect();

	// Once per frame, after the temporal pass (FSR2/TAA) if any: those read the sentinel themselves,
	// so the decode runs after them. Records the decode, then FSR's prepare in a driver callback.
	void prepare(const Parameters &p_params);

private:
	MotionVectorDecodeShaderRD mvec_decode_shader;
	RID mvec_decode_version;
	RID mvec_decode_pipeline;
	Parameters last_parameters;

	static void _prepare_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata);
};

} // namespace RendererRD
