/**************************************************************************/
/*  render_forward_clustered_pt.h                                         */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/effects/finite_layer.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"

namespace RendererSceneRenderImplementation {

// Path-tracing / DLSS variant of the clustered renderer. Inherits all of the
// raster setup and resource management from RenderForwardClustered and overrides
// only the scene render entry points to add the raytraced opaque path, DLSS Ray
// Reconstruction, and the associated debug visualizations.
class RenderForwardClusteredPT : public RenderForwardClustered {
	/* Raytracing */

	RenderRaytracing *raytracing = nullptr;

	bool _setup_rt();
	void _age_out_motion_vectors(const RenderDataRD *p_render_data);

	// Hit groups compiled when their instances are created rather than first traced (rt_precache_materials).
	static bool _rt_precaches_hit_groups();
	void _rt_precache_hit_groups();
	void _rt_publish_pending_hit_groups();
	// SceneShaderRaytracing::get_pending_hit_group_count(), read by any thread (RenderingServer).
	SafeNumeric<uint32_t> rt_pending_hit_groups;
	// The transparent pass traces its lights' shadows through the TLAS (USE_RT_SHADOWS) on this device.
	static bool _transparent_pass_traces_shadows();
	// DLSS-RR's transparency layer (Integration Guide 3.4.10): the transparent surfaces drawn apart, upscaled by RR without
	// being denoised. Unshaded ones only (nothing traced in them), blended so that the target holds premultiplied color and
	// coverage (mix, premultiplied alpha), reading nothing from the screen.
	static bool _rr_transparency_layer_enabled();
	// The layer's copy for RR, NaN and Inf cleared, and the warning naming the effects drawn where they were.
	RendererRD::FiniteLayer *finite_layer = nullptr;
	void _warn_non_finite_layer(const RenderDataRD *p_render_data, const RendererRD::FiniteLayer::Report &p_report, uint32_t p_from, uint32_t p_to);
	static bool _rr_transparency_layer_surface(const GeometryInstanceSurfaceDataCache *p_surf);
	// The factor that brings the rasterized instances' probes (pt_probes) to the units a lightmap capture is given in.
	float _pt_probe_sh_scale(const RenderDataRD *p_render_data, float p_luminance_multiplier);

protected:
	virtual void _render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) override;
	virtual void _render_buffers_debug_draw(const RenderDataRD *p_render_data) override;
	virtual void _free_rt_viewport_state(RenderSceneBuffersRD *p_render_buffers) override;

public:
	virtual bool environment_pathtracing_replaces_shadow_maps(RID p_env) const override;
	virtual void update() override;
	virtual uint32_t pathtracing_get_pending_hit_group_count() const override;
	// Sets up what the first path traced frame would (the path tracer's shaders, the transparent pass' ray traced
	// shadow variants), for the shader baker in an editor that has traced nothing. False when the device cannot path
	// trace.
	bool setup_raytracing_for_shader_baker();

	RenderForwardClusteredPT();
	~RenderForwardClusteredPT();
};

} // namespace RendererSceneRenderImplementation
