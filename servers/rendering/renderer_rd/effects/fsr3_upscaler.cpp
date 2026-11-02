/**************************************************************************/
/*  fsr3_upscaler.cpp                                                     */
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

#include "fsr3_upscaler.h"

#include "core/config/engine.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

#ifdef D3D12_ENABLED
#include "drivers/d3d12/fsr_upscaler_d3d12.h"
#endif

using namespace RendererRD;

static bool fsr3_is_d3d12() {
	return RD::get_singleton()->get_device_api_name().to_lower() == "d3d12";
}

FSR3UpscalerContext::~FSR3UpscalerContext() {
	if (ffx_context) {
		// AMD's sample flushes the GPU before destroying a context: frames already handed to the replay thread
		// record their dispatches first, then every frame in flight completes (as DLSSContextInner does).
		RD::get_singleton()->wait_for_frame_threads();
		RD::get_singleton()->_flush_and_stall_for_all_frames();
#ifdef D3D12_ENABLED
		FSRUpscalerD3D12::destroy_context(ffx_context);
#endif
		ffx_context = nullptr;
	}
	if (reactive_mask.is_valid()) {
		RD::get_singleton()->free_rid(reactive_mask);
	}
}

FSR3UpscalerEffect::FSR3UpscalerEffect() {
	// Compiled on the first upscale(): the mode is rarely used.
}

FSR3UpscalerEffect::~FSR3UpscalerEffect() {
	if (mvec_decode_version.is_valid()) {
		mvec_decode_shader.version_free(mvec_decode_version);
	}
	if (reactive_version.is_valid()) {
		reactive_shader.version_free(reactive_version);
	}
}

FSR3UpscalerContext *FSR3UpscalerEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
#ifdef D3D12_ENABLED
	ERR_FAIL_COND_V(!fsr3_is_d3d12(), nullptr);

	FSRUpscalerD3D12::ContextParams params;
	params.device = (void *)RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_LOGICAL_DEVICE);
	params.max_render_width = p_internal_size.width;
	params.max_render_height = p_internal_size.height;
	params.upscale_width = p_target_size.width;
	params.upscale_height = p_target_size.height;
	// Godot upscales before tonemapping, in linear HDR, with a reversed-Z depth and a finite far plane.
	params.high_dynamic_range = true;
	params.depth_inverted = true;
	// AMD recommends its input checker in development builds only: here, with --gpu-validation.
	params.debug_checking = Engine::get_singleton()->is_validation_layers_enabled();
	void *ffx_context = FSRUpscalerD3D12::create_context(params);
	if (!ffx_context) {
		return nullptr;
	}

	FSR3UpscalerContext *context = memnew(FSR3UpscalerContext);
	context->ffx_context = ffx_context;
	context->internal_size = p_internal_size;
	context->target_size = p_target_size;

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R8_UNORM;
	tf.width = p_internal_size.width;
	tf.height = p_internal_size.height;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	context->reactive_mask = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(context->reactive_mask, "FSR3 Reactive Mask");
	return context;
#else
	return nullptr;
#endif
}

void FSR3UpscalerEffect::upscale(const Parameters &p_params) {
#ifdef D3D12_ENABLED
	FSR3UpscalerContext *context = p_params.context;
	ERR_FAIL_NULL(context);
	ERR_FAIL_NULL(context->ffx_context);

	if (mvec_decode_version.is_null()) {
		Vector<String> modes;
		modes.push_back("\n");
		mvec_decode_shader.initialize(modes, "");
		mvec_decode_version = mvec_decode_shader.version_create();
		mvec_decode_pipeline = RD::get_singleton()->compute_pipeline_create(mvec_decode_shader.version_get_shader(mvec_decode_version, 0));

		reactive_shader.initialize(modes, "");
		reactive_version = reactive_shader.version_create();
		reactive_pipeline = RD::get_singleton()->compute_pipeline_create(reactive_shader.version_get_shader(reactive_version, 0));
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);

	// Complete motion vectors: pixels without object motion get the camera's, from depth. The same pass as DLSS's
	// (DLSSEffect::upscale()); AMD's upscaler takes no reprojection matrix.
	{
		RD::get_singleton()->draw_command_begin_label("FSR3 Decode Invalid Motion Vectors");
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

	// The reactive mask: the color's alpha in an R8 texture, which AMD's DLL reads directly (no swizzled view).
	{
		RD::get_singleton()->draw_command_begin_label("FSR3 Reactive Mask");
		RD::Uniform u_color_texture(RD::UNIFORM_TYPE_TEXTURE, 0, p_params.color);
		RD::Uniform u_reactive_image(RD::UNIFORM_TYPE_IMAGE, 0, context->reactive_mask);

		RID shader = reactive_shader.version_get_shader(reactive_version, 0);
		ERR_FAIL_COND(shader.is_null());

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, reactive_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_color_texture), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_reactive_image), 1);

		int32_t push_constants[4] = { p_params.internal_size.width, p_params.internal_size.height, 0, 0 };
		RD::get_singleton()->compute_list_set_push_constant(compute_list, push_constants, sizeof(push_constants));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_params.internal_size.width, p_params.internal_size.height, 1);
		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	RenderingDevice *rd = RD::get_singleton();
	CallbackPayload *payload = memnew(CallbackPayload);
	payload->ffx_context = context->ffx_context;
	payload->params = p_params;
	payload->reset = context->reset;
	context->reset = false;
	payload->color_resource = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_params.color);
	payload->depth_resource = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_params.depth);
	payload->velocity_resource = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_params.velocity);
	payload->reactive_resource = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, context->reactive_mask);
	payload->output_resource = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_params.output);

	// Every texture the dispatch reads or writes, the output included: the render graph orders the passes that read
	// the output after it only if it is declared (DLSS-OUTPUT-GRAPH-FIXES). GENERAL (D3D12_BARRIER_LAYOUT_COMMON) is a
	// write usage for the graph; FidelityFX transitions with legacy barriers, valid with enhanced ones only through
	// COMMON, so the textures are announced as FFX_API_RESOURCE_STATE_COMMON.
	RD::CallbackResource resources[5];
	resources[0].rid = p_params.color;
	resources[1].rid = p_params.depth;
	resources[2].rid = p_params.velocity;
	resources[3].rid = context->reactive_mask;
	resources[4].rid = p_params.output;
	for (RD::CallbackResource &resource : resources) {
		resource.usage = RD::CALLBACK_RESOURCE_USAGE_GENERAL;
	}
	rd->driver_callback_add((RDD::DriverCallback)FSR3UpscalerEffect::_upscale_graph_callback, payload, VectorView<RD::CallbackResource>(resources, 5));
#endif
}

void FSR3UpscalerEffect::_upscale_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
#ifdef D3D12_ENABLED
	CallbackPayload *payload = (CallbackPayload *)p_userdata;
	const Parameters &p = payload->params;

	FSRUpscalerD3D12::DispatchParams dispatch;
	dispatch.native_command_list = p_driver->command_buffer_get_native_handle(p_command_buffer);
	dispatch.color_resource = payload->color_resource;
	dispatch.depth_resource = payload->depth_resource;
	dispatch.motion_vectors_resource = payload->velocity_resource;
	dispatch.reactive_resource = payload->reactive_resource;
	dispatch.output_resource = payload->output_resource;
	dispatch.render_width = p.internal_size.width;
	dispatch.render_height = p.internal_size.height;
	dispatch.upscale_width = p.target_size.width;
	dispatch.upscale_height = p.target_size.height;
	dispatch.jitter_x = p.jitter.x;
	dispatch.jitter_y = p.jitter.y;
	// Godot's velocity buffer is in UV space, as for FSR2 (effects/fsr2.cpp).
	dispatch.motion_vector_scale_x = float(p.internal_size.width);
	dispatch.motion_vector_scale_y = float(p.internal_size.height);
	dispatch.sharpening = p.sharpness > 1e-6f;
	dispatch.sharpness = p.sharpness;
	dispatch.frame_time_delta_ms = p.delta_time * 1000.0f;
	dispatch.reset = payload->reset;
	dispatch.camera_near = p.z_near;
	dispatch.camera_far = p.z_far;
	dispatch.fov_vertical_radians = Math::deg_to_rad(p.fovy);
	FSRUpscalerD3D12::dispatch(payload->ffx_context, dispatch);
	memdelete(payload);
#endif
}
