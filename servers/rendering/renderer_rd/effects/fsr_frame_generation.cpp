/**************************************************************************/
/*  fsr_frame_generation.cpp                                              */
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

#include "fsr_frame_generation.h"

#ifdef D3D12_ENABLED
#include "drivers/d3d12/fsr_frame_generation_d3d12.h"
#include "servers/rendering/renderer_rd/effects/dlss.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#endif

using namespace RendererRD;

static RID fsr_frame_generation_source;

void FSRFrameGenerationEffect::set_source(RID p_render_target) {
	fsr_frame_generation_source = p_render_target;
}

bool FSRFrameGenerationEffect::is_source(RID p_render_target) {
#ifdef D3D12_ENABLED
	return p_render_target.is_valid() && p_render_target == fsr_frame_generation_source && FSRFrameGenerationD3D12::is_active();
#else
	return false;
#endif
}

void FSRFrameGenerationEffect::stop() {
#ifdef D3D12_ENABLED
	FSRFrameGenerationD3D12::stop_generating();
#endif
}

FSRFrameGenerationEffect::FSRFrameGenerationEffect() {
	// The decode shader is compiled on the first prepare(): FSR can become the provider at any time.
}

FSRFrameGenerationEffect::~FSRFrameGenerationEffect() {
	if (mvec_decode_version.is_valid()) {
		mvec_decode_shader.version_free(mvec_decode_version);
	}
}

void FSRFrameGenerationEffect::prepare(const Parameters &p_params) {
#ifdef D3D12_ENABLED
	if (mvec_decode_version.is_null()) {
		Vector<String> modes;
		modes.push_back("\n");
		mvec_decode_shader.initialize(modes, "");
		mvec_decode_version = mvec_decode_shader.version_create();
		mvec_decode_pipeline = RD::get_singleton()->compute_pipeline_create(mvec_decode_shader.version_get_shader(mvec_decode_version, 0));
	}
	ERR_FAIL_COND(mvec_decode_version.is_null());

	// Complete motion vectors: pixels without object motion get the camera's, from depth. The same
	// pass as DLSS's (DLSSEffect::upscale()); FSR's prepare takes no reprojection matrix.
	{
		RD::get_singleton()->draw_command_begin_label("FSR FG Decode Invalid Motion Vectors");
		UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
		ERR_FAIL_NULL(uniform_set_cache);

		RD::Uniform u_velocity_image(RD::UNIFORM_TYPE_IMAGE, 0, p_params.velocity);
		RD::Uniform u_depth_texture(RD::UNIFORM_TYPE_TEXTURE, 0, p_params.depth);

		RID shader = mvec_decode_shader.version_get_shader(mvec_decode_version, 0);
		ERR_FAIL_COND(shader.is_null());

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, mvec_decode_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_velocity_image), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_depth_texture), 1);

		const RD::TextureFormat texture_format = RD::get_singleton()->texture_get_format(p_params.velocity);
		float push_constants[20];
		push_constants[0] = texture_format.width;
		push_constants[1] = texture_format.height;
		push_constants[2] = 0.0f;
		push_constants[3] = 0.0f;
		memcpy(push_constants + 4, &p_params.reprojection.columns[0].x, sizeof(float) * 16);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, push_constants, sizeof(push_constants));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, texture_format.width, texture_format.height, 1);
		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	// FSR's prepare reads depth and motion vectors in its own dispatch, on the graph's command list.
	RD::CallbackResource resources[2];
	resources[0].rid = p_params.depth;
	// GENERAL (D3D12_BARRIER_LAYOUT_COMMON): FidelityFX transitions with legacy barriers, valid with
	// enhanced ones only through COMMON. Announced as FFX_API_RESOURCE_STATE_COMMON.
	resources[0].usage = RD::CALLBACK_RESOURCE_USAGE_GENERAL;
	resources[1].rid = p_params.velocity;
	resources[1].usage = RD::CALLBACK_RESOURCE_USAGE_GENERAL;
	CallbackPayload *payload = memnew(CallbackPayload);
	payload->params = p_params;
	payload->depth_resource = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_params.depth);
	payload->motion_vectors_resource = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_params.velocity);
	pending_payloads.push_back(payload);
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)FSRFrameGenerationEffect::_prepare_graph_callback, payload, VectorView<RD::CallbackResource>(resources, 2));
#endif
}

LocalVector<FSRFrameGenerationEffect::CallbackPayload *> FSRFrameGenerationEffect::pending_payloads;

void FSRFrameGenerationEffect::_capture_hudless(CallbackPayload *p_payload) {
	const RID hudless = DLSSEffect::get_frame_generation_hudless();
	p_payload->hudless_resource = hudless.is_valid() ? RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, hudless) : 0;
	p_payload->hudless_final = true;
}

void FSRFrameGenerationEffect::finalize_frame_callbacks() {
	for (CallbackPayload *payload : pending_payloads) {
		_capture_hudless(payload);
	}
	pending_payloads.clear();
}

void FSRFrameGenerationEffect::_prepare_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
#ifdef D3D12_ENABLED
	CallbackPayload *payload = (CallbackPayload *)p_userdata;
	if (!payload->hudless_final) {
		// Replayed before its frame's recording ended (a flush, on the render thread).
		_capture_hudless(payload);
		pending_payloads.erase(payload);
	}
	const Parameters &p = payload->params;

	// Same inputs as the DLSS path (DLSSEffect::_upscale_internal()).
	FSRFrameGenerationD3D12::PrepareParams fg;
	fg.native_command_list = p_driver->command_buffer_get_native_handle(p_command_buffer);
	fg.depth_resource = payload->depth_resource;
	fg.motion_vectors_resource = payload->motion_vectors_resource;
	fg.render_width = p.internal_size.width;
	fg.render_height = p.internal_size.height;
	fg.jitter_x = p.jitter.x;
	fg.jitter_y = p.jitter.y;
	fg.frame_time_delta_ms = p.delta_time * 1000.0f;
	fg.camera_near = p.z_near;
	fg.camera_far = p.z_far;
	fg.fov_vertical_radians = Math::deg_to_rad(p.fovy);
	fg.depth_inverted = p.reverse_depth;
	fg.hudless_resource = payload->hudless_resource;
	const Basis &basis = p.cam_transform.get_basis();
	const Vector3 origin = p.cam_transform.get_origin();
	const Vector3 up = basis.get_column(1).normalized();
	const Vector3 right = basis.get_column(0).normalized();
	const Vector3 forward = (-basis.get_column(2)).normalized();
	for (int i = 0; i < 3; i++) {
		fg.camera_position[i] = origin[i];
		fg.camera_up[i] = up[i];
		fg.camera_right[i] = right[i];
		fg.camera_forward[i] = forward[i];
	}
	FSRFrameGenerationD3D12::prepare(fg);
#endif
}
