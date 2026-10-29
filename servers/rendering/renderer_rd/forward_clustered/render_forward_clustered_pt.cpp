/**************************************************************************/
/*  render_forward_clustered_pt.cpp                                       */
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

#include "render_forward_clustered_pt.h"

#include "core/os/os.h"
#include "servers/rendering/renderer_rd/environment/fog.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_default.h" // IWYU pragma: keep. RENDER_TIMESTAMP macro uses RSG.

using namespace RendererSceneRenderImplementation;

void RenderForwardClusteredPT::_render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) {
	ERR_FAIL_NULL(p_render_data);

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	Ref<RenderBufferDataForwardClustered> rb_data;
	if (rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
		// This will not be available when rendering reflection probes.
		rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}
	bool is_reflection_probe = p_render_data->reflection_probe.is_valid();

	const bool use_rt = !is_reflection_probe && p_render_data->environment.is_valid() &&
			RendererEnvironmentStorage::get_singleton()->environment_get_pathtracing_enabled(p_render_data->environment) &&
			_setup_rt();
	if (!use_rt) {
		_age_out_motion_vectors(p_render_data);

		// Path tracing is off for this view: drop its state, its textures and the DLSS Ray Reconstruction ones, then,
		// once no view has traced for a while, every acceleration structure and cache, for the raster effects
		// (SDFGI, SSIL...) that take their place.
		// GODOT_RT_KEEP_CACHES=1 keeps everything but the DLSS Ray Reconstruction textures.
		static const bool keep_caches = OS::get_singleton()->get_environment("GODOT_RT_KEEP_CACHES") == "1";
		if (rb_data.is_valid()) {
			if (raytracing && keep_caches) {
				if (raytracing->dlss_rr_has_buffers(rb.ptr())) {
					raytracing->dlss_rr_free_buffers(rb.ptr());
				}
			} else if (raytracing) {
				_free_rt_viewport_state(rb.ptr());
			}
			rb->set_depth_reconstruct_requested(false);
		}
		if (raytracing && !is_reflection_probe && !keep_caches) {
			raytracing->free_caches_if_unused();
		}
		RenderForwardClustered::_render_scene(p_render_data, p_default_bg_color);
		return;
	}

	if (!rt_precache_materials_enabled && _rt_precaches_hit_groups()) {
		// From the first traced frame on, every surface built queues its material's hit group (_geometry_instance_update());
		// those built before, once.
		rt_precache_materials_enabled = true;
		_rt_precache_all_surface_materials();
	}

	scene_state.used_uniform_buffer_count = 0;

	static const int texture_multisamples[RSE::VIEWPORT_MSAA_MAX] = { 1, 2, 4, 8 };

	RENDER_TIMESTAMP("Prepare 3D Scene");

	// Compositor effect requirements.
	bool ce_needs_motion_vectors = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_MOTION_VECTORS);
	bool ce_needs_normal_roughness = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_ROUGHNESS);

	current_cluster_builder = rb_data->cluster_builder;
	p_render_data->voxel_gi_count = 0;

	ERR_FAIL_NULL(current_cluster_builder);

	// As raster: the volumetric fog and the transparent pass read the VoxelGI textures (white defaults without any).
	gi.setup_voxel_gi_instances(p_render_data, p_render_data->render_buffers, p_render_data->scene_data->cam_transform, *p_render_data->voxel_gi_instances, p_render_data->voxel_gi_count);

	p_render_data->cluster_buffer = current_cluster_builder->get_cluster_buffer();
	p_render_data->cluster_size = current_cluster_builder->get_cluster_size();
	p_render_data->cluster_max_elements = current_cluster_builder->get_max_cluster_elements();

	_update_vrs(rb);

	RENDER_TIMESTAMP("Setup 3D Scene");

	bool using_debug_mvs = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MOTION_VECTORS;
	bool using_taa = rb->get_use_taa();

	Scale3DMode scale_type = _resolve_scale_3d_mode(rb);
	bool using_upscaling = scale_type != SCALE_3D_NONE;

	const bool feeds_fsr_frame_generation = _feeds_fsr_frame_generation(p_render_data);

	// Motion vectors are produced by the path tracer (rt_velocity_image)
	bool motion_vectors_required = using_debug_mvs || ce_needs_motion_vectors || using_taa || using_upscaling || feeds_fsr_frame_generation;

	p_render_data->scene_data->calculate_motion_vectors = motion_vectors_required;
	p_render_data->scene_data->directional_light_count = 0;
	p_render_data->scene_data->opaque_prepass_threshold = 0.99f;

	uint32_t color_pass_flags = 0;
	bool reverse_cull = p_render_data->scene_data->cam_transform.basis.determinant() < 0;

	Size2i screen_size = rb->get_internal_size();

	if (p_render_data->scene_data->calculate_motion_vectors) {
		color_pass_flags |= COLOR_PASS_FLAG_MOTION_VECTORS;
		scene_shader.enable_advanced_shader_group();
		global_pipeline_data_required.use_motion_vectors = true;
	}

	// Free GPU resources for the screen-space effects the path tracer replaces.
	rb->clear_context(RB_SCOPE_SSIL);
	rb->clear_context(RB_SCOPE_SSAO);
	rb->clear_context(RB_SCOPE_SSR);

	if (p_render_data->scene_data->view_count > 1) {
		color_pass_flags |= COLOR_PASS_FLAG_MULTIVIEW;
		scene_shader.shader.enable_group(SceneShaderForwardClustered::SHADER_GROUP_MULTIVIEW);
		global_pipeline_data_required.use_multiview = true;
	}

	RID color_only_framebuffer = rb_data->get_color_only_fb();
	RendererRD::MaterialStorage::Samplers samplers = rb->get_samplers();

	p_render_data->scene_data->emissive_exposure_normalization = -1.0;

	RD::get_singleton()->draw_command_begin_label("Render Setup");

	_setup_lightmaps(p_render_data, *p_render_data->lightmaps, p_render_data->scene_data->cam_transform);

	p_render_data->scene_data->directional_light_count = _count_directional_lights(p_render_data);

	_setup_environment(p_render_data, false, screen_size, screen_size, p_default_bg_color, false);

	// May have changed due to the above (light buffer enlarged, as an example).
	_update_render_base_uniform_set();

	_age_out_motion_vectors(p_render_data);

	RENDER_TIMESTAMP("Fill Render Lists");

	// TLAS is built from rt_instances and the path tracer writes velocity itself,
	// so this collapses to populating the ALPHA (transparent) list only.
	_fill_render_list(RENDER_LIST_OPAQUE, p_render_data, PASS_MODE_COLOR, false, false, false, false, true);

	int *render_info = p_render_data->render_info ? p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE] : (int *)nullptr;

	render_list[RENDER_LIST_ALPHA].sort_by_reverse_depth_and_priority();
	// The list in three ranges, each in its order, before their instance data is written. First the pathtracing_rasterized
	// surfaces (opaque and cut-out cards the camera rays leave out): their depth pass and color, before every transparent
	// surface, as opaque geometry is. Then the other transparent surfaces. Last, with DLSS-RR, its transparency layer's
	// effects, drawn into a target of their own.
	uint32_t rasterized_count = 0;
	uint32_t rr_layer_from = UINT32_MAX;
	{
		const bool use_layer = raytracing && raytracing->dlss_rr_has_buffers(rb.ptr()) && p_render_data->scene_data->view_count == 1 && rb->get_msaa_3d() == RSE::VIEWPORT_MSAA_DISABLED && _rr_transparency_layer_enabled();
		LocalVector<GeometryInstanceSurfaceDataCache *> &elements = render_list[RENDER_LIST_ALPHA].elements;
		thread_local LocalVector<GeometryInstanceSurfaceDataCache *> rasterized;
		thread_local LocalVector<GeometryInstanceSurfaceDataCache *> lit;
		thread_local LocalVector<GeometryInstanceSurfaceDataCache *> layer;
		rasterized.clear();
		lit.clear();
		layer.clear();
		for (GeometryInstanceSurfaceDataCache *surf : elements) {
			if (surf->rt_pass_flags & GeometryInstanceSurfaceDataCache::FLAG_PT_RASTERIZED) {
				rasterized.push_back(surf);
			} else if (use_layer && _rr_transparency_layer_surface(surf)) {
				layer.push_back(surf);
			} else {
				lit.push_back(surf);
			}
		}
		if (!rasterized.is_empty() || !layer.is_empty()) {
			uint32_t at = 0;
			for (GeometryInstanceSurfaceDataCache *surf : rasterized) {
				elements[at++] = surf;
			}
			for (GeometryInstanceSurfaceDataCache *surf : lit) {
				elements[at++] = surf;
			}
			for (GeometryInstanceSurfaceDataCache *surf : layer) {
				elements[at++] = surf;
			}
		}
		rasterized_count = rasterized.size();
		if (use_layer) {
			rr_layer_from = rasterized.size() + lit.size();
		}
	}
	_fill_instance_data(RENDER_LIST_ALPHA, render_info);

	// RT pipeline flags (packed with sample count / max bounces). Computed once
	// here and reused at trace-dispatch time below so the uniform set and the
	// pipeline agree on spec-constant values.
	uint32_t rt_flags = SceneShaderRaytracing::RT_FLAG_NONE;
	// Captured from build_tlas/update_uniform_set so the trace-dispatch block
	// below can bind without RenderRaytracing keeping hidden "current" state.
	RID rt_uniform_set;
	RTViewportState *rt_state = nullptr;
	bool using_depth_reconstruct = false;

	// The path tracer produces the opaque color, but transparents are still
	// rasterized on top, which needs the reflection-probe / light / decal buffers
	// and the cluster grid to be valid for the current frame. Before the acceleration structures, as raster renders its
	// shadows before the opaque pass: the shadow passes recorded between the TLAS build and the trace left the TLAS
	// faulty (VK_NV_ray_tracing_validation FAULTY_AS, then a device loss) whenever the volumetric fog was on.
	if (rb_data.is_valid() && is_environment(p_render_data->environment) && environment_get_volumetric_fog_enabled(p_render_data->environment)) {
		// The volumetric fog lights its volume through the shadow maps, as in raster: render them, then update it.
		_pre_opaque_render(p_render_data, false, false, false, false, nullptr, RID());
	} else {
		uint32_t pt_directional_light_count = 0;
		uint32_t pt_positional_light_count = 0;
		_setup_lights_cluster_decals(p_render_data, pt_directional_light_count, pt_positional_light_count);
		if (rb_data.is_valid()) {
			// Frees the volume of a fog just turned off.
			_update_volumetric_fog(rb, p_render_data->environment, p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_transform, p_render_data->scene_data->prev_cam_transform.affine_inverse(), p_render_data->shadow_atlas, pt_directional_light_count, false, pt_positional_light_count, p_render_data->voxel_gi_count, *p_render_data->fog_volumes);
		}
	}

	if (rb_data.is_valid() && raytracing && raytracing->get_shader()) {
		RENDER_TIMESTAMP("Build Acceleration Structures");
		RID rt_environment = p_render_data->environment.is_valid() ? p_render_data->environment : RID();
		rt_flags = SceneShaderRaytracing::compute_rt_flags(rt_environment);

		const bool dlss_rr_enabled = (rt_flags & SceneShaderRaytracing::RT_FLAG_DLSS_RR_ENABLED) != 0;
		if (dlss_rr_enabled) {
			raytracing->dlss_rr_ensure_buffers(rb.ptr());
			using_depth_reconstruct = true;
		} else if (raytracing->dlss_rr_has_buffers(rb.ptr())) {
			raytracing->dlss_rr_free_buffers(rb.ptr());
		}

		// The surfaces built by the render list fill above, out of the TLAS ones included.
		_rt_precache_hit_groups();
		rt_state = raytracing->build_tlas(p_render_data, rt_flags);
		_rt_publish_pending_hit_groups();
	} else if (rb_data.is_valid() && raytracing && raytracing->dlss_rr_has_buffers(rb.ptr())) {
		// No RT shader available: free DLSS RR buffers so DLSS falls back to SR.
		raytracing->dlss_rr_free_buffers(rb.ptr());
	}

	RD::get_singleton()->draw_command_end_label();

	// Transparent geometry can still use lightmaps; make sure the matching shader
	// group and pipeline requirements are live before we (re)compile pipelines.
	if (scene_state.used_lightmap || scene_state.lightmaps_used > 0) {
		scene_shader.enable_advanced_shader_group(p_render_data->scene_data->view_count > 1);
		global_pipeline_data_required.use_lightmaps = true;
	}

	// The transparent surfaces are shadowed by rays through the TLAS, as the path traced ones.
	const bool rt_shadows = rt_state && rt_state->tlas.is_valid() && p_render_data->scene_data->view_count == 1 && _transparent_pass_traces_shadows();
	if (rt_shadows) {
		scene_shader.enable_rt_shadows_shader_group();
		global_pipeline_data_required.use_rt_shadows = true;
	}

	// Recompile pipelines if any of the requirements changed this frame.
	_update_dirty_geometry_pipelines();

	RID radiance_texture;
	bool draw_sky = false;
	bool draw_sky_fog_only = false;
	// We invert luminance_multiplier for sky so that we can combine it with exposure value.
	float sky_luminance_multiplier = 1.0 / rb->get_luminance_multiplier();
	float sky_brightness_multiplier = 1.0;

	Color clear_color;
	// As raster's load_color: BG_CANVAS and BG_KEEP keep the color buffer's pixels where the camera rays missed.
	bool keep_background = false;
	// REFLECTION_SOURCE_BG: where the background is a color, the rays that miss along a specular lobe show it.
	bool reflection_from_background = false;

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW) {
		clear_color = Color(0, 0, 0, 1); //in overdraw mode, BG should always be black
	} else if (is_environment(p_render_data->environment)) {
		RSE::EnvironmentBG bg_mode = environment_get_background(p_render_data->environment);
		float bg_energy_multiplier = environment_get_bg_energy_multiplier(p_render_data->environment);
		bg_energy_multiplier *= environment_get_bg_intensity(p_render_data->environment);
		RSE::EnvironmentReflectionSource reflection_source = environment_get_reflection_source(p_render_data->environment);
		reflection_from_background = reflection_source == RSE::ENV_REFLECTION_SOURCE_BG;

		if (p_render_data->camera_attributes.is_valid()) {
			bg_energy_multiplier *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}

		switch (bg_mode) {
			case RSE::ENV_BG_CLEAR_COLOR:
			case RSE::ENV_BG_COLOR: {
				clear_color = bg_mode == RSE::ENV_BG_CLEAR_COLOR ? p_default_bg_color : environment_get_bg_color(p_render_data->environment);

				if (!p_render_data->transparent_bg && (rb->has_custom_data(RB_SCOPE_FOG) || environment_get_fog_enabled(p_render_data->environment))) {
					draw_sky_fog_only = true;
					RendererRD::MaterialStorage::get_singleton()->material_set_param(sky.sky_scene_state.fog_material, "clear_color", Variant(clear_color));
				}

				clear_color = clear_color.srgb_to_linear();
				clear_color.r *= bg_energy_multiplier;
				clear_color.g *= bg_energy_multiplier;
				clear_color.b *= bg_energy_multiplier;
			} break;
			case RSE::ENV_BG_SKY: {
				draw_sky = !p_render_data->transparent_bg;
			} break;
			case RSE::ENV_BG_CANVAS: {
				RID texture = RendererRD::TextureStorage::get_singleton()->render_target_get_rd_texture(rb->get_render_target());
				bool convert_to_linear = !RendererRD::TextureStorage::get_singleton()->render_target_is_using_hdr(rb->get_render_target());
				copy_effects->copy_to_fb_rect(texture, color_only_framebuffer, Rect2i(), false, false, false, false, RID(), false, false, convert_to_linear);
				keep_background = true;
			} break;
			case RSE::ENV_BG_KEEP: {
				keep_background = true;
			} break;
			case RSE::ENV_BG_CAMERA_FEED: {
			} break;
			default: {
			}
		}

		// setup sky if used for ambient, reflections, or background
		if (draw_sky || draw_sky_fog_only || (reflection_source == RSE::ENV_REFLECTION_SOURCE_BG && bg_mode == RSE::ENV_BG_SKY) || reflection_source == RSE::ENV_REFLECTION_SOURCE_SKY || environment_get_ambient_source(p_render_data->environment) == RSE::ENV_AMBIENT_SOURCE_SKY) {
			RENDER_TIMESTAMP("Setup Sky");
			RD::get_singleton()->draw_command_begin_label("Setup Sky");

			// Setup our sky render information for this frame/viewport
			sky.setup_sky(p_render_data, screen_size);

			sky_brightness_multiplier *= bg_energy_multiplier;

			RID sky_rid = environment_get_sky(p_render_data->environment);
			if (sky_rid.is_valid()) {
				sky.update_radiance_buffers(rb, p_render_data->environment, p_render_data->scene_data->cam_transform.origin, time, sky_luminance_multiplier, sky_brightness_multiplier);
				radiance_texture = sky.sky_get_radiance_texture_rd(sky_rid);
			} else {
				// do not try to draw sky if invalid
				draw_sky = false;
			}

			if (draw_sky || draw_sky_fog_only) {
				// update sky half/quarter res buffers (if required)
				sky.update_res_buffers(rb, p_render_data->environment, time, sky_luminance_multiplier, sky_brightness_multiplier);
			}

			RD::get_singleton()->draw_command_end_label();
		}

		if (bg_mode != RSE::ENV_BG_CLEAR_COLOR && bg_mode != RSE::ENV_BG_COLOR) {
			clear_color = clear_color.srgb_to_linear();
		}
	} else {
		clear_color = p_default_bg_color.srgb_to_linear();
	}

	// After this point clear_color has linear encoding.
	RSE::ViewportMSAA msaa = rb->get_msaa_3d();
	bool use_msaa = msaa != RSE::VIEWPORT_MSAA_DISABLED;

	bool ce_pre_opaque_wants_resolved = use_msaa &&
			(_compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE) ||
					_compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE));
	bool ce_post_opaque_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE);
	bool ce_pre_transparent_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);

	SceneShaderForwardClustered::ShaderSpecialization base_specialization = scene_shader.default_specialization;
	base_specialization.use_depth_fog = p_render_data->environment.is_valid() && environment_get_fog_mode(p_render_data->environment) == RSE::EnvironmentFogMode::ENV_FOG_MODE_DEPTH;

	{
		if (ce_pre_opaque_wants_resolved) {
			// The path tracer hasn't produced color/depth yet at this point.
			WARN_PRINT_ONCE("Pre opaque rendering effects can't access resolved color/depth buffers when path tracing.");
		}

		RENDER_TIMESTAMP("Process Pre Opaque Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE, p_render_data);
	}

	// The trace's uniform set is made last, with its output textures: the sky setup above can create the radiance texture,
	// and freeing the previous one freed a set made with it ("uniform_set is null" on the first frames).
	if (rt_state) {
		raytracing->rt_ensure_textures(rb.ptr());
		// Where raster doesn't draw the sky it clears to clear_color: the camera rays that miss show it (black for
		// BG_SKY in a transparent viewport and for BG_CAMERA_FEED; unused where the background is kept).
		const bool background_is_color = !draw_sky;
		rt_uniform_set = raytracing->update_uniform_set(rt_state, p_render_data, rt_flags, background_is_color, clear_color, background_is_color && reflection_from_background);
	}

	// Execute raytracing (replaces the opaque + motion vector pass). Without a uniform set this frame (the pipeline was
	// being rebuilt for a new material), nothing is traced: RenderingDevice only validates sets in debug builds.
	if (rb_data.is_valid() && raytracing && raytracing->get_shader() && rt_uniform_set.is_valid()) {
		RD::get_singleton()->draw_command_begin_label("Raytracing");
		RENDER_TIMESTAMP("TLAS Build");

		RENDER_TIMESTAMP("Pathtracer");

		// rt_flags was computed at TLAS-build time above and matches what
		// update_uniform_set used, so pipeline + uniform set stay in sync.
		RID rt_pipeline = raytracing->get_shader()->get_raytracing_pipeline(rt_flags);
		if (!rt_pipeline.is_valid()) {
			ERR_PRINT_ONCE_ED("Failed to get raytracing pipeline. Aborting render.");
			return;
		}
		RD::RaytracingListID raytracing_list = RD::get_singleton()->raytracing_list_begin();
		RD::get_singleton()->raytracing_list_bind_raytracing_pipeline(raytracing_list, rt_pipeline);
		RD::get_singleton()->raytracing_list_bind_uniform_set(raytracing_list, rt_uniform_set, 0);

		// Bindless set 1 can be unset when no bindless textures are live
		RID bindless_set = raytracing->get_bindless_uniform_set();
		if (bindless_set.is_valid()) {
			RD::get_singleton()->raytracing_list_bind_uniform_set(raytracing_list, bindless_set, 1);
		}
		RID bindless_array_set = raytracing->get_bindless_array_uniform_set();
		if (bindless_array_set.is_valid()) {
			RD::get_singleton()->raytracing_list_bind_uniform_set(raytracing_list, bindless_array_set, 2);
		}

		// Make sure BDA referenced buffers are registered as dependencies -- these cannot be inferred by the draw graph.
		raytracing->register_raytracing_buffer_dependencies(raytracing_list);

		// Raytracing dispatches at internal (pre-upscale) size
		Size2i rt_size = rb->get_internal_size();
		RD::get_singleton()->raytracing_list_trace_rays(raytracing_list, 0, raytracing->get_shader()->get_hit_sbt(rt_flags), rt_size.width, rt_size.height, 1);
		RD::get_singleton()->raytracing_list_end();

		RD::get_singleton()->draw_command_end_label();

		// Copy RT depth (R32F storage image) to D32F depth buffer after tracing.
		if (rb_data.is_valid() && raytracing->rt_has_depth_texture(rb.ptr())) {
			RENDER_TIMESTAMP("Copy RT Depth (R32F -> D32F)");
			RD::get_singleton()->draw_command_begin_label("Copy RT Depth");
			copy_effects->copy_r32f_to_depth_fb(raytracing->rt_get_depth_texture(rb.ptr()), rb_data->get_depth_fb(), Rect2i(0, 0, rb->get_internal_size().x, rb->get_internal_size().y));
			RD::get_singleton()->draw_command_end_label();
		}

		raytracing->apply_volumetric_fog(p_render_data, fog_use_legacy_blending_get());
		raytracing->copy_output_texture(p_render_data, keep_background);
	}

	// The sky where the camera rays missed (their depth is the far plane), drawn as raster draws it: the miss shader only
	// reads the radiance octmap, far too coarse for a background (a sky shader's stars become a grid of blotches).
	if (draw_sky || draw_sky_fog_only) {
		RENDER_TIMESTAMP("Render Sky");

		RD::get_singleton()->draw_command_begin_label("Draw Sky");
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(color_only_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 1.0f, 0u, p_render_data->render_region);

		sky.draw_sky(draw_list, rb, p_render_data->environment, color_only_framebuffer, time, sky_luminance_multiplier, sky_brightness_multiplier);

		RD::get_singleton()->draw_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	{
		// RT output is already in internal_texture, so the color MSAA resolve is
		// skipped; only depth is resolved for compositor effects that need it.
		if (ce_post_opaque_resolved_depth) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
			}
		}

		RENDER_TIMESTAMP("Process Post Opaque Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE, p_render_data);
	}

	if (use_msaa) {
		RENDER_TIMESTAMP("Resolve MSAA");

		// RT writes internal_texture, so only depth is resolved here.
		if (scene_state.used_depth_texture || scene_state.used_normal_texture || ce_needs_normal_roughness || ce_pre_transparent_resolved_depth) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
			}
		}
	}

	{
		RENDER_TIMESTAMP("Process Post Sky Compositor Effects");
		// Don't need to check for depth or color resolve here, we've already triggered it.
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_SKY, p_render_data);
	}

	if (using_upscaling) {
		// Make sure the upscaled texture is initialized, but not necessarily filled, before running screen copies
		// so it properly detect if a dedicated copy texture should be used.
		rb->ensure_upscaled();
	}

	if (scene_state.used_screen_texture || global_surface_data.screen_texture_used) {
		RENDER_TIMESTAMP("Copy Screen Texture");

		_render_buffers_ensure_screen_texture(p_render_data);

		if (scene_state.used_screen_texture) {
			// Copy screen texture to backbuffer so we can read from it
			_render_buffers_copy_screen_texture(p_render_data);
		}
	}

	if (scene_state.used_depth_texture || global_surface_data.depth_texture_used) {
		RENDER_TIMESTAMP("Copy Depth Texture");

		_render_buffers_ensure_depth_texture(p_render_data);

		if (scene_state.used_depth_texture) {
			// Copy depth texture to backbuffer so we can read from it
			_render_buffers_copy_depth_texture(p_render_data);
		}
	}

	{
		RENDER_TIMESTAMP("Process Pre Transparent Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT, p_render_data);
	}

	// DLSS RR's color before transparency guide: what the transparent pass is about to draw over. And its diffuse albedo
	// where the camera rays missed: the background as drawn.
	if (raytracing && raytracing->dlss_rr_has_buffers(rb.ptr())) {
		raytracing->dlss_rr_write_background_albedo(p_render_data);
		copy_effects->copy_to_rect(rb->get_internal_texture(), raytracing->dlss_rr_get_color_before_transparency(rb.ptr()), Rect2i(Point2i(), rb->get_internal_size()));
	}

	RID rr_transparency_layer;
	{
		RENDER_TIMESTAMP("Render 3D Transparent Pass");

		RD::get_singleton()->draw_command_begin_label("Render 3D Transparent Pass");

		// The path traced reflections of the transparent surfaces, in place of their probes and sky where they match.
		front_layer_reflection = (raytracing && raytracing->get_front_layer_instance_count() > 0) ? raytracing->rt_get_front_layer_texture(rb.ptr()) : RID();

		uint32_t transparent_pass_uniform_buffer_index = _setup_environment(p_render_data, false, screen_size, screen_size, p_default_bg_color, false);

		RID rt_shadow_texture_set;
		if (rt_shadows && scene_shader.is_rt_shadows_shader_group_enabled() && rt_state->geometry_buffer.is_valid() && rt_state->material_buffer.is_valid()) {
			rt_shadow_texture_set = raytracing->get_fragment_texture_set(scene_shader.default_shader_rt_shadows_rd, RT_SHADOW_TEXTURES_UNIFORM_SET);
		}
		const bool use_rt_shadows = rt_shadow_texture_set.is_valid();
		if (use_rt_shadows) {
			rt_shadow_tlas = rt_state->tlas;
			rt_shadow_geometry_buffer = rt_state->geometry_buffer;
			rt_shadow_material_buffer = rt_state->material_buffer;
		}
		RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, radiance_texture, samplers, transparent_pass_uniform_buffer_index, true);
		rt_shadow_tlas = RID();
		rt_shadow_geometry_buffer = RID();
		rt_shadow_material_buffer = RID();

		{
			uint32_t transparent_color_pass_flags = (color_pass_flags | uint32_t(COLOR_PASS_FLAG_TRANSPARENT)) & ~uint32_t(COLOR_PASS_FLAG_SEPARATE_SPECULAR);
			// Motion vectors should not be overwritten by transparent objects.
			transparent_color_pass_flags &= ~uint32_t(COLOR_PASS_FLAG_MOTION_VECTORS);
			if (use_rt_shadows) {
				transparent_color_pass_flags |= COLOR_PASS_FLAG_RT_SHADOWS;
			}

			RID alpha_framebuffer = rb_data->get_color_pass_fb(transparent_color_pass_flags);
			const uint32_t alpha_count = render_list[RENDER_LIST_ALPHA].elements.size();
			const uint32_t lit_count = MIN(rr_layer_from, alpha_count);
			RenderListParameters render_list_params(render_list[RENDER_LIST_ALPHA].elements.ptr(), render_list[RENDER_LIST_ALPHA].element_info.ptr(), lit_count, reverse_cull, PASS_MODE_COLOR, transparent_color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization);
			render_list_params.rt_shadow_texture_uniform_set = rt_shadow_texture_set;
			// The alpha test reads the vertices through their addresses.
			thread_local LocalVector<RID> rt_shadow_buffers;
			rt_shadow_buffers.clear();
			if (use_rt_shadows) {
				raytracing->get_buffers_read_by_address(rt_shadow_buffers);
				render_list_params.buffer_dependencies = &rt_shadow_buffers;
			}
			if (rasterized_count > 0) {
				// The pathtracing_rasterized cards' depth first, cut out as raster's depth pass does: the camera rays left
				// them out, so the depth buffer has what is behind them. Their color then passes the depth test where it
				// is theirs, and the surfaces drawn after them (the DLSS-RR transparency layer: a portal's glow) stay behind.
				// The depth pass' own render pass set, as raster's depth pre-pass builds it (its shaders bind other formats).
				RID depth_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, nullptr, RID(), samplers, transparent_pass_uniform_buffer_index);
				RenderListParameters depth_params(render_list[RENDER_LIST_ALPHA].elements.ptr(), render_list[RENDER_LIST_ALPHA].element_info.ptr(), rasterized_count, reverse_cull, PASS_MODE_DEPTH, 0, rb_data.is_null(), p_render_data->directional_light_soft_shadows, depth_uniform_set, false, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization);
				_render_list_with_draw_list(&depth_params, rb_data->get_depth_fb(), RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0u, p_render_data->render_region);
			}
			_render_list_with_draw_list(&render_list_params, alpha_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0u, p_render_data->render_region);

			if (rr_layer_from != UINT32_MAX) {
				// The effects, into a target cleared to transparent with the same format as the color, tested against the
				// same depth: the pipelines are the color pass' own. Mix and premultiplied alpha leave premultiplied color
				// in RGB and the coverage in alpha, the layer DLSS-RR composites over its denoised output.
				if (!rb->has_texture(RB_SCOPE_DLSS_RR, SNAME("transparency_layer"))) {
					rb->create_texture(RB_SCOPE_DLSS_RR, SNAME("transparency_layer"), rb->get_base_data_format(), RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT, RD::TEXTURE_SAMPLES_1);
				}
				rr_transparency_layer = rb->get_texture(RB_SCOPE_DLSS_RR, SNAME("transparency_layer"));
				RID layer_framebuffer;
				if (rb->has_texture(RB_SCOPE_VRS, RB_TEXTURE)) {
					layer_framebuffer = FramebufferCacheRD::get_singleton()->get_cache_multiview(1, rr_transparency_layer, RID(), RID(), rb->get_depth_texture(), rb->get_texture(RB_SCOPE_VRS, RB_TEXTURE));
				} else {
					layer_framebuffer = FramebufferCacheRD::get_singleton()->get_cache_multiview(1, rr_transparency_layer, RID(), RID(), rb->get_depth_texture());
				}
				RenderListParameters layer_params(render_list[RENDER_LIST_ALPHA].elements.ptr() + lit_count, render_list[RENDER_LIST_ALPHA].element_info.ptr() + lit_count, alpha_count - lit_count, reverse_cull, PASS_MODE_COLOR, transparent_color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, lit_count, base_specialization);
				layer_params.rt_shadow_texture_uniform_set = rt_shadow_texture_set;
				layer_params.buffer_dependencies = render_list_params.buffer_dependencies;
				// Pipelines of their own, where additive blending writes no alpha (PipelineKey::rr_transparency_layer).
				layer_params.rr_transparency_layer = true;
				const Vector<Color> transparent_black = { Color(0, 0, 0, 0) };
				_render_list_with_draw_list(&layer_params, layer_framebuffer, RD::DRAW_CLEAR_COLOR_0, transparent_black, 0.0f, 0u, p_render_data->render_region);

				// RR composites the layer over its output and keeps the result in its history: a NaN or Inf an effect's
				// shader computes there became a black area that spread as the camera moved (godot3d's fights with
				// skeleton archers and mages). In the noisy color the same texel stays a local black spot. An invalid
				// texel adds and covers nothing, so it is cleared to 0 in the copy RR reads.
				if (!rb->has_texture(RB_SCOPE_DLSS_RR, SNAME("transparency_layer_finite"))) {
					rb->create_texture(RB_SCOPE_DLSS_RR, SNAME("transparency_layer_finite"), rb->get_base_data_format(), RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT, RD::TEXTURE_SAMPLES_1);
				}
				const RID layer_drawn = rr_transparency_layer;
				rr_transparency_layer = rb->get_texture(RB_SCOPE_DLSS_RR, SNAME("transparency_layer_finite"));
				copy_effects->copy_to_rect(layer_drawn, rr_transparency_layer, Rect2i(Point2i(), rb->get_internal_size()), false, false, false, false, false, false, true);
			}
		}
		front_layer_reflection = RID();

		RD::get_singleton()->draw_command_end_label();
	}

	RENDER_TIMESTAMP("Resolve");

	RD::get_singleton()->draw_command_begin_label("Resolve");

	if (use_msaa) {
		// RT writes internal_texture and the path tracer writes velocity itself, so
		// only depth needs resolving.
		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
		}
	}

	RD::get_singleton()->draw_command_end_label();

	{
		RENDER_TIMESTAMP("Process Post Transparent Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, p_render_data);
	}

	rb->set_depth_reconstruct_requested(using_depth_reconstruct);

	if (using_upscaling || using_taa) {
		DLSSRRGuideBuffers dlss_rr_guides;
		if (raytracing && raytracing->dlss_rr_has_buffers(rb.ptr())) {
			dlss_rr_guides.active = true;
			dlss_rr_guides.diffuse_albedo = raytracing->dlss_rr_get_diffuse_albedo(rb.ptr());
			dlss_rr_guides.specular_albedo = raytracing->dlss_rr_get_specular_albedo(rb.ptr());
			dlss_rr_guides.normal_roughness = raytracing->dlss_rr_get_normal_roughness(rb.ptr());
			dlss_rr_guides.specular_hit_dist = raytracing->dlss_rr_get_specular_hit_dist(rb.ptr());
			dlss_rr_guides.color_before_transparency = raytracing->dlss_rr_get_color_before_transparency(rb.ptr());
			dlss_rr_guides.transparency_layer = rr_transparency_layer;
			dlss_rr_guides.depth = raytracing->dlss_rr_get_depth(rb.ptr());
		}
		_render_3d_upscaling(p_render_data, scale_type, using_taa, time_step, dlss_rr_guides);
	}

	if (scale_type != SCALE_3D_DLSS) {
		if (feeds_fsr_frame_generation) {
			_prepare_fsr_frame_generation(p_render_data, time_step);
		} else if (RendererRD::FSRFrameGenerationEffect::is_source(rb->get_render_target())) {
			RendererRD::FSRFrameGenerationEffect::stop();
		}
	}

	_debug_draw_cluster(rb);

	RENDER_TIMESTAMP("Tonemap");

	_render_buffers_post_process_and_tonemap(p_render_data);

	_render_buffers_debug_draw(p_render_data);
}

// Raytracing methods

bool RenderForwardClusteredPT::_rr_transparency_layer_enabled() {
	// GODOT_RR_TRANSPARENCY_LAYER=0: the effects stay in RR's noisy input, as before (comparisons).
	static const bool off = OS::get_singleton()->get_environment("GODOT_RR_TRANSPARENCY_LAYER") == "0";
	return !off;
}

bool RenderForwardClusteredPT::_rr_transparency_layer_surface(const GeometryInstanceSurfaceDataCache *p_surf) {
	// Unshaded: nothing in it is traced, so nothing to denoise; RR smeared such effects (godot3d's buff halo and its
	// sparkles, moving without motion vectors) as if they were noise. Add writes no alpha into the layer (its pipelines
	// there, PipelineKey::rr_transparency_layer). Sub and mul darken what is under
	// them, which the premultiplied layer cannot hold.
	// Not a pathtracing_rasterized surface (a sprite): RR moves the layer by the motion vectors under it, which the layer
	// does not write, so a card standing in front of a farther background smeared as the camera moved. Drawn into the
	// color, the card is noise free there too.
	const SceneShaderForwardClustered::ShaderData *shader = p_surf->shader;
	if (!shader || !shader->unshaded || (p_surf->rt_pass_flags & GeometryInstanceSurfaceDataCache::FLAG_PT_RASTERIZED) || shader->uses_screen_texture || shader->uses_depth_texture || shader->uses_normal_texture) {
		return false;
	}
	return shader->blend_mode == RendererRD::MaterialStorage::ShaderData::BLEND_MODE_MIX || shader->blend_mode == RendererRD::MaterialStorage::ShaderData::BLEND_MODE_PREMULTIPLIED_ALPHA ||
			shader->blend_mode == RendererRD::MaterialStorage::ShaderData::BLEND_MODE_ADD;
}

bool RenderForwardClusteredPT::_transparent_pass_traces_shadows() {
	// GODOT_RT_TRANSPARENT_SHADOWS=0: the shadow maps instead, rendered for every light again (comparisons).
	static const bool off = OS::get_singleton()->get_environment("GODOT_RT_TRANSPARENT_SHADOWS") == "0";
	return RD::get_singleton()->has_feature(RD::SUPPORTS_RAY_QUERY) && !off;
}

bool RenderForwardClusteredPT::environment_pathtracing_replaces_shadow_maps(RID p_env) const {
	// Until the path tracer is set up (its first frame), the transparent pass may still read the shadow maps.
	return environment_get_pathtracing_enabled(p_env) && raytracing && raytracing->get_shader() && raytracing->get_shader()->is_usable() && _transparent_pass_traces_shadows();
}

bool RenderForwardClusteredPT::_setup_rt() {
	if (!RD::get_singleton()->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE)) {
		WARN_PRINT_ONCE("Raytracing not supported on this device.");
		return false;
	}

	if (!raytracing) {
		raytracing = memnew(RenderRaytracing);
		raytracing->initialize(this);
		raytracing->shader = SceneShaderRaytracing::get_singleton();
		String rt_defines;
		rt_defines += "\n#define RT 1\n";
		rt_defines += "\n#define MAX_ROUGHNESS_LOD " + itos(get_roughness_layers() - 1) + ".0\n";
		if (is_using_radiance_octmap_array()) {
			rt_defines += "\n#define USE_RADIANCE_OCTMAP_ARRAY \n";
		}
		if (RD::get_singleton()->has_feature(RD::SUPPORTS_OPACITY_MICROMAP)) {
			// The ray queries may meet opacity micromaps (D3D12 then allocates them with AllocateRayQuery2).
			rt_defines += "\n#define RT_OPACITY_MICROMAPS\n";
		}
		raytracing->shader->init(rt_defines);
		if (!raytracing->shader->is_usable()) {
			// Nothing else of the path tracer is compiled or run: a driver whose shader translation cannot handle its
			// shaders (missing ray tracing features) rasterizes instead of rendering nothing or failing further on.
			ERR_PRINT("Path tracing is unavailable on this device: its base ray tracing pipeline could not be built. Rendering with rasterization instead.");
		}
	}

	return raytracing->get_shader()->is_usable();
}

bool RenderForwardClusteredPT::setup_raytracing_for_shader_baker() {
	if (!_setup_rt()) {
		return false;
	}
	if (_transparent_pass_traces_shadows()) {
		scene_shader.enable_rt_shadows_shader_group();
	}
	return true;
}

bool RenderForwardClusteredPT::_rt_precaches_hit_groups() {
	// GODOT_RT_HG_PRECACHE=0: a hit group is queued when the TLAS first meets its material, as before (comparisons).
	static const bool off = OS::get_singleton()->get_environment("GODOT_RT_HG_PRECACHE") == "0";
	return !off;
}

void RenderForwardClusteredPT::_rt_precache_hit_groups() {
	if (rt_precache_materials.is_empty() || !raytracing) {
		return;
	}
	raytracing->precache_hit_groups(rt_precache_materials);
	rt_precache_materials.clear();
}

void RenderForwardClusteredPT::_rt_publish_pending_hit_groups() {
	// Surfaces built since the last drain may add some: counted as one until then.
	const uint32_t pending = raytracing->get_shader()->get_pending_hit_group_count() + (rt_precache_materials.is_empty() ? 0 : 1);
	rt_pending_hit_groups.set(pending);
}

void RenderForwardClusteredPT::update() {
	RenderForwardClustered::update();

	if (!rt_precache_materials_enabled) {
		return;
	}
	// Frames that trace nothing (a loading screen over a hidden 3D view) still compile what the instances created meanwhile
	// need, for the variant traced last.
	SceneShaderRaytracing *rt_shader = raytracing->get_shader();
	_rt_precache_hit_groups();
	rt_shader->drain_completed_compiles();
	rt_shader->finalize_custom_shaders();
	_rt_publish_pending_hit_groups();
}

uint32_t RenderForwardClusteredPT::pathtracing_get_pending_hit_group_count() const {
	return rt_pending_hit_groups.get();
}

void RenderForwardClusteredPT::_age_out_motion_vectors(const RenderDataRD *p_render_data) {
	if (!p_render_data) {
		return;
	}
	const uint64_t frame = RSG::rasterizer->get_frame_number();

	// Process RT-only instances
	if (p_render_data->rt_instances) {
		for (uint32_t i = 0; i < (uint32_t)p_render_data->rt_instances->size(); i++) {
			GeometryInstanceForwardClustered *inst =
					static_cast<GeometryInstanceForwardClustered *>((*p_render_data->rt_instances)[i]);
			if (inst) {
				inst->age_out_motion(frame);
			}
		}
	}

	// Process raster instances
	if (p_render_data->instances) {
		for (uint32_t i = 0; i < (uint32_t)p_render_data->instances->size(); i++) {
			GeometryInstanceForwardClustered *inst =
					static_cast<GeometryInstanceForwardClustered *>((*p_render_data->instances)[i]);
			if (inst) {
				inst->age_out_motion(frame);
			}
		}
	}
}

void RenderForwardClusteredPT::_free_rt_viewport_state(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);
	if (p_render_buffers->has_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO)) {
		p_render_buffers->clear_context(RB_SCOPE_DLSS_RR);
	}
	if (p_render_buffers->has_texture(RB_SCOPE_RAYTRACING, RB_TEX_RAYTRACING)) {
		p_render_buffers->clear_context(RB_SCOPE_RAYTRACING);
	}
	if (raytracing) {
		raytracing->free_viewport_state(p_render_buffers);
	}
}

void RenderForwardClusteredPT::_render_buffers_debug_draw(const RenderDataRD *p_render_data) {
	RenderForwardClustered::_render_buffers_debug_draw(p_render_data);

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RID render_target = rb->get_render_target();

	// DLSS Ray Reconstruction debug views
	if (raytracing && raytracing->dlss_rr_has_buffers(rb.ptr())) {
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		RID fb = texture_storage->render_target_get_rd_framebuffer(render_target);

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DLSS_RR_DIFFUSE_ALBEDO) {
			copy_effects->copy_to_fb_rect(raytracing->dlss_rr_get_diffuse_albedo(rb.ptr()), fb, Rect2(Vector2(), rtsize), false, false);
		}

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DLSS_RR_SPECULAR_ALBEDO) {
			copy_effects->copy_to_fb_rect(raytracing->dlss_rr_get_specular_albedo(rb.ptr()), fb, Rect2(Vector2(), rtsize), false, false);
		}

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DLSS_RR_NORMAL_ROUGHNESS) {
			copy_effects->copy_to_fb_rect(raytracing->dlss_rr_get_normal_roughness(rb.ptr()), fb, Rect2(Vector2(), rtsize), false, false);
		}

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DLSS_RR_SPECULAR_HIT_DIST) {
			copy_effects->copy_to_fb_rect(raytracing->dlss_rr_get_specular_hit_dist(rb.ptr()), fb, Rect2(Vector2(), rtsize), false, true);
		}
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_RECONSTRUCTED_DEPTH && rb->has_texture(RB_SCOPE_BUFFERS, RB_TEX_RECONSTRUCTED_DEPTH)) {
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		RID fb = texture_storage->render_target_get_rd_framebuffer(render_target);
		RID depth_tex = rb->get_texture(RB_SCOPE_BUFFERS, RB_TEX_RECONSTRUCTED_DEPTH);
		copy_effects->copy_to_fb_rect(depth_tex, fb, Rect2(Vector2(), rtsize), false, true, false, false, RID(), false, false, false, false, Rect2(), 1.0, true, RendererRD::CopyEffects::COPY_TO_FB_FLAG_MODE_LOG_LUMINANCE);
	}
}

RenderForwardClusteredPT::RenderForwardClusteredPT() {
}

RenderForwardClusteredPT::~RenderForwardClusteredPT() {
	if (raytracing) {
		memdelete(raytracing);
	}
}
