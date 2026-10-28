/**************************************************************************/
/*  render_raytracing.cpp                                                 */
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

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/templates/pair.h"
#include "servers/rendering/renderer_rd/environment/fog.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/environment_storage.h"

using namespace RendererSceneRenderImplementation;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

RenderRaytracing *RenderRaytracing::displace_guard_target = nullptr;

void RenderRaytracing::initialize(RenderForwardClustered *p_owner) {
	owner = p_owner;
	displace_guard_target = this;
	bindless_block = memnew(BindlessBlock);
	bindless_array_block = memnew(BindlessBlock);

	// Initialize merged MultiMesh BLAS compute shader.
	Vector<String> merge_modes;
	merge_modes.push_back("\n");
	merge_modes.push_back("\n#define MODE_INDEXED\n");
	mm_merge_shader.shader.initialize(merge_modes);
	mm_merge_shader.version = mm_merge_shader.shader.version_create();
	for (int i = 0; i < MergeShader::MODE_MAX; i++) {
		mm_merge_shader.version_shader[i] = mm_merge_shader.shader.version_get_shader(mm_merge_shader.version, i);
		mm_merge_shader.pipeline[i] = RD::get_singleton()->compute_pipeline_create(mm_merge_shader.version_shader[i]);
	}

	Vector<String> compact_modes;
	compact_modes.push_back("\n#define MODE_MARK\n");
	compact_modes.push_back("\n#define MODE_ASSIGN\n");
	compact_modes.push_back("\n#define MODE_REMAP\n");
	lod_compact_shader.shader.initialize(compact_modes);
	lod_compact_shader.version = lod_compact_shader.shader.version_create();
	for (int i = 0; i < LodCompactShader::MODE_MAX; i++) {
		lod_compact_shader.version_shader[i] = lod_compact_shader.shader.version_get_shader(lod_compact_shader.version, i);
		lod_compact_shader.pipeline[i] = RD::get_singleton()->compute_pipeline_create(lod_compact_shader.version_shader[i]);
	}
	lod_view_dummy_buffer = RD::get_singleton()->storage_buffer_create(16);
	displace_dummy_buffer = RD::get_singleton()->storage_buffer_create(16);

	tlas_expand_supported = RD::get_singleton()->tlas_instance_layout_is_standard() && RD::get_singleton()->has_feature(RD::SUPPORTS_BUFFER_DEVICE_ADDRESS);
	if (tlas_expand_supported) {
		Vector<String> expand_modes;
		expand_modes.push_back("\n");
		tlas_expand_shader.shader.initialize(expand_modes);
		tlas_expand_shader.version = tlas_expand_shader.shader.version_create();
		tlas_expand_shader.version_shader = tlas_expand_shader.shader.version_get_shader(tlas_expand_shader.version, 0);
		tlas_expand_shader.pipeline = RD::get_singleton()->compute_pipeline_create(tlas_expand_shader.version_shader);
	}

	Vector<String> fog_modes;
	fog_modes.push_back("\n");
	volumetric_fog_shader.shader.initialize(fog_modes);
	volumetric_fog_shader.version = volumetric_fog_shader.shader.version_create();
	volumetric_fog_shader.version_shader = volumetric_fog_shader.shader.version_get_shader(volumetric_fog_shader.version, 0);
	volumetric_fog_shader.pipeline = RD::get_singleton()->compute_pipeline_create(volumetric_fog_shader.version_shader);

	Vector<String> sky_guide_modes;
	sky_guide_modes.push_back("\n");
	sky_guide_shader.shader.initialize(sky_guide_modes);
	sky_guide_shader.version = sky_guide_shader.shader.version_create();
	sky_guide_shader.version_shader = sky_guide_shader.shader.version_get_shader(sky_guide_shader.version, 0);
	sky_guide_shader.pipeline = RD::get_singleton()->compute_pipeline_create(sky_guide_shader.version_shader);

	Vector<String> sky_sampling_modes;
	sky_sampling_modes.push_back("\n"); // MODE_LUMINANCE
	sky_sampling_modes.push_back("\n#define USE_RADIANCE_OCTMAP_ARRAY\n"); // MODE_LUMINANCE_ARRAY
	sky_sampling_modes.push_back("\n#define MODE_CDF\n"); // MODE_CDF
	sky_sampling_shader.shader.initialize(sky_sampling_modes);
	sky_sampling_shader.version = sky_sampling_shader.shader.version_create();
	for (int i = 0; i < SkySamplingShader::MODE_MAX; i++) {
		sky_sampling_shader.version_shader[i] = sky_sampling_shader.shader.version_get_shader(sky_sampling_shader.version, i);
		sky_sampling_shader.pipeline[i] = RD::get_singleton()->compute_pipeline_create(sky_sampling_shader.version_shader[i]);
	}

	Vector<String> output_copy_modes;
	output_copy_modes.push_back("\n");
	output_copy_shader.shader.initialize(output_copy_modes);
	output_copy_shader.version = output_copy_shader.shader.version_create();
	output_copy_shader.version_shader = output_copy_shader.shader.version_get_shader(output_copy_shader.version, 0);
	output_copy_shader.pipeline = RD::get_singleton()->compute_pipeline_create(output_copy_shader.version_shader);
}

RenderRaytracing::~RenderRaytracing() {
	// A guard readback still in flight arrives after this object.
	if (displace_guard_target == this) {
		displace_guard_target = nullptr;
	}
	for (KeyValue<RenderSceneBuffersRD *, RTViewportState *> &kv : viewport_states) {
		_free_viewport_state_internal(kv.value);
	}
	viewport_states.clear();

	cleanup_caches();

	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->free_rid(mat_ubo_pool_buffer);
		mat_ubo_pool_buffer = RID();
	}

	if (bindless_block) {
		memdelete(bindless_block);
		bindless_block = nullptr;
	}
	if (bindless_array_block) {
		memdelete(bindless_array_block);
		bindless_array_block = nullptr;
	}
	if (shader) {
		memdelete(shader);
		shader = nullptr;
	}

	mm_merge_shader.shader.version_free(mm_merge_shader.version);
	lod_compact_shader.shader.version_free(lod_compact_shader.version);
	if (lod_view_dummy_buffer.is_valid()) {
		RD::get_singleton()->free_rid(lod_view_dummy_buffer);
	}
	if (displace_dummy_buffer.is_valid()) {
		RD::get_singleton()->free_rid(displace_dummy_buffer);
	}
	if (mm_displace_job_buffer.is_valid()) {
		RD::get_singleton()->free_rid(mm_displace_job_buffer);
	}
	if (tlas_expand_supported) {
		tlas_expand_shader.shader.version_free(tlas_expand_shader.version);
	}
	for (RID buffer : { tlas_expand_job_buffer, tlas_expand_geometry_template_buffer, tlas_expand_material_template_buffer }) {
		if (buffer.is_valid()) {
			RD::get_singleton()->free_rid(buffer);
		}
	}
	volumetric_fog_shader.shader.version_free(volumetric_fog_shader.version);
	sky_guide_shader.shader.version_free(sky_guide_shader.version);
	sky_sampling_shader.shader.version_free(sky_sampling_shader.version);
	output_copy_shader.shader.version_free(output_copy_shader.version);
}

// ---------------------------------------------------------------------------
// Per-viewport state lifecycle
// ---------------------------------------------------------------------------

RTViewportState *RenderRaytracing::_get_or_create_viewport_state(const RenderDataRD *p_render_data) {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RenderSceneBuffersRD *key = p_render_data->render_buffers.ptr();
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::Iterator it = viewport_states.find(key);
	if (it != viewport_states.end()) {
		return it->value;
	}
	RTViewportState *state = memnew(RTViewportState);
	viewport_states.insert(key, state);
	return state;
}

RTViewportState *RenderRaytracing::_get_viewport_state(const RenderDataRD *p_render_data) const {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RenderSceneBuffersRD *key = p_render_data->render_buffers.ptr();
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::ConstIterator it = viewport_states.find(key);
	return (it != viewport_states.end()) ? it->value : nullptr;
}

void RenderRaytracing::_free_viewport_state_internal(RTViewportState *p_state) {
	if (!p_state) {
		return;
	}
	if (p_state->tlas.is_valid()) {
		RD::get_singleton()->free_rid(p_state->tlas);
	}
	if (p_state->geometry_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->geometry_buffer);
	}
	if (p_state->material_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->material_buffer);
	}
	if (p_state->motion_index_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_index_buffer);
	}
	if (p_state->motion_transform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_transform_buffer);
	}
	if (p_state->tlas_instance_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->tlas_instance_buffer);
	}
	if (p_state->light_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->light_buffer);
	}
	if (p_state->light_grid_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->light_grid_buffer);
	}
	if (p_state->sky_sampling_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->sky_sampling_buffer);
	}
	if (p_state->params_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->params_buffer);
	}
	if (p_state->decal_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->decal_buffer);
	}
	if (p_state->decal_grid_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->decal_grid_buffer);
	}
	if (p_state->scene_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_state->scene_uniform_set)) {
		RD::get_singleton()->free_rid(p_state->scene_uniform_set);
	}
	memdelete(p_state);
}

void RenderRaytracing::free_viewport_state(RenderSceneBuffersRD *p_render_buffers) {
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::Iterator it = viewport_states.find(p_render_buffers);
	if (it == viewport_states.end()) {
		return;
	}
	_free_viewport_state_internal(it->value);
	viewport_states.remove(it);
}

// ---------------------------------------------------------------------------
// Raytracing / DLSS-RR output textures (stored on the render buffers via named
// scopes) and the per-viewport DLSS upscaler context (in RTViewportState).
// ---------------------------------------------------------------------------

void RenderRaytracing::rt_ensure_textures(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);

	uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	if (!p_render_buffers->has_texture(RB_SCOPE_RAYTRACING, RB_TEX_RAYTRACING)) {
		p_render_buffers->create_texture(RB_SCOPE_RAYTRACING, RB_TEX_RAYTRACING, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_RAYTRACING, RB_TEX_RT_DEPTH)) {
		p_render_buffers->create_texture(RB_SCOPE_RAYTRACING, RB_TEX_RT_DEPTH, RD::DATA_FORMAT_R32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_RAYTRACING, RB_TEX_RT_FRONT_LAYER)) {
		p_render_buffers->create_texture(RB_SCOPE_RAYTRACING, RB_TEX_RT_FRONT_LAYER, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
}

bool RenderRaytracing::rt_has_texture(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_RAYTRACING, RB_TEX_RAYTRACING);
}

RID RenderRaytracing::rt_get_texture(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_RAYTRACING, RB_TEX_RAYTRACING);
}

bool RenderRaytracing::rt_has_depth_texture(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_RAYTRACING, RB_TEX_RT_DEPTH);
}

RID RenderRaytracing::rt_get_depth_texture(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_RAYTRACING, RB_TEX_RT_DEPTH);
}

RID RenderRaytracing::rt_get_front_layer_texture(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	if (!p_render_buffers->has_texture(RB_SCOPE_RAYTRACING, RB_TEX_RT_FRONT_LAYER)) {
		return RID();
	}
	return p_render_buffers->get_texture(RB_SCOPE_RAYTRACING, RB_TEX_RT_FRONT_LAYER);
}

void RenderRaytracing::dlss_rr_ensure_buffers(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);

	if (p_render_buffers->has_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO)) {
		return;
	}

	uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	// Diffuse Albedo: linear RGB surface color for non-metals (RGBA16F; DLSS-RR expects
	// linear albedo, and float precision avoids 8-bit dark-value banding without gamma packing).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	// Specular Albedo: RGB specular reflection color (RGBA16F; needs the accuracy, fixes banding artifacts).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_ALBEDO, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	// Normal + Roughness: World space normals (RGB) + roughness (A). RGBA16F: the DLSS-RR programming guide asks for
	// RGB_F16 or RGB_F32 normals (4.1.3), and the shaders declare the image rgba16f, a format the view must match.
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_NORMAL_ROUGHNESS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	// Specular Hit Distance: Single channel distance (R16F is sufficient).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_HIT_DIST, RD::DATA_FORMAT_R16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	// The noisy color before the transparent pass draws on it: RR tells the transparents apart and keeps them
	// sharp. The guide says to use the noisy color's format, or precision differences read as transparency.
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_COLOR_BEFORE_TRANSPARENCY, p_render_buffers->get_base_data_format(), usage_bits, RD::TEXTURE_SAMPLES_1);
	// Depth, NDC as rt_depth: where a transparent surface's reflection dominates, that surface's (DLSS-RR Integration
	// Guide, 3.4.1: depth, normal and specular hit distance set consistently on the transparent surface).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DEPTH, RD::DATA_FORMAT_R32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
}

void RenderRaytracing::dlss_rr_free_buffers(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);
	p_render_buffers->clear_context(RB_SCOPE_DLSS_RR);
}

bool RenderRaytracing::dlss_rr_has_buffers(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO);
}

RID RenderRaytracing::dlss_rr_get_diffuse_albedo(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO);
}

RID RenderRaytracing::dlss_rr_get_specular_albedo(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_ALBEDO);
}

RID RenderRaytracing::dlss_rr_get_normal_roughness(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_NORMAL_ROUGHNESS);
}

RID RenderRaytracing::dlss_rr_get_color_before_transparency(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_COLOR_BEFORE_TRANSPARENCY);
}

RID RenderRaytracing::dlss_rr_get_specular_hit_dist(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_HIT_DIST);
}

RID RenderRaytracing::dlss_rr_get_depth(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DEPTH);
}

// ---------------------------------------------------------------------------
// Material UBO sub-allocation pool
//
// Single device-address storage buffer of MAT_UBO_POOL_TOTAL_BYTES, divided
// into MAT_UBO_POOL_CAPACITY fixed-size slots. Allocate/release just bump a
// next-slot counter and a free-list. Per-material UBO writes are
// buffer_update at slot offset, no allocation. mat.uniform_address is
// pool_bda + slot * slot_size, so the closest-hit shader's
// CustomMaterialUniforms(addr) cast lands on the correct slot.
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t MAT_UBO_POOL_SLOT_SIZE = 512;
constexpr uint32_t MAT_UBO_POOL_CAPACITY = 100000;
constexpr uint64_t MAT_UBO_POOL_TOTAL_BYTES = uint64_t(MAT_UBO_POOL_SLOT_SIZE) * MAT_UBO_POOL_CAPACITY;
} // namespace

void RenderRaytracing::mat_ubo_pool_ensure_initialized() {
	if (mat_ubo_pool_buffer.is_valid()) {
		return;
	}
	Vector<uint8_t> init;
	init.resize(MAT_UBO_POOL_TOTAL_BYTES);
	memset(init.ptrw(), 0, MAT_UBO_POOL_TOTAL_BYTES);
	mat_ubo_pool_buffer = RD::get_singleton()->storage_buffer_create(
			MAT_UBO_POOL_TOTAL_BYTES, init, 0,
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->set_resource_name(mat_ubo_pool_buffer, "RT Material UBO Pool");
		mat_ubo_pool_bda = RD::get_singleton()->buffer_get_device_address(mat_ubo_pool_buffer);
	}
}

uint32_t RenderRaytracing::mat_ubo_pool_allocate() {
	mat_ubo_pool_ensure_initialized();
	if (!mat_ubo_pool_buffer.is_valid()) {
		return UINT32_MAX;
	}
	if (mat_ubo_pool_free_count > 0) {
		--mat_ubo_pool_free_count;
		return mat_ubo_pool_free_slots[mat_ubo_pool_free_count];
	}
	if (mat_ubo_pool_next_slot >= MAT_UBO_POOL_CAPACITY) {
		ERR_PRINT_ONCE("RT Material UBO Pool exhausted; falling back to dedicated buffer.");
		return UINT32_MAX;
	}
	return mat_ubo_pool_next_slot++;
}

void RenderRaytracing::mat_ubo_pool_release(uint32_t p_slot) {
	if (p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	if (mat_ubo_pool_free_count < mat_ubo_pool_free_slots.size()) {
		mat_ubo_pool_free_slots[mat_ubo_pool_free_count] = p_slot;
	} else {
		mat_ubo_pool_free_slots.push_back(p_slot);
	}
	++mat_ubo_pool_free_count;
}

void RenderRaytracing::mat_ubo_pool_update(uint32_t p_slot, const void *p_data, uint32_t p_size) {
	if (!mat_ubo_pool_buffer.is_valid() || p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	uint32_t size = MIN(p_size, MAT_UBO_POOL_SLOT_SIZE);
	RD::get_singleton()->buffer_update(mat_ubo_pool_buffer, uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE, size, p_data);
}

uint64_t RenderRaytracing::mat_ubo_pool_get_address(uint32_t p_slot) const {
	return mat_ubo_pool_bda + uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE;
}

// ---------------------------------------------------------------------------
// Cache management
// ---------------------------------------------------------------------------

void RenderRaytracing::cleanup_caches() {
	// Before the surfaces' BLASes, which free the micromap BLASes built from the same buffers.
	_omm_evict(true);

	for (uint32_t i = 0; i < surface_chunks.size(); i++) {
		if (surface_chunks[i]) {
			for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
				RTCacheEntry *entry = &surface_chunks[i][j];
				if (entry->ptr) {
					// Unless the mesh was freed first, which frees the BLAS built from its buffers.
					if (entry->ptr->blas.is_valid() && RD::get_singleton()->acceleration_structure_is_valid(entry->ptr->blas)) {
						RD::get_singleton()->free_rid(entry->ptr->blas);
					}
					memdelete(entry->ptr);
					entry->ptr = nullptr;
				}
			}
			memdelete_arr(surface_chunks[i]);
		}
	}
	surface_chunks.clear();

	RD *rd = RD::get_singleton();

	// Free all cached deformed surface data.
	{
		LocalVector<RID> live = deformed_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTDeformedCacheEntry *e = deformed_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			if (e->ptr) {
				// A BLAS goes with the mesh buffers it was built from.
				if (e->ptr->blas.is_valid() && rd->acceleration_structure_is_valid(e->ptr->blas)) {
					rd->free_rid(e->ptr->blas);
				}
				memdelete(e->ptr);
				e->ptr = nullptr;
			}
			if (e->owned_vb_full.is_valid()) {
				rd->free_rid(e->owned_vb_full);
				e->owned_vb_full = RID();
			}
			if (e->prev_pos_vb.is_valid()) {
				rd->free_rid(e->prev_pos_vb);
				e->prev_pos_vb = RID();
			}
			if (e->displaced_vb.is_valid()) {
				rd->free_rid(e->displaced_vb);
				e->displaced_vb = RID();
			}
			deformed_pool.free(live[i]);
		}
	}
	if (displace_job_buffer.is_valid()) {
		rd->free_rid(displace_job_buffer);
		displace_job_buffer = RID();
		displace_job_capacity = 0;
	}
	if (displace_guard_buffer.is_valid()) {
		rd->free_rid(displace_guard_buffer);
		displace_guard_buffer = RID();
		displace_guard_capacity = 0;
	}
	displace_guard_pending.clear();

	// Free all merged MultiMesh BLAS data.
	{
		LocalVector<RID> live = merged_mm_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			_free_merged_mm_entry(live[i]);
		}
	}
	mm_handles.clear();
	deformed_active_this_frame.clear();
	merged_mm_active_this_frame.clear();
	multimesh_buffers_read_this_frame.clear();

	for (KeyValue<RTLodViewKey, RTLodView> &kv : lod_views) {
		_free_lod_view(kv.value);
	}
	lod_views.clear();

	// Free all cached material data
	for (uint32_t i = 0; i < material_chunks.size(); i++) {
		if (material_chunks[i]) {
			for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
				RTMaterialCacheEntry *entry = &material_chunks[i][j];
				if (entry->ptr) {
					if (entry->ptr->uniform_buffer.is_valid()) {
						mat_ubo_dedicated_buffers.erase(entry->ptr->uniform_buffer);
						RD::get_singleton()->free_rid(entry->ptr->uniform_buffer);
					}
					if (entry->ptr->uniform_pool_slot != UINT32_MAX) {
						mat_ubo_pool_release(entry->ptr->uniform_pool_slot);
					}
					memdelete(entry->ptr);
					entry->ptr = nullptr;
				}
			}
			memdelete_arr(material_chunks[i]);
		}
	}
	material_chunks.clear();

	// Every slot is free again: the pool goes too, mat_ubo_pool_ensure_initialized() makes it again.
	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->free_rid(mat_ubo_pool_buffer);
		mat_ubo_pool_buffer = RID();
	}
	mat_ubo_pool_bda = 0;
	mat_ubo_pool_free_slots.clear();
	mat_ubo_pool_free_count = 0;
	mat_ubo_pool_next_slot = 0;

	// Reset counters
	material_free_slots.clear();
	next_material_slot = 0;
	vram_used = 0;
	last_trace_frame = 0;
	cache_hits = 0;
	cache_misses = 0;
}

void RenderRaytracing::free_caches_if_unused() {
	// A few frames of margin: a view can skip frames (an UPDATE_WHEN_VISIBLE SubViewport), and rebuilding the caches
	// is a hitch.
	const uint32_t RT_UNUSED_FRAMES = 10;
	if (last_trace_frame == 0 || RSG::rasterizer->get_frame_number() - last_trace_frame <= RT_UNUSED_FRAMES) {
		return;
	}
	for (KeyValue<RenderSceneBuffersRD *, RTViewportState *> &kv : viewport_states) {
		_free_viewport_state_internal(kv.value);
	}
	viewport_states.clear();
	cleanup_caches();
}

// ---------------------------------------------------------------------------
// RID helpers
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::get_rid_index(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() & 0xFFFFFFFFULL);
}

uint32_t RenderRaytracing::get_rid_version(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() >> 32);
}

RTCacheEntry *RenderRaytracing::get_surface_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;

	// Grow vector if needed, initializing new slots to nullptr
	while (chunk_idx >= surface_chunks.size()) {
		surface_chunks.push_back(nullptr);
	}

	if (!surface_chunks[chunk_idx]) {
		surface_chunks.set(chunk_idx, memnew_arr(RTCacheEntry, RT_CACHE_CHUNK_SIZE));
		for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
			surface_chunks[chunk_idx][i] = RTCacheEntry();
		}
	}

	return &surface_chunks[chunk_idx][entry_idx];
}

RTMaterialCacheEntry *RenderRaytracing::get_material_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;

	// Grow vector if needed, initializing new slots to nullptr
	while (chunk_idx >= material_chunks.size()) {
		material_chunks.push_back(nullptr);
	}

	if (!material_chunks[chunk_idx]) {
		material_chunks.set(chunk_idx, memnew_arr(RTMaterialCacheEntry, RT_CACHE_CHUNK_SIZE));
		for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
			material_chunks[chunk_idx][i] = RTMaterialCacheEntry();
		}
	}

	return &material_chunks[chunk_idx][entry_idx];
}

uint32_t RenderRaytracing::allocate_material_slot() {
	if (!material_free_slots.is_empty()) {
		uint32_t slot = material_free_slots[material_free_slots.size() - 1];
		material_free_slots.resize(material_free_slots.size() - 1);
		return slot;
	}
	return next_material_slot++;
}

// ---------------------------------------------------------------------------
// Slot-pool access helpers
// ---------------------------------------------------------------------------

RTDeformedCacheEntry *RenderRaytracing::_access_deformed_slot(RID &r_handle) {
	RTDeformedCacheEntry *entry = deformed_pool.get_or_null(r_handle);
	if (!entry) {
		// Stale or never-allocated handle -> fresh slot. RID_Owner default-
		// constructs the entry, so all members start at their cleared values.
		r_handle = deformed_pool.make_rid();
		entry = deformed_pool.get_or_null(r_handle);
	}
	deformed_active_this_frame.push_back(r_handle);
	return entry;
}

const RTMergedMMEntry *RenderRaytracing::_get_merged_mm_entry(RID p_mm_rid, uint32_t p_surface_index) {
	const uint32_t li = p_mm_rid.get_local_index();
	if (li >= mm_handles.size()) {
		return nullptr;
	}
	const MMSurfaceHandles &handles = mm_handles[li];
	if (handles.mm_validator != uint32_t(p_mm_rid.get_id() >> 32) || p_surface_index >= handles.per_surface.size()) {
		return nullptr;
	}
	const RTMergedMMEntry *entry = merged_mm_pool.get_or_null(handles.per_surface[p_surface_index]);
	return entry && entry->blas.is_valid() ? entry : nullptr;
}

void RenderRaytracing::_free_merged_mm_entry(RID p_handle) {
	RTMergedMMEntry *e = merged_mm_pool.get_or_null(p_handle);
	if (!e) {
		return;
	}
	RD *rd = RD::get_singleton();
	if (e->blas.is_valid() && rd->acceleration_structure_is_valid(e->blas)) {
		rd->free_rid(e->blas); // Not when freed with its micromap.
	}
	if (e->merge_uniform_set.is_valid() && rd->uniform_set_is_valid(e->merge_uniform_set)) {
		rd->free_rid(e->merge_uniform_set);
	}
	const RID buffers[] = { e->merged_vtx_buffer, e->merged_attr_buffer, e->replicated_idx_buffer, e->merged_prev_buffer, e->omm_index_buffer };
	for (const RID &buffer : buffers) {
		if (buffer.is_valid()) {
			rd->free_rid(buffer);
		}
	}
	merged_mm_pool.free(p_handle);
}

RTMergedMMEntry *RenderRaytracing::_access_merged_mm_slot(RID &r_handle) {
	RTMergedMMEntry *entry = merged_mm_pool.get_or_null(r_handle);
	if (!entry) {
		r_handle = merged_mm_pool.make_rid();
		entry = merged_mm_pool.get_or_null(r_handle);
	}
	merged_mm_active_this_frame.push_back(r_handle);
	return entry;
}

// ---------------------------------------------------------------------------
// Per-frame preparation
// ---------------------------------------------------------------------------

void RenderRaytracing::prepare_frame() {
	blass.clear();
	blas_transforms.clear();
	instance_flags.clear();
	instance_masks.clear();
	front_layer_instance_count = 0;
	procedural_instance_masks = 0;
	mm_displace_requests.clear();
	sbt_offsets.clear();
	geometry_data.clear();
	material_data.clear();
	motion_indices.clear();
	motion_transforms.clear();
	tlas_expand_jobs.clear();
	tlas_expand_geometry_templates.clear();
	tlas_expand_material_templates.clear();
	tlas_expand_instance_count = 0;
	tlas_expand_motion_count = 0;
	deformed_active_this_frame.clear();
	merged_mm_active_this_frame.clear();
	multimesh_buffers_read_this_frame.clear();

	const uint32_t current_frame = RSG::rasterizer->get_frame_number();
	RD *rd = RD::get_singleton();

	// TTL-evict stale deformed-surface entries.
	{
		static const uint32_t DEFORMED_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracing/deformed_mesh_cache_ttl_frames");
		LocalVector<RID> live = deformed_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTDeformedCacheEntry *e = deformed_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			const uint32_t last_used = MAX(e->last_used_frame, e->displaced_frame);
			if (last_used == 0 || current_frame - last_used <= DEFORMED_CACHE_TTL) {
				continue;
			}
			if (e->ptr) {
				// A BLAS goes with the mesh buffers it was built from.
				if (e->ptr->blas.is_valid() && rd->acceleration_structure_is_valid(e->ptr->blas)) {
					rd->free_rid(e->ptr->blas);
				}
				memdelete(e->ptr);
				e->ptr = nullptr;
			}
			if (e->owned_vb_full.is_valid()) {
				rd->free_rid(e->owned_vb_full);
				e->owned_vb_full = RID();
			}
			if (e->prev_pos_vb.is_valid()) {
				rd->free_rid(e->prev_pos_vb);
				e->prev_pos_vb = RID();
			}
			if (e->displaced_vb.is_valid()) {
				rd->free_rid(e->displaced_vb);
				e->displaced_vb = RID();
			}
			deformed_pool.free(live[i]);
		}
	}

	// TTL-evict stale merged-MultiMesh entries.
	{
		static const uint32_t MM_BLAS_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracing/multimesh_blas_cache_ttl_frames");
		// The entries no view traced for MM_BLAS_CACHE_COLD_FRAMES are kept for when the camera comes back, up to this many
		// bytes of merged buffers: a camera crossing a large map otherwise kept every MultiMesh it saw for the whole TTL
		// (Solfarer: 1.5 GB unused beside 1.5 GB in use). Past it, the least recently used go, down to three quarters of it so
		// that this pass does not run every frame. Not before: a game shows and hides MultiMeshes for a few frames (Solfarer's
		// map chunks: thousands come back 2 to 5 frames later, almost none after 60), and those were made again every time.
		static constexpr uint32_t MM_BLAS_CACHE_COLD_FRAMES = 60;
		static const uint64_t MM_BLAS_CACHE_UNUSED_BYTES = uint64_t(int64_t(GLOBAL_GET("rendering/pathtracing/multimesh_blas_cache_unused_size_mb"))) << 20;
		LocalVector<RID> live = merged_mm_pool.get_owned_list();
		LocalVector<Pair<uint32_t, RID>> unused; // Last used frame, handle.
		uint64_t unused_bytes = 0;
		for (uint32_t i = 0; i < live.size(); i++) {
			RTMergedMMEntry *e = merged_mm_pool.get_or_null(live[i]);
			if (!e || e->last_used_frame == 0) {
				continue;
			}
			const uint32_t age = current_frame - e->last_used_frame;
			if (age > MM_BLAS_CACHE_TTL) {
				_free_merged_mm_entry(live[i]);
			} else if (age > MM_BLAS_CACHE_COLD_FRAMES) {
				unused.push_back({ e->last_used_frame, live[i] });
				unused_bytes += e->buffer_bytes;
			}
		}
		if (unused_bytes > MM_BLAS_CACHE_UNUSED_BYTES) {
			struct OldestFirst {
				_FORCE_INLINE_ bool operator()(const Pair<uint32_t, RID> &p_a, const Pair<uint32_t, RID> &p_b) const { return p_a.first < p_b.first; }
			};
			unused.sort_custom<OldestFirst>();
			const uint64_t target = MM_BLAS_CACHE_UNUSED_BYTES / 4 * 3;
			for (uint32_t i = 0; i < unused.size() && unused_bytes > target; i++) {
				const RTMergedMMEntry *e = merged_mm_pool.get_or_null(unused[i].second);
				unused_bytes -= e->buffer_bytes;
				_free_merged_mm_entry(unused[i].second);
			}
		}

		// The entries of a MultiMesh freed since go now, not after the TTL: a game that frees and makes MultiMeshes as its
		// units move (Solfarer's map chunks) otherwise kept gigabytes of merged buffers. A few hundred handles per frame.
		const uint32_t MM_HANDLES_SWEPT_PER_FRAME = 512;
		RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
		const uint32_t sweep_count = MIN(MM_HANDLES_SWEPT_PER_FRAME, mm_handles.size());
		for (uint32_t n = 0; n < sweep_count; n++) {
			if (mm_handles_sweep_cursor >= mm_handles.size()) {
				mm_handles_sweep_cursor = 0;
			}
			const uint32_t li = mm_handles_sweep_cursor++;
			MMSurfaceHandles &handles = mm_handles[li];
			if (handles.per_surface.is_empty() || mesh_storage->owns_multimesh(RID::from_uint64((uint64_t(handles.mm_validator) << 32) | li))) {
				continue;
			}
			for (RID &handle : handles.per_surface) {
				_free_merged_mm_entry(handle);
				handle = RID();
			}
			handles.per_surface.clear();
		}

		LocalVector<RTLodViewKey> stale;
		for (KeyValue<RTLodViewKey, RTLodView> &kv : lod_views) {
			if (current_frame - kv.value.last_used_frame > MM_BLAS_CACHE_TTL) {
				_free_lod_view(kv.value);
				stale.push_back(kv.key);
			}
		}
		for (const RTLodViewKey &key : stale) {
			lod_views.erase(key);
		}
	}

	// Surfaces and materials no view traced for a while (a dungeon left, a map whose meshes a game rebuilds): a few chunks
	// of each per frame. Their BLAS and buffers otherwise stayed until no view traced at all, growing with every trip.
	static const uint32_t STATIC_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracing/static_cache_ttl_frames");
	const uint32_t SWEEP_CHUNKS_PER_FRAME = 16;
	for (uint32_t sweep = 0; sweep < SWEEP_CHUNKS_PER_FRAME; sweep++) {
		if (!surface_chunks.is_empty()) {
			surface_sweep_chunk = (surface_sweep_chunk + 1) % surface_chunks.size();
			RTCacheEntry *chunk = surface_chunks[surface_sweep_chunk];
			for (uint32_t j = 0; chunk && j < RT_CACHE_CHUNK_SIZE; j++) {
				RTCacheEntry &entry = chunk[j];
				if (!entry.ptr || entry.last_used_frame == 0 || current_frame - entry.last_used_frame <= STATIC_CACHE_TTL) {
					continue;
				}
				// Unless the mesh was freed first, which freed the BLAS built from its buffers.
				if (entry.ptr->blas.is_valid() && rd->acceleration_structure_is_valid(entry.ptr->blas)) {
					rd->free_rid(entry.ptr->blas);
				}
				memdelete(entry.ptr);
				entry = RTCacheEntry();
			}
		}
		if (!material_chunks.is_empty()) {
			material_sweep_chunk = (material_sweep_chunk + 1) % material_chunks.size();
			RTMaterialCacheEntry *chunk = material_chunks[material_sweep_chunk];
			for (uint32_t j = 0; chunk && j < RT_CACHE_CHUNK_SIZE; j++) {
				RTMaterialCacheEntry &entry = chunk[j];
				if (!entry.ptr || entry.last_used_frame == 0 || current_frame - entry.last_used_frame <= STATIC_CACHE_TTL) {
					continue;
				}
				if (entry.ptr->uniform_buffer.is_valid()) {
					mat_ubo_dedicated_buffers.erase(entry.ptr->uniform_buffer);
					rd->free_rid(entry.ptr->uniform_buffer);
				}
				if (entry.ptr->uniform_pool_slot != UINT32_MAX) {
					mat_ubo_pool_release(entry.ptr->uniform_pool_slot);
				}
				memdelete(entry.ptr);
				entry = RTMaterialCacheEntry();
			}
		}
	}

	// Finish async HG compiles so live_ready_mask matches this frame (sync path fills at build_tlas end).
	SceneShaderRaytracing::get_singleton()->drain_completed_compiles();

	// Reset per-frame metrics
	cache_hits = 0;
	cache_misses = 0;

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	if (!bindless_block->is_initialized()) {
		bindless_block->initialize(RD::get_singleton(), texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE));
	}
	if (!bindless_array_block->is_initialized()) {
		bindless_array_block->initialize(RD::get_singleton(), texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE));
	}
	bindless_block->begin_frame();
	bindless_array_block->begin_frame();
}

// ---------------------------------------------------------------------------
// Surface processing
// ---------------------------------------------------------------------------

RTSurfaceData *RenderRaytracing::process_surface(
		const void *p_surf,
		void *p_mesh_surface,
		uint32_t p_surface_invalidation_counter,
		const Transform3D &p_transform,
		LocalVector<RID> &r_dirty_blas_list) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	RID mesh_rid = surf->owner->data->base;
	if (surf->owner->data->base_type == RSE::INSTANCE_MULTIMESH) {
		RID underlying = mesh_storage->multimesh_get_mesh(mesh_rid);
		if (underlying.is_valid()) {
			mesh_rid = underlying;
		}
	}

	uint32_t cache_key = (mesh_rid.get_local_index() << 8) | (surf->surface_index & 0xFF);
	uint32_t mesh_version = get_rid_version(mesh_rid);
	RTCacheEntry *entry = get_surface_cache_entry(cache_key);

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	bool needs_refresh = !entry->ptr ||
			entry->cached_rid_version != mesh_version ||
			entry->cached_counter != p_surface_invalidation_counter;

	if (!needs_refresh && entry->ptr->blas.is_valid()) {
		cache_hits++;
		entry->last_used_frame = current_frame;
		return entry->ptr;
	}

	cache_misses++;

	// Allocate or reuse entry
	if (!entry->ptr) {
		entry->ptr = memnew(RTSurfaceData);
	} else if (entry->ptr->blas.is_valid()) {
		// Same mesh, surface data changed: the BLAS is still live after a region update, but already freed with the old
		// vertex buffer when the mesh replaced its surfaces (AnimatedSprite3D, Label3D...). A version mismatch means the old
		// mesh was deleted, which freed it too.
		if (entry->cached_rid_version == mesh_version && RD::get_singleton()->acceleration_structure_is_valid(entry->ptr->blas)) {
			RD::get_singleton()->free_rid(entry->ptr->blas);
		}
		entry->ptr->blas = RID();
	}

	RTSurfaceData *surf_data = entry->ptr;

	_populate_surface_blas(p_mesh_surface, RID(), false, false, false, cache_key, surf_data, r_dirty_blas_list);

	surf->cached_final_transform_valid = false;

	if (!surf_data->blas.is_valid()) {
		return surf_data;
	}

	entry->cached_counter = p_surface_invalidation_counter;
	entry->cached_rid_version = mesh_version;
	entry->last_used_frame = current_frame;

	return surf_data;
}

// ---------------------------------------------------------------------------
// Deformed surface processing
// ---------------------------------------------------------------------------

RTSurfaceData *RenderRaytracing::process_deformed_surface(
		const void *p_surf,
		void *p_mesh_surface,
		const RTDeformedGeometrySource &p_source,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);

	if (!p_source.current_vb.is_valid()) {
		return nullptr;
	}

	RD *rd = RD::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	// Compute the deformed VB layout.
	uint64_t fmt = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	bool is_2d = fmt & RSE::ARRAY_FLAG_USE_2D_VERTICES;
	bool has_n = fmt & RSE::ARRAY_FORMAT_NORMAL;
	bool has_t = fmt & RSE::ARRAY_FORMAT_TANGENT;
	uint32_t position_stride = is_2d ? (uint32_t)sizeof(float) * 2u : (uint32_t)sizeof(float) * 3u;
	uint32_t tbn_section_per_vertex = has_n ? (has_t ? 8u : 4u) : 0u;
	uint32_t per_vertex_bytes = position_stride + tbn_section_per_vertex;
	uint32_t full_size = per_vertex_bytes * vertex_count;
	uint32_t pos_size = position_stride * vertex_count;

	if (vertex_count == 0 || full_size == 0) {
		return nullptr;
	}

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	uint64_t buffer_id = p_source.current_vb.get_id();

	RTDeformedCacheEntry *entry_ptr = _access_deformed_slot(surf->rt_deformed_handle);
	ERR_FAIL_NULL_V(entry_ptr, nullptr);
	RTDeformedCacheEntry &entry = *entry_ptr;

	bool layout_changed = entry.cached_vertex_count != vertex_count || entry.cached_full_size != full_size;
	bool data_changed = layout_changed ||
			entry.cached_change_stamp != p_source.change_stamp ||
			entry.cached_buffer_id != buffer_id;
	// Out of view and far: the BLAS keeps the positions it has (and the hit shaders the vertices they match) until the
	// surface's turn. Only a refit waits: a new or resized surface is built now.
	const bool defer_update = p_source.defer_update && data_changed && !layout_changed && !p_source.force_rebuild && entry.ptr && entry.blas_built_once && entry.prev_pos_seeded &&
			entry.cached_key_version == p_source.cache_version && entry.cached_surface_counter == p_source.surface_counter && entry.omm_micromap == entry.built_omm_micromap &&
			entry.owned_vb_full_capacity >= full_size;
	if (defer_update) {
		data_changed = false;
	}
	// A micromap freed (evicted) is no longer one to build with.
	if (entry.omm_micromap.is_valid() && !rd->acceleration_structure_is_valid(entry.omm_micromap)) {
		entry.omm_micromap = RID();
	}
	bool needs_full_rebuild = !entry.ptr || !entry.blas_built_once || layout_changed ||
			entry.cached_key_version != p_source.cache_version ||
			entry.cached_surface_counter != p_source.surface_counter ||
			entry.omm_micromap != entry.built_omm_micromap;

	// Storage: a displaced surface's refit reads the positions its BLAS has (raytracing_displace.glsl's NaN guard).
	BitField<RD::BufferCreationBits> owned_full_flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT |
			RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT | RD::BUFFER_CREATION_AS_STORAGE_BIT;
	BitField<RD::BufferCreationBits> prev_flags = RD::BUFFER_CREATION_AS_STORAGE_BIT |
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT;

	// Reallocate owned storage if the surface grew.
	if (entry.owned_vb_full_capacity < full_size) {
		if (entry.ptr && entry.ptr->blas.is_valid()) {
			rd->free_rid(entry.ptr->blas);
			entry.ptr->blas = RID();
			entry.blas_built_once = false;
		}
		if (entry.owned_vb_full.is_valid()) {
			rd->free_rid(entry.owned_vb_full);
		}
		entry.owned_vb_full = rd->vertex_buffer_create(full_size, Span<uint8_t>(), owned_full_flags);
		ERR_FAIL_COND_V(!entry.owned_vb_full.is_valid(), nullptr);
		entry.owned_vb_full_capacity = full_size;
		rd->set_resource_name(entry.owned_vb_full, "RT deformed owned VB [" + itos(p_source.cache_key) + "]");
		entry.prev_pos_seeded = false;
		needs_full_rebuild = true;
	}
	if (entry.prev_pos_vb_capacity < pos_size) {
		if (entry.prev_pos_vb.is_valid()) {
			rd->free_rid(entry.prev_pos_vb);
		}
		entry.prev_pos_vb = rd->storage_buffer_create(pos_size, Vector<uint8_t>(), 0, prev_flags);
		ERR_FAIL_COND_V(!entry.prev_pos_vb.is_valid(), nullptr);
		entry.prev_pos_vb_capacity = pos_size;
		rd->set_resource_name(entry.prev_pos_vb, "RT deformed prev pos VB [" + itos(p_source.cache_key) + "]");
		entry.prev_pos_seeded = false;
	}

	// Per-frame copies run exactly once per frame, even when the same surface is visible from multiple viewports.
	const bool first_touch_this_frame = entry.last_used_frame != current_frame;
	if (first_touch_this_frame) {
		if (p_source.prev_offset != UINT32_MAX && data_changed) {
			rd->buffer_copy(p_source.prev_vb, entry.prev_pos_vb, p_source.prev_offset, 0, pos_size);
			entry.prev_pos_seeded = true;
		} else if (entry.prev_pos_seeded) {
			rd->buffer_copy(entry.owned_vb_full, entry.prev_pos_vb, 0, 0, pos_size);
		}
		if (data_changed || !entry.prev_pos_seeded) {
			rd->buffer_copy(p_source.current_vb, entry.owned_vb_full, 0, 0, full_size);
		}
		if (!entry.prev_pos_seeded) {
			// Seed prev = current so motion vectors are zero on the first frame.
			rd->buffer_copy(entry.owned_vb_full, entry.prev_pos_vb, 0, 0, pos_size);
			entry.prev_pos_seeded = true;
		}
	}

	if (needs_full_rebuild) {
		cache_misses++;
		if (!entry.ptr) {
			entry.ptr = memnew(RTSurfaceData);
		}
		if (entry.ptr->blas.is_valid()) {
			// Freed already with the micromap it was built with, when that one was evicted.
			if (rd->acceleration_structure_is_valid(entry.ptr->blas)) {
				rd->free_rid(entry.ptr->blas);
			}
			entry.ptr->blas = RID();
		}
		_populate_surface_blas(p_mesh_surface, entry.owned_vb_full, true, true, true,
				static_cast<uint32_t>(p_source.cache_key), entry.ptr, r_dirty_blas_list, entry.omm_micromap, entry.omm_level);
		entry.blas_built_once = entry.ptr->blas.is_valid();
		entry.built_omm_micromap = entry.omm_micromap;
	} else if (data_changed) {
		cache_misses++;
		if (p_source.force_rebuild) {
			r_dirty_blas_list.push_back(entry.ptr->blas);
		} else {
			r_dirty_blas_update_list.push_back(entry.ptr->blas);
		}
	} else {
		cache_hits++;
	}

	if (entry.ptr && entry.ptr->blas.is_valid()) {
		entry.ptr->geometry.flags |= RT_GEOM_FLAG_DEFORMED;
		uint64_t prev_addr = rd->buffer_get_device_address(entry.prev_pos_vb);
		entry.ptr->geometry.prev_vertex_buffer_address_lo = static_cast<uint32_t>(prev_addr & 0xFFFFFFFFULL);
		entry.ptr->geometry.prev_vertex_buffer_address_hi = static_cast<uint32_t>(prev_addr >> 32);
	}

	surf->cached_final_transform_valid = false;

	if (defer_update) {
		// Still a change for the next frame. The motion until the update is reported then: prev_pos_vb takes the positions
		// owned_vb_full keeps, those the BLAS was last updated with.
		out_of_view_deferred += first_touch_this_frame ? 1 : 0;
	} else {
		entry.cached_change_stamp = p_source.change_stamp;
		entry.cached_buffer_id = buffer_id;
	}
	entry.cached_key_version = p_source.cache_version;
	entry.cached_surface_counter = p_source.surface_counter;
	entry.cached_vertex_count = vertex_count;
	entry.cached_full_size = full_size;
	entry.last_used_frame = current_frame;

	return entry.ptr;
}

// ---------------------------------------------------------------------------
// Surface BLAS helper (shared between static and deformed paths)
// ---------------------------------------------------------------------------

// Fills RTSurfaceData geometry metadata from the surface format.
// Returns the RIDs needed for BLAS creation so _populate_surface_blas can use them.
static void _fill_surface_geometry_data(
		void *p_mesh_surface,
		bool p_force_uncompressed,
		RTSurfaceData *r_surf_data,
		RID *r_vertex_buffer = nullptr,
		RID *r_attribute_buffer = nullptr,
		RID *r_index_buffer = nullptr) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	bool attributes_compressed = surface_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
	bool compressed = attributes_compressed && !p_force_uncompressed;
	bool is_2d = surface_format & RSE::ARRAY_FLAG_USE_2D_VERTICES;

	r_surf_data->is_compressed = compressed;

	if (compressed) {
		AABB surface_aabb = mesh_storage->mesh_surface_get_aabb(p_mesh_surface);
		r_surf_data->aabb_transform.basis = Basis::from_scale(surface_aabb.size);
		r_surf_data->aabb_transform.origin = surface_aabb.position;
	} else {
		r_surf_data->aabb_transform = Transform3D();
	}

	RT_GeometryData &geom = r_surf_data->geometry;
	memset(&geom, 0, sizeof(geom));

	RID vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	RID attribute_buffer = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0);

	if (r_vertex_buffer) {
		*r_vertex_buffer = vertex_buffer;
	}
	if (r_attribute_buffer) {
		*r_attribute_buffer = attribute_buffer;
	}
	if (r_index_buffer) {
		*r_index_buffer = index_buffer;
	}

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);

	geom.vertex_count = vertex_count;

	// Position stride
	uint32_t position_stride;
	if (is_2d) {
		position_stride = sizeof(float) * 2;
	} else if (compressed) {
		position_stride = sizeof(uint16_t) * 4;
	} else {
		position_stride = sizeof(float) * 3;
	}
	geom.position_stride = position_stride;

	// Normal/tangent layout
	uint32_t normal_stride;
	uint32_t tangent_stride = 0;
	geom.normal_byte_offset = RT_OFFSET_NONE;
	geom.tangent_byte_offset = RT_OFFSET_NONE;
	uint32_t current_offset = position_stride * vertex_count;

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;

	if (compressed) {
		normal_stride = sizeof(uint16_t) * 2;
		if (has_normal) {
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		}
	} else {
		if (has_normal && has_tangent) {
			normal_stride = sizeof(uint16_t) * 4;
			tangent_stride = sizeof(uint16_t) * 4;
			geom.normal_byte_offset = current_offset;
			geom.tangent_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else if (has_normal) {
			normal_stride = sizeof(uint16_t) * 2;
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else {
			normal_stride = 0;
		}
	}
	geom.normal_stride = normal_stride;
	geom.tangent_stride = tangent_stride;
	geom.flags = compressed ? RT_GEOM_FLAG_COMPRESSED : (attributes_compressed ? RT_GEOM_FLAG_COMPRESSED_ATTRIBUTES : 0);

	if (compressed) {
		AABB surface_aabb = mesh_storage->mesh_surface_get_aabb(p_mesh_surface);
		geom.aabb_size_x = surface_aabb.size.x;
		geom.aabb_size_y = surface_aabb.size.y;
		geom.aabb_size_z = surface_aabb.size.z;
		geom.aabb_pos_x = surface_aabb.position.x;
		geom.aabb_pos_y = surface_aabb.position.y;
		geom.aabb_pos_z = surface_aabb.position.z;
	} else {
		geom.aabb_size_x = 1.0f;
		geom.aabb_size_y = 1.0f;
		geom.aabb_size_z = 1.0f;
		geom.aabb_pos_x = 0.0f;
		geom.aabb_pos_y = 0.0f;
		geom.aabb_pos_z = 0.0f;
	}

	// Attribute buffer layout
	uint32_t attrib_offset = 0;
	geom.uv_byte_offset = RT_OFFSET_NONE;
	geom.color_byte_offset = RT_OFFSET_NONE;
	geom.uv2_byte_offset = RT_OFFSET_NONE;

	if (surface_format & RSE::ARRAY_FORMAT_COLOR) {
		geom.color_byte_offset = attrib_offset;
		attrib_offset += sizeof(uint32_t);
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV) {
		geom.uv_byte_offset = attrib_offset;
		attrib_offset += attributes_compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV2) {
		geom.uv2_byte_offset = attrib_offset;
		attrib_offset += attributes_compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	for (int ci = 0; ci < RSE::ARRAY_CUSTOM_COUNT; ci++) {
		const uint32_t fmt_shift[RSE::ARRAY_CUSTOM_COUNT] = { RSE::ARRAY_FORMAT_CUSTOM0_SHIFT, RSE::ARRAY_FORMAT_CUSTOM1_SHIFT, RSE::ARRAY_FORMAT_CUSTOM2_SHIFT, RSE::ARRAY_FORMAT_CUSTOM3_SHIFT };
		if (surface_format & (1ULL << (RSE::ARRAY_CUSTOM0 + ci))) {
			uint32_t fmt = (surface_format >> fmt_shift[ci]) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
			const uint32_t fmtsize[RSE::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
			// Read by vertex() as CUSTOM0-3.
			geom.custom_byte_offsets |= attrib_offset << (ci * 8);
			geom.custom_formats |= (RT_CUSTOM_FORMAT_PRESENT | fmt) << (ci * 4);
			attrib_offset += fmtsize[fmt];
		}
	}
	geom.attribute_stride = attrib_offset;

	// UV scale (fp16 packed, matches GLSL unpackHalf2x16)
	Vector4 uv_scale = mesh_storage->mesh_surface_get_uv_scale(p_mesh_surface);
	geom.uv_scale_packed = (uint32_t(Math::make_half_float(uv_scale.y)) << 16) | Math::make_half_float(uv_scale.x);
	geom.uv2_scale_packed = (uint32_t(Math::make_half_float(uv_scale.w)) << 16) | Math::make_half_float(uv_scale.z);

	// Index format (no device address — caller fills those in)
	if (index_buffer.is_valid() && index_count > 0) {
		bool is_16bit = vertex_count <= 65536 && vertex_count > 0;
		geom.index_format = is_16bit ? RT_INDEX_FORMAT_UINT16 : RT_INDEX_FORMAT_UINT32;
		geom.primitive_count = index_count / 3;
	} else {
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = vertex_count / 3;
	}
}

// A geometry without indices gets its vertex buffer's address as index address, never 0. The shaders do not read it
// (get_triangle_indices_ex() tests RT_INDEX_FORMAT_NONE first), but AMD's Vulkan shader compiler (RX 6600, Adrenalin)
// loads it ahead of that test when the read is invariant in a loop (the decal loop of the hit shaders): from address 0,
// a GPU fault (READ_INVALID at 0) and a lost device. The vertex buffer holds at least 24 bytes per triangle (three
// vertices of 8 bytes or more), more than the 12 bytes per triangle such a load reads.
static _FORCE_INLINE_ void _rt_set_unindexed_index_address(RT_GeometryData &r_geometry) {
	if (r_geometry.index_format == RT_INDEX_FORMAT_NONE) {
		r_geometry.index_buffer_address = r_geometry.vertex_buffer_address;
	}
}

void RenderRaytracing::_populate_surface_blas(
		void *p_mesh_surface,
		RID p_vertex_buffer_override,
		bool p_force_uncompressed,
		bool p_prefer_fast_build,
		bool p_allow_update,
		uint32_t p_cache_key,
		RTSurfaceData *r_surf_data,
		LocalVector<RID> &r_dirty_blas_list,
		RID p_opacity_micromap,
		uint32_t p_opacity_micromap_level) {
	RID vertex_buffer, attribute_buffer, index_buffer;
	_fill_surface_geometry_data(p_mesh_surface, p_force_uncompressed, r_surf_data,
			&vertex_buffer, &attribute_buffer, &index_buffer);

	if (p_vertex_buffer_override.is_valid()) {
		vertex_buffer = p_vertex_buffer_override;
	}

	RD *rd = RD::get_singleton();
	RT_GeometryData &geom = r_surf_data->geometry;

	if (vertex_buffer.is_valid()) {
		geom.vertex_buffer_address = rd->buffer_get_device_address(vertex_buffer);
	}
	if (attribute_buffer.is_valid()) {
		geom.attribute_buffer_address = rd->buffer_get_device_address(attribute_buffer);
	}
	if (index_buffer.is_valid() && geom.index_format != RT_INDEX_FORMAT_NONE) {
		geom.index_buffer_address = rd->buffer_get_device_address(index_buffer);
	}
	_rt_set_unindexed_index_address(geom);

	uint32_t vertex_count = geom.vertex_count;
	uint32_t index_count = (geom.index_format != RT_INDEX_FORMAT_NONE) ? geom.primitive_count * 3 : 0;
	uint32_t position_stride = geom.position_stride;

	bool is_2d = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(p_mesh_surface) & RSE::ARRAY_FLAG_USE_2D_VERTICES;
	bool compressed = r_surf_data->is_compressed;

	// Create BLAS using the new geometry-based API.
	{
		RD::DataFormat pos_format;
		if (is_2d) {
			pos_format = RD::DATA_FORMAT_R32G32_SFLOAT;
		} else if (compressed) {
			pos_format = RD::DATA_FORMAT_R16G16B16A16_UNORM;
		} else {
			pos_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		}

		RD::AccelerationStructureGeometry as_geom;
		// Type defaults to TYPE_TRIANGLES; set explicitly for clarity.
		as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
		as_geom.geometry.triangles.vertex_buffer = vertex_buffer;
		as_geom.geometry.triangles.vertex_stride = position_stride;
		as_geom.geometry.triangles.vertex_count = vertex_count;
		as_geom.geometry.triangles.vertex_format = pos_format;

		if (index_buffer.is_valid() && index_count > 0) {
			as_geom.geometry.triangles.index_buffer = index_buffer;
			as_geom.geometry.triangles.index_count = index_count;
		}

		BitField<RD::AccelerationStructureFlagBits> as_flags = p_prefer_fast_build
				? RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT
				: RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
		if (p_allow_update) {
			as_flags.set_flag(RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
		}

		if (p_opacity_micromap.is_valid()) {
			// One entry per triangle, in triangle order (_omm_entry()): no index buffer.
			as_geom.geometry.triangles.opacity_micromap = p_opacity_micromap;
			RD::OpacityMicromapUsage usage;
			usage.count = (index_count > 0 ? index_count : vertex_count) / 3;
			usage.subdivision_level = p_opacity_micromap_level;
			usage.format = RD::OPACITY_MICROMAP_FORMAT_4_STATE;
			as_geom.geometry.triangles.opacity_micromap_usages.push_back(usage);
		}

		r_surf_data->blas = RD::get_singleton()->blas_create({ &as_geom, 1 }, as_flags);
		if (!r_surf_data->blas.is_valid()) {
			return;
		}
		RD::get_singleton()->set_resource_name(r_surf_data->blas,
				String(p_vertex_buffer_override.is_valid() ? "RT BLAS deformed [" : "RT BLAS [") + itos(p_cache_key) + (p_opacity_micromap.is_valid() ? "] OMM" : "]"));
		r_dirty_blas_list.push_back(r_surf_data->blas);
	}
}

// p_threads threads of a shader with local_size_x = 64 and its index at gl_GlobalInvocationID.x +
// gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 64: the groups beyond the device's X limit (65535 on most GPUs, 4M
// threads) go on Y. A MultiMesh's vertices or a surface's micro-triangles reach that count.
static void _rt_dispatch_linear(RenderingDevice *p_rd, RD::ComputeListID p_compute_list, uint32_t p_threads) {
	if (p_threads == 0) {
		return;
	}
	const uint32_t groups = Math::division_round_up(p_threads, 64u);
	const uint32_t max_x = MAX(uint32_t(p_rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X)), 1u);
	const uint32_t x = MIN(groups, max_x);
	p_rd->compute_list_dispatch(p_compute_list, x, Math::division_round_up(groups, x), 1);
}

// The front layer rays (build_tlas()) leave out an unshaded transparent surface: it has no
// reflection. One drawn without the depth test is not where its triangles are: raster draws it over everything, from its
// fragment code (a decal or a volume projected from the depth buffer, in a box drawn cull_front). Traced, its box hid the
// surfaces behind it, which lost their reflection, and lent its own to the water where its walls cross it (godot3d's
// Rot Blossom: a dark outline on the water). And one whose shader declares render_mode front_layer_reflection_disabled:
// the front layer keeps one surface per pixel, and a thin effect in front of water (godot3d's comet tails) took the water's
// traced reflection from it, which fell back to the brighter radiance one (a white streak).
bool RenderRaytracing::_front_layer_skipped(const void *p_surf) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);
	return !surf->shader || surf->shader->unshaded || surf->shader->front_layer_reflection_disabled ||
			surf->shader->rt_depth_test() != SceneShaderForwardClustered::ShaderData::DEPTH_TEST_ENABLED;
}

// ---------------------------------------------------------------------------
// Displacement by material vertex() code
// ---------------------------------------------------------------------------

// Runs the vertex() of every material that moves VERTEX on its surfaces, so their BLAS holds what raster draws
// (Terrain3D builds its whole relief there). The output goes through the deformed path in build_tlas().
void RenderRaytracing::_displace_surfaces(const RenderDataRD *p_render_data) {
	SceneShaderRaytracing *rt_shader = SceneShaderRaytracing::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RD *rd = RD::get_singleton();
	const uint32_t current_frame = RSG::rasterizer->get_frame_number();

	const bool camera_moved = p_render_data->scene_data->cam_transform != displace_camera_transform || p_render_data->scene_data->cam_projection != displace_camera_projection;
	displaced_evaluated = 0;
	displaced_deferred = 0;
	displaced_vertices = 0;
	displace_camera_moved = camera_moved;
	static const float update_distance = GLOBAL_GET("rendering/pathtracing/displacement_update_distance");
	const uint32_t MAX_UPDATE_PERIOD = 8;
	// Refits between two builds of a displaced surface. godot3d's forest (wind, D3D12, 2026-10-22): the path tracer traced
	// as fast with no periodic build at all as with one every 15 refits, which cost 0.25 ms of BLAS builds more; one every
	// 60 costs ~0.1 ms and bounds the drift of larger motion (a terrain's geomorphing).
	const uint32_t DISPLACED_REBUILD_PERIOD = 60;
	// Read every frame, so it can be compared in place; a change builds every displaced surface again, so that both modes
	// start from clean BLASes.
	const bool displaced_refit = GLOBAL_GET_CACHED(bool, "rendering/pathtracing/displaced_refit");
	const bool refit_switched = displaced_refit != displaced_refit_used;
	displaced_refit_used = displaced_refit;
	const Vector3 camera_origin = p_render_data->scene_data->cam_transform.origin;
	displace_camera_transform = p_render_data->scene_data->cam_transform;
	displace_camera_projection = p_render_data->scene_data->cam_projection;

	// The material a surface draws with: its own (a next pass or the overlay has one), else override, then surface
	// override, then mesh.
	auto surface_material = [mesh_storage](const RenderForwardClustered::GeometryInstanceSurfaceDataCache *p_surf) -> RID {
		if (p_surf->material_rid.is_valid()) {
			return p_surf->material_rid;
		}
		if (p_surf->owner->data->material_override.is_valid()) {
			return p_surf->owner->data->material_override;
		}
		if (p_surf->surface_index < p_surf->owner->data->surface_materials.size() && p_surf->owner->data->surface_materials[p_surf->surface_index].is_valid()) {
			return p_surf->owner->data->surface_materials[p_surf->surface_index];
		}
		RID mesh_rid = p_surf->owner->data->base;
		if (mesh_rid.is_valid() && mesh_storage->owns_mesh(mesh_rid)) {
			return mesh_storage->mesh_surface_get_material(mesh_rid, p_surf->surface_index);
		}
		return RID();
	};

	struct Job {
		RTDeformedCacheEntry *entry = nullptr;
		RID handle;
		RID shader;
		RID pipeline;
		RID material_buffer;
		RID blas_positions; // A refit's: the positions its BLAS has.
		uint32_t vertex_count = 0;
	};
	LocalVector<Job> jobs;
	LocalVector<RT_DisplaceJob> job_data;
	displaced_refitted = 0;

	const PagedArray<RenderGeometryInstance *> &rt_instances = *p_render_data->rt_instances;
	for (uint32_t i = 0; i < (uint32_t)rt_instances.size(); i++) {
		const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
				static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(rt_instances[i]);
		// Skinned and blend shape meshes have their own deformed buffer; MultiMesh and procedural geometry are not covered.
		if (!inst || !inst->data || inst->rt_procedural || inst->data->base_type == RSE::INSTANCE_MULTIMESH || inst->mesh_instance.is_valid()) {
			continue;
		}

		for (const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf = inst->surface_caches; surf; surf = surf->next) {
			// Transparent surfaces too, as build_tlas() traces them for their reflections; not those it leaves out.
			const bool front_layer = surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA;
			if ((front_layer && _front_layer_skipped(surf)) || surf->primitive != RSE::PRIMITIVE_TRIANGLES) {
				continue;
			}
			const RID material_rid = surface_material(surf);
			if (!material_rid.is_valid()) {
				continue;
			}
			const uint16_t material_counter = material_storage->material_get_rt_invalidation_counter(material_rid);
			RTMaterialData *mat_data = process_material(material_rid, material_counter);
			if (!mat_data || mat_data->rt_sbt_offset == 0) {
				continue;
			}
			const SceneShaderRaytracing::CustomShaderEntry *cse = rt_shader->get_custom_shader_entry(mat_data->rt_sbt_offset);
			if (!cse || !cse->writes_vertex) {
				continue;
			}
			RID program_shader;
			const RID pipeline = rt_shader->get_displace_pipeline(mat_data->rt_sbt_offset, program_shader);
			if (!pipeline.is_valid()) {
				continue;
			}

			void *mesh_surface = surf->surface;
			const uint64_t format = mesh_storage->mesh_surface_get_format(mesh_surface);
			const uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(mesh_surface);
			if (vertex_count == 0 || (format & RSE::ARRAY_FLAG_USE_2D_VERTICES)) {
				continue;
			}
			const uint32_t tbn_words = (format & RSE::ARRAY_FORMAT_NORMAL) ? ((format & RSE::ARRAY_FORMAT_TANGENT) ? 2 : 1) : 0;
			const uint32_t size = vertex_count * (3 + tbn_words + 3 + 3) * sizeof(uint32_t);

			RTDeformedCacheEntry *entry = _access_deformed_slot(surf->rt_deformed_handle);
			// An instance uniform animated by a script (a wall growing out of the ground) moves the geometry too.
			const uint32_t instance_uniforms_hash = material_storage->global_shader_parameters_instance_hash(inst->shader_uniforms_offset);
			const bool first = entry->displaced_frame == 0;
			bool reshaped = first || entry->displaced_slot != mat_data->rt_sbt_offset || entry->displaced_transform != inst->transform;
			// A refit kept vertices vertex() turned on or off: built again now, even if nothing else moves.
			bool evaluate = reshaped || entry->displaced_guard_rebuild || refit_switched;
			if (!evaluate && (entry->displaced_deferred || entry->displaced_material_counter != material_counter || entry->displaced_instance_uniforms_hash != instance_uniforms_hash || cse->uses_vertex_time || (cse->vertex_reads_camera && camera_moved))) {
				// What vertex() keeps moving (TIME, the camera, a parameter set every frame: Terrain3D's geomorphing, an
				// instance uniform) is evaluated, and its BLAS refit, every frame near the camera and every N frames beyond,
				// N growing with the distance. The surfaces take turns, and the motion between two updates is reported on the
				// update frame.
				uint32_t period = 1;
				if (update_distance > 0.0f) {
					const Vector3 lo = inst->transformed_aabb.position;
					const Vector3 hi = lo + inst->transformed_aabb.size;
					const float distance = Vector3().max(lo - camera_origin).max(camera_origin - hi).length();
					period = CLAMP(uint32_t(distance / update_distance), 1u, MAX_UPDATE_PERIOD);
				}
				evaluate = (current_frame + uint32_t(surf->rt_deformed_handle.get_id())) % period == 0;
				entry->displaced_deferred = !evaluate;
				displaced_deferred += evaluate ? 0 : 1;
			}
			if (entry->displaced_vb_capacity < size) {
				if (entry->displaced_vb.is_valid()) {
					rd->free_rid(entry->displaced_vb);
				}
				entry->displaced_vb = rd->storage_buffer_create(size, Vector<uint8_t>(), 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
				ERR_CONTINUE(!entry->displaced_vb.is_valid());
				rd->set_resource_name(entry->displaced_vb, "RT displaced VB");
				entry->displaced_vb_capacity = size;
				evaluate = true;
				reshaped = true;
			}
			// What can turn triangles on or off builds the BLAS again: a new surface or buffer, another hit group, the
			// instance moved (Terrain3D's clipmap: the only time its holes move), another micromap. Otherwise (TIME, the
			// camera, a material parameter or an instance uniform set every frame: Terrain3D's _camera_pos) it is refit,
			// and every DISPLACED_REBUILD_PERIOD refits built again, as the BVH of a refit keeps the topology of its build.
			// The refit keeps the triangles' state (raytracing_displace.glsl); a vertex turning on or off sets the guard,
			// which builds the BLAS again a frame later (DXR and Vulkan: an update cannot change a primitive's active
			// state). rendering/pathtracing/displaced_refit off builds again every time.
			const bool rebuild = !displaced_refit || refit_switched || reshaped || entry->displaced_guard_rebuild || entry->displaced_refits >= DISPLACED_REBUILD_PERIOD ||
					!entry->ptr || !rd->acceleration_structure_is_valid(entry->ptr->blas) || !entry->blas_built_once ||
					!entry->owned_vb_full.is_valid() || entry->cached_vertex_count != vertex_count || entry->omm_micromap != entry->built_omm_micromap;
			entry->displaced_frame = current_frame;
			entry->displaced_rebuild = evaluate && rebuild;
			entry->displaced_prev_offset = vertex_count * (3 + tbn_words + 3) * sizeof(uint32_t);
			if (!evaluate) {
				continue;
			}
			if (rebuild) {
				// The surfaces built together (a level loaded) take their periodic rebuilds in turns.
				entry->displaced_refits = first ? uint32_t(surf->rt_deformed_handle.get_id() % DISPLACED_REBUILD_PERIOD) : 0;
				entry->displaced_guard_rebuild = false;
			} else {
				entry->displaced_refits++;
				displaced_refitted++;
			}
			entry->displaced_stamp++;
			entry->displaced_deferred = false;
			displaced_evaluated++;
			displaced_vertices += vertex_count;
			entry->displaced_slot = mat_data->rt_sbt_offset;
			entry->displaced_material_counter = material_counter;
			entry->displaced_instance_uniforms_hash = instance_uniforms_hash;
			entry->displaced_transform = inst->transform;

			RT_DisplaceJob data = {};
			RTSurfaceData source;
			RID vertex_buffer, attribute_buffer;
			_fill_surface_geometry_data(mesh_surface, false, &source, &vertex_buffer, &attribute_buffer);
			data.geometry = source.geometry;
			if (vertex_buffer.is_valid()) {
				data.geometry.vertex_buffer_address = rd->buffer_get_device_address(vertex_buffer);
			}
			if (attribute_buffer.is_valid()) {
				data.geometry.attribute_buffer_address = rd->buffer_get_device_address(attribute_buffer);
			}
			RendererRD::MaterialStorage::store_transform(inst->transform, data.model_matrix);
			RendererRD::MaterialStorage::store_transform(inst->transform.affine_inverse(), data.inv_model_matrix);
			// A teleported instance has no motion from its transform (Terrain3D moves its clipmap that way).
			const Transform3D &prev_transform = inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED ? inst->prev_transform : inst->transform;
			RendererRD::MaterialStorage::store_transform(prev_transform, data.prev_model_matrix);
			RendererRD::MaterialStorage::store_transform(prev_transform.affine_inverse(), data.prev_inv_model_matrix);
			data.material_address = mat_data->data.uniform_address;
			data.tbn_words = tbn_words;
			data.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
			data.mm_instance_count = 0;
			data.refit_guard = rebuild ? 0 : 1;
			job_data.push_back(data);

			mat_ubo_pool_ensure_initialized();
			Job job;
			job.entry = entry;
			job.handle = surf->rt_deformed_handle;
			job.blas_positions = rebuild ? RID() : entry->owned_vb_full;
			job.shader = program_shader;
			job.pipeline = pipeline;
			job.material_buffer = mat_data->uniform_buffer.is_valid() ? mat_data->uniform_buffer : mat_ubo_pool_buffer;
			job.vertex_count = vertex_count;
			jobs.push_back(job);
		}
	}

	if (jobs.is_empty()) {
		return;
	}

	// The textures the materials above registered are in the tables already.
	const RID bindless_set = bindless_block->get_secondary_uniform_set(jobs[0].shader, 1);
	const RID bindless_array_set = bindless_array_block->get_secondary_uniform_set(jobs[0].shader, 2);
	if (!bindless_set.is_valid() || !bindless_array_set.is_valid()) {
		for (Job &job : jobs) {
			job.entry->displaced_frame = 0;
		}
		ERR_FAIL_MSG("RT: no bindless tables for the displacement pass.");
	}

	if (displace_job_capacity < job_data.size()) {
		if (displace_job_buffer.is_valid()) {
			rd->free_rid(displace_job_buffer);
		}
		displace_job_capacity = Math::next_power_of_2(job_data.size());
		displace_job_buffer = rd->storage_buffer_create(displace_job_capacity * sizeof(RT_DisplaceJob));
		rd->set_resource_name(displace_job_buffer, "RT displace jobs");
	}
	rd->buffer_update(displace_job_buffer, 0, job_data.size() * sizeof(RT_DisplaceJob), job_data.ptr());
	if (displace_guard_capacity < displace_job_capacity) {
		if (displace_guard_buffer.is_valid()) {
			rd->free_rid(displace_guard_buffer);
		}
		displace_guard_capacity = displace_job_capacity;
		displace_guard_buffer = rd->storage_buffer_create(displace_guard_capacity * sizeof(uint32_t));
		rd->set_resource_name(displace_guard_buffer, "RT displace guard counts");
	}
	const uint32_t guard_size = jobs.size() * sizeof(uint32_t);
	LocalVector<RID> guard_handles;
	for (const Job &job : jobs) {
		if (job.blas_positions.is_valid()) {
			guard_handles.resize(jobs.size());
			break;
		}
	}
	if (!guard_handles.is_empty()) {
		rd->buffer_clear(displace_guard_buffer, 0, guard_size);
	}

	const LocalVector<RD::Uniform> frame_uniforms = _displace_frame_uniforms(displace_job_buffer);

	RD::get_singleton()->draw_command_begin_label("RT Displace Surfaces");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	RID bound_pipeline;
	LocalVector<RID> job_sets;
	for (uint32_t i = 0; i < jobs.size(); i++) {
		const Job &job = jobs[i];
		if (job.pipeline != bound_pipeline) {
			rd->compute_list_bind_compute_pipeline(compute_list, job.pipeline);
			rd->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache_vec(job.shader, 0, frame_uniforms), 0);
			rd->compute_list_bind_uniform_set(compute_list, bindless_set, 1);
			rd->compute_list_bind_uniform_set(compute_list, bindless_array_set, 2);
			bound_pipeline = job.pipeline;
		}

		RD::Uniform output(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, job.entry->displaced_vb);
		RD::Uniform material(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, job.material_buffer);
		RD::Uniform mm_transforms(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, lod_view_dummy_buffer);
		RD::Uniform mm_remap(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, lod_view_dummy_buffer);
		RD::Uniform mm_previous(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, displace_dummy_buffer);
		RD::Uniform blas_positions(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, job.blas_positions.is_valid() ? job.blas_positions : lod_view_dummy_buffer);
		RD::Uniform guard_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, displace_guard_buffer);
		const RID job_set = rd->uniform_set_create(Vector<RD::Uniform>{ output, material, mm_transforms, mm_remap, mm_previous, blas_positions, guard_counts }, job.shader, 3, /*p_linear_pool=*/true);
		rd->compute_list_bind_uniform_set(compute_list, job_set, 3);
		job_sets.push_back(job_set);
		if (job.blas_positions.is_valid()) {
			guard_handles[i] = job.handle;
		}

		const uint32_t push_constant[4] = { i, 0, 0, 0 };
		rd->compute_list_set_push_constant(compute_list, push_constant, sizeof(push_constant));
		_rt_dispatch_linear(rd, compute_list, job.vertex_count);
	}
	rd->compute_list_end();
	for (const RID &job_set : job_sets) {
		rd->free_rid(job_set);
	}
	if (!guard_handles.is_empty()) {
		// Read when the GPU is done with the frame, without waiting for it.
		displace_guard_serial++;
		displace_guard_pending.insert(displace_guard_serial, guard_handles);
		rd->buffer_get_data_async(displace_guard_buffer, callable_mp_static(&RenderRaytracing::_displace_guard_arrived_static).bind(displace_guard_serial), 0, guard_size);
	}
	RD::get_singleton()->draw_command_end_label();
}

void RenderRaytracing::_displace_guard_arrived_static(const Vector<uint8_t> &p_data, uint64_t p_serial) {
	if (displace_guard_target) {
		displace_guard_target->_displace_guard_arrived(p_data, p_serial);
	}
}

void RenderRaytracing::_displace_guard_arrived(const Vector<uint8_t> &p_data, uint64_t p_serial) {
	const LocalVector<RID> *handles = displace_guard_pending.getptr(p_serial);
	if (!handles) {
		return;
	}
	const uint32_t *counts = reinterpret_cast<const uint32_t *>(p_data.ptr());
	const uint32_t count = MIN(handles->size(), uint32_t(p_data.size() / sizeof(uint32_t)));
	for (uint32_t i = 0; i < count; i++) {
		if (counts[i] == 0 || !(*handles)[i].is_valid()) {
			continue;
		}
		RTDeformedCacheEntry *entry = deformed_pool.get_or_null((*handles)[i]);
		if (entry) {
			entry->displaced_guard_rebuild = true;
			displaced_guard_rebuilds++;
		}
	}
	displace_guard_pending.erase(p_serial);
}

LocalVector<RD::Uniform> RenderRaytracing::_displace_frame_uniforms(RID p_job_buffer) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	LocalVector<RD::Uniform> frame_uniforms;
	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(owner->scene_state.uniform_buffers[0]);
		frame_uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 14;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		const RID global_buffer = material_storage->global_shader_uniforms_get_storage_buffer();
		u.append_id(global_buffer.is_valid() ? global_buffer : mesh_storage->get_default_rd_storage_buffer());
		frame_uniforms.push_back(u);
	}
	material_storage->samplers_rd_get_default().append_uniforms(frame_uniforms, 16);
	{
		RD::Uniform u;
		u.binding = 30;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(p_job_buffer);
		frame_uniforms.push_back(u);
	}
	return frame_uniforms;
}

// ---------------------------------------------------------------------------
// Opacity micromaps
// ---------------------------------------------------------------------------

bool RenderRaytracing::_omm_enabled() {
	// GODOT_RT_OPACITY_MICROMAPS=0 turns them off without touching the project, for comparisons.
	static const bool omm_enabled = GLOBAL_GET("rendering/pathtracing/opacity_micromaps") && RD::get_singleton()->has_feature(RD::SUPPORTS_OPACITY_MICROMAP) &&
			OS::get_singleton()->get_environment("GODOT_RT_OPACITY_MICROMAPS") != "0";
	return omm_enabled;
}

RTOmmEntry *RenderRaytracing::_omm_entry(void *p_mesh_surface, RID p_mesh, uint32_t p_surface_index, uint32_t p_surface_counter, RID p_material, uint16_t p_material_counter, const RTMaterialData *p_mat_data, bool p_multimesh, uint32_t p_lod) {
	RD *rd = RD::get_singleton();
	if (!_omm_enabled() || !p_mat_data || !p_material.is_valid() || !p_mesh_surface || !p_mesh.is_valid()) {
		return nullptr;
	}

	// The cut-out must be the same for every instance: a MultiMesh's instance colors multiply COLOR.
	SceneShaderRaytracing *rt_shader = SceneShaderRaytracing::get_singleton();
	const uint32_t slot = p_mat_data->rt_sbt_offset;
	if (slot != 0) {
		const SceneShaderRaytracing::CustomShaderEntry *cse = rt_shader->get_custom_shader_entry(slot);
		if (!cse || !cse->alpha_bakeable || (p_multimesh && cse->alpha_reads_color)) {
			return nullptr;
		}
	} else if (p_multimesh && (p_mat_data->data.flags & RT_MAT_FLAG_VERTEX_COLOR_ALBEDO)) {
		return nullptr;
	}

	const uint32_t current_frame = RSG::rasterizer->get_frame_number();
	RTOmmKey key;
	key.mesh = p_mesh;
	key.surface_index = p_surface_index;
	key.surface_counter = p_surface_counter;
	key.material = p_material;
	key.material_counter = p_material_counter;
	key.lod = p_lod;
	// A surface baked once it lasts RT_OMM_STABLE_FRAMES: a sprite's mesh replaced every frame would be baked and its BLAS
	// built again every frame.
	static constexpr uint32_t RT_OMM_STABLE_FRAMES = 10;
	RTOmmEntry *existing = omm_entries.getptr(key);
	if (!existing) {
		// The material's counter moves with every parameter or shader change, the surface's with its data: the entry of
		// the previous key can never be asked for again. Kept until the TTL, every change held one more micromap and its
		// BLAS (+13 MB per tree surface, a minute of shader swaps under path tracing: +1.6 GB). It goes at the next sweep,
		// once nothing traced in the current frame uses it.
		RTOmmKey slot_key = key;
		slot_key.surface_counter = 0;
		slot_key.material_counter = 0;
		RTOmmKey *previous = omm_current_keys.getptr(slot_key);
		if (previous) {
			RTOmmEntry *stale = omm_entries.getptr(*previous);
			if (stale) {
				stale->superseded = true;
			}
			*previous = key;
		} else {
			omm_current_keys.insert(slot_key, key);
		}
		RTOmmEntry &waiting = omm_entries.insert(key, RTOmmEntry())->value;
		waiting.first_seen_frame = current_frame;
		waiting.last_used_frame = current_frame;
		return nullptr;
	}
	existing->last_used_frame = current_frame;
	if (existing->micromap.is_valid() || existing->failed) {
		return existing->failed ? nullptr : existing;
	}
	if (existing->bake_state != RTOmmEntry::BAKE_NONE) {
		// Under way: this frame's share uses the surface and material as they are now.
		if (existing->refreshed_frame != current_frame && _omm_fill_job(*existing, p_mesh_surface, p_mat_data, p_lod)) {
			existing->refreshed_frame = current_frame;
		}
		return nullptr;
	}
	if (current_frame - existing->first_seen_frame < RT_OMM_STABLE_FRAMES) {
		return nullptr;
	}

	RTOmmEntry &entry = *existing;
	entry.failed = true;

	entry.bake_pipeline = rt_shader->get_omm_bake_pipeline(slot, entry.bake_shader);
	if (!entry.bake_pipeline.is_valid() || !_omm_fill_job(entry, p_mesh_surface, p_mat_data, p_lod)) {
		return nullptr;
	}

	static const uint32_t max_level = CLAMP(uint32_t(int(GLOBAL_GET("rendering/pathtracing/opacity_micromap_subdivision_level"))), 1u, 8u);
	// A surface of many triangles is subdivided less, to keep its micromap within RT_OMM_MAX_MICRO_TRIANGLES (16 MiB of
	// states, one bake thread each); one too large even at level 1 goes without.
	static constexpr uint64_t RT_OMM_MAX_MICRO_TRIANGLES = uint64_t(1) << 26;
	const uint32_t triangle_count = entry.job.triangle_count;
	uint32_t level = max_level;
	while (level > 1 && (uint64_t(triangle_count) << (level * 2)) > RT_OMM_MAX_MICRO_TRIANGLES) {
		level--;
	}
	if ((uint64_t(triangle_count) << (level * 2)) > RT_OMM_MAX_MICRO_TRIANGLES) {
		return nullptr;
	}
	const uint64_t bits_per_entry = (uint64_t(1) << (level * 2)) * 2;
	const uint64_t data_size = STEPIFY((uint64_t(triangle_count) * bits_per_entry + 7) / 8, 4);
	const uint64_t desc_size = uint64_t(triangle_count) * 8;
	ERR_FAIL_COND_V(data_size > UINT32_MAX || desc_size > UINT32_MAX, nullptr);

	const BitField<RD::BufferCreationBits> input_bits = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_OPACITY_MICROMAP_BUILD_INPUT_READ_ONLY_BIT;
	entry.data_buffer = rd->storage_buffer_create(uint32_t(data_size), Span<uint8_t>(), 0, input_bits);
	entry.desc_buffer = rd->storage_buffer_create(uint32_t(desc_size), Span<uint8_t>(), 0, input_bits);
	entry.cost_buffer = rd->storage_buffer_create(triangle_count * sizeof(uint32_t));
	ERR_FAIL_COND_V(!entry.data_buffer.is_valid() || !entry.desc_buffer.is_valid() || !entry.cost_buffer.is_valid(), nullptr);
	rd->set_resource_name(entry.data_buffer, "RT OMM data");
	rd->set_resource_name(entry.desc_buffer, "RT OMM descriptions");
	rd->set_resource_name(entry.cost_buffer, "RT OMM bake costs");

	RD::OpacityMicromapUsage usage;
	usage.count = triangle_count;
	usage.subdivision_level = level;
	usage.format = RD::OPACITY_MICROMAP_FORMAT_4_STATE;
	entry.pending_micromap = rd->opacity_micromap_create({ &usage, 1 }, entry.data_buffer, 0, entry.desc_buffer, 0);
	ERR_FAIL_COND_V(!entry.pending_micromap.is_valid(), nullptr);
	rd->set_resource_name(entry.pending_micromap, "RT OMM");
	entry.level = level;
	entry.data_size = uint32_t(data_size);

	RT_OmmBakeJob &job = entry.job;
	job.level = level;
	job.data_offset = 0;
	job.desc_offset = 0;
	job.max_samples = RT_OMM_MAX_SAMPLES;

	// Baked over the next frames (_omm_dispatch_bakes()); until then the surface is traced without the micromap.
	entry.bake_state = RTOmmEntry::BAKE_ESTIMATE;
	entry.bake_serial = ++omm_bake_serial;
	entry.refreshed_frame = current_frame;
	omm_bake_queue.push_back(key);
	entry.failed = false;
	return nullptr;
}

bool RenderRaytracing::_omm_fill_job(RTOmmEntry &r_entry, void *p_mesh_surface, const RTMaterialData *p_mat_data, uint32_t p_lod) {
	RD *rd = RD::get_singleton();
	// The surface at rest, as the mesh stores it: the micromap goes with its triangles, wherever they are moved.
	RTSurfaceData rest;
	RID vertex_buffer, attribute_buffer, index_buffer;
	_fill_surface_geometry_data(p_mesh_surface, false, &rest, &vertex_buffer, &attribute_buffer, &index_buffer);
	RT_GeometryData &geometry = rest.geometry;
	if (p_lod > 0) {
		// A LOD's triangles: its own indices into the same vertices, in the order the merged BLASes replicate them
		// (_get_lod_view() keeps it), with the format of LOD 0's.
		RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
		index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, p_lod);
		const uint32_t lod_index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, p_lod);
		if (!index_buffer.is_valid() || lod_index_count == 0 || geometry.index_format == RT_INDEX_FORMAT_NONE) {
			return false;
		}
		geometry.primitive_count = lod_index_count / 3;
	}
	if (vertex_buffer.is_valid()) {
		geometry.vertex_buffer_address = rd->buffer_get_device_address(vertex_buffer);
	}
	if (attribute_buffer.is_valid()) {
		geometry.attribute_buffer_address = rd->buffer_get_device_address(attribute_buffer);
	}
	if (index_buffer.is_valid() && geometry.index_format != RT_INDEX_FORMAT_NONE) {
		geometry.index_buffer_address = rd->buffer_get_device_address(index_buffer);
	}
	if (geometry.primitive_count == 0 || !vertex_buffer.is_valid()) {
		return false;
	}
	// A bake under way keeps its triangles: the key holds the surface's counter, so they cannot change.
	RT_OmmBakeJob &job = r_entry.job;
	ERR_FAIL_COND_V(r_entry.bake_state != RTOmmEntry::BAKE_NONE && job.triangle_count != geometry.primitive_count, false);

	job.geometry = geometry;
	job.triangle_count = geometry.primitive_count;
	job.material_address = p_mat_data->data.uniform_address;
	job.albedo_texture_idx = p_mat_data->data.albedo_texture_idx;
	job.albedo_alpha = p_mat_data->data.albedo_color[3];
	job.uv1_scale[0] = p_mat_data->data.uv1_scale[0];
	job.uv1_scale[1] = p_mat_data->data.uv1_scale[1];
	job.uv1_offset[0] = p_mat_data->data.uv1_offset[0];
	job.uv1_offset[1] = p_mat_data->data.uv1_offset[1];
	job.use_vertex_color = (p_mat_data->data.flags & RT_MAT_FLAG_VERTEX_COLOR_ALBEDO) ? 1 : 0;
	mat_ubo_pool_ensure_initialized();
	r_entry.material_buffer = p_mat_data->uniform_buffer.is_valid() ? p_mat_data->uniform_buffer : mat_ubo_pool_buffer;
	return true;
}

RID RenderRaytracing::_omm_static_blas(RTOmmEntry *p_entry, void *p_mesh_surface, LocalVector<RID> &r_dirty_blas_list) {
	RD *rd = RD::get_singleton();
	if (p_entry->surf_data.blas.is_valid() && rd->acceleration_structure_is_valid(p_entry->surf_data.blas)) {
		return p_entry->surf_data.blas;
	}
	// The surface's BLAS again, as process_surface() builds it, with the micromap.
	p_entry->surf_data = RTSurfaceData();
	_populate_surface_blas(p_mesh_surface, RID(), false, false, false, 0, &p_entry->surf_data, r_dirty_blas_list, p_entry->micromap, p_entry->level);
	return p_entry->surf_data.blas;
}

void RenderRaytracing::_omm_estimate_arrived_static(const Vector<uint8_t> &p_data, uint64_t p_serial) {
	if (displace_guard_target) {
		displace_guard_target->_omm_estimate_arrived(p_data, p_serial);
	}
}

void RenderRaytracing::_omm_estimate_arrived(const Vector<uint8_t> &p_data, uint64_t p_serial) {
	const RTOmmKey *key = omm_estimates_pending.getptr(p_serial);
	if (!key) {
		return;
	}
	// Gone, or started again, since: its estimate is of no use.
	RTOmmEntry *entry = omm_entries.getptr(*key);
	if (entry && entry->bake_serial == p_serial && entry->bake_state == RTOmmEntry::BAKE_ESTIMATING &&
			p_data.size() >= int64_t(entry->job.triangle_count * sizeof(uint32_t))) {
		entry->triangle_cost.resize(entry->job.triangle_count);
		memcpy(entry->triangle_cost.ptr(), p_data.ptr(), entry->job.triangle_count * sizeof(uint32_t));
		entry->next_micro = 0;
		entry->remaining_cost = 0;
		for (const uint32_t cost : entry->triangle_cost) {
			entry->remaining_cost += uint64_t(cost) * RT_OMM_COST_UNIT;
		}
		entry->bake_state = RTOmmEntry::BAKE_RANGES;
	}
	omm_estimates_pending.erase(p_serial);
}

void RenderRaytracing::_omm_dispatch_bakes() {
	if (omm_bake_queue.is_empty()) {
		return;
	}
	RD *rd = RD::get_singleton();
	const uint32_t current_frame = RSG::rasterizer->get_frame_number();
	// The bakes' share of a frame: RT_OMM_BAKE_BUDGET estimated evaluations, under 2 ms of GPU on an RTX 5070 (~1 ms on
	// average; godot3d's Mir forest, PT-OMM-BAKE-BUDGET-FIXES.md). A bake costs ~4 evaluations per texel of the surface's
	// UV area: one tiling a 1024² texture several times took 0.4 s in one frame. An entry advances only in a frame that
	// asked for it (its inputs taken from the live surface); a frame's first share holds at least one micro-triangle, so
	// every bake ends.
	static constexpr uint64_t RT_OMM_BAKE_BUDGET = uint64_t(64) << 20;
	// The estimate pass: vertex() at a triangle's corners and one evaluation, counted as this many evaluations.
	static constexpr uint64_t RT_OMM_ESTIMATE_COST = 16;
	uint64_t budget = RT_OMM_BAKE_BUDGET;

	struct Work {
		RTOmmKey key;
		RTOmmEntry *entry;
		uint32_t first; // Triangle (estimate) or micro-triangle (bake) of the first thread.
		uint32_t count; // Threads.
		bool estimate;
		bool last;
	};
	// The bakes with the least work left first (estimates first: one evaluation per triangle), so that one costly surface
	// does not hold back the many cheap ones: after a material change re-keys a forest's leaves, they all wait for their
	// micromaps again, and are traced without them until then.
	struct Queued {
		uint64_t cost;
		RTOmmKey key;
		bool operator<(const Queued &p_other) const { return cost < p_other.cost; }
	};
	LocalVector<Queued> queued;
	for (const RTOmmKey &key : omm_bake_queue) {
		const RTOmmEntry *entry = omm_entries.getptr(key);
		if (entry && entry->bake_state != RTOmmEntry::BAKE_NONE) { // Else evicted, failed or done.
			queued.push_back({ entry->bake_state == RTOmmEntry::BAKE_RANGES ? entry->remaining_cost : 0, key });
		}
	}
	queued.sort();

	LocalVector<Work> work;
	LocalVector<RTOmmKey> remaining;
	for (const Queued &q : queued) {
		const RTOmmKey &key = q.key;
		RTOmmEntry *entry = omm_entries.getptr(key);
		remaining.push_back(key);
		if (entry->refreshed_frame != current_frame || (budget == 0 && !work.is_empty())) {
			continue;
		}
		if (entry->bake_state == RTOmmEntry::BAKE_ESTIMATE) {
			work.push_back({ key, entry, 0, entry->job.triangle_count, true, false });
			budget -= MIN(budget, uint64_t(entry->job.triangle_count) * RT_OMM_ESTIMATE_COST);
		} else if (entry->bake_state == RTOmmEntry::BAKE_RANGES) {
			// Micro-triangles in a row, a triangle's estimate spread evenly over its own: a triangle too costly for one frame
			// is split.
			const uint32_t micro_count = 1u << (entry->level * 2);
			const uint64_t total = uint64_t(entry->job.triangle_count) * micro_count;
			const uint64_t first = entry->next_micro;
			uint64_t end = first;
			while (end < total) {
				const uint64_t triangle_cost = uint64_t(entry->triangle_cost[end / micro_count]) * RT_OMM_COST_UNIT;
				const uint64_t per_micro = MAX(uint64_t(1), triangle_cost / micro_count);
				const uint64_t left_in_triangle = micro_count - end % micro_count;
				uint64_t take = MIN(left_in_triangle, budget / per_micro);
				if (take == 0) {
					if (end != first || !work.is_empty()) {
						break;
					}
					take = 1;
				}
				budget -= MIN(budget, take * per_micro);
				entry->remaining_cost -= MIN(entry->remaining_cost, take * per_micro);
				end += take;
				if (take < left_in_triangle) {
					break;
				}
			}
			if (end > first) {
				work.push_back({ key, entry, uint32_t(first), uint32_t(end - first), false, end == total });
			}
		}
	}
	omm_bake_queue = remaining;
	if (work.is_empty()) {
		return;
	}

	// The textures the materials registered are in the tables already.
	const RID bindless_set = bindless_block->get_secondary_uniform_set(work[0].entry->bake_shader, 1);
	const RID bindless_array_set = bindless_array_block->get_secondary_uniform_set(work[0].entry->bake_shader, 2);
	ERR_FAIL_COND_MSG(!bindless_set.is_valid() || !bindless_array_set.is_valid(), "RT: no bindless tables for the opacity micromap bakes.");

	if (omm_job_capacity < work.size()) {
		if (omm_job_buffer.is_valid()) {
			rd->free_rid(omm_job_buffer);
		}
		omm_job_capacity = Math::next_power_of_2(work.size());
		omm_job_buffer = rd->storage_buffer_create(omm_job_capacity * sizeof(RT_OmmBakeJob));
		rd->set_resource_name(omm_job_buffer, "RT OMM bake jobs");
	}
	LocalVector<RT_OmmBakeJob> job_data;
	job_data.resize(work.size());
	for (uint32_t i = 0; i < work.size(); i++) {
		job_data[i] = work[i].entry->job;
		if (!work[i].estimate && work[i].first == 0) {
			// The bake ORs the states in.
			rd->buffer_clear(work[i].entry->data_buffer, 0, work[i].entry->data_size);
		}
	}
	rd->buffer_update(omm_job_buffer, 0, job_data.size() * sizeof(RT_OmmBakeJob), job_data.ptr());

	const LocalVector<RD::Uniform> frame_uniforms = _displace_frame_uniforms(omm_job_buffer);

	RENDER_TIMESTAMP("RT Bake Opacity Micromaps");
	rd->draw_command_begin_label("RT Bake Opacity Micromaps");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	RID bound_pipeline;
	LocalVector<RID> job_sets;
	for (uint32_t i = 0; i < work.size(); i++) {
		const Work &w = work[i];
		RTOmmEntry &entry = *w.entry;
		if (entry.bake_pipeline != bound_pipeline) {
			rd->compute_list_bind_compute_pipeline(compute_list, entry.bake_pipeline);
			rd->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache_vec(entry.bake_shader, 0, frame_uniforms), 0);
			rd->compute_list_bind_uniform_set(compute_list, bindless_set, 1);
			rd->compute_list_bind_uniform_set(compute_list, bindless_array_set, 2);
			bound_pipeline = entry.bake_pipeline;
		}
		RD::Uniform data(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, entry.data_buffer);
		RD::Uniform descs(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, entry.desc_buffer);
		RD::Uniform material(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, entry.material_buffer);
		RD::Uniform costs(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, entry.cost_buffer);
		const RID job_set = rd->uniform_set_create(Vector<RD::Uniform>{ data, descs, material, costs }, entry.bake_shader, 3, /*p_linear_pool=*/true);
		rd->compute_list_bind_uniform_set(compute_list, job_set, 3);
		job_sets.push_back(job_set);

		const uint32_t push_constant[4] = { i, 0, w.first, w.estimate ? 1u : 0u };
		rd->compute_list_set_push_constant(compute_list, push_constant, sizeof(push_constant));
		_rt_dispatch_linear(rd, compute_list, w.count);
	}
	rd->compute_list_end();
	for (const RID &job_set : job_sets) {
		rd->free_rid(job_set);
	}
	for (const Work &w : work) {
		RTOmmEntry &entry = *w.entry;
		if (w.estimate) {
			// Read when the GPU is done with the frame, without waiting for it.
			entry.bake_state = RTOmmEntry::BAKE_ESTIMATING;
			omm_estimates_pending.insert(entry.bake_serial, w.key);
			rd->buffer_get_data_async(entry.cost_buffer, callable_mp_static(&RenderRaytracing::_omm_estimate_arrived_static).bind(entry.bake_serial), 0, entry.job.triangle_count * sizeof(uint32_t));
			continue;
		}
		entry.next_micro = w.first + w.count;
		if (w.last) {
			// Its micromap, before the BLAS builds that read it: the next frames' callers get it.
			rd->opacity_micromap_build(entry.pending_micromap);
			entry.micromap = entry.pending_micromap;
			entry.pending_micromap = RID();
			entry.bake_state = RTOmmEntry::BAKE_NONE;
			entry.triangle_cost.reset();
			rd->free_rid(entry.cost_buffer);
			entry.cost_buffer = RID();
		}
	}
	rd->draw_command_end_label();
}

void RenderRaytracing::_omm_free_entry(RTOmmEntry &p_entry) {
	RD *rd = RD::get_singleton();
	// The BLAS goes with the mesh buffers it was built from, and with the micromap; the micromap with its buffers.
	if (p_entry.surf_data.blas.is_valid() && rd->acceleration_structure_is_valid(p_entry.surf_data.blas)) {
		rd->free_rid(p_entry.surf_data.blas);
	}
	if (p_entry.micromap.is_valid() && rd->acceleration_structure_is_valid(p_entry.micromap)) {
		rd->free_rid(p_entry.micromap);
	}
	// A bake under way: its micromap is not built yet; the queue and an estimate on its way back find the entry gone.
	if (p_entry.pending_micromap.is_valid() && rd->acceleration_structure_is_valid(p_entry.pending_micromap)) {
		rd->free_rid(p_entry.pending_micromap);
	}
	if (p_entry.cost_buffer.is_valid()) {
		rd->free_rid(p_entry.cost_buffer);
	}
	if (p_entry.data_buffer.is_valid()) {
		rd->free_rid(p_entry.data_buffer);
	}
	if (p_entry.desc_buffer.is_valid()) {
		rd->free_rid(p_entry.desc_buffer);
	}
	p_entry = RTOmmEntry();
}

void RenderRaytracing::_omm_evict(bool p_all) {
	const uint32_t current_frame = RSG::rasterizer->get_frame_number();
	static const uint32_t ttl = uint32_t(int(GLOBAL_GET("rendering/pathtracing/static_cache_ttl_frames")));
	LocalVector<RTOmmKey> expired;
	for (KeyValue<RTOmmKey, RTOmmEntry> &E : omm_entries) {
		// An entry still waiting to be baked holds nothing: one a second or two old is a surface that did not last.
		const uint32_t entry_ttl = (E.value.micromap.is_valid() || E.value.failed) ? ttl : 120;
		// A superseded entry still used in the last frames waits: another viewport may trace it in this frame, or still
		// ask for it with a counter it read before the change.
		const bool superseded = E.value.superseded && current_frame - E.value.last_used_frame > 2;
		if (p_all || superseded || current_frame - E.value.last_used_frame > entry_ttl) {
			_omm_free_entry(E.value);
			expired.push_back(E.key);
		}
	}
	for (const RTOmmKey &key : expired) {
		omm_entries.erase(key);
		RTOmmKey slot = key;
		slot.surface_counter = 0;
		slot.material_counter = 0;
		const RTOmmKey *current = omm_current_keys.getptr(slot);
		if (current && *current == key) {
			omm_current_keys.erase(slot);
		}
	}
	if (p_all) {
		omm_bake_queue.clear();
		omm_estimates_pending.clear();
		if (omm_job_buffer.is_valid()) {
			RD::get_singleton()->free_rid(omm_job_buffer);
			omm_job_buffer = RID();
			omm_job_capacity = 0;
		}
	}
}

void RenderRaytracing::_displace_merged_multimeshes() {
	if (mm_displace_requests.is_empty()) {
		return;
	}
	SceneShaderRaytracing *rt_shader = SceneShaderRaytracing::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RD *rd = RD::get_singleton();

	struct Job {
		const MMDisplaceRequest *request = nullptr;
		RID shader;
		RID pipeline;
		RID material_buffer;
	};
	LocalVector<Job> jobs;
	LocalVector<RT_DisplaceJob> job_data;
	for (const MMDisplaceRequest &request : mm_displace_requests) {
		RID program_shader;
		const RID pipeline = rt_shader->get_displace_pipeline(request.mat_data->rt_sbt_offset, program_shader);
		if (!pipeline.is_valid()) {
			continue;
		}
		const uint64_t format = mesh_storage->mesh_surface_get_format(request.mesh_surface);

		RT_DisplaceJob data = {};
		RTSurfaceData source;
		RID vertex_buffer, attribute_buffer;
		_fill_surface_geometry_data(request.mesh_surface, false, &source, &vertex_buffer, &attribute_buffer);
		data.geometry = source.geometry;
		if (vertex_buffer.is_valid()) {
			data.geometry.vertex_buffer_address = rd->buffer_get_device_address(vertex_buffer);
		}
		if (attribute_buffer.is_valid()) {
			data.geometry.attribute_buffer_address = rd->buffer_get_device_address(attribute_buffer);
		}
		RendererRD::MaterialStorage::store_transform(request.transform, data.model_matrix);
		RendererRD::MaterialStorage::store_transform(request.transform.affine_inverse(), data.inv_model_matrix);
		RendererRD::MaterialStorage::store_transform(request.prev_transform, data.prev_model_matrix);
		RendererRD::MaterialStorage::store_transform(request.prev_transform.affine_inverse(), data.prev_inv_model_matrix);
		data.material_address = request.mat_data->data.uniform_address;
		data.tbn_words = (format & RSE::ARRAY_FORMAT_NORMAL) ? ((format & RSE::ARRAY_FORMAT_TANGENT) ? 2 : 1) : 0;
		data.instance_uniforms_ofs = request.instance_uniforms_ofs;
		data.mm_instance_count = request.mm_count;
		data.mm_vertex_count = request.vertex_count;
		data.mm_stride = request.mm_stride;
		data.mm_offset = request.mm_offset;
		data.mm_use_remap = request.remap_buffer.is_valid() ? 1 : 0;
		data.mm_color_offset = request.mm_color_offset;
		data.mm_custom_offset = request.mm_custom_offset;
		job_data.push_back(data);

		mat_ubo_pool_ensure_initialized();
		Job job;
		job.request = &request;
		job.shader = program_shader;
		job.pipeline = pipeline;
		job.material_buffer = request.mat_data->uniform_buffer.is_valid() ? request.mat_data->uniform_buffer : mat_ubo_pool_buffer;
		jobs.push_back(job);
	}
	if (jobs.is_empty()) {
		return;
	}

	const RID bindless_set = bindless_block->get_secondary_uniform_set(jobs[0].shader, 1);
	const RID bindless_array_set = bindless_array_block->get_secondary_uniform_set(jobs[0].shader, 2);
	ERR_FAIL_COND_MSG(!bindless_set.is_valid() || !bindless_array_set.is_valid(), "RT: no bindless tables for the MultiMesh displacement.");

	if (mm_displace_job_capacity < job_data.size()) {
		if (mm_displace_job_buffer.is_valid()) {
			rd->free_rid(mm_displace_job_buffer);
		}
		mm_displace_job_capacity = Math::next_power_of_2(job_data.size());
		mm_displace_job_buffer = rd->storage_buffer_create(mm_displace_job_capacity * sizeof(RT_DisplaceJob));
		rd->set_resource_name(mm_displace_job_buffer, "RT MultiMesh displace jobs");
	}
	rd->buffer_update(mm_displace_job_buffer, 0, job_data.size() * sizeof(RT_DisplaceJob), job_data.ptr());
	const LocalVector<RD::Uniform> frame_uniforms = _displace_frame_uniforms(mm_displace_job_buffer);

	rd->draw_command_begin_label("RT Displace MultiMeshes");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	RID bound_pipeline;
	LocalVector<RID> job_sets;
	for (uint32_t i = 0; i < jobs.size(); i++) {
		const Job &job = jobs[i];
		const MMDisplaceRequest &request = *job.request;
		if (job.pipeline != bound_pipeline) {
			rd->compute_list_bind_compute_pipeline(compute_list, job.pipeline);
			rd->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache_vec(job.shader, 0, frame_uniforms), 0);
			rd->compute_list_bind_uniform_set(compute_list, bindless_set, 1);
			rd->compute_list_bind_uniform_set(compute_list, bindless_array_set, 2);
			bound_pipeline = job.pipeline;
		}

		// The displaced positions replace the merged ones, which the BLAS build reads next.
		RD::Uniform output(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, request.merged_vtx_buffer);
		RD::Uniform material(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, job.material_buffer);
		RD::Uniform mm_transforms(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, request.mm_gpu_buffer);
		RD::Uniform mm_remap(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, request.remap_buffer.is_valid() ? request.remap_buffer : lod_view_dummy_buffer);
		RD::Uniform mm_previous(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, request.merged_prev_buffer);
		// A merged MultiMesh folds removed points onto the origin, it needs no guard.
		RD::Uniform blas_positions(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, lod_view_dummy_buffer);
		RD::Uniform guard_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, displace_dummy_buffer);
		const RID job_set = rd->uniform_set_create(Vector<RD::Uniform>{ output, material, mm_transforms, mm_remap, mm_previous, blas_positions, guard_counts }, job.shader, 3, /*p_linear_pool=*/true);
		rd->compute_list_bind_uniform_set(compute_list, job_set, 3);
		job_sets.push_back(job_set);

		const uint32_t push_constant[4] = { i, 0, 0, 0 };
		rd->compute_list_set_push_constant(compute_list, push_constant, sizeof(push_constant));
		_rt_dispatch_linear(rd, compute_list, request.mm_count * request.vertex_count);
	}
	rd->compute_list_end();
	for (const RID &job_set : job_sets) {
		rd->free_rid(job_set);
	}
	rd->draw_command_end_label();
}

// ---------------------------------------------------------------------------
// Uniform packing (file-local helpers)
// ---------------------------------------------------------------------------

static float _def_real(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? u.default_value[idx].real : 0.0f;
}

static int32_t _def_sint(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? u.default_value[idx].sint : 0;
}

static uint32_t _def_uint(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? u.default_value[idx].uint : 0u;
}

static uint32_t _def_bool(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? (uint32_t)u.default_value[idx].boolean : 0u;
}

// Colors as the raster path fills them (MaterialStorage::fill_std140_uniform()): a Color value is made linear unless the
// uniform disables color conversion, a default value only when the uniform is a source_color.
static Color _uniform_color(const ShaderLanguage::ShaderNode::Uniform &u, const Color &p_color) {
	return u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_COLOR_CONVERSION_DISABLED ? p_color : p_color.srgb_to_linear();
}

static Color _default_color(const ShaderLanguage::ShaderNode::Uniform &u) {
	const Color c(_def_real(u, 0), _def_real(u, 1), _def_real(u, 2), _def_real(u, 3));
	return u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SOURCE_COLOR ? c.srgb_to_linear() : c;
}

static void pack_uniform(const ShaderLanguage::ShaderNode::Uniform &u, const Variant &val, uint8_t *dst) {
	using SL = ShaderLanguage;

	switch (u.type) {
		case SL::TYPE_FLOAT: {
			float v = val.get_type() == Variant::FLOAT ? (float)(double)val : _def_real(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_INT: {
			int32_t v = val.get_type() == Variant::INT ? (int32_t)(int64_t)val : _def_sint(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_UINT: {
			uint32_t v = val.get_type() == Variant::INT ? (uint32_t)(int64_t)val : _def_uint(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_BOOL: {
			uint32_t v = val.get_type() == Variant::BOOL ? (uint32_t)(bool)val : _def_bool(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_VEC2: {
			float fv[2];
			if (val.get_type() == Variant::VECTOR2) {
				Vector2 v = val;
				fv[0] = (float)v.x;
				fv[1] = (float)v.y;
			} else {
				fv[0] = _def_real(u, 0);
				fv[1] = _def_real(u, 1);
			}
			memcpy(dst, fv, 8);
		} break;
		case SL::TYPE_VEC3: {
			float fv[3] = {};
			if (val.get_type() == Variant::VECTOR3) {
				Vector3 v = val;
				fv[0] = (float)v.x;
				fv[1] = (float)v.y;
				fv[2] = (float)v.z;
			} else {
				const Color c = val.get_type() == Variant::COLOR ? _uniform_color(u, val) : _default_color(u);
				fv[0] = c.r;
				fv[1] = c.g;
				fv[2] = c.b;
			}
			memcpy(dst, fv, 12);
		} break;
		case SL::TYPE_VEC4: {
			float fv[4] = {};
			if (val.get_type() == Variant::VECTOR4) {
				Vector4 v = val;
				fv[0] = (float)v.x;
				fv[1] = (float)v.y;
				fv[2] = (float)v.z;
				fv[3] = (float)v.w;
			} else {
				const Color c = val.get_type() == Variant::COLOR ? _uniform_color(u, val) : _default_color(u);
				fv[0] = c.r;
				fv[1] = c.g;
				fv[2] = c.b;
				fv[3] = c.a;
			}
			memcpy(dst, fv, 16);
		} break;
		case SL::TYPE_IVEC2: {
			int32_t iv[2];
			if (val.get_type() == Variant::VECTOR2I) {
				Vector2i v = val;
				iv[0] = v.x;
				iv[1] = v.y;
			} else {
				iv[0] = _def_sint(u, 0);
				iv[1] = _def_sint(u, 1);
			}
			memcpy(dst, iv, 8);
		} break;
		case SL::TYPE_IVEC3: {
			int32_t iv[3] = {};
			if (val.get_type() == Variant::VECTOR3I) {
				Vector3i v = val;
				iv[0] = v.x;
				iv[1] = v.y;
				iv[2] = v.z;
			} else {
				for (int i = 0; i < 3; i++) {
					iv[i] = _def_sint(u, i);
				}
			}
			memcpy(dst, iv, 12);
		} break;
		case SL::TYPE_IVEC4: {
			int32_t iv[4] = {};
			if (val.get_type() == Variant::VECTOR4I) {
				Vector4i v = val;
				iv[0] = v.x;
				iv[1] = v.y;
				iv[2] = v.z;
				iv[3] = v.w;
			} else {
				for (int i = 0; i < 4; i++) {
					iv[i] = _def_sint(u, i);
				}
			}
			memcpy(dst, iv, 16);
		} break;
		case SL::TYPE_UVEC2: {
			uint32_t uv[2];
			if (val.get_type() == Variant::VECTOR2I) {
				Vector2i v = val;
				uv[0] = (uint32_t)v.x;
				uv[1] = (uint32_t)v.y;
			} else {
				uv[0] = _def_uint(u, 0);
				uv[1] = _def_uint(u, 1);
			}
			memcpy(dst, uv, 8);
		} break;
		case SL::TYPE_UVEC3: {
			uint32_t uv[3] = {};
			if (val.get_type() == Variant::VECTOR3I) {
				Vector3i v = val;
				uv[0] = (uint32_t)v.x;
				uv[1] = (uint32_t)v.y;
				uv[2] = (uint32_t)v.z;
			} else {
				for (int i = 0; i < 3; i++) {
					uv[i] = _def_uint(u, i);
				}
			}
			memcpy(dst, uv, 12);
		} break;
		case SL::TYPE_UVEC4: {
			uint32_t uv[4] = {};
			if (val.get_type() == Variant::VECTOR4I) {
				Vector4i v = val;
				uv[0] = (uint32_t)v.x;
				uv[1] = (uint32_t)v.y;
				uv[2] = (uint32_t)v.z;
				uv[3] = (uint32_t)v.w;
			} else {
				for (int i = 0; i < 4; i++) {
					uv[i] = _def_uint(u, i);
				}
			}
			memcpy(dst, uv, 16);
		} break;
		case SL::TYPE_BVEC2: {
			uint32_t bv[2] = { _def_bool(u, 0), _def_bool(u, 1) };
			memcpy(dst, bv, 8);
		} break;
		case SL::TYPE_BVEC3: {
			uint32_t bv[3] = { _def_bool(u, 0), _def_bool(u, 1), _def_bool(u, 2) };
			memcpy(dst, bv, 12);
		} break;
		case SL::TYPE_BVEC4: {
			uint32_t bv[4] = { _def_bool(u, 0), _def_bool(u, 1), _def_bool(u, 2), _def_bool(u, 3) };
			memcpy(dst, bv, 16);
		} break;
		case SL::TYPE_MAT2: {
			// std140: mat2 = 2 column vec2s, each padded to vec4 (2x16 = 32 bytes).
			float m[8] = {};
			if (val.get_type() == Variant::TRANSFORM2D) {
				Transform2D t = val;
				m[0] = (float)t[0].x;
				m[1] = (float)t[0].y;
				m[4] = (float)t[1].x;
				m[5] = (float)t[1].y;
			} else {
				for (int i = 0; i < 4; i++) {
					m[(i / 2) * 4 + (i % 2)] = _def_real(u, i);
				}
			}
			memcpy(dst, m, 32);
		} break;
		case SL::TYPE_MAT3: {
			// std140: mat3 = 3 column vec3s, each padded to vec4 (3x16 = 48 bytes).
			float m[12] = {};
			if (val.get_type() == Variant::BASIS) {
				Basis b = val;
				for (int col = 0; col < 3; col++) {
					Vector3 c = b.get_column(col);
					m[col * 4 + 0] = (float)c.x;
					m[col * 4 + 1] = (float)c.y;
					m[col * 4 + 2] = (float)c.z;
				}
			} else {
				for (int i = 0; i < 9; i++) {
					m[(i / 3) * 4 + (i % 3)] = _def_real(u, i);
				}
			}
			memcpy(dst, m, 48);
		} break;
		case SL::TYPE_MAT4: {
			// std140: mat4 = 4 column vec4s (4x16 = 64 bytes).
			float m[16] = {};
			if (val.get_type() == Variant::PROJECTION) {
				Projection p = val;
				for (int col = 0; col < 4; col++) {
					m[col * 4 + 0] = (float)p.columns[col].x;
					m[col * 4 + 1] = (float)p.columns[col].y;
					m[col * 4 + 2] = (float)p.columns[col].z;
					m[col * 4 + 3] = (float)p.columns[col].w;
				}
			} else if (val.get_type() == Variant::TRANSFORM3D) {
				Transform3D t = val;
				Projection p(t);
				for (int col = 0; col < 4; col++) {
					m[col * 4 + 0] = (float)p.columns[col].x;
					m[col * 4 + 1] = (float)p.columns[col].y;
					m[col * 4 + 2] = (float)p.columns[col].z;
					m[col * 4 + 3] = (float)p.columns[col].w;
				}
			} else {
				for (int i = 0; i < 16; i++) {
					m[i] = _def_real(u, i);
				}
			}
			memcpy(dst, m, 64);
		} break;
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Procedural geometry processing
// ---------------------------------------------------------------------------

void RenderRaytracing::update_procedural_blas(RTProceduralState *p_state, LocalVector<RID> &r_dirty_blas_list) {
	// Pack AABB data into a byte buffer.
	Vector<uint8_t> aabb_bytes;
	uint32_t aabb_count = 1;

	if (p_state->aabb_data.size() >= 6 && (p_state->aabb_data.size() % 6) == 0) {
		aabb_count = p_state->aabb_data.size() / 6;
		aabb_bytes.resize(p_state->aabb_data.size() * sizeof(float));
		memcpy(aabb_bytes.ptrw(), p_state->aabb_data.ptr(), aabb_bytes.size());
	} else {
		const AABB &a = p_state->culling_aabb;
		float single[6] = {
			(float)a.position.x, (float)a.position.y, (float)a.position.z,
			(float)(a.position.x + a.size.x), (float)(a.position.y + a.size.y), (float)(a.position.z + a.size.z)
		};
		aabb_bytes.resize(sizeof(single));
		memcpy(aabb_bytes.ptrw(), single, sizeof(single));
	}

	uint32_t required_bytes = aabb_bytes.size();
	bool needs_new_blas = false;

	// Grow-only: only recreate the buffer when capacity is exceeded or count changed.
	if (required_bytes > p_state->gpu_buffer_capacity || aabb_count != p_state->aabb_count) {
		if (p_state->blas.is_valid()) {
			RD::get_singleton()->free_rid(p_state->blas);
			p_state->blas = RID();
		}
		if (p_state->gpu_buffer.is_valid()) {
			RD::get_singleton()->free_rid(p_state->gpu_buffer);
		}
		p_state->gpu_buffer = RD::get_singleton()->storage_buffer_create(required_bytes, aabb_bytes,
				0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		p_state->gpu_buffer_capacity = required_bytes;
		p_state->aabb_count = aabb_count;
		needs_new_blas = true;
	} else {
		// Buffer is large enough -- just update contents.
		RD::get_singleton()->buffer_update(p_state->gpu_buffer, 0, required_bytes, aabb_bytes.ptr());
		needs_new_blas = !p_state->blas.is_valid();
	}

	if (needs_new_blas) {
		ERR_FAIL_COND(!p_state->gpu_buffer.is_valid());

		RD::AccelerationStructureGeometry geom;
		geom.type = RD::AccelerationStructureGeometry::TYPE_AABBS;
		geom.geometry.aabbs.buffer = p_state->gpu_buffer;
		geom.geometry.aabbs.count = aabb_count;
		geom.geometry.aabbs.stride = 24; // VkAabbPositionsKHR: two float3 (min, max).
		p_state->blas = RD::get_singleton()->blas_create({ &geom, 1 }, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	}

	// BDA for shader access.
	if (p_state->expose_bounds && p_state->gpu_buffer.is_valid()) {
		p_state->gpu_buffer_address = RD::get_singleton()->buffer_get_device_address(p_state->gpu_buffer);
	} else {
		p_state->gpu_buffer_address = 0;
	}

	if (p_state->blas.is_valid()) {
		r_dirty_blas_list.push_back(p_state->blas);
	}
}

// ---------------------------------------------------------------------------
// Material processing
// ---------------------------------------------------------------------------

RTMaterialData *RenderRaytracing::_compiling_material_fallback(const RTMaterialData *p_mat_data, bool p_cut_out) {
	// A neutral standard material, so the ground and the walls do not vanish for the seconds a new pipeline takes to
	// compile (a pipeline cache miss: a new shader or engine version). Cut-out and billboard materials wait: their shape
	// comes from their shader, a gray card would be worse than nothing.
	if (p_cut_out) {
		return nullptr;
	}
	const SceneShaderRaytracing::CustomShaderEntry *entry = SceneShaderRaytracing::get_singleton()->get_custom_shader_entry(p_mat_data->rt_sbt_offset);
	if (!entry || entry->writes_modelview || entry->uses_alpha_clip || entry->alpha_texture_buffer_offset != UINT32_MAX) {
		return nullptr;
	}
	static RTMaterialData s_compiling_mat;
	static bool s_compiling_mat_initialized = false;
	if (!s_compiling_mat_initialized) {
		s_compiling_mat = *process_material(RID(), 0);
		for (int i = 0; i < 3; i++) {
			s_compiling_mat.data.albedo_color[i] = 0.3f;
		}
		s_compiling_mat_initialized = true;
	}
	return &s_compiling_mat;
}

bool RenderRaytracing::is_base_material(RID p_material, const Variant &p_albedo) {
	// BaseMaterial3D's shaders are evaluated from their parameters (hit group 0); any other shader runs its own code, even
	// when it names its uniforms as BaseMaterial3D does. A Shader resource converted from a StandardMaterial3D keeps the
	// generated header, often with an edited vertex(): only the generated shaders have no resource path.
	const RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	return p_albedo.get_type() == Variant::COLOR && material_storage->material_get_shader_path_hint(p_material).is_empty() &&
			material_storage->material_get_shader_code_rt(p_material).begins_with("// NOTE: Shader automatically converted from ");
}

void RenderRaytracing::precache_hit_groups(const LocalVector<RID> &p_materials) {
	// Registering a material's shader queues its hit group for the next pipeline build (SceneShaderRaytracing): the
	// build process_material() starts when the TLAS first meets it, a few seconds of a cut-out left out of the scene.
	// The programs its surfaces need when they first appear are started too (displacement, opacity micromap bake), so
	// that their first frame does not stall on compiling them; BaseMaterial3D's bake program once.
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	SceneShaderRaytracing *rt_shader = SceneShaderRaytracing::get_singleton();
	rt_shader->precompile_slot_programs(0, _omm_enabled());
	HashSet<RID> seen;
	for (const RID &material : p_materials) {
		if (seen.has(material)) {
			continue;
		}
		seen.insert(material);
		// Freed since its surface was built.
		if (!material_storage->owns_material(material) || is_base_material(material, material_storage->material_get_param(material, "albedo"))) {
			continue;
		}
		const uint32_t slot = rt_shader->register_custom_shader(material_storage->material_get_shader_id(material), material);
		if (slot != 0) {
			rt_shader->precompile_slot_programs(slot, _omm_enabled());
		}
	}
}

RTMaterialData *RenderRaytracing::process_material(RID p_material_rid, uint16_t p_material_invalidation_counter) {
	// Static default material for invalid/null materials
	static RTMaterialData s_default_mat;
	static bool s_default_mat_initialized = false;
	if (!s_default_mat_initialized) {
		s_default_mat.data.albedo_color[0] = 1.0f;
		s_default_mat.data.albedo_color[1] = 1.0f;
		s_default_mat.data.albedo_color[2] = 1.0f;
		s_default_mat.data.albedo_color[3] = 1.0f;
		s_default_mat.data.emission_color[0] = 0.0f;
		s_default_mat.data.emission_color[1] = 0.0f;
		s_default_mat.data.emission_color[2] = 0.0f;
		s_default_mat.data.emission_strength = 0.0f;
		s_default_mat.data.roughness = 1.0f;
		s_default_mat.data.specular = 0.5f;
		s_default_mat.data.ao_strength = 1.0f;
		s_default_mat.data.uv1_scale[0] = 1.0f;
		s_default_mat.data.uv1_scale[1] = 1.0f;
		s_default_mat.data.uv1_offset[0] = 0.0f;
		s_default_mat.data.uv1_offset[1] = 0.0f;
		s_default_mat_initialized = true;
	}

	if (!p_material_rid.is_valid()) {
		return &s_default_mat;
	}

	// Cache lookup
	uint32_t mat_idx = get_rid_index(p_material_rid);
	uint32_t mat_version = get_rid_version(p_material_rid);
	RTMaterialCacheEntry *entry = get_material_cache_entry(mat_idx);

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	bool needs_refresh = !entry->ptr ||
			entry->cached_rid_version != mat_version ||
			entry->cached_counter != p_material_invalidation_counter;

	if (!needs_refresh) {
		entry->last_used_frame = current_frame;
		if (entry->ptr->is_custom_shader) {
			uint32_t shader_id = RendererRD::MaterialStorage::get_singleton()->material_get_shader_id(p_material_rid);
			uint32_t old_sbt = entry->ptr->rt_sbt_offset;
			uint32_t new_sbt = SceneShaderRaytracing::get_singleton()->register_custom_shader(shader_id, p_material_rid, &entry->ptr->shader_failed);
			entry->ptr->rt_sbt_offset = new_sbt;
			// HG slot change invalidates cached UBO layout / BDA.
			if (old_sbt != new_sbt) {
				needs_refresh = true;
			}
		}
		if (!needs_refresh) {
			return entry->ptr;
		}
	}

	// Cache miss - need to rebuild material
	if (!entry->ptr) {
		entry->ptr = memnew(RTMaterialData);
	}

	RTMaterialData *mat_data = entry->ptr;
	RT_MaterialData &mat = mat_data->data;

	// Initialize defaults
	mat.albedo_color[0] = 1.0f;
	mat.albedo_color[1] = 1.0f;
	mat.albedo_color[2] = 1.0f;
	mat.albedo_color[3] = 1.0f;
	mat.emission_color[0] = 0.0f;
	mat.emission_color[1] = 0.0f;
	mat.emission_color[2] = 0.0f;
	mat.emission_strength = 0.0f;
	mat.metallic = 0.0f;
	mat.roughness = 1.0f;
	mat.specular = 0.5f;
	mat.ao_strength = 1.0f;
	mat.flags = 0;
	mat.albedo_texture_idx = 0;
	mat.normal_texture_idx = 0;
	mat.orm_texture_idx = 0;
	mat.emission_texture_idx = 0;
	mat.uv1_scale[0] = 1.0f;
	mat.uv1_scale[1] = 1.0f;
	mat.uv1_offset[0] = 0.0f;
	mat.uv1_offset[1] = 0.0f;
	mat.uv1_scale_z = 1.0f;
	mat.uv1_offset_z = 0.0f;
	mat.uv1_blend_sharpness = 1.0f;
	mat.normal_map_depth = 1.0f;
	mat.uniform_address = 0;
	mat.backlight[0] = 0.0f;
	mat.backlight[1] = 0.0f;
	mat.backlight[2] = 0.0f;
	mat.backlight_texture_idx = 0;
	mat.ao_texture_idx = 0;
	mat.ao_channel = 0;
	mat.ao_light_affect = 0.0f;
	mat.ao_pad = 0;

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	// Helper lambda to get texture from material parameter
	// p_srgb should be true for color textures (albedo, emission) that need sRGB->linear conversion
	auto get_material_texture = [&](const StringName &p_param, bool p_srgb = false) -> RID {
		Variant tex_var = material_storage->material_get_param(p_material_rid, p_param);
		if (tex_var.get_type() == Variant::OBJECT || tex_var.get_type() == Variant::RID) {
			RID tex_rid = tex_var;
			if (tex_rid.is_valid()) {
				return texture_storage->texture_get_rd_texture(tex_rid, p_srgb);
			}
		}
		return RID();
	};

	// Textures
	// Albedo is a color texture - needs sRGB->linear conversion
	RID albedo_rd = get_material_texture("texture_albedo", true);
	if (albedo_rd.is_valid()) {
		mat.albedo_texture_idx = bindless_block->add_texture(albedo_rd);
	}

	RID normal_rd = get_material_texture("texture_normal");
	if (normal_rd.is_valid()) {
		mat.normal_texture_idx = bindless_block->add_texture(normal_rd);
		mat.flags |= RT_MAT_FLAG_HAS_NORMAL_MAP;

		Variant normal_scale_var = material_storage->material_get_param(p_material_rid, "normal_scale");
		if (normal_scale_var.get_type() == Variant::FLOAT) {
			mat.normal_map_depth = normal_scale_var;
		}
	}

	RID orm_rd = get_material_texture("texture_orm");
	if (orm_rd.is_valid()) {
		mat.orm_texture_idx = bindless_block->add_texture(orm_rd);
	} else {
		RID roughness_rd = get_material_texture("texture_roughness");
		if (roughness_rd.is_valid()) {
			mat.orm_texture_idx = bindless_block->add_texture(roughness_rd);
		}
	}

	// Emission is a color texture - needs sRGB->linear conversion
	RID emission_rd = get_material_texture("texture_emission", true);
	if (emission_rd.is_valid()) {
		mat.emission_texture_idx = bindless_block->add_texture(emission_rd);
		mat.flags |= RT_MAT_FLAG_HAS_EMISSION_TEX;
		// Set sensible defaults for emission when texture is present
		mat.emission_color[0] = 1.0f;
		mat.emission_color[1] = 1.0f;
		mat.emission_color[2] = 1.0f;
		mat.emission_strength = 1.0f;
	}

	// Material properties
	// Colors declared with source_color in Godot shaders are stored in sRGB;
	// material_get_param returns the raw sRGB value, so we convert to linear here.
	Variant albedo_var = material_storage->material_get_param(p_material_rid, "albedo");
	if (is_base_material(p_material_rid, albedo_var)) {
		Color c = ((Color)albedo_var).srgb_to_linear();
		mat.albedo_color[0] = c.r;
		mat.albedo_color[1] = c.g;
		mat.albedo_color[2] = c.b;
		mat.albedo_color[3] = c.a;
		mat_data->rt_sbt_offset = 0;
		mat_data->is_custom_shader = false;
		mat_data->shader_failed = false;
	} else {
		mat_data->is_custom_shader = true;
		uint32_t shader_id = material_storage->material_get_shader_id(p_material_rid);
		mat_data->rt_sbt_offset = SceneShaderRaytracing::get_singleton()->register_custom_shader(shader_id, p_material_rid, &mat_data->shader_failed);

		const SceneShaderRaytracing::CustomShaderEntry *cse =
				SceneShaderRaytracing::get_singleton()->get_custom_shader_entry(mat_data->rt_sbt_offset);
		if (cse && cse->uniform_total_size > 0) {
			Vector<uint8_t> ubo_data;
			ubo_data.resize(cse->uniform_total_size);
			memset(ubo_data.ptrw(), 0, cse->uniform_total_size);

			for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &kv : cse->uniforms) {
				const ShaderLanguage::ShaderNode::Uniform &u = kv.value;
				// Instance uniforms live in the global buffer (rt_instance_uniforms_ofs), not in the material's: as raster's
				// MaterialData::update_uniform_buffer(), skip them. Their order is left at 0, so packing one wrote over the
				// material's first uniform (godot3d's grass: rain_wetness's global index, which then read grass_lod_width).
				if (ShaderLanguage::is_sampler_type(u.type) || u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
					continue;
				}
				if (u.order < 0 || u.order >= (int)cse->uniform_offsets.size()) {
					continue;
				}

				uint32_t offset = cse->uniform_offsets[u.order];
				uint32_t size = ShaderLanguage::get_datatype_size(u.type);
				if (u.array_size > 0) {
					const uint32_t padded = 16 * u.array_size;
					size *= u.array_size;
					if (size % padded != 0) {
						size += padded - (size % padded);
					}
				}
				if (offset + size > cse->uniform_total_size) {
					continue;
				}

				uint8_t *dst = ubo_data.ptrw() + offset;

				if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
					int32_t idx = material_storage->global_shader_uniform_get_buffer_index(kv.key);
					uint32_t uidx = (idx >= 0) ? (uint32_t)idx : 0;
					memcpy(dst, &uidx, sizeof(uint32_t));
				} else if (u.array_size > 0) {
					Variant val = material_storage->material_get_param(p_material_rid, kv.key);
					RendererRD::MaterialStorage::fill_std140_uniform(u, val.get_type() == Variant::NIL ? nullptr : &val, dst, true);
				} else {
					Variant val = material_storage->material_get_param(p_material_rid, kv.key);
					pack_uniform(u, val, dst);
				}
			}

			RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
			for (int ti = 0; ti < cse->texture_uniforms.size(); ti++) {
				const SceneShaderRaytracing::TextureUniformInfo &tui = cse->texture_uniforms[ti];
				BindlessBlock *table = tui.is_array ? bindless_array_block : bindless_block;
				uint32_t bindless_idx = 0;

				if (tui.is_global) {
					RID tex_rid = material_storage->global_shader_uniform_get_texture(tui.name);
					if (tex_rid.is_valid()) {
						RID rd_tex = ts->texture_get_rd_texture(tex_rid, tui.use_color);
						if (rd_tex.is_valid()) {
							bindless_idx = table->add_texture(rd_tex);
						}
					}
				} else {
					Variant tex_var = material_storage->material_get_param(p_material_rid, tui.name);
					if (tex_var.get_type() == Variant::OBJECT || tex_var.get_type() == Variant::RID) {
						RID tex_rid = tex_var;
						if (tex_rid.is_valid()) {
							RID rd_tex = ts->texture_get_rd_texture(tex_rid, tui.use_color);
							if (rd_tex.is_valid()) {
								bindless_idx = table->add_texture(rd_tex);
							}
						}
					}
				}

				if (bindless_idx == 0 && tui.hint != ShaderLanguage::ShaderNode::Uniform::HINT_NONE) {
					using Hint = ShaderLanguage::ShaderNode::Uniform::Hint;
					using DefaultTexture = RendererRD::TextureStorage::DefaultRDTexture;
					DefaultTexture default_type;
					switch (tui.hint) {
						case Hint::HINT_DEFAULT_BLACK:
							default_type = tui.is_array ? DefaultTexture::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : DefaultTexture::DEFAULT_RD_TEXTURE_BLACK;
							break;
						case Hint::HINT_DEFAULT_TRANSPARENT:
							default_type = tui.is_array ? DefaultTexture::DEFAULT_RD_TEXTURE_2D_ARRAY_TRANSPARENT : DefaultTexture::DEFAULT_RD_TEXTURE_TRANSPARENT;
							break;
						case Hint::HINT_NORMAL:
							default_type = tui.is_array ? DefaultTexture::DEFAULT_RD_TEXTURE_2D_ARRAY_NORMAL : DefaultTexture::DEFAULT_RD_TEXTURE_NORMAL;
							break;
						case Hint::HINT_ANISOTROPY:
							default_type = tui.is_array ? DefaultTexture::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE : DefaultTexture::DEFAULT_RD_TEXTURE_ANISO;
							break;
						default:
							default_type = tui.is_array ? DefaultTexture::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE : DefaultTexture::DEFAULT_RD_TEXTURE_WHITE;
							break;
					}
					RID default_tex = ts->texture_rd_get_default(default_type);
					if (default_tex.is_valid()) {
						bindless_idx = table->add_texture(default_tex);
					}
				}

				if (tui.buffer_offset + 4 <= cse->uniform_total_size) {
					memcpy(ubo_data.ptrw() + tui.buffer_offset, &bindless_idx, 4);
				}
			}

			// If the shader declared hint_alpha, mirror that bindless index into
			// mat.albedo_texture_idx so ray_query_alpha_test() can sample it cheaply.
			if (cse->alpha_texture_buffer_offset != UINT32_MAX &&
					cse->alpha_texture_buffer_offset + 4 <= cse->uniform_total_size) {
				uint32_t alpha_idx = 0;
				memcpy(&alpha_idx, ubo_data.ptr() + cse->alpha_texture_buffer_offset, 4);
				mat.albedo_texture_idx = alpha_idx;
			}

			// Try the suballoc pool first. Common materials (UBO <= slot size)
			// just buffer_update an existing slot - O(1), no driver allocation,
			// no per-frame storage_buffer_create cost.
			bool used_pool = false;
			if (cse->uniform_total_size <= MAT_UBO_POOL_SLOT_SIZE) {
				if (mat_data->uniform_pool_slot == UINT32_MAX) {
					mat_data->uniform_pool_slot = mat_ubo_pool_allocate();
				}
				if (mat_data->uniform_pool_slot != UINT32_MAX) {
					// Transitioning from a dedicated buffer back into the pool.
					if (mat_data->uniform_buffer.is_valid()) {
						mat_ubo_dedicated_buffers.erase(mat_data->uniform_buffer);
						RD::get_singleton()->free_rid(mat_data->uniform_buffer);
						mat_data->uniform_buffer = RID();
					}
					mat_ubo_pool_update(mat_data->uniform_pool_slot, ubo_data.ptr(), cse->uniform_total_size);
					mat.uniform_address = mat_ubo_pool_get_address(mat_data->uniform_pool_slot);
					used_pool = true;
				}
			}

			if (!used_pool) {
				// Oversized or pool exhausted: a buffer of the material's own, updated in place by the next rebuilds of the
				// same size. Recreating it on every rebuild allocated a buffer (and on D3D12 a GPU pointer descriptor) every
				// frame for a material changed every frame: Terrain3D's (36 KB of region maps) follows the camera.
				if (mat_data->uniform_pool_slot != UINT32_MAX) {
					mat_ubo_pool_release(mat_data->uniform_pool_slot);
					mat_data->uniform_pool_slot = UINT32_MAX;
				}
				if (mat_data->uniform_buffer.is_valid() && mat_data->uniform_buffer_size == cse->uniform_total_size) {
					RD::get_singleton()->buffer_update(mat_data->uniform_buffer, 0, cse->uniform_total_size, ubo_data.ptr());
				} else {
					if (mat_data->uniform_buffer.is_valid()) {
						mat_ubo_dedicated_buffers.erase(mat_data->uniform_buffer);
						RD::get_singleton()->free_rid(mat_data->uniform_buffer);
					}
					print_verbose(vformat("RT: material uniforms in a buffer of their own (%s): sbt_offset=%d, %d bytes, pool slots %d bytes.",
							cse->uniform_total_size > MAT_UBO_POOL_SLOT_SIZE ? "larger than a pool slot" : "pool exhausted",
							mat_data->rt_sbt_offset, cse->uniform_total_size, MAT_UBO_POOL_SLOT_SIZE));
					mat_data->uniform_buffer = RD::get_singleton()->storage_buffer_create(cse->uniform_total_size, ubo_data, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
					mat_data->uniform_buffer_size = cse->uniform_total_size;
					mat_ubo_dedicated_buffers.insert(mat_data->uniform_buffer);
					RD::get_singleton()->set_resource_name(mat_data->uniform_buffer, String("RT Material UBO [sbt=") + itos(mat_data->rt_sbt_offset) + "]");
				}
				mat.uniform_address = RD::get_singleton()->buffer_get_device_address(mat_data->uniform_buffer);
			}
		}
	}

	Variant metallic_var = material_storage->material_get_param(p_material_rid, "metallic");
	if (metallic_var.get_type() == Variant::FLOAT) {
		mat.metallic = metallic_var;
	}

	Variant roughness_var = material_storage->material_get_param(p_material_rid, "roughness");
	if (roughness_var.get_type() == Variant::FLOAT) {
		mat.roughness = roughness_var;
	}

	Variant specular_var = material_storage->material_get_param(p_material_rid, "specular");
	if (specular_var.get_type() == Variant::FLOAT) {
		mat.specular = specular_var;
	}

	Variant emission_var = material_storage->material_get_param(p_material_rid, "emission");
	if (emission_var.get_type() == Variant::COLOR) {
		Color c = ((Color)emission_var).srgb_to_linear();
		mat.emission_color[0] = c.r;
		mat.emission_color[1] = c.g;
		mat.emission_color[2] = c.b;
	}

	Variant emission_energy_var = material_storage->material_get_param(p_material_rid, "emission_energy");
	if (emission_energy_var.get_type() == Variant::FLOAT) {
		mat.emission_strength = emission_energy_var;
	}

	// UV1 scale and offset (z is only used by triplanar mapping).
	Variant uv1_scale_var = material_storage->material_get_param(p_material_rid, "uv1_scale");
	if (uv1_scale_var.get_type() == Variant::VECTOR3) {
		Vector3 s = uv1_scale_var;
		mat.uv1_scale[0] = s.x;
		mat.uv1_scale[1] = s.y;
		mat.uv1_scale_z = s.z;
	}

	Variant uv1_offset_var = material_storage->material_get_param(p_material_rid, "uv1_offset");
	if (uv1_offset_var.get_type() == Variant::VECTOR3) {
		Vector3 o = uv1_offset_var;
		mat.uv1_offset[0] = o.x;
		mat.uv1_offset[1] = o.y;
		mat.uv1_offset_z = o.z;
	}

	// BaseMaterial3D's triplanar mapping and billboard modes are shader features, not parameters: read them from its
	// generated code.
	mat_data->billboard_mode = RT_BILLBOARD_DISABLED;
	mat_data->billboard_keep_scale = false;
	if (!mat_data->is_custom_shader) {
		const String code = material_storage->material_get_shader_code_rt(p_material_rid);
		if (code.contains("// Billboard Mode: Enabled")) {
			mat_data->billboard_mode = RT_BILLBOARD_ENABLED;
		} else if (code.contains("// Billboard Mode: Y-Billboard")) {
			mat_data->billboard_mode = RT_BILLBOARD_FIXED_Y;
		}
		mat_data->billboard_keep_scale = code.contains("// Billboard Keep Scale: Enabled");
		if (code.contains(", unshaded")) {
			mat.flags |= RT_MAT_FLAG_UNSHADED;
		}
		// Ambient occlusion: its own texture on UV1 (triplanar too), or the ORM texture's red channel. Not on UV2.
		if (code.contains("AO = dot(texture(texture_ambient_occlusion, base_uv)") || code.contains("AO = dot(triplanar_texture(texture_ambient_occlusion, uv1_power_normal")) {
			const RID ao_rd = get_material_texture("texture_ambient_occlusion");
			if (ao_rd.is_valid()) {
				mat.ao_texture_idx = bindless_block->add_texture(ao_rd);
				mat.flags |= RT_MAT_FLAG_AO_TEXTURE;
				const Variant channel_var = material_storage->material_get_param(p_material_rid, "ao_texture_channel");
				Vector4 channel_mask(1, 0, 0, 0);
				if (channel_var.get_type() == Variant::VECTOR4) {
					channel_mask = channel_var;
				} else if (channel_var.get_type() == Variant::PLANE) {
					const Plane p = channel_var;
					channel_mask = Vector4(p.normal.x, p.normal.y, p.normal.z, p.d);
				}
				mat.ao_channel = channel_mask.y > 0.5f ? 1 : (channel_mask.z > 0.5f ? 2 : (channel_mask.w > 0.5f ? 3 : 0));
			}
		} else if (code.contains("AO = orm_tex.r;")) {
			mat.flags |= RT_MAT_FLAG_AO_ORM;
		}
		if (mat.flags & (RT_MAT_FLAG_AO_TEXTURE | RT_MAT_FLAG_AO_ORM)) {
			const Variant affect_var = material_storage->material_get_param(p_material_rid, "ao_light_affect");
			if (affect_var.get_type() == Variant::FLOAT) {
				mat.ao_light_affect = affect_var;
			}
		}
		// texture_filter is a sampler hint in the generated code, not a parameter.
		if (code.contains(", filter_linear_mipmap_anisotropic")) {
			mat.flags |= RT_MAT_FLAG_ANISOTROPIC_FILTER;
		}
		if (code.contains("// Vertex Color Use as Albedo: Enabled")) {
			mat.flags |= RT_MAT_FLAG_VERTEX_COLOR_ALBEDO;
			if (code.contains("// Vertex Color is sRGB: Enabled")) {
				mat.flags |= RT_MAT_FLAG_VERTEX_COLOR_SRGB;
			}
		}
		if (code.contains(", specular_disabled")) {
			mat.flags |= RT_MAT_FLAG_SPECULAR_DISABLED;
		}
		if (code.contains("BACKLIGHT = (backlight.rgb")) {
			mat.flags |= RT_MAT_FLAG_BACKLIGHT;
			Variant backlight_var = material_storage->material_get_param(p_material_rid, "backlight");
			if (backlight_var.get_type() == Variant::COLOR) {
				const Color c = ((Color)backlight_var).srgb_to_linear();
				mat.backlight[0] = c.r;
				mat.backlight[1] = c.g;
				mat.backlight[2] = c.b;
			}
			const RID backlight_rd = get_material_texture("texture_backlight", true);
			if (backlight_rd.is_valid()) {
				mat.backlight_texture_idx = bindless_block->add_texture(backlight_rd);
				mat.flags |= RT_MAT_FLAG_HAS_BACKLIGHT_TEX;
			}
		}
		if (code.contains("// Emission: Enabled")) {
			mat.flags |= RT_MAT_FLAG_EMISSION;
			if (code.contains("// Emission Operator: Add")) {
				mat.flags |= RT_MAT_FLAG_EMISSION_ADD;
			}
		}
		if (code.contains("varying vec3 uv1_triplanar_pos;")) {
			mat.flags |= RT_MAT_FLAG_UV1_TRIPLANAR;
			if (code.contains("uv1_triplanar_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz")) {
				mat.flags |= RT_MAT_FLAG_UV1_WORLD_TRIPLANAR;
			}
			Variant sharpness_var = material_storage->material_get_param(p_material_rid, "uv1_blend_sharpness");
			if (sharpness_var.get_type() == Variant::FLOAT) {
				mat.uv1_blend_sharpness = sharpness_var;
			}
		}
	}

	// Point filtering: check if material requests nearest filtering (e.g. pixel art).
	// BaseMaterial3D exposes this as "texture_filter" int param (0=nearest, 1=linear, etc.).
	Variant filter_var = material_storage->material_get_param(p_material_rid, "texture_filter");
	if (filter_var.get_type() == Variant::INT) {
		int filter_mode = filter_var;
		// 0 = TEXTURE_FILTER_NEAREST, 2 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS,
		// 4 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC
		if (filter_mode == 0 || filter_mode == 2 || filter_mode == 4) {
			mat.flags |= RT_MAT_FLAG_POINT_FILTER;
		}
	}

	// Update cache entry
	entry->cached_counter = p_material_invalidation_counter;
	entry->cached_rid_version = mat_version;
	entry->last_used_frame = current_frame;

	return mat_data;
}

// ---------------------------------------------------------------------------
// Acceleration structure building
// ---------------------------------------------------------------------------

void RenderRaytracing::build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list) {
	RENDER_TIMESTAMP("BLAS Build");

	for (const RID &blas_rid : p_dirty_blas_list) {
		if (blas_rid.is_valid()) {
			RD::get_singleton()->blas_build(blas_rid);
		}
	}

	for (const RID &blas_rid : p_dirty_blas_update_list) {
		if (blas_rid.is_valid()) {
			RD::get_singleton()->blas_update(blas_rid);
		}
	}

	RENDER_TIMESTAMP("TLAS Build");

	uint32_t needed = MAX(blass.size() + tlas_expand_instance_count, (uint32_t)1);
	if (!p_state->tlas.is_valid() || needed > p_state->tlas_max_instances) {
		if (p_state->tlas.is_valid()) {
			RD::get_singleton()->free_rid(p_state->tlas);
		}
		p_state->tlas_max_instances = needed * 2;
		p_state->tlas = RD::get_singleton()->tlas_create(p_state->tlas_max_instances, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		RD::get_singleton()->set_resource_name(p_state->tlas, "RT TLAS");
	}

	LocalVector<RD::AccelerationStructureInstance> instances;
	instances.resize(blass.size());
	for (uint32_t i = 0; i < blass.size(); i++) {
		RD::AccelerationStructureInstance &inst = instances[i];
		inst.id = i;
		inst.transform = blas_transforms[i];
		inst.blas = blass[i];
		inst.flags = BitField<RD::AccelerationStructureInstanceFlagBits>(instance_flags[i]);
		inst.mask = instance_masks[i];
		uint32_t sbt_off = (i < sbt_offsets.size()) ? sbt_offsets[i] : 0;
		inst.hit_sbt_range = RD::HitShaderBindingTableRange((1ULL << 32) | uint64_t(sbt_off));
	}

	if (tlas_expand_instance_count > 0) {
		_build_tlas_with_expanded_multimeshes(p_state, instances);
		return;
	}
	RD::get_singleton()->tlas_build(p_state->tlas, instances);
}

void RenderRaytracing::_build_tlas_with_expanded_multimeshes(RTViewportState *p_state, LocalVector<RD::AccelerationStructureInstance> &p_instances) {
	RD *rd = RD::get_singleton();
	DEV_ASSERT(geometry_data.size() == p_instances.size() && motion_indices.size() == p_instances.size());
	const uint32_t cpu_count = p_instances.size();
	const uint32_t total = cpu_count + tlas_expand_instance_count;
	const uint32_t instance_size = rd->tlas_get_instance_size();

	if (!p_state->tlas_instance_buffer.is_valid() || p_state->tlas_instance_buffer_capacity < total) {
		if (p_state->tlas_instance_buffer.is_valid()) {
			rd->free_rid(p_state->tlas_instance_buffer);
		}
		p_state->tlas_instance_buffer_capacity = p_state->tlas_max_instances;
		p_state->tlas_instance_buffer = rd->storage_buffer_create(p_state->tlas_instance_buffer_capacity * instance_size, {}, 0,
				RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		ERR_FAIL_COND(!p_state->tlas_instance_buffer.is_valid());
		rd->set_resource_name(p_state->tlas_instance_buffer, "RT TLAS instances");
	}

	// The CPU's instances, then for each job its instance's last words (BLAS, mask, flags, hit group) as the driver writes them.
	const uint32_t job_count = tlas_expand_jobs.size();
	thread_local LocalVector<RD::AccelerationStructureInstance> templates;
	thread_local LocalVector<uint8_t> encoded;
	templates.resize(job_count);
	for (uint32_t i = 0; i < job_count; i++) {
		const TlasExpandJob &job = tlas_expand_jobs[i];
		RD::AccelerationStructureInstance &inst = templates[i];
		inst = RD::AccelerationStructureInstance();
		inst.blas = job.blas;
		inst.mask = job.mask;
		inst.flags = BitField<RD::AccelerationStructureInstanceFlagBits>(job.flags);
		inst.hit_sbt_range = RD::HitShaderBindingTableRange((1ULL << 32) | uint64_t(job.sbt_offset));
	}
	encoded.resize(MAX(cpu_count, job_count) * instance_size);
	ERR_FAIL_COND(rd->tlas_instances_encode(templates, encoded.ptr()) != OK);
	for (uint32_t i = 0; i < job_count; i++) {
		memcpy(tlas_expand_jobs[i].data.instance_tail, encoded.ptr() + i * instance_size + 48, 16);
	}
	if (cpu_count > 0) {
		ERR_FAIL_COND(rd->tlas_instances_encode(p_instances, encoded.ptr()) != OK);
		rd->buffer_update(p_state->tlas_instance_buffer, 0, cpu_count * instance_size, encoded.ptr());
	}

	// The jobs and their records' templates.
	thread_local LocalVector<RT_TlasExpandJob> job_data;
	job_data.resize(job_count);
	for (uint32_t i = 0; i < job_count; i++) {
		job_data[i] = tlas_expand_jobs[i].data;
	}
	const uint32_t template_count = tlas_expand_geometry_templates.size();
	if (tlas_expand_job_capacity < job_count) {
		if (tlas_expand_job_buffer.is_valid()) {
			rd->free_rid(tlas_expand_job_buffer);
		}
		tlas_expand_job_capacity = Math::next_power_of_2(job_count);
		tlas_expand_job_buffer = rd->storage_buffer_create(tlas_expand_job_capacity * sizeof(RT_TlasExpandJob));
		rd->set_resource_name(tlas_expand_job_buffer, "RT TLAS expand jobs");
	}
	if (tlas_expand_template_capacity < template_count) {
		for (RID *buffer : { &tlas_expand_geometry_template_buffer, &tlas_expand_material_template_buffer }) {
			if (buffer->is_valid()) {
				rd->free_rid(*buffer);
			}
		}
		tlas_expand_template_capacity = Math::next_power_of_2(template_count);
		tlas_expand_geometry_template_buffer = rd->storage_buffer_create(tlas_expand_template_capacity * sizeof(RT_GeometryData));
		tlas_expand_material_template_buffer = rd->storage_buffer_create(tlas_expand_template_capacity * sizeof(RT_MaterialData));
		rd->set_resource_name(tlas_expand_geometry_template_buffer, "RT TLAS expand geometry templates");
		rd->set_resource_name(tlas_expand_material_template_buffer, "RT TLAS expand material templates");
	}
	rd->buffer_update(tlas_expand_job_buffer, 0, job_count * sizeof(RT_TlasExpandJob), job_data.ptr());
	rd->buffer_update(tlas_expand_geometry_template_buffer, 0, template_count * sizeof(RT_GeometryData), tlas_expand_geometry_templates.ptr());
	rd->buffer_update(tlas_expand_material_template_buffer, 0, template_count * sizeof(RT_MaterialData), tlas_expand_material_templates.ptr());

	const LocalVector<RD::Uniform> uniforms = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, tlas_expand_job_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, tlas_expand_geometry_template_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, tlas_expand_material_template_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_state->tlas_instance_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_state->geometry_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_state->material_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_state->motion_index_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_state->motion_transform_buffer),
	};
	const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(tlas_expand_shader.version_shader, 0, uniforms);

	struct {
		uint32_t job_count;
		uint32_t first_slot;
		uint32_t instance_count;
		uint32_t geometry_words;
		uint32_t material_words;
		uint32_t mm_offset_word;
		uint32_t mm_prev_offset_word;
		uint32_t pad;
	} params = { job_count, cpu_count, tlas_expand_instance_count, uint32_t(sizeof(RT_GeometryData) / 4), uint32_t(sizeof(RT_MaterialData) / 4),
		uint32_t(offsetof(RT_GeometryData, multimesh_offset) / 4), uint32_t(offsetof(RT_GeometryData, multimesh_previous_offset) / 4), 0 };

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, tlas_expand_shader.pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	rd->compute_list_set_push_constant(compute_list, &params, sizeof(params));
	// Read through their addresses: the graph orders this pass after their updates.
	for (const TlasExpandJob &job : tlas_expand_jobs) {
		rd->compute_list_add_buffer_dependency(compute_list, job.mm_gpu_buffer, false);
	}
	rd->compute_list_dispatch_threads(compute_list, tlas_expand_instance_count, 1, 1);
	rd->compute_list_end();

	// Every BLAS the instances reference: the build waits for those built this frame.
	thread_local LocalVector<RID> tlas_blases;
	tlas_blases.clear();
	for (const RD::AccelerationStructureInstance &inst : p_instances) {
		if (inst.blas.is_valid()) {
			tlas_blases.push_back(inst.blas);
		}
	}
	for (const TlasExpandJob &job : tlas_expand_jobs) {
		tlas_blases.push_back(job.blas);
	}
	rd->tlas_build_from_buffer(p_state->tlas, p_state->tlas_instance_buffer, total, tlas_blases);
}

void RenderRaytracing::finalize_buffers(RTViewportState *p_state) {
	// Grow-only uploads. Callers must not free these in prepare_frame(). p_size: the CPU's part, uploaded; p_total_size:
	// with what the GPU writes after it (_build_tlas_with_expanded_multimeshes()).
	auto update_or_grow = [](RID &p_buffer, uint32_t &p_capacity, const void *p_data, uint32_t p_size, uint32_t p_total_size) {
		if (p_total_size == 0) {
			return;
		}
		if (p_total_size > p_capacity) {
			if (p_buffer.is_valid()) {
				RD::get_singleton()->free_rid(p_buffer);
			}
			// With room for the GPU's part to grow a little (units entering the view) without a new buffer each frame.
			p_capacity = p_size == p_total_size ? p_total_size : STEPIFY(p_total_size + p_total_size / 4, 16u);
			p_buffer = RD::get_singleton()->storage_buffer_create(p_capacity);
		}
		if (p_size > 0) {
			RD::get_singleton()->buffer_update(p_buffer, 0, p_size, p_data);
		}
	};

	const uint32_t expanded = tlas_expand_instance_count;
	update_or_grow(p_state->geometry_buffer, p_state->geometry_buffer_capacity,
			geometry_data.ptr(), geometry_data.size() * sizeof(RT_GeometryData), (geometry_data.size() + expanded) * sizeof(RT_GeometryData));
	update_or_grow(p_state->material_buffer, p_state->material_buffer_capacity,
			material_data.ptr(), material_data.size() * sizeof(RT_MaterialData), (material_data.size() + expanded) * sizeof(RT_MaterialData));
	update_or_grow(p_state->motion_index_buffer, p_state->motion_index_buffer_capacity,
			motion_indices.ptr(), motion_indices.size() * sizeof(int32_t), (motion_indices.size() + expanded) * sizeof(int32_t));
	update_or_grow(p_state->motion_transform_buffer, p_state->motion_transform_buffer_capacity,
			motion_transforms.ptr(), motion_transforms.size() * sizeof(RT_InstanceMotionData), (motion_transforms.size() + tlas_expand_motion_count) * sizeof(RT_InstanceMotionData));
}

// ---------------------------------------------------------------------------
// Merged MultiMesh BLAS builder
// ---------------------------------------------------------------------------

void RenderRaytracing::_free_lod_view(RTLodView &p_view) {
	RD *rd = RD::get_singleton();
	if (p_view.vertex_remap_buffer.is_valid()) {
		rd->free_rid(p_view.vertex_remap_buffer);
		p_view.vertex_remap_buffer = RID();
	}
	if (p_view.index_buffer.is_valid()) {
		rd->free_rid(p_view.index_buffer);
		p_view.index_buffer = RID();
	}
}

const RTLodView *RenderRaytracing::_get_lod_view(void *p_mesh_surface, uint32_t p_lod) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(p_mesh_surface);
	const uint32_t current_frame = RSG::rasterizer->get_frame_number();
	RTLodViewKey key;
	key.surface = p_mesh_surface;
	key.lod = p_lod;

	RTLodView *view = lod_views.getptr(key);
	if (view && view->surface_counter != surface_counter) {
		_free_lod_view(*view);
		lod_views.erase(key);
		view = nullptr;
	}
	if (view) {
		view->last_used_frame = current_frame;
		return view;
	}

	const RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, p_lod);
	const uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, p_lod);
	const uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	ERR_FAIL_COND_V(!index_buffer.is_valid() || index_count == 0 || vertex_count == 0, nullptr);

	// Built on the GPU: reading the indices back would stall it in the middle of the frame.
	RD *rd = RD::get_singleton();
	const uint32_t kept_bound = MIN(vertex_count, index_count);
	Vector<uint8_t> zeros;
	zeros.resize(MAX(vertex_count * uint32_t(sizeof(uint32_t)), 16u));
	memset(zeros.ptrw(), 0, zeros.size());

	RTLodView new_view;
	new_view.vertex_count = kept_bound;
	new_view.index_count = index_count;
	new_view.surface_counter = surface_counter;
	new_view.last_used_frame = current_frame;
	new_view.vertex_remap_buffer = rd->storage_buffer_create(kept_bound * sizeof(uint32_t), Span<uint8_t>(zeros.ptr(), kept_bound * sizeof(uint32_t)));
	new_view.index_buffer = rd->storage_buffer_create(index_count * sizeof(uint32_t));
	const RID kept_buffer = rd->storage_buffer_create(vertex_count * sizeof(uint32_t), Span<uint8_t>(zeros.ptr(), vertex_count * sizeof(uint32_t)));
	const RID counter_buffer = rd->storage_buffer_create(16, Span<uint8_t>(zeros.ptr(), 16));
	if (!new_view.vertex_remap_buffer.is_valid() || !new_view.index_buffer.is_valid() || !kept_buffer.is_valid() || !counter_buffer.is_valid()) {
		_free_lod_view(new_view);
		if (kept_buffer.is_valid()) {
			rd->free_rid(kept_buffer);
		}
		if (counter_buffer.is_valid()) {
			rd->free_rid(counter_buffer);
		}
		ERR_FAIL_V(nullptr);
	}
	rd->set_resource_name(new_view.vertex_remap_buffer, "RT LOD vertex remap");
	rd->set_resource_name(new_view.index_buffer, "RT LOD indices");

	Vector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, kept_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, counter_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, new_view.vertex_remap_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, new_view.index_buffer));
	const RID uniform_set = rd->uniform_set_create(uniforms, lod_compact_shader.version_shader[0], 0, /*p_linear_pool=*/true);

	const uint64_t index_address = rd->buffer_get_device_address(index_buffer);
	const uint32_t params[8] = { uint32_t(index_address), uint32_t(index_address >> 32), index_count, vertex_count <= 65536 ? 1u : 0u, vertex_count, 0, 0, 0 };
	const uint32_t threads[LodCompactShader::MODE_MAX] = { index_count, vertex_count, index_count };

	RD::ComputeListID compute_list = rd->compute_list_begin();
	for (int mode = 0; mode < LodCompactShader::MODE_MAX; mode++) {
		if (mode > 0) {
			rd->compute_list_add_barrier(compute_list);
		}
		rd->compute_list_bind_compute_pipeline(compute_list, lod_compact_shader.pipeline[mode]);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		rd->compute_list_set_push_constant(compute_list, params, sizeof(params));
		rd->compute_list_dispatch_threads(compute_list, threads[mode], 1, 1);
	}
	rd->compute_list_end();

	// Freed once the GPU is done with this frame.
	rd->free_rid(uniform_set);
	rd->free_rid(kept_buffer);
	rd->free_rid(counter_buffer);
	return &lod_views.insert(key, new_view)->value;
}

bool RenderRaytracing::_set_multimesh_instance_data(RT_GeometryData &r_geom, RID p_mm_rid, RID p_mm_gpu_buffer, uint32_t p_vertex_count) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const bool uses_colors = mesh_storage->multimesh_uses_colors(p_mm_rid);
	const bool uses_custom_data = mesh_storage->multimesh_uses_custom_data(p_mm_rid);
	uint32_t current_offset = 0;
	uint32_t previous_offset = 0;
	// The previous transforms are kept while a viewport uses motion vectors (DLSS, TAA), as raster reads them.
	const bool moved = p_vertex_count != UINT32_MAX && mesh_storage->_multimesh_get_motion_vectors_offsets(p_mm_rid, current_offset, previous_offset) && previous_offset != current_offset;
	// A merged BLAS always: its hit shaders find each instance's transform there (its object space, as raster's).
	if ((!uses_colors && !uses_custom_data && p_vertex_count == UINT32_MAX) || !p_mm_gpu_buffer.is_valid()) {
		return false;
	}
	// The 3D layout, as raster reads it: 12 floats of transform, then 4 of color and 4 of custom data when used.
	const uint32_t color_offset = uses_colors ? 12 : 0xFFFF;
	const uint32_t custom_offset = uses_custom_data ? (uses_colors ? 16 : 12) : 0xFFFF;
	const uint64_t address = RD::get_singleton()->buffer_get_device_address(p_mm_gpu_buffer);
	r_geom.multimesh_address_lo = uint32_t(address);
	r_geom.multimesh_address_hi = uint32_t(address >> 32);
	r_geom.multimesh_stride = mesh_storage->multimesh_get_stride(p_mm_rid);
	r_geom.multimesh_offset = mesh_storage->multimesh_get_current_instance_offset(p_mm_rid);
	r_geom.multimesh_previous_offset = moved ? previous_offset : r_geom.multimesh_offset;
	r_geom.multimesh_vertex_count = p_vertex_count;
	r_geom.multimesh_data_offsets = color_offset | (custom_offset << 16);
	return true;
}

bool RenderRaytracing::_mm_fits_merged_blas(void *p_mesh_surface, uint32_t p_mm_count, const RTLodView *p_lod_view) const {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);
	if (p_lod_view) {
		vertex_count = p_lod_view->vertex_count;
		index_buffer = p_lod_view->index_buffer;
		index_count = p_lod_view->index_count;
	}
	const uint32_t prim_count = (index_buffer.is_valid() && index_count > 0) ? (index_count / 3) : (vertex_count / 3);
	if (prim_count == 0 || vertex_count == 0) {
		return false;
	}
	// Too large: one TLAS instance per instance instead.
	static const uint32_t MM_MERGED_BLAS_MAX_TRIANGLES = (uint32_t)GLOBAL_GET("rendering/pathtracing/multimesh_merged_blas_max_triangles");
	return (uint64_t)p_mm_count * prim_count <= MM_MERGED_BLAS_MAX_TRIANGLES;
}

bool RenderRaytracing::_build_merged_mm_blas(
		RID p_mm_rid,
		RID p_mm_gpu_buffer,
		void *p_mesh_surface,
		uint32_t p_mm_count,
		uint32_t p_surface_index,
		uint32_t p_surface_counter,
		uint32_t p_lod,
		bool p_displace,
		bool p_reevaluate,
		RTMaterialData *p_mat_data,
		RID p_material,
		uint16_t p_material_counter,
		bool p_allow_omm,
		const Transform3D &p_transform,
		const Transform3D &p_prev_transform,
		uint32_t p_instance_uniforms_ofs,
		RD::ComputeListID p_compute_list,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list,
		RTSurfaceData *r_surf_data) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	// Vertices of the source mesh, and of every instance in the merged buffers (fewer than the mesh's for a LOD).
	const uint32_t src_vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	uint32_t vertex_count = src_vertex_count;
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);
	const RTLodView *lod_view = nullptr;
	if (p_lod > 0) {
		// Looked up (and created) in phase 1; this only finds it.
		lod_view = lod_views.getptr({ p_mesh_surface, p_lod });
		if (lod_view) {
			vertex_count = lod_view->vertex_count;
			index_buffer = lod_view->index_buffer;
			index_count = lod_view->index_count;
		}
	}
	const uint32_t lod = lod_view ? p_lod : 0;
	bool indexed = index_buffer.is_valid() && index_count > 0;
	uint32_t prim_count = indexed ? (index_count / 3) : (vertex_count / 3);

	if (!_mm_fits_merged_blas(p_mesh_surface, p_mm_count, lod_view)) {
		return false;
	}
	const uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);

	// Side table -> pool: dense direct index keyed by RID local_index, with a
	// validator stamp to detect RID local-index recycling for a different MM.
	const uint32_t li = p_mm_rid.get_local_index();
	const uint32_t mm_validator = static_cast<uint32_t>(p_mm_rid.get_id() >> 32);
	if (li >= mm_handles.size()) {
		mm_handles.resize(li + 1);
	}
	MMSurfaceHandles &mm_entry = mm_handles[li];
	if (mm_entry.mm_validator != mm_validator) {
		// Local index has been recycled (or first-ever access). Release any slots
		// the previous owner left behind so we don't leak until TTL.
		for (RID &handle : mm_entry.per_surface) {
			_free_merged_mm_entry(handle);
			handle = RID();
		}
		mm_entry.per_surface.clear();
		mm_entry.mm_validator = mm_validator;
	}

	RTMergedMMEntry *entry_ptr = _access_merged_mm_slot(mm_entry.surface_handle(p_surface_index));
	ERR_FAIL_NULL_V(entry_ptr, false);
	RTMergedMMEntry &entry = *entry_ptr;
	const uint32_t cache_key = (li << 8) | (p_surface_index & 0xFF); // Used only for resource naming below.

	const uint32_t current_frame = RSG::rasterizer->get_frame_number();

	// First viewport this frame does the merge dispatch + BLAS refit; later
	// viewports referencing the same MultiMesh-surface reuse the result.
	// `last_used_frame` doubles as the per-frame guard, so capture the gate
	// BEFORE bumping it.
	const bool first_touch_this_frame = entry.last_used_frame != current_frame;
	entry.last_used_frame = current_frame;

	// Detect whether instance transforms moved since our last bake. Without this
	// we'd re-merge + refit every frame even for fully-static MultiMeshes, which
	// is significant wasted compute for high-instance-count crowds/foliage.
	const uint64_t mm_last_change = mesh_storage->multimesh_get_last_change(p_mm_rid);
	const bool transforms_changed = (entry.cached_mm_last_change != mm_last_change);

	bool structure_changed = (entry.last_mm_count != p_mm_count ||
			entry.last_surface_counter != p_surface_counter ||
			entry.last_lod != lod);

	if (structure_changed) {
		// Other triangles are active: a full build, in place when the capacity below holds them.
		entry.blas_built_once = false;
		entry.last_mm_count = p_mm_count;
		entry.last_surface_counter = p_surface_counter;
		entry.last_lod = lod;
	}

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;
	bool has_tbn = has_normal;
	// Compressed source: [UNORM16x4 positions × V] + [octahedral normal (UNORM16x2) × V], the tangent in each
	// position's w. The merge shader decompresses it; the merged buffers below are always uncompressed.
	const bool compressed = surface_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;

	// Layout of uncompressed vertex buffer: [float3 positions × V] + [packed TBN × V].
	// normal_stride = 8 when both normal+tangent present (two uint16x2 packed), 4 with normal only.
	uint32_t tbn_stride = 0;
	if (has_normal && has_tangent) {
		tbn_stride = 8; // 2 × uint32 (normal oct, tangent oct+sign)
	} else if (has_normal) {
		tbn_stride = 4; // 1 × uint32 (normal oct only)
	}
	// Byte offset of the TBN block in the source vertex buffer, after all its positions.
	uint32_t src_tbn_byte_offset = src_vertex_count * (compressed ? 8 : 12);

	// Described as an uncompressed surface: positions and TBN are, its attributes keep their compressed layout (flag).
	RTSurfaceData meta_sd;
	_fill_surface_geometry_data(p_mesh_surface, true, &meta_sd);
	uint32_t attrib_stride = meta_sd.geometry.attribute_stride;
	bool has_attr = attrib_stride > 0;
	const uint32_t MIN_ATTR_BYTES = 16;

	RD *rd = RD::get_singleton();
	BitField<RD::BufferCreationBits> gpu_buf_flags =
			RD::BUFFER_CREATION_AS_STORAGE_BIT |
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT |
			RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;

	// --- Capacity: units entering or leaving a MultiMesh, or a LOD switch, change N*V and N*I every few frames. Within
	// the capacity the buffers, the BLAS and its scratch stay; past it they are all made again (the BLAS's geometry
	// description holds the buffers and the counts), and so they are when the capacity the need would get now has been
	// under half of the current one for MM_SHRINK_DELAY_FRAMES: zooming back and forth switches LODs every few frames. ---
	static constexpr uint32_t MM_SHRINK_DELAY_FRAMES = 300;
	auto capacity_for = [](uint32_t p_count) { return p_count + p_count / 4 + 16; };
	const uint32_t used_vertices = p_mm_count * vertex_count;
	const uint32_t used_indices = indexed ? p_mm_count * index_count : 0;
	// An indexed BLAS keeps one unused vertex for its unused indices.
	const uint32_t needed_vertices = indexed ? used_vertices + 1 : used_vertices;
	bool shrink = false;
	if (capacity_for(needed_vertices) < entry.cap_vertices / 2) {
		if (entry.oversized_since_frame == 0) {
			entry.oversized_since_frame = current_frame;
		}
		shrink = current_frame - entry.oversized_since_frame >= MM_SHRINK_DELAY_FRAMES;
	} else {
		entry.oversized_since_frame = 0;
	}
	const bool reallocate = !entry.merged_vtx_buffer.is_valid() || entry.indexed != indexed ||
			entry.last_tbn_stride != tbn_stride || entry.last_attrib_stride != attrib_stride ||
			needed_vertices > entry.cap_vertices || used_indices > entry.cap_indices || shrink;
	if (reallocate) {
		uint32_t cap_vertices = capacity_for(needed_vertices);
		uint32_t cap_indices = 0;
		if (indexed) {
			cap_indices = capacity_for(used_indices);
			cap_indices -= cap_indices % 3;
		} else {
			cap_vertices -= cap_vertices % 3; // Non-indexed: the vertices are the triangles.
		}

		if (entry.blas.is_valid() && rd->acceleration_structure_is_valid(entry.blas)) {
			rd->free_rid(entry.blas); // Not when freed with its micromap.
		}
		entry.blas = RID();
		if (entry.merge_uniform_set.is_valid() && rd->uniform_set_is_valid(entry.merge_uniform_set)) {
			rd->free_rid(entry.merge_uniform_set);
		}
		entry.merge_uniform_set = RID();
		RID *buffers[] = { &entry.merged_vtx_buffer, &entry.merged_attr_buffer, &entry.replicated_idx_buffer, &entry.merged_prev_buffer };
		for (RID *buffer : buffers) {
			if (buffer->is_valid()) {
				rd->free_rid(*buffer);
				*buffer = RID();
			}
		}
		entry.cap_vertices = cap_vertices;
		entry.cap_indices = cap_indices;
		entry.buffer_bytes = uint64_t(cap_vertices) * (12 + (has_tbn ? tbn_stride : 0) + attrib_stride) + uint64_t(cap_indices) * 4;
		entry.oversized_since_frame = 0;
		entry.indexed = indexed;
		entry.last_tbn_stride = tbn_stride;
		entry.last_attrib_stride = attrib_stride;
		entry.blas_built_once = false;
		entry.displaced = false; // The previous positions are gone with merged_prev_buffer.

		// [float3 pos × cap] + [packed TBN × cap] (if TBN present).
		entry.merged_vtx_buffer = rd->vertex_buffer_create(cap_vertices * (12 + (has_tbn ? tbn_stride : 0)), {}, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_vtx_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_vtx_buffer, "RT MM merged vtx [" + itos(cache_key) + "]");

		// attribute_stride bytes per merged vertex.
		entry.merged_attr_buffer = rd->storage_buffer_create(has_attr ? cap_vertices * attrib_stride : MIN_ATTR_BYTES, {}, 0, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_attr_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_attr_buffer, "RT MM merged attr [" + itos(cache_key) + "]");

		if (indexed) {
			entry.replicated_idx_buffer = rd->index_buffer_create(cap_indices, RD::INDEX_BUFFER_FORMAT_UINT32, {}, false, gpu_buf_flags);
			ERR_FAIL_COND_V(!entry.replicated_idx_buffer.is_valid(), false);
			rd->set_resource_name(entry.replicated_idx_buffer, "RT MM replicated idx [" + itos(cache_key) + "]");
		}
	}

	// --- The instances' cut-out: the surface's micromap (its LOD's) shared by every instance through an index buffer
	// (merged triangle i references entry i mod the instance's triangle count), so no bake per merged BLAS. ---
	const uint32_t cap_triangles = indexed ? entry.cap_indices / 3 : entry.cap_vertices / 3;
	RID omm_micromap;
	uint32_t omm_level = 0;
	if (p_allow_omm) {
		const RTOmmEntry *omm = _omm_entry(p_mesh_surface, mesh_storage->multimesh_get_mesh(p_mm_rid), p_surface_index, p_surface_counter, p_material, p_material_counter, p_mat_data, true, lod);
		if (omm && omm->micromap.is_valid() && rd->acceleration_structure_is_valid(omm->micromap)) {
			omm_micromap = omm->micromap;
			omm_level = omm->level;
		}
	}
	if (entry.blas.is_valid() && !rd->acceleration_structure_is_valid(entry.blas)) {
		// Freed with the micromap it depended on (its entry evicted).
		entry.blas = RID();
		entry.blas_built_once = false;
	}
	const bool omm_index_stale = omm_micromap.is_valid() &&
			(!entry.omm_index_buffer.is_valid() || entry.omm_index_triangles != cap_triangles || entry.omm_index_period != prim_count);
	if (entry.blas.is_valid() && (entry.built_omm_micromap != omm_micromap || omm_index_stale)) {
		// The geometry description holds the micromap and its indices: a full build of a new BLAS (a refit keeps them).
		// Freed first: the BLAS depends on the index buffer replaced below.
		rd->free_rid(entry.blas);
		entry.blas = RID();
		entry.blas_built_once = false;
	}
	if (omm_index_stale) {
		const bool use_16bit = prim_count <= 65536;
		Vector<uint8_t> omm_indices;
		omm_indices.resize(STEPIFY(cap_triangles * (use_16bit ? 2u : 4u), 4u));
		uint32_t entry_index = 0;
		for (uint32_t t = 0; t < cap_triangles; t++) {
			if (use_16bit) {
				reinterpret_cast<uint16_t *>(omm_indices.ptrw())[t] = uint16_t(entry_index);
			} else {
				reinterpret_cast<uint32_t *>(omm_indices.ptrw())[t] = entry_index;
			}
			entry_index = entry_index + 1 == prim_count ? 0 : entry_index + 1;
		}
		if (entry.omm_index_buffer.is_valid()) {
			rd->free_rid(entry.omm_index_buffer);
		}
		entry.omm_index_buffer = rd->storage_buffer_create(omm_indices.size(), Span<uint8_t>(omm_indices.ptr(), omm_indices.size()), 0,
				RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		ERR_FAIL_COND_V(!entry.omm_index_buffer.is_valid(), false);
		rd->set_resource_name(entry.omm_index_buffer, "RT MM merged OMM indices [" + itos(cache_key) + "]");
		entry.omm_index_triangles = cap_triangles;
		entry.omm_index_period = prim_count;
		entry.omm_index_16bit = use_16bit;
	} else if (!omm_micromap.is_valid() && entry.omm_index_buffer.is_valid() && !entry.blas.is_valid()) {
		rd->free_rid(entry.omm_index_buffer);
		entry.omm_index_buffer = RID();
		entry.omm_index_triangles = 0;
		entry.omm_index_period = 0;
	}

	RID src_attr_buf;
	if (has_attr) {
		src_attr_buf = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
		ERR_FAIL_COND_V(!src_attr_buf.is_valid(), false);
	} else {
		src_attr_buf = entry.merged_attr_buffer;
	}

	MergeShader::Mode merge_mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;

	// Pre-validate source vertex buffer before opening the compute list.
	RID vtx_buf = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	ERR_FAIL_COND_V(!vtx_buf.is_valid(), false);
	uint64_t vtx_bda = rd->buffer_get_device_address(vtx_buf);

	// Positions displaced while near the camera go back to rest once it is no longer displaced.
	const bool needs_rebake = first_touch_this_frame &&
			(transforms_changed || !entry.blas_built_once || (entry.displaced && !p_displace));

	// --- Displacement by the material's vertex(), after the merge (_displace_merged_multimeshes()) ---
	if (p_displace && !entry.merged_prev_buffer.is_valid()) {
		entry.merged_prev_buffer = rd->storage_buffer_create(entry.cap_vertices * 12, {}, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
		ERR_FAIL_COND_V(!entry.merged_prev_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_prev_buffer, "RT MM displaced prev [" + itos(cache_key) + "]");
		entry.displaced = false; // Its previous positions are not written yet.
	}
	// Evaluated after every merge, every frame when vertex() reads the time or the camera, and when the material's
	// parameters or the instance uniforms change (godot3d's Rot Blossom grows its MultiMeshes with one).
	const uint32_t displace_inputs = p_displace ? hash_murmur3_one_32(p_material_counter, RendererRD::MaterialStorage::get_singleton()->global_shader_parameters_instance_hash(int32_t(p_instance_uniforms_ofs))) : 0;
	const bool displace_now = p_displace && first_touch_this_frame && (needs_rebake || p_reevaluate || !entry.displaced || entry.displaced_inputs != displace_inputs);

	// --- Single merged dispatch: bake vertices + TBN + attributes + (optional) indices ---
	if (needs_rebake) {
		Vector<RD::Uniform> uniforms;
		{
			auto push_buf = [&](uint32_t binding, RID buf) {
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
				u.binding = binding;
				u.append_id(buf);
				uniforms.push_back(u);
			};
			push_buf(0, entry.merged_vtx_buffer);
			push_buf(1, p_mm_gpu_buffer);
			push_buf(2, entry.merged_attr_buffer);
			push_buf(3, src_attr_buf);
			if (indexed) {
				push_buf(4, entry.replicated_idx_buffer);
				// Bound, not read through addresses: the graph orders the LOD view's compaction before this pass.
				push_buf(5, lod_view ? lod_view->vertex_remap_buffer : lod_view_dummy_buffer);
				push_buf(6, lod_view ? lod_view->index_buffer : lod_view_dummy_buffer);
			}
		}
		RID set_buffers[7] = {};
		for (const RD::Uniform &u : uniforms) {
			set_buffers[u.binding] = u.get_id(0);
		}
		bool reuse = entry.merge_uniform_set.is_valid() && rd->uniform_set_is_valid(entry.merge_uniform_set) && entry.merge_set_mode == uint32_t(merge_mode);
		for (uint32_t i = 0; i < 7 && reuse; i++) {
			reuse = entry.merge_set_buffers[i] == set_buffers[i];
		}
		if (!reuse) {
			if (entry.merge_uniform_set.is_valid() && rd->uniform_set_is_valid(entry.merge_uniform_set)) {
				rd->free_rid(entry.merge_uniform_set);
			}
			entry.merge_uniform_set = rd->uniform_set_create(uniforms, mm_merge_shader.version_shader[merge_mode], 0);
			ERR_FAIL_COND_V(!entry.merge_uniform_set.is_valid(), false);
			entry.merge_set_mode = uint32_t(merge_mode);
			for (uint32_t i = 0; i < 7; i++) {
				entry.merge_set_buffers[i] = set_buffers[i];
			}
		}
		const RID merge_uniform_set = entry.merge_uniform_set;
		uint32_t mm_stride = mesh_storage->multimesh_get_stride(p_mm_rid);
		uint32_t mm_cur_offset = mesh_storage->multimesh_get_current_instance_offset(p_mm_rid);
		uint32_t tbn_stride_words = tbn_stride / 4;
		// In the merged vertex buffer the TBN block starts after all cap_vertices float3 positions.
		uint32_t dst_tbn_base_words = entry.cap_vertices * 3;

		struct MergePC {
			uint32_t src_vtx_lo, src_vtx_hi;
			// MODE_INDEXED only -- present in struct (uploaded only when indexed):
			uint32_t src_idx_lo, src_idx_hi;
			uint32_t index_count, src_is_16bit;
			uint32_t use_lod_view, pad; // 1: vertices and indices come from the LOD view (bindings 5 and 6).
			// Common tail:
			uint32_t vertex_count, instance_count;
			uint32_t pos_stride_words;
			uint32_t src_tbn_base_words;
			uint32_t tbn_stride_words;
			uint32_t dst_tbn_base_words;
			uint32_t mm_stride, mm_offset;
			uint32_t has_tbn;
			uint32_t attr_stride_words;
			uint32_t compressed;
			float aabb_position[3];
			float aabb_size[3];
			uint32_t cap_vertices, cap_indices;
		} pc;

		pc.src_vtx_lo = uint32_t(vtx_bda);
		pc.src_vtx_hi = uint32_t(vtx_bda >> 32);

		if (indexed) {
			uint64_t src_idx_bda = lod_view ? 0 : rd->buffer_get_device_address(index_buffer);
			pc.src_idx_lo = uint32_t(src_idx_bda);
			pc.src_idx_hi = uint32_t(src_idx_bda >> 32);
			pc.index_count = index_count;
			pc.src_is_16bit = (!lod_view && src_vertex_count <= 65536) ? 1u : 0u;
			pc.use_lod_view = lod_view ? 1u : 0u;
			pc.pad = 0;
		}

		pc.vertex_count = vertex_count;
		pc.instance_count = p_mm_count;
		pc.pos_stride_words = compressed ? 2 : 3;
		pc.src_tbn_base_words = src_tbn_byte_offset / 4;
		pc.tbn_stride_words = tbn_stride_words;
		pc.dst_tbn_base_words = dst_tbn_base_words;
		pc.mm_stride = mm_stride;
		pc.mm_offset = mm_cur_offset;
		pc.has_tbn = has_tbn ? 1u : 0u;
		pc.attr_stride_words = has_attr ? (attrib_stride / 4) : 0u;
		pc.compressed = compressed ? 1u : 0u;
		// The AABB raster decompresses the positions with (RTSurfaceData::aabb_transform for an unmerged surface).
		const AABB surface_aabb = compressed ? mesh_storage->mesh_surface_get_aabb(p_mesh_surface) : AABB();
		for (int axis = 0; axis < 3; axis++) {
			pc.aabb_position[axis] = surface_aabb.position[axis];
			pc.aabb_size[axis] = surface_aabb.size[axis];
		}
		pc.cap_vertices = entry.cap_vertices;
		pc.cap_indices = entry.cap_indices;

		MergeShader::Mode mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;
		rd->compute_list_bind_compute_pipeline(p_compute_list, mm_merge_shader.pipeline[mode]);
		rd->compute_list_bind_uniform_set(p_compute_list, merge_uniform_set, 0);
		if (indexed) {
			rd->compute_list_set_push_constant(p_compute_list, &pc, sizeof(MergePC));
		} else {
			// Re-pack the non-indexed PC so that the common tail follows
			// src_vtx_lo/hi without the index gap.
			struct MergePCNonIndexed {
				uint32_t src_vtx_lo, src_vtx_hi;
				uint32_t vertex_count, instance_count;
				uint32_t pos_stride_words;
				uint32_t src_tbn_base_words;
				uint32_t tbn_stride_words;
				uint32_t dst_tbn_base_words;
				uint32_t mm_stride, mm_offset;
				uint32_t has_tbn;
				uint32_t attr_stride_words;
				uint32_t compressed;
				float aabb_position[3];
				float aabb_size[3];
				uint32_t cap_vertices, cap_indices;
			} pc_ni;
			pc_ni.src_vtx_lo = pc.src_vtx_lo;
			pc_ni.src_vtx_hi = pc.src_vtx_hi;
			pc_ni.vertex_count = pc.vertex_count;
			pc_ni.instance_count = pc.instance_count;
			pc_ni.pos_stride_words = pc.pos_stride_words;
			pc_ni.src_tbn_base_words = pc.src_tbn_base_words;
			pc_ni.tbn_stride_words = pc.tbn_stride_words;
			pc_ni.dst_tbn_base_words = pc.dst_tbn_base_words;
			pc_ni.mm_stride = pc.mm_stride;
			pc_ni.mm_offset = pc.mm_offset;
			pc_ni.has_tbn = pc.has_tbn;
			pc_ni.attr_stride_words = pc.attr_stride_words;
			pc_ni.compressed = pc.compressed;
			memcpy(pc_ni.aabb_position, pc.aabb_position, sizeof(pc.aabb_position));
			memcpy(pc_ni.aabb_size, pc.aabb_size, sizeof(pc.aabb_size));
			pc_ni.cap_vertices = pc.cap_vertices;
			pc_ni.cap_indices = pc.cap_indices;
			rd->compute_list_set_push_constant(p_compute_list, &pc_ni, sizeof(MergePCNonIndexed));
		}

		// Single thread count: every thread processes one vertex (idx < N*V)
		// and -- for MODE_INDEXED -- one output index (idx < N*I) using disjoint
		// destination buffers, so no in-shader barrier is required. A build also writes the unused vertices and indices
		// up to the capacity; a refit leaves them as they are.
		uint32_t thread_count = entry.blas_built_once ? MAX(used_vertices, used_indices) : MAX(entry.cap_vertices, entry.cap_indices);
		rd->compute_list_dispatch_threads(p_compute_list, thread_count, 1, 1);
	}

	// --- Build or refit the merged BLAS (uses merged_vtx_buffer for positions) ---
	if (!entry.blas.is_valid()) {
		RD::AccelerationStructureGeometry as_geom;
		as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
		as_geom.geometry.triangles.vertex_buffer = entry.merged_vtx_buffer;
		as_geom.geometry.triangles.vertex_stride = 12; // float3, positions section only
		as_geom.geometry.triangles.vertex_count = entry.cap_vertices;
		as_geom.geometry.triangles.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;

		if (indexed) {
			as_geom.geometry.triangles.index_buffer = entry.replicated_idx_buffer;
			as_geom.geometry.triangles.index_count = entry.cap_indices;
		}

		if (omm_micromap.is_valid()) {
			as_geom.geometry.triangles.opacity_micromap = omm_micromap;
			as_geom.geometry.triangles.opacity_micromap_index_buffer = entry.omm_index_buffer;
			as_geom.geometry.triangles.opacity_micromap_index_format = entry.omm_index_16bit ? RD::INDEX_BUFFER_FORMAT_UINT16 : RD::INDEX_BUFFER_FORMAT_UINT32;
			RD::OpacityMicromapUsage usage;
			usage.count = cap_triangles;
			usage.subdivision_level = omm_level;
			usage.format = RD::OPACITY_MICROMAP_FORMAT_4_STATE;
			as_geom.geometry.triangles.opacity_micromap_usages.push_back(usage);
		}
		entry.built_omm_micromap = omm_micromap;
		entry.built_omm_level = omm_level;

		BitField<RD::AccelerationStructureFlagBits> as_flags =
				RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT |
				RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT;
		entry.blas = rd->blas_create({ &as_geom, 1 }, as_flags);
		ERR_FAIL_COND_V(!entry.blas.is_valid(), false);
		rd->set_resource_name(entry.blas, "RT MM merged BLAS [" + itos(cache_key) + (omm_micromap.is_valid() ? "] OMM" : "]"));
	}

	if (needs_rebake || displace_now) {
		if (!entry.blas_built_once) {
			r_dirty_blas_list.push_back(entry.blas);
			entry.blas_built_once = true;
		} else {
			r_dirty_blas_update_list.push_back(entry.blas);
		}
	}
	if (needs_rebake) {
		entry.cached_mm_last_change = mm_last_change;
	}
	if (displace_now) {
		MMDisplaceRequest request;
		request.merged_vtx_buffer = entry.merged_vtx_buffer;
		request.merged_prev_buffer = entry.merged_prev_buffer;
		request.mm_gpu_buffer = p_mm_gpu_buffer;
		request.remap_buffer = lod_view ? lod_view->vertex_remap_buffer : RID();
		request.mesh_surface = p_mesh_surface;
		request.mm_count = p_mm_count;
		request.vertex_count = vertex_count;
		request.mm_stride = mesh_storage->multimesh_get_stride(p_mm_rid);
		request.mm_offset = mesh_storage->multimesh_get_current_instance_offset(p_mm_rid);
		// The 3D layout: 12 floats of transform, then 4 of color and 4 of custom data when used.
		const bool mm_colors = mesh_storage->multimesh_uses_colors(p_mm_rid);
		request.mm_color_offset = mm_colors ? 12 : UINT32_MAX;
		request.mm_custom_offset = mesh_storage->multimesh_uses_custom_data(p_mm_rid) ? (mm_colors ? 16 : 12) : UINT32_MAX;
		request.mat_data = p_mat_data;
		request.transform = p_transform;
		request.prev_transform = p_prev_transform;
		request.instance_uniforms_ofs = p_instance_uniforms_ofs;
		mm_displace_requests.push_back(request);
	}
	if (first_touch_this_frame) {
		entry.displaced = p_displace;
		entry.displaced_this_frame = displace_now;
		if (displace_now) {
			entry.displaced_inputs = displace_inputs;
		}
	}

	// --- Populate r_surf_data from the metadata already computed above, then override merged buffer addresses.
	*r_surf_data = meta_sd;
	r_surf_data->blas = entry.blas;
	r_surf_data->is_compressed = false;
	r_surf_data->aabb_transform = Transform3D();

	RT_GeometryData &geom = r_surf_data->geometry;

	// Point vertex address at merged buffer (positions + TBN all baked world-space).
	geom.vertex_buffer_address = rd->buffer_get_device_address(entry.merged_vtx_buffer);
	geom.vertex_count = p_mm_count * vertex_count;
	geom.position_stride = 12; // float3, uncompressed
	geom.flags &= ~RT_GEOM_FLAG_COMPRESSED;
	if (entry.displaced_this_frame) {
		// Moving with its vertex() this frame: the previous positions give the motion vectors.
		const uint64_t prev_address = rd->buffer_get_device_address(entry.merged_prev_buffer);
		geom.flags |= RT_GEOM_FLAG_DEFORMED;
		geom.prev_vertex_buffer_address_lo = static_cast<uint32_t>(prev_address & 0xFFFFFFFFULL);
		geom.prev_vertex_buffer_address_hi = static_cast<uint32_t>(prev_address >> 32);
	}

	// TBN section starts after all positions in the merged vertex buffer.
	if (has_tbn) {
		uint32_t tbn_base = entry.cap_vertices * 12;
		geom.normal_byte_offset = tbn_base;
		geom.normal_stride = tbn_stride;
		if (has_tangent) {
			geom.tangent_byte_offset = tbn_base; // tangent is at +4 from normal within the same stride pair
			geom.tangent_stride = tbn_stride;
		} else {
			geom.tangent_byte_offset = RT_OFFSET_NONE;
			geom.tangent_stride = 0;
		}
	}

	// Point attribute address at fully replicated attribute buffer.
	if (has_attr && entry.merged_attr_buffer.is_valid()) {
		geom.attribute_buffer_address = rd->buffer_get_device_address(entry.merged_attr_buffer);
	}

	// Its instances' COLOR, INSTANCE_CUSTOM and motion, read from the MultiMesh buffer: instance = merged vertex / vertex_count.
	if (_set_multimesh_instance_data(geom, p_mm_rid, p_mm_gpu_buffer, vertex_count)) {
		multimesh_buffers_read_this_frame.push_back(p_mm_gpu_buffer);
	}

	// Point index address at the replicated (uint32) index buffer.
	if (indexed && entry.replicated_idx_buffer.is_valid()) {
		geom.index_buffer_address = rd->buffer_get_device_address(entry.replicated_idx_buffer);
		geom.index_format = RT_INDEX_FORMAT_UINT32; // always uint32 in replicated buffer
		geom.primitive_count = p_mm_count * prim_count;
	} else {
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = p_mm_count * prim_count;
		_rt_set_unindexed_index_address(geom);
	}

	return true;
}

// ---------------------------------------------------------------------------
// TLAS creation (main entry point per frame)
// ---------------------------------------------------------------------------

_FORCE_INLINE_ static uint32_t _rt_indices_to_primitives(RSE::PrimitiveType p_primitive, uint32_t p_indices) {
	static const uint32_t divisor[RSE::PRIMITIVE_MAX] = { 1, 2, 1, 3, 1 };
	static const uint32_t subtractor[RSE::PRIMITIVE_MAX] = { 0, 0, 1, 0, 2 };
	return (p_indices - subtractor[p_primitive]) / divisor[p_primitive];
}

// The instance transform BaseMaterial3D's billboard modes give in vertex() (MODELVIEW_MATRIX without the view).
static Transform3D _billboard_transform(const Transform3D &p_model, const Transform3D &p_camera, RTBillboardMode p_mode, bool p_keep_scale) {
	Basis basis = p_camera.basis;
	if (p_mode == RT_BILLBOARD_FIXED_Y) {
		const Vector3 up(0, 1, 0);
		const Vector3 x = up.cross(p_camera.basis.get_column(2));
		const Vector3 z = p_camera.basis.get_column(0).cross(up);
		if (x.length_squared() < CMP_EPSILON2 || z.length_squared() < CMP_EPSILON2) {
			return p_model; // Camera looking straight up or down: raster divides by zero there.
		}
		basis.set_column(0, x.normalized());
		basis.set_column(1, up);
		basis.set_column(2, z.normalized());
	}
	if (p_keep_scale) {
		basis = basis * Basis::from_scale(p_model.basis.get_scale_abs());
	}
	return Transform3D(basis, p_model.origin);
}

RTViewportState *RenderRaytracing::build_tlas(const RenderDataRD *p_render_data, uint32_t p_rt_flags) {
	if (!p_render_data || !p_render_data->rt_instances) {
		return nullptr;
	}
	{
		const uint32_t frame = RSG::rasterizer->get_frame_number();
		if (frame - omm_sweep_frame >= 60) {
			omm_sweep_frame = frame;
			_omm_evict(false);
		}
	}

	RTViewportState *state = _get_or_create_viewport_state(p_render_data);
	if (!state) {
		return nullptr;
	}

	prepare_frame();
	last_trace_frame = MAX(RSG::rasterizer->get_frame_number(), 1u);

	// Builds bundle if needed; live_ready_mask drives TLAS inclusion below.
	SceneShaderRaytracing *rt_shader_singleton = SceneShaderRaytracing::get_singleton();
	rt_shader_singleton->ensure_pipeline_bundle(p_rt_flags);

	_displace_surfaces(p_render_data);
	const uint32_t current_frame = RSG::rasterizer->get_frame_number();

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	LocalVector<RID> dirty_blas_list;
	LocalVector<RID> dirty_blas_update_list;

	// Out of view, geometry that keeps deforming (skinning, blend shapes, the wind on merged MultiMeshes) is updated, and
	// its BLAS refit, every frame within out_of_view_update_distance of the camera and every N frames beyond, N growing
	// with the distance: Unreal's dynamic geometry update budget for ray tracing. What is in view is updated every frame,
	// so primary visibility never lags; only reflections and shadows of what is out of view and far do. The instances take
	// turns, and the motion between two updates is reported on the update frame.
	static const float out_of_view_update_distance = GLOBAL_GET("rendering/pathtracing/out_of_view_update_distance");
	const uint32_t OUT_OF_VIEW_MAX_UPDATE_PERIOD = 8;
	out_of_view_deferred = 0;
	Vector<Plane> view_planes;
	Vector3 view_points[8];
	if (out_of_view_update_distance > 0.0f) {
		view_planes = p_render_data->scene_data->cam_projection.get_projection_planes(p_render_data->scene_data->cam_transform);
		if (!p_render_data->scene_data->cam_projection.get_endpoints(p_render_data->scene_data->cam_transform, view_points)) {
			view_planes.clear();
		}
	}
	// An instance of the TLAS waits for its material's hit group: those nearest the camera are compiled first.
	const Vector3 request_origin = p_render_data->scene_data->cam_transform.origin;
	auto request_hit_group = [&](uint32_t p_slot_index, const AABB &p_aabb) {
		const float distance = Vector3().max(p_aabb.position - request_origin).max(request_origin - (p_aabb.position + p_aabb.size)).length();
		SceneShaderRaytracing::get_singleton()->request_hit_group(p_slot_index, distance);
	};
	// False when the instance's BLAS update waits for its next turn this frame.
	auto update_turn = [&](const AABB &p_aabb, uint64_t p_key) -> bool {
		if (view_planes.is_empty() || p_aabb.intersects_convex_shape(view_planes.ptr(), view_planes.size(), view_points, 8)) {
			return true;
		}
		const Vector3 camera_origin = p_render_data->scene_data->cam_transform.origin;
		const float distance = Vector3().max(p_aabb.position - camera_origin).max(camera_origin - (p_aabb.position + p_aabb.size)).length();
		const uint32_t period = CLAMP(uint32_t(distance / out_of_view_update_distance), 1u, OUT_OF_VIEW_MAX_UPDATE_PERIOD);
		return (current_frame + uint32_t(p_key)) % period == 0;
	};

#ifdef TOOLS_ENABLED
	uint32_t tlas_instance_count = 0;
	uint32_t tlas_primitive_count = 0;
	uint32_t rt_blas_builds = 0;
	uint32_t rt_blas_refits = 0;
	uint32_t rt_triangles_built = 0;
	uint32_t rt_triangles_refit = 0;
	const bool collect_render_info = (p_render_data->render_info != nullptr);
#endif

	// Which rays see an instance, from its cast_shadow setting.
	auto instance_mask = [](const RenderGeometryInstanceBase *p_instance) -> uint8_t {
		switch (p_instance->data->cast_shadows) {
			case RSE::SHADOW_CASTING_SETTING_OFF:
				return RT_INSTANCE_MASK_CAMERA;
			case RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY:
				return RT_INSTANCE_MASK_SHADOW;
			case RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED:
				return RT_INSTANCE_MASK_CAMERA | RT_INSTANCE_MASK_SHADOW | RT_INSTANCE_MASK_DOUBLE_SIDED_SHADOW;
			default:
				return RT_INSTANCE_MASK_CAMERA | RT_INSTANCE_MASK_SHADOW;
		}
	};

	// -----------------------------------------------------------------------
	// Phase 1: CPU / buffer-update work
	// -----------------------------------------------------------------------
	struct PendingMMSurface {
		RID mm_rid;
		RID mm_gpu_buffer;
		const RenderForwardClustered::GeometryInstanceSurfaceDataCache *mm_surf;
		void *mesh_surface;
		uint32_t mm_count;
		uint32_t surface_index;
		uint32_t surface_counter;
		uint32_t lod;
		const RTLodView *lod_view;
		bool displace; // Its material's vertex() moves it and it is near enough.
		bool reevaluate; // That vertex() reads the time or the camera.
		uint16_t material_counter; // Its parameters' version: a change evaluates vertex() again.
		Transform3D instance_transform;
		Transform3D prev_instance_transform;
		bool transform_moved;
		RTMaterialData *mat_data;
		RID material_rid;
		bool material_ready; // Not the stand-in of a hit group still compiling.
		uint32_t inst_flags;
		bool per_instance; // One TLAS instance per instance (_build_tlas_with_expanded_multimeshes()), not a merged BLAS.
	};
	LocalVector<PendingMMSurface> pending_mm_surfaces;

	const PagedArray<RenderGeometryInstance *> &rt_instances = *p_render_data->rt_instances;
	for (uint32_t i = 0; i < (uint32_t)rt_instances.size(); i++) {
		const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
				static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(rt_instances[i]);
		if (!inst || !inst->data) {
			continue;
		}
		const Transform3D &instance_transform = inst->transform;

		// Determine previous-frame transform for motion vectors.
		const Transform3D &prev_instance_transform =
				(inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::TELEPORTED)
				? inst->transform
				: inst->prev_transform;

		// Handle procedural RT instances (intersection shaders).
		if (inst->rt_procedural) {
			SceneShaderRaytracing *rt_shader = SceneShaderRaytracing::get_singleton();
			RTProceduralState *ps = inst->rt_procedural;

			// Intersection code comes from ShaderMaterial on material_override.
			if (!inst->data || !inst->data->material_override.is_valid()) {
				continue;
			}
			RID proc_material_rid = inst->data->material_override;
			uint32_t shader_id = material_storage->material_get_shader_id(proc_material_rid);
			if (shader_id == 0) {
				continue;
			}

			uint32_t hg_index = rt_shader->register_procedural_shader(shader_id, proc_material_rid);
			if (hg_index == 0) {
				continue;
			}
			if (!rt_shader->is_hg_ready_in_bundle(hg_index, p_rt_flags)) {
				request_hit_group(hg_index, inst->transformed_aabb);
				continue;
			}

			if (ps->dirty) {
#ifdef TOOLS_ENABLED
				uint32_t pre_proc_build_size = dirty_blas_list.size();
#endif
				update_procedural_blas(ps, dirty_blas_list);
				ps->dirty = false;
#ifdef TOOLS_ENABLED
				if (collect_render_info) {
					rt_blas_builds += dirty_blas_list.size() - pre_proc_build_size;
				}
#endif
			}

			if (ps->blas.is_valid()) {
				blass.push_back(ps->blas);
				blas_transforms.push_back(instance_transform);
				sbt_offsets.push_back(hg_index);

				RT_GeometryData geom = {};
				geom.flags = RT_GEOM_FLAG_PROCEDURAL;
				geom.vertex_buffer_address = ps->gpu_buffer_address;
				geom.aabb_size_x = (float)ps->culling_aabb.size.x;
				geom.aabb_size_y = (float)ps->culling_aabb.size.y;
				geom.aabb_size_z = (float)ps->culling_aabb.size.z;
				geom.layer_mask = inst->layer_mask;
				geometry_data.push_back(geom);

				if (inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED) {
					motion_indices.push_back((int32_t)motion_transforms.size());
					RT_InstanceMotionData motion = {};
					RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_instance_transform, motion.prev_object_to_world);
					motion_transforms.push_back(motion);
				} else {
					motion_indices.push_back(-1);
				}

				// Material for procedural geometry (already validated above).
				uint16_t proc_mat_counter = material_storage->material_get_rt_invalidation_counter(proc_material_rid);
				RTMaterialData *proc_mat_data = process_material(proc_material_rid, proc_mat_counter);
				material_data.push_back(proc_mat_data->data);
				material_data[material_data.size() - 1].instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);

				// Procedural instances disable triangle culling. They are opaque unless the material cuts out: an opaque
				// hit runs no any-hit shader (Vulkan spec "Ray Opacity Culling", DXR spec "Any hit shaders"), so a
				// forced opaque instance ignored ALPHA_SCISSOR_THRESHOLD and discard.
				uint32_t inst_flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
				const SceneShaderRaytracing::CustomShaderEntry *proc_entry = rt_shader->get_custom_shader_entry(hg_index);
				if (!proc_entry || !proc_entry->uses_alpha_clip) {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				}
				instance_flags.push_back(inst_flags);
				// A ray query only reports an AABB candidate: the shader must find the hit itself
				// (rayQueryGenerateIntersectionEXT, CommitProceduralPrimitiveHit), and the intersection is this material's
				// code, which only its hit group has. The ray query loops trace these bits with traceRay instead.
				uint8_t proc_mask = instance_mask(inst);
				if (proc_mask & RT_INSTANCE_MASK_SHADOW) {
					proc_mask |= RT_INSTANCE_MASK_PROCEDURAL_SHADOW;
				}
				if (proc_mask & RT_INSTANCE_MASK_CAMERA) {
					proc_mask |= RT_INSTANCE_MASK_PROCEDURAL_CAMERA;
				}
				procedural_instance_masks |= proc_mask & (RT_INSTANCE_MASK_PROCEDURAL_SHADOW | RT_INSTANCE_MASK_PROCEDURAL_CAMERA);
				instance_masks.push_back(proc_mask);
			}
			continue;
		}

		// MultiMesh: resolve materials and warm data cache now.
		// Compute dispatches and TLAS assembly are deferred to Phase 2.
		if (inst->data->base_type == RSE::INSTANCE_MULTIMESH) {
			RID mm_rid = inst->data->base;

			if (mesh_storage->multimesh_get_transform_format(mm_rid) != RSE::MULTIMESH_TRANSFORM_3D) {
				continue;
			}

			uint32_t mm_count = mesh_storage->multimesh_get_instances_to_draw(mm_rid);
			if (mm_count == 0) {
				continue;
			}

			RID mm_gpu_buffer = mesh_storage->multimesh_get_gpu_buffer(mm_rid);

			bool transform_moved = (inst->transform_status ==
					RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED);

			// The LOD raster draws the MultiMesh with (RenderForwardClustered::_fill_render_list): a merged BLAS is built
			// from it, so distant foliage costs the trace what it costs raster, not its full detail.
			float lod_distance = 1.0;
			if (!p_render_data->scene_data->cam_orthogonal) {
				const Vector3 aabb_min = inst->transformed_aabb.position;
				const Vector3 aabb_max = inst->transformed_aabb.position + inst->transformed_aabb.size;
				const Vector3 camera_position = p_render_data->scene_data->main_cam_transform.origin;
				lod_distance = Vector3().max(aabb_min - camera_position).max(camera_position - aabb_max).length();
			}

			const RenderForwardClustered::GeometryInstanceSurfaceDataCache *mm_surf = inst->surface_caches;
			while (mm_surf) {
				if (mm_surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) {
					mm_surf = mm_surf->next;
					continue;
				}

				void *mesh_surface = mm_surf->surface;
				uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mesh_surface);

				RID material_rid;
				if (mm_surf->material_rid.is_valid()) {
					material_rid = mm_surf->material_rid;
				} else if (mm_surf->owner->data->material_override.is_valid()) {
					material_rid = mm_surf->owner->data->material_override;
				} else if (mm_surf->surface_index < mm_surf->owner->data->surface_materials.size() &&
						mm_surf->owner->data->surface_materials[mm_surf->surface_index].is_valid()) {
					material_rid = mm_surf->owner->data->surface_materials[mm_surf->surface_index];
				} else {
					RID mesh_rid = mesh_storage->multimesh_get_mesh(mm_rid);
					if (mesh_rid.is_valid() && mesh_storage->owns_mesh(mesh_rid)) {
						material_rid = mesh_storage->mesh_surface_get_material(mesh_rid, mm_surf->surface_index);
					}
				}

				uint16_t material_counter = material_storage->material_get_rt_invalidation_counter(material_rid);
				RTMaterialData *mat_data = process_material(material_rid, material_counter);

				bool mm_material_ready = true;
				if (mat_data->rt_sbt_offset > 0 &&
						!rt_shader_singleton->is_hg_ready_in_bundle(mat_data->rt_sbt_offset, p_rt_flags)) {
					mm_material_ready = false;
					request_hit_group(mat_data->rt_sbt_offset, inst->transformed_aabb);
					const SceneShaderForwardClustered::ShaderData *cut_shader = mm_surf->shader;
					mat_data = _compiling_material_fallback(mat_data, cut_shader && (cut_shader->uses_alpha || cut_shader->uses_blend_alpha || cut_shader->uses_alpha_clip || cut_shader->uses_alpha_antialiasing || cut_shader->uses_discard));
					if (!mat_data) {
						mm_surf = mm_surf->next;
						continue;
					}
				}

				uint32_t inst_flags = 0;
				if (mm_surf->shader) {
					switch (mm_surf->shader->rt_cull_mode()) {
						case RSE::CULL_MODE_DISABLED:
							inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
							inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
							break;
						case RSE::CULL_MODE_FRONT:
							break;
						case RSE::CULL_MODE_BACK:
						default:
							inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
							break;
					}
				} else {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
				}
				if (mat_data->rt_sbt_offset > 0) {
					const SceneShaderRaytracing::CustomShaderEntry *cse =
							rt_shader_singleton->get_custom_shader_entry(mat_data->rt_sbt_offset);
					if (!cse || (!cse->uses_alpha_clip && cse->alpha_texture_buffer_offset == UINT32_MAX)) {
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
					}
				} else {
					bool is_alpha = mm_surf->shader &&
							(mm_surf->shader->uses_alpha_clip || mm_surf->shader->uses_blend_alpha || mm_surf->shader->uses_alpha);
					if (!is_alpha) {
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
					}
				}
				// Near the camera, a material whose vertex() moves the MultiMesh (wind) is evaluated on it, as raster draws it.
				bool displace = false;
				bool reevaluate = false;
				static const float mm_displacement_distance = GLOBAL_GET("rendering/pathtracing/multimesh_displacement_distance");
				if (mm_displacement_distance > 0.0f && lod_distance <= mm_displacement_distance && mat_data->rt_sbt_offset > 0) {
					const SceneShaderRaytracing::CustomShaderEntry *displace_cse = rt_shader_singleton->get_custom_shader_entry(mat_data->rt_sbt_offset);
					RID displace_shader;
					if (displace_cse && displace_cse->writes_vertex && rt_shader_singleton->get_displace_pipeline(mat_data->rt_sbt_offset, displace_shader).is_valid()) {
						displace = true;
						reevaluate = displace_cse->uses_vertex_time || (displace_cse->vertex_reads_camera && displace_camera_moved);
						if (reevaluate && !update_turn(inst->transformed_aabb, mm_rid.get_id())) {
							// The wind waits for its turn out of view; a change of the instances or the material still evaluates it.
							reevaluate = false;
							out_of_view_deferred++;
						}
					}
				}

				// One TLAS instance per instance, sharing the mesh's BLAS, written on the GPU from the MultiMesh buffer. A merged
				// BLAS is built again or refit whenever its instances change (moving units: hundreds of refits a frame, a
				// hierarchy that degrades, a copy of the mesh per instance in video memory); it is kept for the displaced
				// MultiMeshes, whose vertices only it holds. Measured on Solfarer's army and godot3d's village: faster on the
				// render thread and the GPU, less memory, also for static foliage.
				const bool per_instance = tlas_expand_supported && mm_gpu_buffer.is_valid() && !displace;

				uint32_t lod = 0;
				const RTLodView *lod_view = nullptr;
				// GODOT_RT_MULTIMESH_LOD=0 merges every MultiMesh at its full detail. One TLAS instance per instance traces
				// the mesh itself: its BLAS is shared, whatever the instance count.
				static const bool multimesh_lod = OS::get_singleton()->get_environment("GODOT_RT_MULTIMESH_LOD") != "0";
				if (!per_instance && multimesh_lod && p_render_data->scene_data->screen_mesh_lod_threshold > 0.0 && mesh_storage->mesh_surface_has_lod(mesh_surface)) {
					uint32_t lod_indices = 0;
					const float lod_model_scale = inst->lod_model_scale * inst->lod_bias;
					const float lod_distance_scaled = lod_distance * p_render_data->scene_data->lod_distance_multiplier;
					const float lod_threshold = p_render_data->scene_data->screen_mesh_lod_threshold;
					lod = mesh_storage->mesh_surface_get_lod(mesh_surface, lod_model_scale, lod_distance_scaled, lod_threshold, lod_indices);
					// A LOD switch builds the merged BLAS again, which raster pays nothing for. The LOD built stays while the
					// distance is within RT_MULTIMESH_LOD_HYSTERESIS of one that picks it, so that a distance about a threshold
					// (an AABB following moving units) does not switch it back and forth.
					const RTMergedMMEntry *built = _get_merged_mm_entry(mm_rid, mm_surf->surface_index);
					const uint32_t built_lod = built ? built->last_lod : UINT32_MAX;
					if (built_lod != UINT32_MAX && built_lod != lod) {
						static constexpr float RT_MULTIMESH_LOD_HYSTERESIS = 0.1f;
						const uint32_t nearer_lod = mesh_storage->mesh_surface_get_lod(mesh_surface, lod_model_scale, lod_distance_scaled * (1.0f - RT_MULTIMESH_LOD_HYSTERESIS), lod_threshold, lod_indices);
						const uint32_t farther_lod = mesh_storage->mesh_surface_get_lod(mesh_surface, lod_model_scale, lod_distance_scaled * (1.0f + RT_MULTIMESH_LOD_HYSTERESIS), lod_threshold, lod_indices);
						if (built_lod >= nearer_lod && built_lod <= farther_lod) {
							lod = built_lod;
						}
					}
					if (lod > 0) {
						// Read here: fetching a new LOD's indices waits for the GPU, which cannot happen inside the compute list of phase 2.
						lod_view = _get_lod_view(mesh_surface, lod);
						if (!lod_view) {
							lod = 0;
						}
					}
					// A zoom switches thousands of MultiMeshes at once, each a full build of its BLAS on the GPU. A switch the
					// instance count does not force anyway is spread over the frames: past the frame's budget of triangles, the
					// MultiMesh keeps the LOD it is built with, a frame longer.
					if (built_lod != UINT32_MAX && lod != built_lod && built->last_mm_count == mm_count) {
						static const uint64_t LOD_SWITCH_TRIANGLES_PER_FRAME = uint64_t(int64_t(GLOBAL_GET("rendering/pathtracing/multimesh_lod_switch_triangles_per_frame")));
						const uint32_t frame_number = RSG::rasterizer->get_frame_number();
						if (lod_switch_frame != frame_number) {
							lod_switch_frame = frame_number;
							lod_switch_triangles = 0;
						}
						const uint32_t switch_indices = lod_view ? lod_view->index_count : mesh_storage->mesh_surface_get_index_count(mesh_surface, 0);
						const uint64_t switch_triangles = uint64_t(mm_count) * (switch_indices > 0 ? switch_indices : mesh_storage->mesh_surface_get_vertex_count(mesh_surface)) / 3;
						if (lod_switch_triangles > 0 && lod_switch_triangles + switch_triangles > LOD_SWITCH_TRIANGLES_PER_FRAME) {
							lod = built_lod;
							lod_view = lod > 0 ? _get_lod_view(mesh_surface, lod) : nullptr;
							if (lod > 0 && !lod_view) {
								lod = 0;
							}
						} else {
							lod_switch_triangles += switch_triangles;
						}
					}
				}

				if (!(tlas_expand_supported && mm_gpu_buffer.is_valid()) && (!mm_gpu_buffer.is_valid() || !_mm_fits_merged_blas(mesh_surface, mm_count, lod_view))) {
					// One TLAS instance per instance, without writing them on the GPU, needs the instances on the CPU. A copy
					// of the buffer that waited for the GPU stalled the frame for every new MultiMesh (foliage streamed in as
					// the camera moves): the copy is asynchronous, and the MultiMesh traced once it is there.
					if (!mesh_storage->multimesh_get_local_data_ptr_async(mm_rid)) {
						mm_surf = mm_surf->next;
						continue;
					}
				}

				PendingMMSurface pending;
				pending.mm_rid = mm_rid;
				pending.mm_gpu_buffer = mm_gpu_buffer;
				pending.mm_surf = mm_surf;
				pending.mesh_surface = mesh_surface;
				pending.mm_count = mm_count;
				pending.surface_index = mm_surf->surface_index;
				pending.surface_counter = surface_counter;
				pending.lod = lod;
				pending.lod_view = lod_view;
				pending.displace = displace;
				pending.reevaluate = reevaluate;
				pending.material_counter = material_counter;
				pending.instance_transform = instance_transform;
				pending.prev_instance_transform = prev_instance_transform;
				pending.transform_moved = transform_moved;
				pending.mat_data = mat_data;
				pending.material_rid = material_rid;
				pending.material_ready = mm_material_ready;
				pending.inst_flags = inst_flags;
				pending.per_instance = per_instance;
				pending_mm_surfaces.push_back(pending);

				mm_surf = mm_surf->next;
			}
			continue;
		}

		// Walk the surface cache linked list.
		const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf = inst->surface_caches;
		bool instance_static = inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::NONE;
		while (surf) {
			// Surfaces of the raster transparent pass are only seen by the front layer rays, for their reflections.
			const bool front_layer = surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA;
			if (front_layer && _front_layer_skipped(surf)) {
				surf = surf->next;
				continue;
			}

			void *mesh_surface = surf->surface;
			uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mesh_surface);

#ifdef TOOLS_ENABLED
			uint32_t pre_build_size = dirty_blas_list.size();
			uint32_t pre_refit_size = dirty_blas_update_list.size();
#endif

			// MeshInstance skinning/blend shapes provide a deformed vertex buffer.
			RTSurfaceData *surf_data = nullptr;
			if (inst->mesh_instance.is_valid()) {
				RID curr_vb = mesh_storage->mesh_instance_get_vertex_buffer(inst->mesh_instance, surf->surface_index);
				if (curr_vb.is_valid()) {
					RTDeformedGeometrySource src;
					src.current_vb = curr_vb;
					src.prev_vb = mesh_storage->mesh_instance_get_prev_vertex_buffer(inst->mesh_instance, surf->surface_index);
					src.change_stamp = mesh_storage->mesh_instance_get_last_change(inst->mesh_instance, surf->surface_index);
					uint64_t mi_id = inst->mesh_instance.get_id();
					uint32_t mi_index = static_cast<uint32_t>(mi_id & 0xFFFFFFFFULL);
					src.cache_version = static_cast<uint32_t>(mi_id >> 32);
					src.cache_key = (static_cast<uint64_t>(mi_index) << 16) | (surf->surface_index & 0xFFFFu);
					src.surface_counter = surface_counter;
					src.defer_update = !update_turn(inst->transformed_aabb, mi_index);
					surf_data = process_deformed_surface(surf, mesh_surface, src, dirty_blas_list, dirty_blas_update_list);
				}
			}
			if (!surf_data && !inst->mesh_instance.is_valid()) {
				// Displaced by its material's vertex() this frame.
				RTDeformedCacheEntry *displaced = deformed_pool.get_or_null(surf->rt_deformed_handle);
				if (displaced && displaced->displaced_frame == current_frame) {
					RTDeformedGeometrySource src;
					src.current_vb = displaced->displaced_vb;
					src.change_stamp = displaced->displaced_stamp;
					src.cache_key = surf->rt_deformed_handle.get_id();
					src.surface_counter = surface_counter;
					src.force_rebuild = displaced->displaced_rebuild;
					src.prev_vb = displaced->displaced_vb;
					src.prev_offset = displaced->displaced_prev_offset;
					surf_data = process_deformed_surface(surf, mesh_surface, src, dirty_blas_list, dirty_blas_update_list);
					if (surf_data && surf_data->blas.is_valid()) {
						// The rest positions follow the deformed vertex buffer's sections.
						const RT_GeometryData &g = surf_data->geometry;
						const uint32_t tbn_bytes = g.normal_byte_offset == RT_OFFSET_NONE ? 0 : g.normal_stride;
						const uint64_t rest_address = RD::get_singleton()->buffer_get_device_address(displaced->displaced_vb) + uint64_t(g.vertex_count) * (g.position_stride + tbn_bytes);
						surf_data->geometry.flags |= RT_GEOM_FLAG_DISPLACED;
						surf_data->geometry.rest_vertex_buffer_address_lo = static_cast<uint32_t>(rest_address & 0xFFFFFFFFULL);
						surf_data->geometry.rest_vertex_buffer_address_hi = static_cast<uint32_t>(rest_address >> 32);
					}
				}
			}
			if (!surf_data) {
				surf_data = process_surface(surf, mesh_surface, surface_counter, instance_transform, dirty_blas_list);
			}
			if (!surf_data || !surf_data->blas.is_valid()) {
				surf = surf->next;
				continue;
			}

			// Resolve material before TLAS so we can skip surfaces whose HG is not live yet (the surface's own, which is how
			// a next pass or the overlay is traced with its material, then override > surface > mesh).
			RID material_rid;
			if (surf->material_rid.is_valid()) {
				material_rid = surf->material_rid;
			} else if (surf->owner->data->material_override.is_valid()) {
				material_rid = surf->owner->data->material_override;
			} else if (surf->surface_index < surf->owner->data->surface_materials.size() &&
					surf->owner->data->surface_materials[surf->surface_index].is_valid()) {
				material_rid = surf->owner->data->surface_materials[surf->surface_index];
			} else {
				RID mesh_rid = surf->owner->data->base;
				if (mesh_rid.is_valid() && mesh_storage->owns_mesh(mesh_rid)) {
					material_rid = mesh_storage->mesh_surface_get_material(mesh_rid, surf->surface_index);
				}
			}

			uint16_t material_counter = material_storage->material_get_rt_invalidation_counter(material_rid);
			RTMaterialData *mat_data = process_material(material_rid, material_counter);
			// The front layer only gives a transparent surface the reflection its own material shapes; the raster
			// transparent pass draws it either way. A stand-in (for a shader no hit group can run, or one still
			// compiling) would hide the surfaces behind it with a reflection of the wrong material: they keep theirs.
			if (front_layer && mat_data->shader_failed) {
				surf = surf->next;
				continue;
			}
			// Not the stand-in of a hit group still compiling: its cut-out may be baked.
			bool material_ready = true;

			if (mat_data->rt_sbt_offset > 0 &&
					!rt_shader_singleton->is_hg_ready_in_bundle(mat_data->rt_sbt_offset, p_rt_flags)) {
				material_ready = false;
				request_hit_group(mat_data->rt_sbt_offset, inst->transformed_aabb);
				if (front_layer) {
					surf = surf->next;
					continue;
				}
				const SceneShaderForwardClustered::ShaderData *cut_shader = surf->shader;
				mat_data = _compiling_material_fallback(mat_data, cut_shader && (cut_shader->uses_alpha || cut_shader->uses_blend_alpha || cut_shader->uses_alpha_clip || cut_shader->uses_alpha_antialiasing || cut_shader->uses_discard));
				if (!mat_data) {
					surf = surf->next;
					continue;
				}
			}

			// Compute or reuse cached final transform (instance * aabb_transform for compressed meshes).
			Transform3D final_transform;
			const bool billboard = mat_data->billboard_mode != RT_BILLBOARD_DISABLED;
			if (billboard) {
				final_transform = _billboard_transform(instance_transform, p_render_data->scene_data->cam_transform, mat_data->billboard_mode, mat_data->billboard_keep_scale);
				if (surf_data->is_compressed) {
					final_transform = final_transform * surf_data->aabb_transform;
				}
			} else if (instance_static && surf->cached_final_transform_valid) {
				final_transform = surf->cached_final_transform;
			} else {
				final_transform = instance_transform;
				if (surf_data->is_compressed) {
					final_transform = instance_transform * surf_data->aabb_transform;
				}
				surf->cached_final_transform = final_transform;
				surf->cached_final_transform_valid = true;
			}
			blas_transforms.push_back(final_transform);

			blass.push_back(surf_data->blas);
			geometry_data.push_back(surf_data->geometry);
			geometry_data[geometry_data.size() - 1].layer_mask = inst->layer_mask;

			if (billboard) {
				// Turning with the camera is motion too.
				motion_indices.push_back((int32_t)motion_transforms.size());
				RT_InstanceMotionData motion = {};
				Transform3D prev_final = _billboard_transform(prev_instance_transform, p_render_data->scene_data->prev_cam_transform, mat_data->billboard_mode, mat_data->billboard_keep_scale);
				if (surf_data->is_compressed) {
					prev_final = prev_final * surf_data->aabb_transform;
				}
				RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_final, motion.prev_object_to_world);
				motion_transforms.push_back(motion);
			} else if (inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED) {
				motion_indices.push_back((int32_t)motion_transforms.size());
				RT_InstanceMotionData motion = {};
				Transform3D prev_final = prev_instance_transform;
				if (surf_data->is_compressed) {
					prev_final = prev_instance_transform * surf_data->aabb_transform;
				}
				RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_final, motion.prev_object_to_world);
				motion_transforms.push_back(motion);
			} else {
				motion_indices.push_back(-1);
			}

#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count++;
				uint32_t vertices = mesh_storage->mesh_surface_get_vertices_drawn_count(mesh_surface);
				uint32_t prim_count = _rt_indices_to_primitives(surf->primitive, vertices);
				tlas_primitive_count += prim_count;
				uint32_t build_delta = dirty_blas_list.size() - pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif

			sbt_offsets.push_back(mat_data->rt_sbt_offset);
			material_data.push_back(mat_data->data);
			material_data[material_data.size() - 1].instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);

			// Determine per-instance TLAS flags from material properties.
			uint32_t inst_flags = 0;
			if (surf->shader) {
				switch (surf->shader->rt_cull_mode()) {
					case RSE::CULL_MODE_DISABLED:
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
						break;
					case RSE::CULL_MODE_FRONT:
						break;
					case RSE::CULL_MODE_BACK:
					default:
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
						break;
				}
			} else {
				inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
			}

			if (front_layer) {
				// A blended surface is the front layer wherever raster draws it: an alpha scissor cuts it, and so does a
				// custom shader's `discard` (its any-hit runs the fragment code, SceneShaderRaytracing::_preprocess_shader()).
				// Forced opaque, a quad that discards its empty texels (godot3d's health bars) took the whole rectangle from
				// the water behind it, which lost its traced reflection there.
				bool cut = surf->shader->uses_alpha_clip;
				if (mat_data->rt_sbt_offset > 0) {
					const SceneShaderRaytracing::CustomShaderEntry *cse = rt_shader_singleton->get_custom_shader_entry(mat_data->rt_sbt_offset);
					cut = cut || (cse && cse->uses_alpha_clip);
				}
				if (!cut) {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				}
			} else if (mat_data->rt_sbt_offset > 0) {
				// Custom shader: enable any-hit if it uses alpha clip or has a hint_albedo texture.
				const SceneShaderRaytracing::CustomShaderEntry *cse =
						rt_shader_singleton->get_custom_shader_entry(mat_data->rt_sbt_offset);
				if (!cse || (!cse->uses_alpha_clip && cse->alpha_texture_buffer_offset == UINT32_MAX)) {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				}
			} else {
				// Standard material: FORCE_OPAQUE if no alpha usage.
				bool is_alpha = surf->shader && (surf->shader->uses_alpha_clip || surf->shader->uses_blend_alpha || surf->shader->uses_alpha);
				if (!is_alpha) {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				}
			}
			instance_flags.push_back(inst_flags);
			// A surface whose rays run an any-hit: its cut-out baked into an opacity micromap, when the material allows. A
			// static surface's BLAS is built again with it; a deformed copy's is, the next frame (process_deformed_surface()).
			{
				RTOmmEntry *omm = nullptr;
				if (material_ready && !front_layer && !(inst_flags & RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT)) {
					omm = _omm_entry(mesh_surface, surf->owner->data->base, surf->surface_index, surface_counter, material_rid, material_counter, mat_data, false);
				}
				if (surf_data->geometry.flags & RT_GEOM_FLAG_DEFORMED) {
					RTDeformedCacheEntry *deformed = deformed_pool.get_or_null(surf->rt_deformed_handle);
					if (deformed) {
						deformed->omm_micromap = omm ? omm->micromap : RID();
						deformed->omm_level = omm ? omm->level : 0;
					}
				} else if (omm) {
					const RID omm_blas = _omm_static_blas(omm, mesh_surface, dirty_blas_list);
					if (omm_blas.is_valid()) {
						blass[blass.size() - 1] = omm_blas;
					}
				}
			}
			if (front_layer) {
				instance_masks.push_back(RT_INSTANCE_MASK_FRONT_LAYER);
				front_layer_instance_count++;
			} else {
				instance_masks.push_back(instance_mask(inst));
			}

			surf = surf->next;
		}
	}

	// -----------------------------------------------------------------------
	// Phase 2: GPU compute — merged MultiMesh BLAS dispatches.
	// -----------------------------------------------------------------------
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	for (const PendingMMSurface &pending : pending_mm_surfaces) {
#ifdef TOOLS_ENABLED
		uint32_t mm_pre_build_size = dirty_blas_list.size();
		uint32_t mm_pre_refit_size = dirty_blas_update_list.size();
#endif
		RTSurfaceData merged_sd;
		bool use_merged = !pending.per_instance && pending.mm_gpu_buffer.is_valid() &&
				_build_merged_mm_blas(pending.mm_rid, pending.mm_gpu_buffer, pending.mesh_surface,
						pending.mm_count, pending.surface_index, pending.surface_counter, pending.lod,
						pending.displace, pending.reevaluate, pending.mat_data, pending.material_rid, pending.material_counter,
						pending.material_ready && !(pending.inst_flags & RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT),
						pending.instance_transform, pending.prev_instance_transform,
						uint32_t(pending.mm_surf->owner->shader_uniforms_offset),
						compute_list, dirty_blas_list, dirty_blas_update_list, &merged_sd);

		if (use_merged) {
			blass.push_back(merged_sd.blas);
			blas_transforms.push_back(pending.instance_transform);
			geometry_data.push_back(merged_sd.geometry);
			geometry_data[geometry_data.size() - 1].layer_mask = pending.mm_surf->owner->layer_mask;
			sbt_offsets.push_back(pending.mat_data->rt_sbt_offset);
			material_data.push_back(pending.mat_data->data);
			material_data[material_data.size() - 1].instance_uniforms_ofs = uint32_t(pending.mm_surf->owner->shader_uniforms_offset);
			// The node's motion; its instances' own is the hit shader's (multimesh_previous_position()).
			if (pending.transform_moved) {
				motion_indices.push_back((int32_t)motion_transforms.size());
				RT_InstanceMotionData motion = {};
				RendererRD::MaterialStorage::store_transform_transposed_3x4(pending.prev_instance_transform, motion.prev_object_to_world);
				motion_transforms.push_back(motion);
			} else {
				motion_indices.push_back(-1);
			}
			instance_flags.push_back(pending.inst_flags);
			instance_masks.push_back(instance_mask(pending.mm_surf->owner));
#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count++;
				uint32_t prim_count = merged_sd.geometry.primitive_count;
				tlas_primitive_count += prim_count;
				uint32_t build_delta = dirty_blas_list.size() - mm_pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - mm_pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif
		} else if (tlas_expand_supported && pending.mm_gpu_buffer.is_valid()) {
			// One TLAS instance per instance, sharing the mesh's BLAS, written on the GPU from the MultiMesh buffer
			// (_build_tlas_with_expanded_multimeshes()): no copy of the instances to the CPU, no work per instance here.
			RTSurfaceData *surf_data = process_surface(pending.mm_surf, pending.mesh_surface,
					pending.surface_counter, pending.instance_transform, dirty_blas_list);
			if (!surf_data || !surf_data->blas.is_valid()) {
				continue;
			}

			RT_GeometryData mm_geometry = surf_data->geometry;
			if (_set_multimesh_instance_data(mm_geometry, pending.mm_rid, pending.mm_gpu_buffer, UINT32_MAX)) {
				multimesh_buffers_read_this_frame.push_back(pending.mm_gpu_buffer);
			}
			mm_geometry.layer_mask = pending.mm_surf->owner->layer_mask;
			RT_MaterialData mm_material = pending.mat_data->data;
			mm_material.instance_uniforms_ofs = uint32_t(pending.mm_surf->owner->shader_uniforms_offset);

			TlasExpandJob job;
			RendererRD::MaterialStorage::store_transform_transposed_3x4(pending.instance_transform, job.data.node);
			RendererRD::MaterialStorage::store_transform_transposed_3x4(pending.transform_moved ? pending.prev_instance_transform : pending.instance_transform, job.data.prev_node);
			RendererRD::MaterialStorage::store_transform_transposed_3x4(surf_data->is_compressed ? surf_data->aabb_transform : Transform3D(), job.data.aabb);
			const uint64_t mm_address = RD::get_singleton()->buffer_get_device_address(pending.mm_gpu_buffer);
			job.data.mm_address_lo = uint32_t(mm_address);
			job.data.mm_address_hi = uint32_t(mm_address >> 32);
			job.data.mm_stride = mesh_storage->multimesh_get_stride(pending.mm_rid);
			job.data.mm_offset = mesh_storage->multimesh_get_current_instance_offset(pending.mm_rid);
			// The instances' previous transforms, as raster's motion vectors read them (while a viewport uses them).
			uint32_t mm_motion_current_offset = 0;
			uint32_t mm_prev_offset = 0;
			const bool mm_moved = mesh_storage->_multimesh_get_motion_vectors_offsets(pending.mm_rid, mm_motion_current_offset, mm_prev_offset) && mm_prev_offset != job.data.mm_offset;
			job.data.mm_prev_offset = mm_moved ? mm_prev_offset : job.data.mm_offset;
			job.data.template_index = tlas_expand_geometry_templates.size();
			tlas_expand_geometry_templates.push_back(mm_geometry);
			tlas_expand_material_templates.push_back(mm_material);
			job.blas = surf_data->blas;
			if (pending.material_ready && !(pending.inst_flags & RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT)) {
				RTOmmEntry *omm = _omm_entry(pending.mesh_surface, mesh_storage->multimesh_get_mesh(pending.mm_rid), pending.surface_index, pending.surface_counter, pending.material_rid, pending.material_counter, pending.mat_data, true);
				const RID omm_blas = omm ? _omm_static_blas(omm, pending.mesh_surface, dirty_blas_list) : RID();
				if (omm_blas.is_valid()) {
					job.blas = omm_blas;
				}
			}
			job.mm_gpu_buffer = pending.mm_gpu_buffer;
			job.count = pending.mm_count;
			job.sbt_offset = pending.mat_data->rt_sbt_offset;
			job.flags = pending.inst_flags;
			job.mask = instance_mask(pending.mm_surf->owner);
			tlas_expand_jobs.push_back(job);

#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count += pending.mm_count;
				uint32_t vertices = mesh_storage->mesh_surface_get_vertices_drawn_count(pending.mesh_surface);
				uint32_t prim_count = _rt_indices_to_primitives(pending.mm_surf->primitive, vertices);
				tlas_primitive_count += prim_count * pending.mm_count;
				uint32_t build_delta = dirty_blas_list.size() - mm_pre_build_size;
				rt_blas_builds += build_delta;
				rt_triangles_built += prim_count * build_delta;
			}
#endif
		} else {
			// Without the standard instance layout: the instances read back to the CPU, one TLAS instance each.
			const float *mm_data = mesh_storage->multimesh_get_local_data_ptr_async(pending.mm_rid);
			if (!mm_data) {
				continue;
			}

			const uint32_t mm_stride = mesh_storage->multimesh_get_stride(pending.mm_rid);
			const uint32_t mm_cur_offset = mesh_storage->multimesh_get_current_instance_offset(pending.mm_rid);

			RTSurfaceData *surf_data = process_surface(pending.mm_surf, pending.mesh_surface,
					pending.surface_counter, pending.instance_transform, dirty_blas_list);
			if (!surf_data || !surf_data->blas.is_valid()) {
				continue;
			}

			RT_GeometryData mm_geometry = surf_data->geometry;
			const bool reads_multimesh = _set_multimesh_instance_data(mm_geometry, pending.mm_rid, pending.mm_gpu_buffer, UINT32_MAX);
			if (reads_multimesh) {
				multimesh_buffers_read_this_frame.push_back(pending.mm_gpu_buffer);
			}
			// The instances' previous transforms, as raster's motion vectors read them (while a viewport uses them).
			uint32_t mm_motion_current_offset = 0;
			uint32_t mm_prev_offset = 0;
			const bool mm_moved = mesh_storage->_multimesh_get_motion_vectors_offsets(pending.mm_rid, mm_motion_current_offset, mm_prev_offset) && mm_prev_offset != mm_cur_offset;
			auto read_mm_xform = [](const float *d) {
				Transform3D xform;
				xform.basis.rows[0][0] = d[0];
				xform.basis.rows[0][1] = d[1];
				xform.basis.rows[0][2] = d[2];
				xform.origin.x = d[3];
				xform.basis.rows[1][0] = d[4];
				xform.basis.rows[1][1] = d[5];
				xform.basis.rows[1][2] = d[6];
				xform.origin.y = d[7];
				xform.basis.rows[2][0] = d[8];
				xform.basis.rows[2][1] = d[9];
				xform.basis.rows[2][2] = d[10];
				xform.origin.z = d[11];
				return xform;
			};

			for (uint32_t mi = 0; mi < pending.mm_count; mi++) {
				const Transform3D mm_xform = read_mm_xform(mm_data + (mm_cur_offset + mi) * mm_stride);
				const Transform3D prev_mm_xform = mm_moved ? read_mm_xform(mm_data + (mm_prev_offset + mi) * mm_stride) : mm_xform;

				Transform3D final_transform = pending.instance_transform * mm_xform;
				if (surf_data->is_compressed) {
					final_transform = final_transform * surf_data->aabb_transform;
				}

				blass.push_back(surf_data->blas);
				blas_transforms.push_back(final_transform);
				geometry_data.push_back(mm_geometry);
				geometry_data[geometry_data.size() - 1].layer_mask = pending.mm_surf->owner->layer_mask;
				geometry_data[geometry_data.size() - 1].multimesh_offset = mm_cur_offset + mi;
				geometry_data[geometry_data.size() - 1].multimesh_previous_offset = mm_cur_offset + mi;
				sbt_offsets.push_back(pending.mat_data->rt_sbt_offset);
				material_data.push_back(pending.mat_data->data);
				material_data[material_data.size() - 1].instance_uniforms_ofs = uint32_t(pending.mm_surf->owner->shader_uniforms_offset);

				// Each instance's own motion too: a moving unit of a MultiMesh had none, and DLSS kept its texture where it
				// was on screen.
				if (pending.transform_moved || prev_mm_xform != mm_xform) {
					Transform3D prev_final = (pending.transform_moved ? pending.prev_instance_transform : pending.instance_transform) * prev_mm_xform;
					if (surf_data->is_compressed) {
						prev_final = prev_final * surf_data->aabb_transform;
					}
					motion_indices.push_back((int32_t)motion_transforms.size());
					RT_InstanceMotionData motion = {};
					RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_final, motion.prev_object_to_world);
					motion_transforms.push_back(motion);
				} else {
					motion_indices.push_back(-1);
				}

				instance_flags.push_back(pending.inst_flags);
				instance_masks.push_back(instance_mask(pending.mm_surf->owner));
			}

#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count += pending.mm_count;
				uint32_t vertices = mesh_storage->mesh_surface_get_vertices_drawn_count(pending.mesh_surface);
				uint32_t prim_count = _rt_indices_to_primitives(pending.mm_surf->primitive, vertices);
				tlas_primitive_count += prim_count * pending.mm_count;
				uint32_t build_delta = dirty_blas_list.size() - mm_pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - mm_pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif
		}
	}

	// The shadow rays ignore the faces turned away from the light, as raster's shadow pass culls them. A double-sided
	// shadow caster gets a second instance only the shadow rays see, with face culling off.
	const uint32_t instance_count = blass.size();
	for (uint32_t i = 0; i < instance_count; i++) {
		if (!(instance_masks[i] & RT_INSTANCE_MASK_DOUBLE_SIDED_SHADOW)) {
			continue;
		}
		instance_masks[i] &= ~RT_INSTANCE_MASK_DOUBLE_SIDED_SHADOW;
		if (instance_flags[i] & RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT) {
			continue;
		}
		instance_masks[i] &= ~RT_INSTANCE_MASK_SHADOW;
		blass.push_back(blass[i]);
		blas_transforms.push_back(blas_transforms[i]);
		geometry_data.push_back(geometry_data[i]);
		material_data.push_back(material_data[i]);
		sbt_offsets.push_back(sbt_offsets[i]);
		motion_indices.push_back(motion_indices[i]);
		instance_flags.push_back(instance_flags[i] | RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT);
		instance_masks.push_back(RT_INSTANCE_MASK_SHADOW);
	}
	const uint32_t expand_job_count = tlas_expand_jobs.size();
	for (uint32_t i = 0; i < expand_job_count; i++) {
		TlasExpandJob &job = tlas_expand_jobs[i];
		if (!(job.mask & RT_INSTANCE_MASK_DOUBLE_SIDED_SHADOW)) {
			continue;
		}
		job.mask &= ~RT_INSTANCE_MASK_DOUBLE_SIDED_SHADOW;
		if (job.flags & RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT) {
			continue;
		}
		job.mask &= ~RT_INSTANCE_MASK_SHADOW;
		TlasExpandJob shadow_job = job;
		shadow_job.flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
		shadow_job.mask = RT_INSTANCE_MASK_SHADOW;
		tlas_expand_jobs.push_back(shadow_job);
	}
	// The GPU's instances, records and motion transforms follow the CPU's.
	for (TlasExpandJob &job : tlas_expand_jobs) {
		job.data.first_slot = blass.size() + tlas_expand_instance_count;
		job.data.first_motion = motion_transforms.size() + tlas_expand_motion_count;
		tlas_expand_instance_count += job.count;
		tlas_expand_motion_count += job.count;
	}

	// Phase 3: BLAS / TLAS build.
#ifdef TOOLS_ENABLED
	if (collect_render_info) {
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += tlas_instance_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += tlas_primitive_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TLAS_INSTANCES] += tlas_instance_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_BUILDS] += rt_blas_builds;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_REFITS] += rt_blas_refits;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_BUILT] += rt_triangles_built;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_REFIT] += rt_triangles_refit;
	}
#endif

#ifdef TOOLS_ENABLED
	// GODOT_RT_LOG_CACHES=1: the size of every cache and of the frame's work every 600 frames, to see what grows over a
	// session (dungeon trips, streaming). Editor builds only, like the frame's counters it prints.
	static const bool log_caches = OS::get_singleton()->get_environment("GODOT_RT_LOG_CACHES") == "1";
	if (log_caches && RSG::rasterizer->get_frame_number() % 600 == 0) {
		uint32_t live_surfaces = 0;
		for (uint32_t i = 0; i < surface_chunks.size(); i++) {
			if (surface_chunks[i]) {
				for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
					live_surfaces += surface_chunks[i][j].ptr && surface_chunks[i][j].ptr->blas.is_valid() ? 1 : 0;
				}
			}
		}
		uint32_t live_materials = 0;
		for (uint32_t i = 0; i < material_chunks.size(); i++) {
			if (material_chunks[i]) {
				for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
					live_materials += material_chunks[i][j].ptr ? 1 : 0;
				}
			}
		}
		// The merged buffers by when they were last traced: the unused budget holds those older than 60 frames.
		uint64_t merged_mb[3] = {};
		uint32_t merged_traced = 0;
		uint32_t merged_traced_omm = 0;
		for (const RID &rid : merged_mm_pool.get_owned_list()) {
			const RTMergedMMEntry *e = merged_mm_pool.get_or_null(rid);
			const uint32_t age = RSG::rasterizer->get_frame_number() - e->last_used_frame;
			merged_mb[age <= 2 ? 0 : (age <= 60 ? 1 : 2)] += e->buffer_bytes;
			if (age <= 2) {
				merged_traced++;
				merged_traced_omm += e->built_omm_micromap.is_valid() ? 1 : 0;
			}
		}
		print_line(vformat("RTCACHES merged MultiMeshes traced in the last 2 frames: %d, %d with opacity micromaps", merged_traced, merged_traced_omm));
		print_line(vformat("RTCACHES merged MultiMesh buffers: %d MB traced in the last 2 frames, %d MB in the last 60, %d MB older | RD buffers %d MB, textures %d MB",
				int(merged_mb[0] >> 20), int(merged_mb[1] >> 20), int(merged_mb[2] >> 20),
				int(RD::get_singleton()->get_memory_usage(RD::MEMORY_BUFFERS) >> 20), int(RD::get_singleton()->get_memory_usage(RD::MEMORY_TEXTURES) >> 20)));
		const SceneShaderRaytracing *rt_shader = SceneShaderRaytracing::get_singleton();
		print_line(vformat("RTCACHES out of view: %d BLAS updates waiting for their turn", out_of_view_deferred));
		print_line(vformat("RTCACHES displaced: %d refit, %d guard rebuilds since the previous log", displaced_refitted, displaced_guard_rebuilds));
		displaced_guard_rebuilds = 0;
		print_line(vformat("RTCACHES frame %d: TLAS %d instances, %d BLAS builds, %d refits | displaced %d (%d vertices), %d deferred | surfaces %d, merged MultiMeshes %d, deformed %d, LOD views %d, materials %d, dedicated UBOs %d | hit groups %d, pipeline swaps %d | video %d MB",
				RSG::rasterizer->get_frame_number(), tlas_instance_count, rt_blas_builds, rt_blas_refits,
				displaced_evaluated, int64_t(displaced_vertices), displaced_deferred,
				live_surfaces, merged_mm_pool.get_rid_count(), deformed_pool.get_rid_count(), lod_views.size(), live_materials, mat_ubo_dedicated_buffers.size(),
				rt_shader->get_hit_group_slot_count(), rt_shader->get_pipeline_swap_count(),
				int(RD::get_singleton()->get_memory_usage(RD::MEMORY_TOTAL) / (1024 * 1024))));
	}
#endif

	SceneShaderRaytracing::get_singleton()->finalize_custom_shaders();

	RD::get_singleton()->compute_list_end();

	// After the merges: the displaced positions overwrite the merged ones before the BLAS builds read them.
	_displace_merged_multimeshes();

	// The records first: the instances written on the GPU add theirs after these.
	finalize_buffers(state);
	// The cut-outs baked into opacity micromaps and the micromaps built, before the BLASes that use them.
	_omm_dispatch_bakes();
	build_acceleration_structures(state, dirty_blas_list, dirty_blas_update_list);

	return state;
}

// ---------------------------------------------------------------------------
// Light gathering
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::gather_lights(const RenderDataRD *p_render_data, RT_LightData *r_light_data, uint32_t p_max_lights) {
	uint32_t rt_light_count = 0;
	rt_directional_projector = RID();

	if (!p_render_data || !p_render_data->lights) {
		return rt_light_count;
	}

	RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
	const Transform3D &cam_xform = p_render_data->scene_data->cam_transform;
	const Vector3 cam_pos = cam_xform.origin;
	const Vector3 cam_forward = -cam_xform.basis.get_column(2).normalized(); // -Z is forward in Godot.

	// Compute light energy matching rasterizer conventions
	auto compute_light_energy = [&](RID p_base, RSE::LightType p_type) -> float {
		float sign = ls->light_is_negative(p_base) ? -1.0f : 1.0f;
		float e = sign * ls->light_get_param(p_base, RSE::LIGHT_PARAM_ENERGY);
		if (owner->is_using_physical_light_units()) {
			e *= ls->light_get_param(p_base, RSE::LIGHT_PARAM_INTENSITY);
			if (p_type == RSE::LIGHT_OMNI) {
				e *= 1.0f / (Math::PI * 4.0f);
			} else if (p_type == RSE::LIGHT_SPOT) {
				e *= 1.0f / Math::PI;
			}
		} else {
			e *= Math::PI;
		}
		if (p_render_data->camera_attributes.is_valid()) {
			e *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}
		return e;
	};

	// Scoring helper: approximate power/solid-angle contribution.
	struct LightScore {
		RID light_instance;
		float score;
	};

	LocalVector<LightScore> positional_lights;

	// Lights behind the camera are deprioritized. The penalty ramps in linearly
	// over BEHIND_FRUSTUM_BUFFER_M so nearby behind-camera lights aren't punished harshly.
	// At or in front of the camera plane: no penalty. Beyond the buffer: full penalty.
	static const float BEHIND_FRUSTUM_PENALTY = 0.05f;
	static const float BEHIND_FRUSTUM_BUFFER_M = 20.0f;

	// Helper: score a positional light and add to candidates.
	auto score_positional_light = [&](RID light_instance) {
		RID base = ls->light_instance_get_base_light(light_instance);
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		Vector3 light_pos = xform.origin;
		Vector3 to_light = light_pos - cam_pos;
		float dist_sq = MAX(to_light.dot(to_light), 0.01f);
		Color color = ls->light_get_color(base);
		float energy = ls->light_get_param(base, RSE::LIGHT_PARAM_ENERGY);
		float lum = color.r * 0.2126f + color.g * 0.7152f + color.b * 0.0722f;

		// Range factor: strongly prefer lights with a large influence radius.
		// Lights with no range limit (range == 0) are treated as infinite.
		float range = ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
		float range_factor = (range > 0.0f) ? (range * range) : 1.0e12f;

		// Signed depth of the light along the camera forward axis (world units).
		// Positive = in front, negative = behind. Ramp penalty in over the buffer zone.
		float forward_depth = to_light.dot(cam_forward);
		float t = CLAMP((forward_depth + BEHIND_FRUSTUM_BUFFER_M) / BEHIND_FRUSTUM_BUFFER_M, 0.0f, 1.0f);
		float frustum_factor = Math::lerp(BEHIND_FRUSTUM_PENALTY, 1.0f, t);

		float score = (energy * lum * range_factor * frustum_factor) / dist_sq;

		LightScore ls_entry = {};
		ls_entry.light_instance = light_instance;
		ls_entry.score = score;
		positional_lights.push_back(ls_entry);
	};

	// Directional lights from the frustum-culled list (they're global, always included).
	const PagedArray<RID> &lights = *p_render_data->lights;
	for (uint32_t li = 0; li < (uint32_t)lights.size(); li++) {
		RID light_instance = lights[li];
		RID base = ls->light_instance_get_base_light(light_instance);
		RSE::LightType type = ls->light_get_type(base);

		if (type != RSE::LIGHT_DIRECTIONAL) {
			continue;
		}
		if (rt_light_count >= p_max_lights) {
			break;
		}
		RT_LightData &ld = r_light_data[rt_light_count];
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		Vector3 dir = -xform.basis.get_column(2).normalized();
		ld.position[0] = dir.x;
		ld.position[1] = dir.y;
		ld.position[2] = dir.z;
		ld.type = RT_LIGHT_TYPE_DIRECTIONAL;
		Color linear_col = ls->light_get_color(base).srgb_to_linear();
		float energy = compute_light_energy(base, RSE::LIGHT_DIRECTIONAL);
		ld.emission[0] = linear_col.r * energy;
		ld.emission[1] = linear_col.g * energy;
		ld.emission[2] = linear_col.b * energy;
		ld.radius = Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE) * 0.5f); // Half-angle in radians.
		ld.attenuation = 0.0f; // No distance attenuation.
		ld.inv_max_range = -1.0f; // Infinite range.
		ld.max_range_squared = 0.0f;
		ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR);
		ld.indirect_energy = ls->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		ld.inv_spot_attenuation = 0.0f;
		ld.cos_spot_angle = 0.0f;
		ld.spot_direction[0] = 0.0f;
		ld.spot_direction[1] = 0.0f;
		ld.spot_direction[2] = 0.0f;
		ld.cull_mask = ls->light_get_cull_mask(base);
		ld.shadow_opacity = ls->light_has_shadow(base) ? ls->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_OPACITY) : 0.0f;

		// The projector, as raster's update_light_buffers() maps it (in view space there): the first directional light
		// with one has the texture binding.
		memset(ld.projector_uv_x, 0, sizeof(ld.projector_uv_x));
		memset(ld.projector_uv_y, 0, sizeof(ld.projector_uv_y));
		ld.projector_enabled = 0;
		ld.pad[0] = ld.pad[1] = ld.pad[2] = 0;
		const RID projector = ls->light_get_projector(base);
		if (projector.is_valid() && rt_directional_projector.is_null()) {
			const RID projector_texture = RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(projector, true);
			if (projector_texture.is_valid()) {
				const Transform3D world_to_light = xform.orthonormalized().affine_inverse();
				const Vector2 size = ls->light_directional_get_projector_size(base);
				const Vector2 offset = ls->light_directional_get_projector_offset(base);
				for (int k = 0; k < 3; k++) {
					ld.projector_uv_x[k] = world_to_light.basis.rows[0][k] / size.x;
					ld.projector_uv_y[k] = -world_to_light.basis.rows[1][k] / size.y;
				}
				ld.projector_uv_x[3] = (world_to_light.origin.x - offset.x) / size.x + 0.5f;
				ld.projector_uv_y[3] = 0.5f - (world_to_light.origin.y - offset.y) / size.y;
				ld.projector_enabled = 1;
				rt_directional_projector = projector_texture;
			}
		}
		rt_light_count++;
	}

	// Positional lights from the AABB-culled RT list (superset of frustum).
	if (p_render_data->rt_lights) {
		const PagedArray<RID> &rt_lights = *p_render_data->rt_lights;
		for (uint32_t li = 0; li < (uint32_t)rt_lights.size(); li++) {
			score_positional_light(rt_lights[li]);
		}
	}

	// Sort all positional lights by score descending.
	struct LightScoreComparator {
		bool operator()(const LightScore &a, const LightScore &b) const {
			return a.score > b.score;
		}
	};
	positional_lights.sort_custom<LightScoreComparator>();

	// Fill remaining slots with top positional lights.
	for (uint32_t i = 0; i < positional_lights.size() && rt_light_count < p_max_lights; i++) {
		RID light_instance = positional_lights[i].light_instance;
		RID base = ls->light_instance_get_base_light(light_instance);
		RSE::LightType type = ls->light_get_type(base);

		RT_LightData &ld = r_light_data[rt_light_count];
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		ld.position[0] = xform.origin.x;
		ld.position[1] = xform.origin.y;
		ld.position[2] = xform.origin.z;
		ld.type = (type == RSE::LIGHT_SPOT) ? RT_LIGHT_TYPE_SPOT : RT_LIGHT_TYPE_OMNI;

		Color linear_col = ls->light_get_color(base).srgb_to_linear();
		float energy = compute_light_energy(base, type);
		ld.emission[0] = linear_col.r * energy;
		ld.emission[1] = linear_col.g * energy;
		ld.emission[2] = linear_col.b * energy;
		ld.radius = ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE);
		ld.attenuation = ls->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
		float range = ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
		if (range > 0.0f) {
			ld.inv_max_range = 1.0f / range;
			ld.max_range_squared = range * range;
		} else {
			ld.inv_max_range = -1.0f;
			ld.max_range_squared = 0.0f;
		}
		ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f; // Matches rasterizer convention (light_storage.cpp), normalizes 0.5 default to 1.0.
		ld.cull_mask = ls->light_get_cull_mask(base);
		ld.shadow_opacity = ls->light_has_shadow(base) ? ls->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_OPACITY) : 0.0f;
		ld.indirect_energy = ls->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);

		if (type == RSE::LIGHT_SPOT) {
			ld.inv_spot_attenuation = 1.0f / MAX(0.001f, ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION));
			float spot_angle_deg = ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE);
			ld.cos_spot_angle = Math::cos(Math::deg_to_rad(spot_angle_deg));
			Vector3 spot_dir = -xform.basis.get_column(2).normalized();
			ld.spot_direction[0] = spot_dir.x;
			ld.spot_direction[1] = spot_dir.y;
			ld.spot_direction[2] = spot_dir.z;
		} else {
			ld.inv_spot_attenuation = 0.0f;
			ld.cos_spot_angle = 0.0f;
			ld.spot_direction[0] = 0.0f;
			ld.spot_direction[1] = 0.0f;
			ld.spot_direction[2] = 0.0f;
		}
		memset(ld.projector_uv_x, 0, sizeof(ld.projector_uv_x));
		memset(ld.projector_uv_y, 0, sizeof(ld.projector_uv_y));
		ld.projector_enabled = 0;
		ld.pad[0] = ld.pad[1] = ld.pad[2] = 0;
		rt_light_count++;
	}

	return rt_light_count;
}

// ---------------------------------------------------------------------------
// Decals
// ---------------------------------------------------------------------------

// Same as rt_decal_grid_hash() in raytracing_decals_inc.glsl.
static _FORCE_INLINE_ uint32_t _rt_decal_grid_hash(int32_t p_x, int32_t p_y, int32_t p_z) {
	return (uint32_t(p_x) * 73856093u) ^ (uint32_t(p_y) * 19349663u) ^ (uint32_t(p_z) * 83492791u);
}

// A transform as three vec4 rows (dot products in GLSL).
static void _rt_store_rows(const Transform3D &p_xform, float *r_rows) {
	for (int i = 0; i < 3; i++) {
		r_rows[i * 4 + 0] = p_xform.basis.rows[i][0];
		r_rows[i * 4 + 1] = p_xform.basis.rows[i][1];
		r_rows[i * 4 + 2] = p_xform.basis.rows[i][2];
		r_rows[i * 4 + 3] = p_xform.origin[i];
	}
}

static void _rt_store_rect(const Rect2 &p_rect, float *r_rect) {
	r_rect[0] = p_rect.position.x;
	r_rect[1] = p_rect.position.y;
	r_rect[2] = p_rect.size.x;
	r_rect[3] = p_rect.size.y;
}

// Uploads p_size bytes when they differ from what r_uploaded holds, growing the buffer as needed.
template <typename T>
static void _rt_upload_if_changed(RID &r_buffer, uint32_t &r_capacity, LocalVector<T> &r_uploaded, const LocalVector<T> &p_data, const char *p_name) {
	const uint32_t size = p_data.size() * sizeof(T);
	if (r_buffer.is_valid() && r_uploaded.size() == p_data.size() && memcmp(r_uploaded.ptr(), p_data.ptr(), size) == 0) {
		return;
	}
	if (r_capacity < size) {
		if (r_buffer.is_valid()) {
			RD::get_singleton()->free_rid(r_buffer);
		}
		r_capacity = Math::next_power_of_2(size);
		r_buffer = RD::get_singleton()->storage_buffer_create(r_capacity);
		RD::get_singleton()->set_resource_name(r_buffer, p_name);
	}
	RD::get_singleton()->buffer_update(r_buffer, 0, size, p_data.ptr());
	r_uploaded = p_data;
}

uint32_t RenderRaytracing::gather_decals(RTViewportState *p_state, const RenderDataRD *p_render_data) {
	decal_data.clear();
	decal_grid.clear();
	if (!p_render_data || !p_render_data->rt_decals) {
		return 0;
	}

	RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
	const Vector3 cam_pos = p_render_data->scene_data->cam_transform.origin;

	// The decals raster would draw (TextureStorage::update_decal_buffer()), around the camera as well.
	struct DecalSort {
		RID instance;
		RID decal;
		float depth = 0.0;
		float fade = 1.0;
		bool operator<(const DecalSort &p_other) const { return depth < p_other.depth; }
	};
	LocalVector<DecalSort> sorted;
	const PagedArray<RID> &decals = *p_render_data->rt_decals;
	for (uint64_t i = 0; i < decals.size(); i++) {
		DecalSort ds;
		ds.instance = decals[i];
		if (!ts->owns_decal_instance(ds.instance)) {
			continue;
		}
		ds.decal = ts->decal_instance_get_base(ds.instance);
		if (!ts->owns_decal(ds.decal)) {
			continue;
		}
		if (!ts->decal_get_texture(ds.decal, RSE::DECAL_TEXTURE_ALBEDO).is_valid() && !ts->decal_get_texture(ds.decal, RSE::DECAL_TEXTURE_EMISSION).is_valid()) {
			continue; // No albedo, no emission, no decal.
		}
		const float distance = cam_pos.distance_to(ts->decal_instance_get_transform(ds.instance).origin);
		if (ts->decal_is_distance_fade_enabled(ds.decal)) {
			const float fade_begin = ts->decal_get_distance_fade_begin(ds.decal);
			const float fade_length = ts->decal_get_distance_fade_length(ds.decal);
			if (distance > fade_begin) {
				if (distance > fade_begin + fade_length) {
					continue;
				}
				ds.fade = Math::smoothstep(0.0f, 1.0f, 1.0f - float(distance - fade_begin) / fade_length);
			}
		}
		ds.depth = distance - ts->decal_instance_get_sorting_offset(ds.instance);
		sorted.push_back(ds);
	}
	if (sorted.is_empty()) {
		return 0;
	}
	// The nearest RT_DECALS_MAX.
	sorted.sort();
	if (sorted.size() > RT_DECALS_MAX) {
		sorted.resize(RT_DECALS_MAX);
	}

	const uint32_t count = sorted.size();
	decal_data.resize(count);
	LocalVector<AABB> aabbs;
	aabbs.resize(count);

	Transform3D uv_xform;
	uv_xform.basis.scale(Vector3(2.0, 1.0, 2.0));
	uv_xform.origin = Vector3(-1.0, 0.0, -1.0);

	// Farthest first: the hit shaders apply them in index order, and raster draws the nearest decal on top.
	for (uint32_t i = 0; i < count; i++) {
		const DecalSort &ds = sorted[count - 1 - i];
		RT_DecalData &dd = decal_data[i];
		memset(&dd, 0, sizeof(RT_DecalData));

		const Transform3D xform = ts->decal_instance_get_transform(ds.instance);
		const Vector3 extents = ts->decal_get_size(ds.decal) / 2;
		Transform3D scale_xform;
		scale_xform.basis.scale(extents);
		_rt_store_rows((xform * scale_xform * uv_xform).affine_inverse(), dd.xform);
		aabbs[i] = xform.xform(AABB(-extents, extents * 2.0));

		const Vector3 normal = xform.basis.get_column(Vector3::AXIS_Y).normalized();
		dd.normal[0] = normal.x;
		dd.normal[1] = normal.y;
		dd.normal[2] = normal.z;
		dd.normal_fade = ts->decal_get_normal_fade(ds.decal);

		const RID albedo_tex = ts->decal_get_texture(ds.decal, RSE::DECAL_TEXTURE_ALBEDO);
		const RID normal_tex = ts->decal_get_texture(ds.decal, RSE::DECAL_TEXTURE_NORMAL);
		const RID orm_tex = ts->decal_get_texture(ds.decal, RSE::DECAL_TEXTURE_ORM);
		const RID emission_tex = ts->decal_get_texture(ds.decal, RSE::DECAL_TEXTURE_EMISSION);
		if (albedo_tex.is_valid()) {
			_rt_store_rect(ts->decal_atlas_get_texture_rect(albedo_tex), dd.albedo_rect);
		}
		if (normal_tex.is_valid()) {
			_rt_store_rect(ts->decal_atlas_get_texture_rect(normal_tex), dd.normal_rect);
			Transform3D normal_xform;
			normal_xform.basis = xform.basis.orthonormalized();
			_rt_store_rows(normal_xform, dd.normal_xform);
		}
		if (orm_tex.is_valid()) {
			_rt_store_rect(ts->decal_atlas_get_texture_rect(orm_tex), dd.orm_rect);
		}
		if (emission_tex.is_valid()) {
			_rt_store_rect(ts->decal_atlas_get_texture_rect(emission_tex), dd.emission_rect);
		}

		const Color modulate = ts->decal_get_modulate(ds.decal).srgb_to_linear();
		dd.modulate[0] = modulate.r;
		dd.modulate[1] = modulate.g;
		dd.modulate[2] = modulate.b;
		dd.modulate[3] = modulate.a * ds.fade;
		dd.emission_energy = ts->decal_get_emission_energy(ds.decal) * ds.fade;
		dd.albedo_mix = ts->decal_get_albedo_mix(ds.decal);
		dd.mask = ts->decal_get_cull_mask(ds.decal);
		dd.upper_fade = ts->decal_get_upper_fade(ds.decal);
		dd.lower_fade = ts->decal_get_lower_fade(ds.decal);
	}

	// A hashed grid, raster's clusters in world space: every bucket lists the decals whose box touches one of its cells, and
	// a hit tests those of its cell's bucket. The cells are as large as the median decal, which then spans 2x2x2 of them at
	// most; a decal over too many cells is tested at every hit instead.
	LocalVector<float> sizes;
	sizes.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		sizes[i] = aabbs[i].get_longest_axis_size();
	}
	sizes.sort();
	const float cell_size = MAX(sizes[count / 2], 0.01f);
	const float inv_cell_size = 1.0f / cell_size;

	struct CellRange {
		int32_t from[3];
		int32_t to[3];
		bool global = false;
	};
	LocalVector<CellRange> ranges;
	ranges.resize(count);
	LocalVector<uint32_t> global_decals;
	uint64_t entry_bound = 0;
	for (uint32_t i = 0; i < count; i++) {
		CellRange &range = ranges[i];
		// The hit shader floors its position times inv_cell_size in float: the same here, with a margin for the difference
		// between a hit point and the box.
		const AABB box = aabbs[i].grow(cell_size * 0.01f);
		uint64_t cells = 1;
		for (int a = 0; a < 3; a++) {
			const double from = Math::floor(float(box.position[a]) * inv_cell_size);
			const double to = Math::floor(float(box.position[a] + box.size[a]) * inv_cell_size);
			if (!(from >= double(INT32_MIN) && to <= double(INT32_MAX - 1)) || to - from + 1.0 > double(RT_DECAL_GRID_MAX_CELLS)) {
				range.global = true;
				break;
			}
			range.from[a] = int32_t(from);
			range.to[a] = int32_t(to);
			cells *= uint64_t(range.to[a] - range.from[a] + 1);
		}
		if (range.global || cells > RT_DECAL_GRID_MAX_CELLS) {
			range.global = true;
			global_decals.push_back(i);
		} else {
			entry_bound += cells;
		}
	}

	const uint32_t bucket_count = CLAMP(Math::next_power_of_2(uint32_t(MAX<uint64_t>(entry_bound * 2, uint64_t(64)))), 64u, uint32_t(RT_DECAL_GRID_MAX_BUCKETS));
	const uint32_t bucket_mask = bucket_count - 1;
	const uint32_t offsets_base = RT_DECAL_GRID_HEADER + global_decals.size();
	const uint32_t entries_base = offsets_base + bucket_count + 1;

	LocalVector<uint32_t> last_decal; // Two cells of a decal can share a bucket: listed once.
	last_decal.resize(bucket_count);
	LocalVector<uint32_t> bucket_entries;
	bucket_entries.resize(bucket_count);
	auto visit_buckets = [&](uint32_t p_decal, bool p_fill) {
		const CellRange &range = ranges[p_decal];
		for (int32_t z = range.from[2]; z <= range.to[2]; z++) {
			for (int32_t y = range.from[1]; y <= range.to[1]; y++) {
				for (int32_t x = range.from[0]; x <= range.to[0]; x++) {
					const uint32_t bucket = _rt_decal_grid_hash(x, y, z) & bucket_mask;
					if (last_decal[bucket] == p_decal) {
						continue;
					}
					last_decal[bucket] = p_decal;
					if (p_fill) {
						decal_grid[bucket_entries[bucket]++] = p_decal;
					} else {
						bucket_entries[bucket]++;
					}
				}
			}
		}
	};

	for (uint32_t b = 0; b < bucket_count; b++) {
		last_decal[b] = UINT32_MAX;
		bucket_entries[b] = 0;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!ranges[i].global) {
			visit_buckets(i, false);
		}
	}

	uint32_t entry_count = 0;
	for (uint32_t b = 0; b < bucket_count; b++) {
		entry_count += bucket_entries[b];
	}
	decal_grid.resize(entries_base + entry_count);
	memcpy(&decal_grid[0], &inv_cell_size, sizeof(float));
	decal_grid[1] = bucket_mask;
	decal_grid[2] = global_decals.size();
	decal_grid[3] = 0;
	for (uint32_t i = 0; i < global_decals.size(); i++) {
		decal_grid[RT_DECAL_GRID_HEADER + i] = global_decals[i];
	}
	uint32_t entry = entries_base;
	for (uint32_t b = 0; b < bucket_count; b++) {
		decal_grid[offsets_base + b] = entry;
		const uint32_t entries = bucket_entries[b];
		bucket_entries[b] = entry; // Now the next entry to fill.
		last_decal[b] = UINT32_MAX;
		entry += entries;
	}
	decal_grid[offsets_base + bucket_count] = entry;
	for (uint32_t i = 0; i < count; i++) {
		if (!ranges[i].global) {
			visit_buckets(i, true);
		}
	}

	_rt_upload_if_changed(p_state->decal_buffer, p_state->decal_buffer_capacity, p_state->uploaded_decals, decal_data, "RT Decal Buffer");
	_rt_upload_if_changed(p_state->decal_grid_buffer, p_state->decal_grid_buffer_capacity, p_state->uploaded_decal_grid, decal_grid, "RT Decal Grid Buffer");
	return count;
}

// ---------------------------------------------------------------------------
// Light grid
// ---------------------------------------------------------------------------

// The choice of light at a hit (lights_evaluate_direct_lighting(), raytracing_lights_inc.glsl) weighs every light that
// can light the point and only those: a light lights nothing beyond its range, so a hit weighs the lights listed in its
// cell's bucket and the global ones. A hashed grid as gather_decals()'s: every bucket lists the lights whose range sphere
// touches one of its cells. The cells are half the median range, so a typical light spans 5x5x5 of them; directional
// lights, lights without a range and lights over too many cells are global, weighed at every hit. A bucket can hold a
// light of another cell sharing it, which weighs 0 there: the choice is the same as over every light.
// [0] 1 / cell size (float bits), [1] buckets - 1, [2] G, the global lights, [3] unused; [4, 4 + G) those lights;
// [4 + G, 4 + G + buckets] the first entry of each bucket in this buffer (the last one ends the previous), then the entries.
void RenderRaytracing::build_light_grid(RTViewportState *p_state, const RT_LightData *p_lights, uint32_t p_count) {
	light_grid.clear();

	LocalVector<float> ranges;
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_lights[i].type != RT_LIGHT_TYPE_DIRECTIONAL && p_lights[i].max_range_squared > 0.0f) {
			ranges.push_back(Math::sqrt(p_lights[i].max_range_squared));
		}
	}
	float cell_size = 1.0f;
	if (!ranges.is_empty()) {
		ranges.sort();
		cell_size = MAX(ranges[ranges.size() / 2] * 0.5f, 0.01f);
	}
	const float inv_cell_size = 1.0f / cell_size;

	struct CellRange {
		int32_t from[3];
		int32_t to[3];
		bool global = false;
	};
	LocalVector<CellRange> cell_ranges;
	cell_ranges.resize(p_count);
	LocalVector<uint32_t> global_lights;
	uint64_t entry_bound = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		const RT_LightData &light = p_lights[i];
		CellRange &range = cell_ranges[i];
		if (light.type == RT_LIGHT_TYPE_DIRECTIONAL || light.max_range_squared <= 0.0f) {
			range.global = true;
			global_lights.push_back(i);
			continue;
		}
		// The hit shader floors its position times inv_cell_size in float: the same here, with a margin for the difference
		// between a hit point and the sphere.
		const float radius = Math::sqrt(light.max_range_squared) + cell_size * 0.01f;
		uint64_t cells = 1;
		for (int a = 0; a < 3; a++) {
			const double from = Math::floor(float(light.position[a] - radius) * inv_cell_size);
			const double to = Math::floor(float(light.position[a] + radius) * inv_cell_size);
			if (!(from >= double(INT32_MIN) && to <= double(INT32_MAX - 1)) || to - from + 1.0 > double(RT_LIGHT_GRID_MAX_CELLS)) {
				range.global = true;
				break;
			}
			range.from[a] = int32_t(from);
			range.to[a] = int32_t(to);
			cells *= uint64_t(range.to[a] - range.from[a] + 1);
		}
		if (range.global || cells > RT_LIGHT_GRID_MAX_CELLS) {
			range.global = true;
			global_lights.push_back(i);
		} else {
			entry_bound += cells;
		}
	}

	const uint32_t bucket_count = CLAMP(Math::next_power_of_2(uint32_t(MAX<uint64_t>(entry_bound * 2, uint64_t(64)))), 64u, uint32_t(RT_LIGHT_GRID_MAX_BUCKETS));
	const uint32_t bucket_mask = bucket_count - 1;
	const uint32_t offsets_base = RT_LIGHT_GRID_HEADER + global_lights.size();
	const uint32_t entries_base = offsets_base + bucket_count + 1;

	LocalVector<uint32_t> last_light; // Two cells of a light can share a bucket: listed once.
	last_light.resize(bucket_count);
	LocalVector<uint32_t> bucket_entries;
	bucket_entries.resize(bucket_count);
	auto visit_buckets = [&](uint32_t p_light, bool p_fill) {
		const CellRange &range = cell_ranges[p_light];
		const Vector3 center(p_lights[p_light].position[0], p_lights[p_light].position[1], p_lights[p_light].position[2]);
		const float radius = Math::sqrt(p_lights[p_light].max_range_squared) + cell_size * 0.01f;
		for (int32_t z = range.from[2]; z <= range.to[2]; z++) {
			for (int32_t y = range.from[1]; y <= range.to[1]; y++) {
				for (int32_t x = range.from[0]; x <= range.to[0]; x++) {
					// Only the cells the range sphere reaches: its distance to the cell's box.
					const Vector3 cell_min = Vector3(x, y, z) * cell_size;
					const Vector3 nearest = center.clamp(cell_min, cell_min + Vector3(cell_size, cell_size, cell_size));
					if (nearest.distance_squared_to(center) > radius * radius) {
						continue;
					}
					const uint32_t bucket = _rt_decal_grid_hash(x, y, z) & bucket_mask;
					if (last_light[bucket] == p_light) {
						continue;
					}
					last_light[bucket] = p_light;
					if (p_fill) {
						light_grid[bucket_entries[bucket]++] = p_light;
					} else {
						bucket_entries[bucket]++;
					}
				}
			}
		}
	};

	for (uint32_t b = 0; b < bucket_count; b++) {
		last_light[b] = UINT32_MAX;
		bucket_entries[b] = 0;
	}
	for (uint32_t i = 0; i < p_count; i++) {
		if (!cell_ranges[i].global) {
			visit_buckets(i, false);
		}
	}

	uint32_t entry_count = 0;
	for (uint32_t b = 0; b < bucket_count; b++) {
		entry_count += bucket_entries[b];
	}
	light_grid.resize(entries_base + entry_count);
	memcpy(&light_grid[0], &inv_cell_size, sizeof(float));
	light_grid[1] = bucket_mask;
	light_grid[2] = global_lights.size();
	light_grid[3] = 0;
	for (uint32_t i = 0; i < global_lights.size(); i++) {
		light_grid[RT_LIGHT_GRID_HEADER + i] = global_lights[i];
	}
	uint32_t entry = entries_base;
	for (uint32_t b = 0; b < bucket_count; b++) {
		light_grid[offsets_base + b] = entry;
		const uint32_t entries = bucket_entries[b];
		bucket_entries[b] = entry; // Now the next entry to fill.
		last_light[b] = UINT32_MAX;
		entry += entries;
	}
	light_grid[offsets_base + bucket_count] = entry;
	for (uint32_t i = 0; i < p_count; i++) {
		if (!cell_ranges[i].global) {
			visit_buckets(i, true);
		}
	}

	_rt_upload_if_changed(p_state->light_grid_buffer, p_state->light_grid_buffer_capacity, p_state->uploaded_light_grid, light_grid, "RT Light Grid Buffer");
}

// ---------------------------------------------------------------------------
// Sky sampling distribution
// ---------------------------------------------------------------------------

// The payload counts a path's bounces on 4 bits (get_total_bounces(), raytracing_inc.glsl), next to the sky's MIS share.
static_assert(SceneShaderRaytracing::RT_MAX_BOUNCES < 16, "The path payload's bounce counters hold 4 bits.");

void RenderRaytracing::build_sky_sampling(RTViewportState *p_state, RID p_radiance_texture, bool p_octmap_array, float p_border_size) {
	RD *rd = RD::get_singleton();
	if (!p_state->sky_sampling_buffer.is_valid()) {
		p_state->sky_sampling_buffer = rd->storage_buffer_create(SKY_SAMPLING_FLOATS * sizeof(float));
		rd->set_resource_name(p_state->sky_sampling_buffer, "RT Sky Sampling");
	}

	// Rebuilt every frame: a sky updated in real time or by parts (Sky.process_mode) changes it without telling, and both
	// passes together cost a few microseconds.
	SkySamplingShader::PushConstant push_constant = {};
	push_constant.border_size[0] = p_border_size;
	push_constant.border_size[1] = 1.0f - p_border_size * 2.0f;

	const SkySamplingShader::Mode luminance_mode = p_octmap_array ? SkySamplingShader::MODE_LUMINANCE_ARRAY : SkySamplingShader::MODE_LUMINANCE;
	RD::Uniform buffer(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_state->sky_sampling_buffer);
	RD::Uniform radiance(RD::UNIFORM_TYPE_TEXTURE, 1, p_radiance_texture);
	RD::Uniform sampler(RD::UNIFORM_TYPE_SAMPLER, 2, RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));

	RENDER_TIMESTAMP("RT Sky Sampling");
	rd->draw_command_begin_label("RT Sky Sampling");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, sky_sampling_shader.pipeline[luminance_mode]);
	rd->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache(sky_sampling_shader.version_shader[luminance_mode], 0, buffer, radiance, sampler), 0);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(SkySamplingShader::PushConstant));
	rd->compute_list_dispatch_threads(compute_list, SKY_SAMPLING_SIZE, SKY_SAMPLING_SIZE, 1);
	rd->compute_list_add_barrier(compute_list);
	rd->compute_list_bind_compute_pipeline(compute_list, sky_sampling_shader.pipeline[SkySamplingShader::MODE_CDF]);
	rd->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache(sky_sampling_shader.version_shader[SkySamplingShader::MODE_CDF], 0, buffer), 0);
	rd->compute_list_dispatch(compute_list, 1, 1, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
}

// ---------------------------------------------------------------------------
// Uniform set update
// ---------------------------------------------------------------------------

RID RenderRaytracing::update_uniform_set(RTViewportState *p_state, const RenderDataRD *p_render_data, uint32_t p_rt_flags, bool p_background_is_color, const Color &p_background_color, bool p_reflection_shows_background) {
	ERR_FAIL_NULL_V(p_state, RID());

	Ref<RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
	if (p_render_data && p_render_data->render_buffers.is_valid()) {
		if (p_render_data->render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
			rb_data = p_render_data->render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		}
	}

	if (rb_data.is_null()) {
		return RID();
	}

	RenderSceneBuffersRD *rb = p_render_data->render_buffers.ptr();

	// SET 0 indices must match raytracing_common_inc.glsl / scene_raytracing_raygen.glsl / samplers includes.
	Vector<RD::Uniform> uniforms;

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		rt_ensure_textures(rb);
		u.append_id(rt_get_texture(rb));
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE;
		ERR_FAIL_COND_V(p_state->tlas == RID(), RID());
		u.append_id(p_state->tlas);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(owner->scene_state.uniform_buffers[0]);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->geometry_buffer.is_valid()) {
			u.append_id(p_state->geometry_buffer);
		} else {
			// Use a default buffer if no geometry
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 4: Per-instance motion index buffer (int32 per TLAS instance, -1 = no motion).
	{
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->motion_index_buffer.is_valid()) {
			u.append_id(p_state->motion_index_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Motion transforms past sampler block growth reservation (bindings 28-31).
	{
		RD::Uniform u;
		u.binding = 32;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->motion_transform_buffer.is_valid()) {
			u.append_id(p_state->motion_transform_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 5: Material buffer.
	{
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->material_buffer.is_valid()) {
			u.append_id(p_state->material_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// The sky radiance octahedral map (binding 7), and the sky's sampling distribution made from it (binding 40), whose use
	// the params tell the shaders (RT_PARAM_ENV_SAMPLING).
	const bool use_octmap_array = owner->is_using_radiance_octmap_array();
	RID radiance_texture;
	bool sky_sampling = false;
	if (p_render_data && p_render_data->environment.is_valid()) {
		RID sky_rid = owner->environment_get_sky(p_render_data->environment);
		if (sky_rid.is_valid()) {
			radiance_texture = use_octmap_array
					? owner->sky.sky_get_radiance_texture_rd(sky_rid)
					: owner->sky.sky_get_radiance_2d_texture_rd(sky_rid);
			// Environment.pathtracing_sky_sampling: off, nothing is built and the sky is found by BRDF sampling alone.
			if (radiance_texture.is_valid() && RendererEnvironmentStorage::get_singleton()->environment_get_pathtracing_sky_sampling(p_render_data->environment)) {
				build_sky_sampling(p_state, radiance_texture, use_octmap_array, owner->sky.sky_get_uv_border_size(sky_rid));
				sky_sampling = true;
			}
		}
	}

	// Binding 6: Raytracing params + unjittered VP matrices.
	{
		struct {
			//TODO: I see no reason why we need to pass this as a float array
			float params[16];
			float prev_vp_unjittered[16];
			float curr_vp_unjittered[16];
		} rt_ubo = {};
		static_assert(sizeof(rt_ubo) == 48 * sizeof(float));

		if (p_render_data && p_render_data->environment.is_valid()) {
			RendererEnvironmentStorage *env_storage = RendererEnvironmentStorage::get_singleton();
			RID env = p_render_data->environment;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_VIS_MODE] = (float)env_storage->environment_get_pathtracing_debug_mode(env);
			// Read by the shaders instead of specialization constants: changing them keeps the pipeline.
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_SAMPLE_COUNT] = (float)MAX(1, env_storage->environment_get_pathtracing_samples_per_pixel(env));
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_MAX_BOUNCES] = (float)CLAMP(env_storage->environment_get_pathtracing_max_bounces(env), 1, SceneShaderRaytracing::RT_MAX_BOUNCES);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_DENOISER] = (float)(int)env_storage->environment_get_pathtracing_denoiser(env);
		}

		// rt_params layout (see RaytracingParamIndex enum):
		// [0] = VIS_MODE, [1] = SAMPLE_COUNT, [2] = MAX_BOUNCES,
		// [3] = DLSS_RR_ENABLED, [14] = LIGHT_COUNT, [15] = FRAME_INDEX
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_FRAME_INDEX] = float(p_state->frame_counter++);
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_FRONT_LAYER_COUNT] = float(front_layer_instance_count);
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_PROCEDURAL_MASKS] = float(procedural_instance_masks);
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_ENV_SAMPLING] = sky_sampling ? 1.0 : 0.0;
		uint32_t rt_param_flags = 0;
		if (GLOBAL_GET_CACHED(bool, "rendering/pathtracing/light_sampling_ris")) {
			rt_param_flags |= SceneShaderRaytracing::RT_PARAM_FLAG_LIGHT_SAMPLING_RIS;
		}
		if (GLOBAL_GET_CACHED(bool, "rendering/pathtracing/russian_roulette")) {
			rt_param_flags |= SceneShaderRaytracing::RT_PARAM_FLAG_RUSSIAN_ROULETTE;
		}
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_FLAGS] = float(rt_param_flags);
		if (p_background_is_color) {
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_BACKGROUND_COLOR + 0] = p_background_color.r;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_BACKGROUND_COLOR + 1] = p_background_color.g;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_BACKGROUND_COLOR + 2] = p_background_color.b;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_BACKGROUND_COLOR_USED] = 1.0;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_REFLECTION_BACKGROUND] = p_reflection_shows_background ? 1.0 : 0.0;
		}

		// Unjittered VP for motion vectors (matches raster convention).
		{
			Projection correction;
			correction.set_depth_correction(true);

			Projection prev_vp = (correction * p_render_data->scene_data->prev_cam_projection) * Projection(p_render_data->scene_data->prev_cam_transform.affine_inverse());
			RendererRD::MaterialStorage::store_camera(prev_vp, rt_ubo.prev_vp_unjittered);

			Projection curr_vp = (correction * p_render_data->scene_data->cam_projection) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
			RendererRD::MaterialStorage::store_camera(curr_vp, rt_ubo.curr_vp_unjittered);
		}

		// --- Light gathering ---
		uint32_t rt_light_count = 0;
		LocalVector<RT_LightData> rt_light_data;
		rt_light_data.resize(RT_LIGHTS_MAX);
		memset(rt_light_data.ptr(), 0, RT_LIGHTS_MAX * sizeof(RT_LightData));

		rt_light_count = gather_lights(p_render_data, rt_light_data.ptr(), RT_LIGHTS_MAX);
		build_light_grid(p_state, rt_light_data.ptr(), rt_light_count);

		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_LIGHT_COUNT] = float(rt_light_count);
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_DECAL_COUNT] = float(gather_decals(p_state, p_render_data));

		// Upload light buffer.
		{
			uint32_t buf_size = RT_LIGHTS_MAX * sizeof(RT_LightData);
			if (!p_state->light_buffer.is_valid()) {
				p_state->light_buffer = RD::get_singleton()->storage_buffer_create(buf_size);
				RD::get_singleton()->set_resource_name(p_state->light_buffer, "RT Light Buffer");
			}
			RD::get_singleton()->buffer_update(p_state->light_buffer, 0, buf_size, rt_light_data.ptr());
		}

		if (!p_state->params_buffer.is_valid()) {
			p_state->params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(rt_ubo));
			RD::get_singleton()->set_resource_name(p_state->params_buffer, "RT Params Buffer");
		}
		RD::get_singleton()->buffer_update(p_state->params_buffer, 0, sizeof(rt_ubo), &rt_ubo);

		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(p_state->params_buffer);
		uniforms.push_back(u);
	}

	// Binding 7: Sky radiance octahedral map (for pathtracing sky sampling).
	{
		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

		// Fall back to a default whose type matches the shader's declared binding.
		if (!radiance_texture.is_valid()) {
			radiance_texture = texture_storage->texture_rd_get_default(use_octmap_array
							? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK
							: RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		}

		RD::Uniform u;
		u.binding = 7;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}

	// Binding 8: Sampler for radiance texture (linear filtering with mipmaps and clamp).
	{
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.append_id(RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
				RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
		uniforms.push_back(u);
	}

	// Bindings 9-12: DLSS Ray Reconstruction output buffers (only in DLSS RR shader variant).
	bool dlss_rr_enabled = dlss_rr_has_buffers(rb);
	if (dlss_rr_enabled) {
		// Binding 9: DLSS RR Diffuse Albedo
		{
			RD::Uniform u;
			u.binding = 9;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_diffuse_albedo(rb));
			uniforms.push_back(u);
		}

		// Binding 10: DLSS RR Specular Albedo
		{
			RD::Uniform u;
			u.binding = 10;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_specular_albedo(rb));
			uniforms.push_back(u);
		}

		// Binding 11: DLSS RR Normal + Roughness
		{
			RD::Uniform u;
			u.binding = 11;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_normal_roughness(rb));
			uniforms.push_back(u);
		}

		// Binding 12: DLSS RR Specular Hit Distance
		{
			RD::Uniform u;
			u.binding = 12;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_specular_hit_dist(rb));
			uniforms.push_back(u);
		}

		// Binding 39: DLSS RR Depth (the raygen writes it, see dlss_rr_get_depth()).
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 39, dlss_rr_get_depth(rb)));
	}

	// Binding 13: Light buffer (SSBO).
	{
		RD::Uniform u;
		u.binding = 13;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->light_buffer.is_valid()) {
			u.append_id(p_state->light_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 42: Light grid (build_light_grid()).
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 42, p_state->light_grid_buffer.is_valid() ? p_state->light_grid_buffer : RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer()));

	// Binding 14: Global shader uniforms SSBO.
	{
		RD::Uniform u;
		u.binding = 14;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID buf = RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer();
		if (buf.is_valid()) {
			u.append_id(buf);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 15: RT depth output (R32F storage image for writing depth from closest_hit/miss).
	{
		RD::Uniform u;
		u.binding = 15;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(rt_get_depth_texture(rb));
		uniforms.push_back(u);
	}

	// Bindings 16-27: Material samplers (12 filter/repeat combinations). The view's own, as raster's: they carry the mipmap
	// bias of its 3D scaling (DLSS) and the project's anisotropic filtering level.
	rb->get_samplers().append_uniforms(uniforms, 16);

	// Binding 29: Front layer reflection output (RGBA16F: reflected radiance, view depth of the transparent surface).
	{
		rt_ensure_textures(rb);
		RD::Uniform u;
		u.binding = 29;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(rt_get_front_layer_texture(rb));
		uniforms.push_back(u);
	}

	// Binding 28: Velocity output (RG16F). Past the 16-27 sampler range.
	{
		rb->ensure_velocity();
		RD::Uniform u;
		u.binding = 28;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(rb->get_velocity_buffer(false));
		uniforms.push_back(u);
	}

	// Bindings 33-37: the decals, their grid, the decal atlas (linear and sRGB views) and raster's decal sampler.
	{
		RID default_buffer = RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer();
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 33, p_state->decal_buffer.is_valid() ? p_state->decal_buffer : default_buffer));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 34, p_state->decal_grid_buffer.is_valid() ? p_state->decal_grid_buffer : default_buffer));

		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
		RID default_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RID decal_atlas = texture_storage->decal_atlas_get_texture();
		RID decal_atlas_srgb = texture_storage->decal_atlas_get_texture_srgb();
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 35, decal_atlas.is_valid() ? decal_atlas : default_texture));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 36, decal_atlas_srgb.is_valid() ? decal_atlas_srgb : default_texture));

		RSE::CanvasItemTextureFilter decal_filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS;
		switch (owner->decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST: {
				decal_filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST;
			} break;
			case RSE::DECAL_FILTER_LINEAR: {
				decal_filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR;
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS: {
				decal_filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS;
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS: {
				decal_filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS;
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				decal_filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC;
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				decal_filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC;
			} break;
		}
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 37, rb->get_samplers().get_sampler(decal_filter, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));

		// Binding 38: the directional light projector (gather_lights()), white when none.
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 38, rt_directional_projector.is_valid() ? rt_directional_projector : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE)));

		// Binding 40: the sky's sampling distribution, read only where RT_PARAM_ENV_SAMPLING says it was built.
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 40, p_state->sky_sampling_buffer.is_valid() ? p_state->sky_sampling_buffer : default_buffer));
	}

	RID shader_rd = shader ? shader->get_pipeline_shader_rd(p_rt_flags) : RID();

	RID result;
	if (shader_rd.is_valid()) {
		// Release the previous frame's scene uniform set now (before allocating
		// this frame's). Disposal is deferred by RenderingDevice until the frame
		// that used it has finished on the GPU, so this is safe and prevents the
		// uniform_set_owner pool from leaking one RID per accumulated frame.
		//
		// The set references render-buffer resources, so RenderingDevice may have
		// already auto-freed it via its dependency cascade (e.g. a texture/buffer
		// it referenced was recreated). RID::is_valid() only checks for non-null,
		// so we must query uniform_set_is_valid() to avoid a double free.
		if (p_state->scene_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_state->scene_uniform_set)) {
			RD::get_singleton()->free_rid(p_state->scene_uniform_set);
		}
		p_state->scene_uniform_set = RID();

		result = RD::get_singleton()->uniform_set_create(
				uniforms,
				shader_rd,
				RenderForwardClustered::SCENE_UNIFORM_SET,
				/*p_linear_pool=*/true);
		p_state->scene_uniform_set = result;

		// === SET 1: Bindless textures ===
		if (bindless_block && bindless_block->is_initialized()) {
			bindless_block->finalize(shader_rd, 1);
			bindless_uniform_set = bindless_block->get_uniform_set();
		}
		if (bindless_array_block && bindless_array_block->is_initialized()) {
			bindless_array_block->finalize(shader_rd, 2);
			bindless_array_uniform_set = bindless_array_block->get_uniform_set();
		}
	}

	return result;
}

// ---------------------------------------------------------------------------
// Trace-time buffer dependencies
// ---------------------------------------------------------------------------

void RenderRaytracing::register_raytracing_buffer_dependencies(RD::RaytracingListID p_list) {
	RD *rd = RD::get_singleton();
	thread_local LocalVector<RID> buffers;
	buffers.clear();
	get_buffers_read_by_address(buffers);
	for (const RID &buffer : buffers) {
		rd->raytracing_list_add_buffer_dependency(p_list, buffer, /*p_writable=*/false);
	}
}

void RenderRaytracing::get_buffers_read_by_address(LocalVector<RID> &r_buffers) {
	// The hit shader reads vertex / attribute / index data via BDA stored in
	// RT_GeometryData. The draw graph cannot infer those reads from the BDA, so
	// every per-frame-written buffer that the trace will touch via BDA must be
	// declared -- otherwise the producing copy/dispatch may not be visible
	// to the trace. (Static mesh VB/AB/IB are uploaded once via the transfer
	// worker and don't need per-frame barriers.)

	// Mat UBO pool buffer dependency.
	if (mat_ubo_pool_buffer.is_valid()) {
		r_buffers.push_back(mat_ubo_pool_buffer);
	}
	// Materials too large for a pool slot: their buffers can be created this frame, the upload of their data
	// must come before the trace.
	for (const RID &buffer : mat_ubo_dedicated_buffers) {
		r_buffers.push_back(buffer);
	}

	// Deformed surface buffer dependencies.
	for (uint32_t i = 0; i < deformed_active_this_frame.size(); i++) {
		RTDeformedCacheEntry *e = deformed_pool.get_or_null(deformed_active_this_frame[i]);
		if (!e) {
			continue;
		}
		if (e->owned_vb_full.is_valid()) {
			r_buffers.push_back(e->owned_vb_full);
		}
		if (e->prev_pos_vb.is_valid()) {
			r_buffers.push_back(e->prev_pos_vb);
		}
		if (e->displaced_vb.is_valid()) {
			r_buffers.push_back(e->displaced_vb);
		}
	}

	// Merged MultiMesh buffer dependencies.
	for (uint32_t i = 0; i < merged_mm_active_this_frame.size(); i++) {
		RTMergedMMEntry *e = merged_mm_pool.get_or_null(merged_mm_active_this_frame[i]);
		if (!e) {
			continue;
		}
		if (e->merged_vtx_buffer.is_valid()) {
			r_buffers.push_back(e->merged_vtx_buffer);
		}
		if (e->merged_attr_buffer.is_valid()) {
			r_buffers.push_back(e->merged_attr_buffer);
		}
		if (e->replicated_idx_buffer.is_valid()) {
			r_buffers.push_back(e->replicated_idx_buffer);
		}
		if (e->displaced && e->merged_prev_buffer.is_valid()) {
			r_buffers.push_back(e->merged_prev_buffer);
		}
	}
	// MultiMesh buffers, written by every change of their instances.
	for (const RID &buffer : multimesh_buffers_read_this_frame) {
		r_buffers.push_back(buffer);
	}
}

// ---------------------------------------------------------------------------
// Output copy
// ---------------------------------------------------------------------------

void RenderRaytracing::apply_volumetric_fog(const RenderDataRD *p_render_data, bool p_legacy_blending) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	if (!rb->has_custom_data(RB_SCOPE_FOG) || !rt_has_texture(rb.ptr()) || !rt_has_depth_texture(rb.ptr())) {
		return;
	}
	Ref<RendererRD::Fog::VolumetricFog> fog = rb->get_custom_data(RB_SCOPE_FOG);

	// The same parameters as the scene shader's (RenderForwardClustered::_setup_environment()).
	VolumetricFogShader::PushConstant push_constant = {};
	RendererRD::MaterialStorage::store_camera(p_render_data->scene_data->get_cam_projection().inverse(), push_constant.inv_projection);
	const Size2i size = rb->get_internal_size();
	push_constant.size[0] = size.width;
	push_constant.size[1] = size.height;
	push_constant.inv_length = fog->length > 0.0 ? 1.0 / fog->length : 1.0;
	push_constant.detail_spread = fog->spread > 0.0 ? 1.0 / fog->spread : 1.0;
	push_constant.legacy_blending = p_legacy_blending;

	RD::Uniform color(RD::UNIFORM_TYPE_IMAGE, 0, rt_get_texture(rb.ptr()));
	RD::Uniform depth(RD::UNIFORM_TYPE_IMAGE, 1, rt_get_depth_texture(rb.ptr()));
	RD::Uniform fog_map(RD::UNIFORM_TYPE_TEXTURE, 2, fog->fog_map);
	RD::Uniform sampler(RD::UNIFORM_TYPE_SAMPLER, 3, RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));

	RD *rd = RD::get_singleton();
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, volumetric_fog_shader.pipeline);
	rd->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache(volumetric_fog_shader.version_shader, 0, color, depth, fog_map, sampler), 0);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(VolumetricFogShader::PushConstant));
	rd->compute_list_dispatch_threads(compute_list, size.width, size.height, 1);
	rd->compute_list_end();
}

void RenderRaytracing::dlss_rr_write_background_albedo(const RenderDataRD *p_render_data) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	if (!dlss_rr_has_buffers(rb.ptr()) || !rt_has_depth_texture(rb.ptr())) {
		return;
	}

	SkyGuideShader::PushConstant push_constant = {};
	const Size2i size = rb->get_internal_size();
	push_constant.size[0] = size.width;
	push_constant.size[1] = size.height;

	RD::Uniform color(RD::UNIFORM_TYPE_TEXTURE, 0, rb->get_internal_texture());
	RD::Uniform depth(RD::UNIFORM_TYPE_IMAGE, 1, rt_get_depth_texture(rb.ptr()));
	RD::Uniform albedo(RD::UNIFORM_TYPE_IMAGE, 2, dlss_rr_get_diffuse_albedo(rb.ptr()));

	RD *rd = RD::get_singleton();
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, sky_guide_shader.pipeline);
	rd->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache(sky_guide_shader.version_shader, 0, color, depth, albedo), 0);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(SkyGuideShader::PushConstant));
	rd->compute_list_dispatch_threads(compute_list, size.width, size.height, 1);
	rd->compute_list_end();
}

void RenderRaytracing::copy_output_texture(const RenderDataRD *p_render_data, bool p_keep_background) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	if (!rt_has_texture(rb.ptr()) || !rt_has_depth_texture(rb.ptr())) {
		return;
	}

	OutputCopyShader::PushConstant push_constant = {};
	const Size2i size = rb->get_internal_size();
	push_constant.size[0] = size.width;
	push_constant.size[1] = size.height;
	push_constant.keep_background = p_keep_background ? 1 : 0;

	RD::Uniform source(RD::UNIFORM_TYPE_TEXTURE, 0, rt_get_texture(rb.ptr()));
	RD::Uniform depth(RD::UNIFORM_TYPE_IMAGE, 1, rt_get_depth_texture(rb.ptr()));

	RD *rd = RD::get_singleton();
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, output_copy_shader.pipeline);
	for (uint32_t v = 0; v < rb->get_view_count(); v++) {
		RD::Uniform dest(RD::UNIFORM_TYPE_IMAGE, 2, rb->get_internal_texture(v));
		rd->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache(output_copy_shader.version_shader, 0, source, depth, dest), 0);
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(OutputCopyShader::PushConstant));
		rd->compute_list_dispatch_threads(compute_list, size.width, size.height, 1);
	}
	rd->compute_list_end();
}
