/**************************************************************************/
/*  render_raytracing.h                                                   */
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

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/bindless_block.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/multimesh_merge.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_lod_compact.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_output_copy.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_sky_guide.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_sky_sampling.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_tlas_expand.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_volumetric_fog.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

// The path tracer's own textures, in a scope of their own so a view that stops path tracing frees them.
#define RB_SCOPE_RAYTRACING SNAME("raytracing")
#define RB_TEX_RAYTRACING SNAME("raytracing")
#define RB_TEX_RT_DEPTH SNAME("rt_depth")
#define RB_TEX_RT_FRONT_LAYER SNAME("rt_front_layer")

#define RB_SCOPE_DLSS_RR SNAME("dlss_rr")
#define RB_TEX_DLSS_RR_DIFFUSE_ALBEDO SNAME("diffuse_albedo")
#define RB_TEX_DLSS_RR_SPECULAR_ALBEDO SNAME("specular_albedo")
#define RB_TEX_DLSS_RR_NORMAL_ROUGHNESS SNAME("normal_roughness")
#define RB_TEX_DLSS_RR_SPECULAR_HIT_DIST SNAME("specular_hit_dist")
#define RB_TEX_DLSS_RR_COLOR_BEFORE_TRANSPARENCY SNAME("color_before_transparency")
#define RB_TEX_DLSS_RR_DEPTH SNAME("rr_depth")

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererSceneRenderImplementation {

class RenderForwardClustered;
class SceneShaderRaytracing;

// Must match GLSL GeometryData (std430, 176 bytes).
struct alignas(16) RT_GeometryData {
	uint64_t vertex_buffer_address;
	uint64_t attribute_buffer_address;
	uint64_t index_buffer_address;
	uint32_t vertex_count;
	uint32_t position_stride;
	uint32_t normal_byte_offset;
	uint32_t normal_stride;
	uint32_t tangent_byte_offset;
	uint32_t tangent_stride;
	uint32_t attribute_stride;
	uint32_t uv_byte_offset;
	uint32_t uv_scale_packed;
	uint32_t index_format;
	uint32_t primitive_count;
	uint32_t flags;
	float aabb_size_x;
	float aabb_size_y;
	float aabb_size_z;
	uint32_t color_byte_offset;
	float aabb_pos_x;
	float aabb_pos_y;
	float aabb_pos_z;
	// For deformed geometry: previous-frame position buffer used for motion vectors.
	uint32_t prev_vertex_buffer_address_lo;
	uint32_t prev_vertex_buffer_address_hi;
	// For displaced geometry: positions before vertex() moved them (float3 packed).
	uint32_t rest_vertex_buffer_address_lo;
	uint32_t rest_vertex_buffer_address_hi;
	uint32_t layer_mask; // The instance's VisualInstance3D layers, against each light's cull mask.
	uint32_t uv2_byte_offset;
	uint32_t uv2_scale_packed; // fp16 x, fp16 y
	// CUSTOM0-3: byte offset in an attribute (8 bits each, CUSTOM0 lowest), and 4 bits each of RT_CUSTOM_FORMAT_PRESENT
	// | RSE::ArrayCustomFormat. 0: none, the value of a zeroed RT_GeometryData.
	uint32_t custom_byte_offsets;
	uint32_t custom_formats;
	// The MultiMesh buffer whose instances this geometry draws, for COLOR and INSTANCE_CUSTOM (0: not a MultiMesh).
	uint32_t multimesh_address_lo;
	uint32_t multimesh_address_hi;
	uint32_t multimesh_stride; // In floats.
	uint32_t multimesh_offset; // The geometry's first instance in the buffer.
	uint32_t multimesh_vertex_count; // A merged BLAS's vertices per instance; UINT32_MAX: one instance, at multimesh_offset.
	uint32_t multimesh_data_offsets; // In floats in an instance: the color (low 16 bits), the custom data (high), 0xFFFF: none.
	// A merged BLAS's first instance in the buffer's previous transforms (motion vectors); multimesh_offset when its instances
	// did not move, and for one instance, whose motion is the TLAS instance's.
	uint32_t multimesh_previous_offset;
	uint32_t multimesh_pad[3];
};
static_assert(sizeof(RT_GeometryData) == 176, "RT_GeometryData must be 176 bytes for std430");

static constexpr uint32_t RT_CUSTOM_FORMAT_PRESENT = 8u;

/// Per-instance motion data for velocity computation (matches GLSL InstanceMotionData, 48 bytes).
struct RT_InstanceMotionData {
	float prev_object_to_world[12]; // Previous object-to-world (mat3x4, transposed 3x4).
};
static_assert(sizeof(RT_InstanceMotionData) == 48, "RT_InstanceMotionData must be 48 bytes");

// Must match GLSL MaterialData (std430, 144 bytes).
struct alignas(16) RT_MaterialData {
	uint32_t albedo_texture_idx;
	uint32_t normal_texture_idx;
	uint32_t orm_texture_idx;
	uint32_t emission_texture_idx;
	float albedo_color[4];
	float emission_color[3];
	float emission_strength;
	float metallic;
	float roughness;
	float ao_strength;
	uint32_t flags;
	float uv1_scale[2];
	float uv1_offset[2];
	float normal_map_depth; // Strength [0..N], default 1.0 (not Z-depth).
	float specular; // Dielectric specular [0..1], default 0.5 -> F0 = 0.04.
	uint64_t uniform_address; // BDA for custom shader uniform buffer (0 = none).
	float uv1_scale_z; // Triplanar only.
	float uv1_offset_z;
	float uv1_blend_sharpness;
	uint32_t instance_uniforms_ofs; // The instance's `instance uniform` values in the global shader uniform buffer.
	float backlight[3]; // BaseMaterial3D's backlight color, linear (RT_MAT_FLAG_BACKLIGHT).
	uint32_t backlight_texture_idx; // Added to it (RT_MAT_FLAG_HAS_BACKLIGHT_TEX).
	uint32_t ao_texture_idx; // BaseMaterial3D's ambient occlusion texture (RT_MAT_FLAG_AO_TEXTURE).
	uint32_t ao_channel; // Its channel, 0-3 (ao_texture_channel).
	float ao_light_affect; // With RT_MAT_FLAG_AO_TEXTURE or RT_MAT_FLAG_AO_ORM.
	uint32_t ao_pad;
};
static_assert(sizeof(RT_MaterialData) == 144, "RT_MaterialData must be 144 bytes for std430");

// A MultiMesh traced as one TLAS instance per instance, the instances written on the GPU (raytracing_tlas_expand.glsl
// ExpandJob, std430).
struct RT_TlasExpandJob {
	float node[12]; // Rows of the MultiMesh node's transform.
	float prev_node[12]; // In the previous frame.
	float aabb[12]; // A compressed mesh's AABB transform, identity otherwise.
	uint32_t instance_tail[4]; // Words 12-15 of the TLAS instance, as the driver writes them.
	uint32_t mm_address_lo;
	uint32_t mm_address_hi;
	uint32_t mm_stride;
	uint32_t mm_offset;
	uint32_t mm_prev_offset;
	uint32_t first_slot;
	uint32_t first_motion;
	uint32_t template_index;
};
static_assert(sizeof(RT_TlasExpandJob) == 192, "RT_TlasExpandJob must be 192 bytes for std430");

// Light types for raytracing (matches GLSL RT_LIGHT_TYPE_* defines).
enum RTLightType : uint32_t {
	RT_LIGHT_TYPE_OMNI = 0,
	RT_LIGHT_TYPE_DIRECTIONAL = 1,
	RT_LIGHT_TYPE_SPOT = 3,
};

// Must match GLSL RTLightData (std430, 128 bytes).
struct alignas(16) RT_LightData {
	float position[3]; // World position (omni/spot) or direction (directional, normalized).
	uint32_t type;
	float emission[3];
	float radius;
	float attenuation;
	float inv_max_range; // 1.0/range, or -1.0 for infinite.
	float max_range_squared; // range*range, or 0.0 for infinite.
	float specular_amount;
	float indirect_energy;
	float inv_spot_attenuation;
	float cos_spot_angle;
	uint32_t cull_mask; // Light3D.light_cull_mask: the layers this light lights.
	float spot_direction[3];
	float shadow_opacity; // 0 without shadows: no shadow ray, as raster.
	// Directional: world position (w = 1) to the UV of rt_directional_light_projector, when projector_enabled (as raster's
	// DirectionalLightData, in world space).
	float projector_uv_x[4];
	float projector_uv_y[4];
	uint32_t projector_enabled;
	uint32_t pad[3];
};
static_assert(sizeof(RT_LightData) == 128, "RT_LightData must be 128 bytes for std430");

// Must match GLSL RTDecalData (raytracing_decals_inc.glsl, std430, 224 bytes). What raster's DecalData holds in view space
// (TextureStorage::update_decal_buffer()), in world space.
struct alignas(16) RT_DecalData {
	float xform[12]; // World to decal space, rows: x and z in [0, 1], y in [-1, 1] inside its box.
	float albedo_rect[4]; // In the decal atlas; zero: no texture.
	float normal_rect[4];
	float orm_rect[4];
	float emission_rect[4];
	float modulate[4]; // Linear, its alpha times the distance fade.
	float emission_energy; // Times the distance fade.
	uint32_t mask; // Decal.cull_mask, against the instance's layers.
	float upper_fade;
	float lower_fade;
	float normal_xform[12]; // The decal's orthonormalized basis, rows: its normal map to world space.
	float normal[3]; // Its Y axis, world space, for the normal fade.
	float normal_fade;
	float albedo_mix;
	uint32_t pad[3];
};
static_assert(sizeof(RT_DecalData) == 224, "RT_DecalData must be 224 bytes for std430");

enum {
	// Decals around the camera the hit shaders see, the nearest ones (raster's max_clustered_elements limits the frustum's).
	RT_DECALS_MAX = 4096,
	// A decal over more grid cells than this is tested at every hit instead (gather_decals()).
	RT_DECAL_GRID_MAX_CELLS = 128,
	RT_DECAL_GRID_MAX_BUCKETS = 1 << 16,
	RT_DECAL_GRID_HEADER = 4,
};

enum {
	// The lights the path tracer gets, the strongest first (gather_lights()); a hit weighs only those of its light grid
	// cell (build_light_grid()), so their number costs little. Mirrored as RT_LIGHTS_MAX in raytracing_lights_inc.glsl.
	RT_LIGHTS_MAX = 256,
	// A light over more light grid cells than this is weighed at every hit instead (build_light_grid()).
	RT_LIGHT_GRID_MAX_CELLS = 512,
	RT_LIGHT_GRID_MAX_BUCKETS = 1 << 16,
	RT_LIGHT_GRID_HEADER = 4,
};

enum {
	RT_OFFSET_NONE = 0xFFFFFFFFu,
	RT_CACHE_CHUNK_SIZE = 256,
	RT_CACHE_CHUNK_SHIFT = 8,
	RT_CACHE_CHUNK_MASK = 255,
};

// TLAS instance masks: what the camera and bounce rays see, and what blocks the shadow rays, as GeometryInstance3D.cast_shadow.
enum : uint8_t {
	RT_INSTANCE_MASK_CAMERA = 1u,
	RT_INSTANCE_MASK_SHADOW = 2u,
	RT_INSTANCE_MASK_FRONT_LAYER = 4u, // Transparent surfaces, for their reflections only (RenderForwardClustered::front_layer_reflection).
	// Procedural geometry (RTProceduralInstance3D) also carries these next to SHADOW and CAMERA: a ray query cannot run
	// its intersection shader, so the ray query loops trace them apart with traceRay (RT_PARAM_PROCEDURAL_MASKS).
	RT_INSTANCE_MASK_PROCEDURAL_SHADOW = 8u,
	RT_INSTANCE_MASK_PROCEDURAL_CAMERA = 0x10u,
	// In place of CAMERA for a surface the raster pass draws over the traced image (render_mode pathtracing_rasterized):
	// every ray but the camera's sees it (RT_MASK_BOUNCE).
	RT_INSTANCE_MASK_SECONDARY = 0x20u,
	RT_INSTANCE_MASK_DOUBLE_SIDED_SHADOW = 0x80u, // Build-time only: the shadow rays need both faces, see build_tlas().
};

// Material flags for RT (matches GLSL mat_flags bit layout).
enum {
	RT_MAT_FLAG_HAS_NORMAL_MAP = 1u,
	RT_MAT_FLAG_HAS_EMISSION_TEX = 2u,
	RT_MAT_FLAG_POINT_FILTER = 4u,
	RT_MAT_FLAG_UV1_TRIPLANAR = 8u,
	RT_MAT_FLAG_UV1_WORLD_TRIPLANAR = 16u,
	RT_MAT_FLAG_EMISSION = 32u, // BaseMaterial3D's emission is enabled.
	RT_MAT_FLAG_EMISSION_ADD = 64u, // Emission operator: add (else multiply).
	RT_MAT_FLAG_UNSHADED = 128u,
	RT_MAT_FLAG_VERTEX_COLOR_ALBEDO = 256u,
	RT_MAT_FLAG_VERTEX_COLOR_SRGB = 512u,
	RT_MAT_FLAG_SPECULAR_DISABLED = 1024u, // No specular from the lights (render_mode specular_disabled).
	RT_MAT_FLAG_BACKLIGHT = 2048u,
	RT_MAT_FLAG_HAS_BACKLIGHT_TEX = 4096u,
	RT_MAT_FLAG_ANISOTROPIC_FILTER = 8192u, // texture_filter LINEAR_WITH_MIPMAPS_ANISOTROPIC.
	RT_MAT_FLAG_AO_TEXTURE = 16384u, // Ambient occlusion from its texture, on UV1.
	RT_MAT_FLAG_AO_ORM = 32768u, // Ambient occlusion from the ORM texture's red channel.
};

// Index format for RT geometry (matches GLSL fetch_indices).
enum {
	RT_INDEX_FORMAT_UINT16 = 0,
	RT_INDEX_FORMAT_UINT32 = 1,
	RT_INDEX_FORMAT_NONE = 2,
};

enum {
	RT_GEOM_FLAG_COMPRESSED = 1u,
	RT_GEOM_FLAG_PROCEDURAL = 2u,
	// Set when the BLAS uses a per-frame-deformed vertex buffer.
	RT_GEOM_FLAG_DEFORMED = 4u,
	// Deformed by the material's vertex(): the rest positions are kept for the hit shaders.
	RT_GEOM_FLAG_DISPLACED = 8u,
	// Uncompressed positions over a compressed attribute buffer (deformed copies of compressed meshes).
	RT_GEOM_FLAG_COMPRESSED_ATTRIBUTES = 16u,
};

// Must match GLSL DisplaceJob (raytracing_displace.glsl, std430).
struct alignas(16) RT_DisplaceJob {
	float model_matrix[16];
	float inv_model_matrix[16];
	float prev_model_matrix[16];
	float prev_inv_model_matrix[16];
	RT_GeometryData geometry; // The mesh surface as stored.
	uint64_t material_address;
	uint32_t tbn_words;
	uint32_t instance_uniforms_ofs;
	// A merged MultiMesh (_displace_merged_multimeshes()), when mm_instance_count is not 0: one thread per vertex of each
	// instance, MODEL_MATRIX = model_matrix * the instance's transform.
	uint32_t mm_instance_count;
	uint32_t mm_vertex_count; // Per instance, in the merged buffers.
	uint32_t mm_stride; // Of the MultiMesh buffer, in floats.
	uint32_t mm_offset; // Instances to skip in it (its motion vectors double buffer).
	uint32_t mm_use_remap; // 1: the merged vertices are a LOD view's (binding 3 maps them to the mesh's).
	uint32_t mm_color_offset; // In floats in an instance, or UINT32_MAX without colors.
	uint32_t mm_custom_offset; // Same for the custom data.
	// 1: the BLAS is refit with the output, which keeps the activity of each vertex of its last build (bindings 5 and 6).
	uint32_t refit_guard;
};
static_assert(sizeof(RT_DisplaceJob) == 480, "RT_DisplaceJob must be 480 bytes for std430");

// Must match GLSL OmmBakeJob (raytracing_omm_bake.glsl, std430).
struct alignas(16) RT_OmmBakeJob {
	RT_GeometryData geometry; // The mesh surface as stored.
	uint64_t material_address; // Its uniforms (custom shaders).
	uint32_t triangle_count;
	uint32_t level;
	uint32_t data_offset; // In words.
	uint32_t desc_offset; // In words.
	// Hit group 0 (BaseMaterial3D).
	uint32_t albedo_texture_idx;
	float albedo_alpha;
	float uv1_scale[2];
	float uv1_offset[2];
	uint32_t use_vertex_color;
	uint32_t max_samples; // Evaluations per micro-triangle edge, beyond which it stays unknown.
	uint32_t pad[2];
};
static_assert(sizeof(RT_OmmBakeJob) == 240, "RT_OmmBakeJob must be 240 bytes for std430");

// Opacity micromap bakes (raytracing_omm_bake.glsl). Every texel under a micro-triangle is examined, as NVIDIA's OMM SDK
// bakes; a micro-triangle is left unknown only when proven mixed, or when its footprint exceeds RT_OMM_MAX_SAMPLES per
// edge, a bound on one thread's work (~0.5 M evaluations), not on the classification.
static constexpr uint32_t RT_OMM_MAX_SAMPLES = 1024;
// The estimate pass counts the bake's evaluations in units of RT_OMM_COST_UNIT (raytracing_omm_bake.glsl's copy).
static constexpr uint32_t RT_OMM_COST_UNIT = 1024;

// A mesh surface's cut-out baked with a material (raytracing_omm_bake.glsl): for its static BLAS built again with it, for
// the BLASes of its deformed copies (skinned, displaced), and for the merged MultiMesh BLASes of a LOD (its triangles).
struct RTOmmKey {
	RID mesh;
	uint32_t surface_index = 0;
	uint32_t surface_counter = 0; // The surface's data changed.
	RID material;
	uint16_t material_counter = 0;
	uint32_t lod = 0; // The LOD's triangles, in its index order.

	bool operator==(const RTOmmKey &p_other) const {
		return mesh == p_other.mesh && surface_index == p_other.surface_index && surface_counter == p_other.surface_counter &&
				material == p_other.material && material_counter == p_other.material_counter && lod == p_other.lod;
	}
	static uint32_t hash(const RTOmmKey &p_key) {
		uint32_t h = hash_murmur3_one_64(p_key.mesh.get_id());
		h = hash_murmur3_one_32(p_key.surface_index, h);
		h = hash_murmur3_one_32(p_key.surface_counter, h);
		h = hash_murmur3_one_32(p_key.lod, h);
		h = hash_murmur3_one_64(p_key.material.get_id(), h);
		return hash_fmix32(hash_murmur3_one_32(p_key.material_counter, h));
	}
};

/// Per-instance state for procedural RT geometry. Heap-allocated, only exists for procedural instances.
struct RTProceduralState {
	AABB culling_aabb;
	PackedFloat32Array aabb_data; // N * 6 floats (min/max per AABB). Empty = single AABB.
	bool expose_bounds = false;
	bool dirty = true;
	RID blas;
	RID gpu_buffer;
	uint32_t gpu_buffer_capacity = 0; // Bytes, grow-only.
	uint64_t gpu_buffer_address = 0; // BDA (0 = not exposed).
	uint32_t aabb_count = 0;
};

struct RTSurfaceData {
	RID blas;
	RT_GeometryData geometry = {};
	Transform3D aabb_transform;
	bool is_compressed = false;
	uint64_t blas_size = 0;
};

struct RTOmmEntry {
	RID micromap; // Set once the bake is done and the micromap built: until then the surface is traced without it.
	RID data_buffer; // The micro-triangles' states.
	RID desc_buffer; // The triangles' descriptions.
	uint32_t level = 0; // Subdivision level of its entries, one per triangle.
	RTSurfaceData surf_data; // The surface's static BLAS built with the micromap, once a static instance asks for it.
	uint32_t first_seen_frame = 0; // Baked once the surface lasts: a mesh replaced every frame (sprites) is not.
	uint32_t last_used_frame = 0;
	bool failed = false;
	bool superseded = false; // Its surface or material changed since: no key will ask for it again.

	// The bake, spread over frames within a GPU budget (_omm_dispatch_bakes()): its cost estimated per triangle, then the
	// micro-triangles baked by ranges, the micromap built after the last one.
	enum BakeState : uint8_t {
		BAKE_NONE, // Not started, or done.
		BAKE_ESTIMATE, // The estimate pass is to be dispatched.
		BAKE_ESTIMATING, // Its result is on its way back (buffer_get_data_async()).
		BAKE_RANGES, // Micro-triangles from next_micro on (all the triangles' in a row) are to be baked.
	};
	BakeState bake_state = BAKE_NONE;
	RID pending_micromap; // The micromap being baked, the entry's once built.
	RID cost_buffer; // Per triangle, the estimated evaluations of its bake (RT_OMM_COST_UNIT each).
	LocalVector<uint32_t> triangle_cost;
	uint64_t remaining_cost = 0; // Estimated evaluations left: the queue serves the least first.
	uint32_t next_micro = 0;
	uint64_t bake_serial = 0; // Matches the estimate on its way back.
	// The bake's inputs, taken again from the live surface and material every frame a caller asks for the entry
	// (refreshed_frame): only then is a range baked, so no buffer address is older than the frame.
	RT_OmmBakeJob job = {};
	RID bake_shader;
	RID bake_pipeline;
	RID material_buffer;
	uint32_t data_size = 0;
	uint32_t refreshed_frame = UINT32_MAX;
};

/// Inputs for a surface backed by a per-frame-deformed vertex buffer.
struct RTDeformedGeometrySource {
	RID current_vb; ///< Vertex buffer (object-space positions) the BLAS is built / refit against.
	RID prev_vb; ///< Previous-frame positions for motion vectors. Optional.
	uint64_t change_stamp = 0; ///< Changes when `current_vb` contents change.
	uint64_t cache_key = 0; ///< Caller-defined cache identity.
	uint32_t cache_version = 0; ///< Changes when the resource behind `cache_key` is recycled.
	uint32_t surface_counter = 0; ///< Changes when the underlying mesh surface changes.
	bool force_rebuild = false; ///< Rebuild instead of refitting when the data changed.
	uint32_t prev_offset = UINT32_MAX; ///< When set and the data changed: previous positions read from `prev_vb` there.
	bool defer_update = false; ///< A change of `current_vb` waits for a later frame (out of view and far): the BLAS keeps the positions it has.
};

// BaseMaterial3D's billboard modes, which place the instance in vertex().
enum RTBillboardMode : uint8_t {
	RT_BILLBOARD_DISABLED,
	RT_BILLBOARD_ENABLED,
	RT_BILLBOARD_FIXED_Y,
};

struct RTMaterialData {
	alignas(16) RT_MaterialData data = {};
	RTBillboardMode billboard_mode = RT_BILLBOARD_DISABLED;
	bool billboard_keep_scale = false;
	uint32_t global_buffer_index = UINT32_MAX;
	uint32_t rt_sbt_offset = 0;
	bool is_custom_shader = false;
	bool shader_failed = false; // A custom shader no hit group can run (SceneShaderRaytracing::register_custom_shader()).
	RID uniform_buffer; // Buffer pointer for mats > 512 bytes.
	uint32_t uniform_buffer_size = 0; // Its size: a rebuild of the same size updates it in place.
	uint32_t uniform_pool_slot = UINT32_MAX; // Index into the material UBO pool, or UINT32_MAX (unused)
	RID albedo_texture_rd;
	RID normal_texture_rd;
	RID orm_texture_rd;
	RID emission_texture_rd;
};

struct RTCacheEntry {
	RTSurfaceData *ptr = nullptr;
	uint32_t last_used_frame = 0;
	uint32_t cached_counter = 0;
	uint32_t cached_rid_version = 0;
	uint8_t failed_attempts = 0;
	uint64_t size_bytes = 0;
};

/// BLAS cache entry for a surface driven by a per-frame-deformed vertex buffer.
///
/// The skinned vertex buffer is owned by the engine's MeshInstance system and may
/// be freed/reallocated outside our control. To decouple BLAS lifetime from that
/// (and keep last-frame positions alive for motion vectors) we copy the skinned VB
/// into our own buffers each frame:
///   * owned_vb_full mirrors the skinned VB layout (positions + N + T) and is what
///     the BLAS and the hit shader read from this frame.
///   * prev_pos_vb stores the previous-frame positions only (float3 packed) for the
///     motion-vector path. Updated from owned_vb_full's position section each frame
///     before owned_vb_full is overwritten with the new skinned data.
struct RTDeformedCacheEntry {
	RTSurfaceData *ptr = nullptr;
	uint32_t last_used_frame = 0;
	uint64_t cached_change_stamp = 0;
	uint32_t cached_key_version = 0;
	uint32_t cached_surface_counter = 0;
	uint64_t cached_buffer_id = 0; // RID id of the deformed vertex buffer at the time of build.
	bool blas_built_once = false; // True once the BLAS has been fully built; subsequent ticks can refit.

	RID owned_vb_full;
	uint32_t owned_vb_full_capacity = 0; // Bytes; grow-only.
	RID prev_pos_vb;
	uint32_t prev_pos_vb_capacity = 0; // Bytes; grow-only.
	uint32_t cached_vertex_count = 0;
	uint32_t cached_full_size = 0;
	bool prev_pos_seeded = false; // False until the first owned->prev copy has run.

	// Displacement by the material's vertex() (_displace_surfaces()).
	RID displaced_vb; // [displaced float3 positions][rest TBN block][rest float3 positions][previous frame's displaced float3 positions].
	uint32_t displaced_vb_capacity = 0; // Bytes; grow-only.
	uint32_t displaced_frame = 0; // Last frame the surface was displaced; 0 = never.
	uint64_t displaced_stamp = 0; // Bumped on each evaluation.
	bool displaced_rebuild = false; // Evaluated this frame and the BLAS is to be built again, not refit (_displace_surfaces()).
	uint32_t displaced_refits = 0; // Since the last rebuild.
	bool displaced_guard_rebuild = false; // A refit kept vertices vertex() turned on or off (raytracing_displace.glsl).
	uint32_t displaced_slot = 0;
	uint32_t displaced_prev_offset = 0; // Bytes to the previous frame's positions in displaced_vb.
	uint16_t displaced_material_counter = 0;
	uint32_t displaced_instance_uniforms_hash = 0; // The instance uniforms vertex() was evaluated with.
	bool displaced_deferred = false; // A change waits for the surface's next update frame (far from the camera).
	Transform3D displaced_transform;

	// The surface's cut-out baked into an opacity micromap (build_tlas() sets it, _omm_entry()), and the one the BLAS
	// was built with: a change builds the BLAS again.
	RID omm_micromap;
	uint32_t omm_level = 0;
	RID built_omm_micromap;
};

/// Cache entry for a per-(MultiMesh, surface) merged BLAS.
/// All vertex data (positions, normals, tangents, UVs, colors) is fully baked per-instance
/// so the hit shader uses the standard code path — no special per-instance lookups.
/// The buffers and the BLAS are sized for cap_vertices / cap_indices, more than the N*V / N*I in use: a change of
/// instance count or LOD within them rebuilds the BLAS in place. The unused vertices are NaN, which makes the triangles
/// using them inactive, and the unused indices point to the last vertex, always unused.
struct RTMergedMMEntry {
	uint32_t cap_vertices = 0;
	uint32_t cap_indices = 0; // 0 if non-indexed.
	uint32_t oversized_since_frame = 0; // The frame from which the capacity has been more than twice the need, or 0.
	uint64_t buffer_bytes = 0; // Of the merged vertex, attribute and index buffers.

	// Merged vertex buffer: [float3 pos × cap_vertices] + [packed TBN × cap_vertices] (if mesh has normals).
	// The BLAS reads only the position section; the hit shader reads TBN via normal_byte_offset.
	RID merged_vtx_buffer;

	// Merged attribute buffer: [UV + color × cap_vertices] replicated per instance.
	RID merged_attr_buffer;

	// Replicated index buffer: uint32 × cap_indices (invalid if non-indexed).
	RID replicated_idx_buffer;

	// The merge pass's uniform set, kept while it binds the same buffers: a MultiMesh whose buffer is set every frame is
	// merged again every frame, and one set per merge and frame (Solfarer: ~30,000 a second) filled the D3D12 heap.
	RID merge_uniform_set;
	RID merge_set_buffers[7];
	uint32_t merge_set_mode = UINT32_MAX;

	RID blas;
	uint32_t last_mm_count = 0;
	uint32_t last_surface_counter = 0;
	uint32_t last_lod = 0;
	uint32_t last_used_frame = 0;
	uint64_t cached_mm_last_change = 0;
	bool blas_built_once = false;
	// Displaced by the material's vertex() near the camera: the positions it wrote are the BLAS's, and the previous
	// frame's give the motion vectors.
	bool displaced = false;
	bool displaced_this_frame = false; // Its positions moved this frame: the previous ones are valid.
	uint32_t displaced_inputs = 0; // Hash of the material's parameters and the instance uniforms vertex() was evaluated with.
	RID merged_prev_buffer; // float3 per merged vertex, the previous frame's displaced positions.
	bool indexed = false; // selects MODE_INDEXED vs MODE_NON_INDEXED variant
	uint32_t last_tbn_stride = 0; // The strides the buffers were sized with.
	uint32_t last_attrib_stride = 0;

	// The instance's cut-out (_omm_entry() of the surface, or of its LOD) for every instance: merged triangle i references
	// entry i mod omm_index_period. Covers the capacity's triangles (the unused ones are inactive anyway). The BLAS was
	// built with built_omm_micromap: another micromap, or none, builds it again.
	RID omm_index_buffer;
	uint32_t omm_index_triangles = 0;
	uint32_t omm_index_period = 0;
	bool omm_index_16bit = false;
	RID built_omm_micromap;
	uint32_t built_omm_level = 0;
};

/// A mesh LOD reduced to the vertices its indices use, for the merged MultiMesh BLASes: a LOD keeps the base
/// vertex buffer, so replicating all of it would cost the merge as much memory as LOD 0. Built on the GPU, so the
/// number of kept vertices is not known here: vertex_count is its bound, min(vertices, indices), and the slots past
/// the kept ones map to vertex 0, which no index reaches.
struct RTLodView {
	RID vertex_remap_buffer; // uint32 per kept vertex: its index in the base vertex buffer.
	RID index_buffer; // uint32 indices into the kept vertices.
	uint32_t vertex_count = 0;
	uint32_t index_count = 0;
	uint32_t surface_counter = 0;
	uint32_t last_used_frame = 0;
};

struct RTLodViewKey {
	const void *surface = nullptr;
	uint32_t lod = 0;

	bool operator==(const RTLodViewKey &p_other) const { return surface == p_other.surface && lod == p_other.lod; }
	uint32_t hash() const { return hash_murmur3_one_32(lod, hash_murmur3_one_64(uint64_t(surface))); }
};

struct RTMaterialCacheEntry {
	RTMaterialData *ptr = nullptr;
	uint32_t last_used_frame = 0;
	uint16_t cached_counter = 0;
	uint32_t cached_rid_version = 0;
};

/// Per-viewport raytracing state.
///
/// Each viewport has its own visibility set (frustum/LOD/visibility ranges), so
/// the TLAS instance composition and per-instance SSBO contents differ across
/// viewports. Sharing them caused the wrong `gl_InstanceCustomIndexEXT` to
/// resolve to the wrong `geometries[]` / `materials[]` entries, dereferencing
/// stale BDAs and faulting the GPU.
///
/// Lifetime is tied to a `RenderSceneBuffersRD`: created lazily on first
/// `build_tlas` for that viewport, freed via `RenderRaytracing::free_viewport_state`
/// from `RenderBufferDataForwardClustered::free_data()`.
struct RTViewportState {
	RID tlas;
	uint32_t tlas_max_instances = 0;

	RID geometry_buffer;
	uint32_t geometry_buffer_capacity = 0;
	RID material_buffer;
	uint32_t material_buffer_capacity = 0;
	RID motion_index_buffer;
	uint32_t motion_index_buffer_capacity = 0;
	RID motion_transform_buffer;
	uint32_t motion_transform_buffer_capacity = 0;
	// The TLAS instances when some are written on the GPU (RenderRaytracing::tlas_expand_jobs), in instances.
	RID tlas_instance_buffer;
	uint32_t tlas_instance_buffer_capacity = 0;

	RID light_buffer;
	// The lights each cell can receive light from (build_light_grid()), uploaded when it changes.
	RID light_grid_buffer;
	uint32_t light_grid_buffer_capacity = 0;
	LocalVector<uint32_t> uploaded_light_grid;
	RID params_buffer;
	// The decals (RT_DecalData) and their grid (gather_decals()), uploaded when they change.
	RID decal_buffer;
	uint32_t decal_buffer_capacity = 0;
	RID decal_grid_buffer;
	uint32_t decal_grid_buffer_capacity = 0;
	LocalVector<RT_DecalData> uploaded_decals;
	LocalVector<uint32_t> uploaded_decal_grid;
	// The sky's sampling distribution for next event estimation of the Environment (raytracing_sky_sampling_inc.glsl),
	// rebuilt every frame with Environment.pathtracing_sky_sampling (RenderRaytracing::build_sky_sampling()).
	RID sky_sampling_buffer;
	RID scene_uniform_set;

	uint32_t frame_counter = 0;
};

class RenderRaytracing {
	friend class RenderForwardClustered;
	friend class RenderForwardClusteredPT;

	RenderForwardClustered *owner = nullptr;
	SceneShaderRaytracing *shader = nullptr;
	BindlessBlock *bindless_block = nullptr;
	BindlessBlock *bindless_array_block = nullptr; // 2D texture arrays, set 2.

	RID bindless_uniform_set;
	RID bindless_array_uniform_set;

	// Caching (chunked sparse caches indexed by RID low bits / 256).
	Vector<RTCacheEntry *> surface_chunks;
	Vector<RTMaterialCacheEntry *> material_chunks;

	// Merged MultiMesh BLAS cache and compute shader.
	struct MergeShader {
		enum Mode {
			MODE_NON_INDEXED = 0,
			MODE_INDEXED = 1,
			MODE_MAX,
		};
		MultimeshMergeShaderRD shader;
		RID version;
		RID version_shader[MODE_MAX];
		RID pipeline[MODE_MAX];
	} mm_merge_shader;

	// Builds the RTLodViews (raytracing_lod_compact.glsl).
	struct LodCompactShader {
		enum Mode {
			MODE_MARK,
			MODE_ASSIGN,
			MODE_REMAP,
			MODE_MAX,
		};
		RaytracingLodCompactShaderRD shader;
		RID version;
		RID version_shader[MODE_MAX];
		RID pipeline[MODE_MAX];
	} lod_compact_shader;
	RID lod_view_dummy_buffer; // Bound in the merge's LOD view slots when a MultiMesh keeps its full mesh.

	// MultiMeshes traced as one TLAS instance per instance, the instances written on the GPU from the MultiMesh buffer
	// (raytracing_tlas_expand.glsl), after the ones the CPU writes. Without the standard instance layout, the instances
	// are read back to the CPU instead (multimesh_get_local_data_ptr_async()), a frame late.
	struct TlasExpandShader {
		RaytracingTlasExpandShaderRD shader;
		RID version;
		RID version_shader;
		RID pipeline;
	} tlas_expand_shader;
	bool tlas_expand_supported = false;
	struct TlasExpandJob {
		RT_TlasExpandJob data;
		RID blas;
		RID mm_gpu_buffer;
		uint32_t count = 0;
		uint32_t sbt_offset = 0;
		uint32_t flags = 0;
		uint8_t mask = 0;
	};
	LocalVector<TlasExpandJob> tlas_expand_jobs;
	LocalVector<RT_GeometryData> tlas_expand_geometry_templates;
	LocalVector<RT_MaterialData> tlas_expand_material_templates;
	uint32_t tlas_expand_instance_count = 0; // Of all the jobs.
	uint32_t tlas_expand_motion_count = 0; // Motion transforms written by the jobs.
	RID tlas_expand_job_buffer;
	uint32_t tlas_expand_job_capacity = 0;
	RID tlas_expand_geometry_template_buffer;
	RID tlas_expand_material_template_buffer;
	uint32_t tlas_expand_template_capacity = 0;
	// Writes the jobs' instances, records and motion after the CPU's, then builds the TLAS from p_instances and them.
	void _build_tlas_with_expanded_multimeshes(RTViewportState *p_state, LocalVector<RD::AccelerationStructureInstance> &p_instances);

	// Merged MultiMeshes whose material's vertex() moves them (wind), displaced after the merge.
	struct MMDisplaceRequest {
		RID merged_vtx_buffer;
		RID merged_prev_buffer;
		RID mm_gpu_buffer;
		RID remap_buffer; // A LOD view's, or none.
		void *mesh_surface = nullptr;
		uint32_t mm_count = 0;
		uint32_t vertex_count = 0; // Per instance, in the merged buffers.
		uint32_t mm_stride = 0;
		uint32_t mm_offset = 0;
		uint32_t mm_color_offset = UINT32_MAX;
		uint32_t mm_custom_offset = UINT32_MAX;
		RTMaterialData *mat_data = nullptr;
		Transform3D transform;
		Transform3D prev_transform;
		uint32_t instance_uniforms_ofs = 0;
	};
	LocalVector<MMDisplaceRequest> mm_displace_requests;
	RID mm_displace_job_buffer;
	uint32_t mm_displace_job_capacity = 0;
	RID displace_dummy_buffer; // Bound in the displacement's MultiMesh slots for the other jobs.
	bool displace_camera_moved = false;
	// The frame's displaced surfaces (GODOT_RT_LOG_CACHES): evaluated, waiting for their update frame, vertices evaluated.
	uint32_t displaced_evaluated = 0;
	uint32_t displaced_deferred = 0;
	uint64_t displaced_vertices = 0;
	uint32_t displaced_refitted = 0; // Of displaced_evaluated, refit instead of built again.
	uint32_t displaced_guard_rebuilds = 0; // BLASes the NaN guard asked to build again, since the previous log.
	// The frame's BLAS updates of deforming geometry waiting for their turn out of view (out_of_view_update_distance).
	uint32_t out_of_view_deferred = 0;
	LocalVector<RD::Uniform> _displace_frame_uniforms(RID p_job_buffer);
	void _displace_merged_multimeshes();

	// The volumetric fog applied over the traced color.
	struct VolumetricFogShader {
		struct PushConstant {
			float inv_projection[16];
			int32_t size[2];
			float inv_length;
			float detail_spread;
			uint32_t legacy_blending;
			uint32_t pad[3];
		};
		RaytracingVolumetricFogShaderRD shader;
		RID version;
		RID version_shader;
		RID pipeline;
	} volumetric_fog_shader;

	// DLSS Ray Reconstruction's diffuse albedo where the camera rays missed, from the drawn background.
	struct SkyGuideShader {
		struct PushConstant {
			int32_t size[2];
			uint32_t pad[2];
		};
		RaytracingSkyGuideShaderRD shader;
		RID version;
		RID version_shader;
		RID pipeline;
	} sky_guide_shader;

	// The sky's sampling distribution (RTViewportState::sky_sampling_buffer): the luminance of the radiance octmap, either
	// layout, then its cumulative distributions.
	struct SkySamplingShader {
		enum Mode {
			MODE_LUMINANCE,
			MODE_LUMINANCE_ARRAY,
			MODE_CDF,
			MODE_MAX,
		};
		struct PushConstant {
			float border_size[2];
			uint32_t pad[2];
		};
		RaytracingSkySamplingShaderRD shader;
		RID version;
		RID version_shader[MODE_MAX];
		RID pipeline[MODE_MAX];
	} sky_sampling_shader;
	// Must match RT_SKY_SAMPLING_SIZE and RT_SKY_SAMPLING_FLOATS (raytracing_sky_sampling_inc.glsl).
	static constexpr uint32_t SKY_SAMPLING_SIZE = 128;
	static constexpr uint32_t SKY_SAMPLING_FLOATS = 2 * SKY_SAMPLING_SIZE * SKY_SAMPLING_SIZE + SKY_SAMPLING_SIZE;
	// Builds p_state's sky sampling distribution from the radiance octmap (Environment.pathtracing_sky_sampling).
	void build_sky_sampling(RTViewportState *p_state, RID p_radiance_texture, bool p_octmap_array, float p_border_size);

	// The path traced color into the scene's color buffer, with the raster's background where the camera rays missed.
	struct OutputCopyShader {
		struct PushConstant {
			int32_t size[2];
			uint32_t keep_background;
			uint32_t pad;
		};
		RaytracingOutputCopyShaderRD shader;
		RID version;
		RID version_shader;
		RID pipeline;
	} output_copy_shader;

	struct MMSurfaceHandles {
		LocalVector<RID> per_surface; // Grown on demand; entry per touched surface index.
		uint32_t mm_validator = 0; // High 32 bits of MM RID id at last access.

		// Resolve / grow storage for `p_surface_index`.
		RID &surface_handle(uint32_t p_surface_index) {
			if (p_surface_index >= per_surface.size()) {
				per_surface.resize(p_surface_index + 1);
			}
			return per_surface[p_surface_index];
		}
	};
	LocalVector<MMSurfaceHandles> mm_handles;
	uint32_t mm_handles_sweep_cursor = 0; // Next handle checked for a freed MultiMesh.
	uint32_t lod_switch_frame = 0; // The frame lod_switch_triangles counts.
	uint64_t lod_switch_triangles = 0; // Merged BLAS triangles built again this frame for a LOD switch alone.

	RID_Owner<RTDeformedCacheEntry> deformed_pool;
	LocalVector<RID> deformed_active_this_frame;

	RID_Owner<RTMergedMMEntry> merged_mm_pool;
	LocalVector<RID> merged_mm_active_this_frame;
	// MultiMesh buffers the hit shaders read this frame for their instances' COLOR and INSTANCE_CUSTOM.
	LocalVector<RID> multimesh_buffers_read_this_frame;
	// Points r_geom at those of p_mm_rid (its current instances), when it has colors or custom data, or is merged
	// (p_vertex_count: vertices per instance; UINT32_MAX for one instance); false otherwise.
	bool _set_multimesh_instance_data(RT_GeometryData &r_geom, RID p_mm_rid, RID p_mm_gpu_buffer, uint32_t p_vertex_count);

	HashMap<RTLodViewKey, RTLodView> lod_views;
	const RTLodView *_get_lod_view(void *p_mesh_surface, uint32_t p_lod);
	void _free_lod_view(RTLodView &p_view);

	RTDeformedCacheEntry *_access_deformed_slot(RID &r_handle);

	// Material vertex() evaluated per vertex before the BLAS builds (raytracing_displace.glsl).
	RID displace_job_buffer;
	uint32_t displace_job_capacity = 0;
	Transform3D displace_camera_transform;
	Projection displace_camera_projection;
	void _displace_surfaces(const RenderDataRD *p_render_data);
	bool displaced_refit_used = true; // rendering/pathtracing/displaced_refit in the previous frame.
	// The refit jobs' NaN guard counts (raytracing_displace.glsl), one per job, read back without waiting: the surfaces of
	// the jobs that kept vertices are built again once they arrive.
	RID displace_guard_buffer;
	uint32_t displace_guard_capacity = 0;
	uint64_t displace_guard_serial = 0;
	HashMap<uint64_t, LocalVector<RID>> displace_guard_pending; // Per readback, the deformed handle of each job (RID() if not a refit).
	static RenderRaytracing *displace_guard_target;
	static void _displace_guard_arrived_static(const Vector<uint8_t> &p_data, uint64_t p_serial);
	void _displace_guard_arrived(const Vector<uint8_t> &p_data, uint64_t p_serial);
	RTMergedMMEntry *_access_merged_mm_slot(RID &r_handle);
	void _free_merged_mm_entry(RID p_handle);
	const RTMergedMMEntry *_get_merged_mm_entry(RID p_mm_rid, uint32_t p_surface_index); // With a BLAS built, or null.

	LocalVector<uint32_t> material_free_slots;
	uint32_t next_material_slot = 0;
	uint64_t vram_used = 0;
	uint32_t last_trace_frame = 0; // 0: nothing cached since the last cleanup_caches().
	uint32_t cache_hits = 0;
	uint32_t cache_misses = 0;

	// Per-frame scratch arrays.
	LocalVector<RT_GeometryData> geometry_data;
	LocalVector<RT_MaterialData> material_data;
	LocalVector<int32_t> motion_indices; ///< Per-instance: index into motion_transforms[], or -1.
	LocalVector<RT_InstanceMotionData> motion_transforms; ///< Compact: only moving instances.
	LocalVector<RID> blass;
	LocalVector<Transform3D> blas_transforms;
	LocalVector<uint32_t> instance_flags;
	LocalVector<uint8_t> instance_masks; // RT_INSTANCE_MASK_* bits.
	LocalVector<RT_DecalData> decal_data;
	LocalVector<uint32_t> decal_grid;
	LocalVector<uint32_t> light_grid;
	LocalVector<uint32_t> sbt_offsets; // 0 = default material hit group
	uint32_t front_layer_instance_count = 0;
	uint32_t procedural_instance_masks = 0; // RT_INSTANCE_MASK_PROCEDURAL_* bits of this frame's instances.

	HashMap<RenderSceneBuffersRD *, RTViewportState *> viewport_states;

	RTViewportState *_get_or_create_viewport_state(const RenderDataRD *p_render_data);
	RTViewportState *_get_viewport_state(const RenderDataRD *p_render_data) const;
	void _free_viewport_state_internal(RTViewportState *p_state);

	// Material UBO sub-allocation pool. One large device-address buffer divided
	// into fixed-size slots for performance reasons, and easier to debug.
	RID mat_ubo_pool_buffer;
	HashSet<RID> mat_ubo_dedicated_buffers; // Read by the trace through device addresses: declared as dependencies.
	uint64_t mat_ubo_pool_bda = 0;
	LocalVector<uint32_t> mat_ubo_pool_free_slots;
	uint32_t mat_ubo_pool_free_count = 0;
	uint32_t mat_ubo_pool_next_slot = 0;

	void mat_ubo_pool_ensure_initialized();
	uint32_t mat_ubo_pool_allocate(); // Returns slot index or UINT32_MAX if pool is exhausted.
	void mat_ubo_pool_release(uint32_t p_slot);
	void mat_ubo_pool_update(uint32_t p_slot, const void *p_data, uint32_t p_size);
	uint64_t mat_ubo_pool_get_address(uint32_t p_slot) const;

	// Cache helpers.
	static uint32_t get_rid_index(RID p_rid);
	static uint32_t get_rid_version(RID p_rid);
	RTCacheEntry *get_surface_cache_entry(uint32_t p_index);
	RTMaterialCacheEntry *get_material_cache_entry(uint32_t p_index);
	// The chunks of the surface and material caches the age sweep looks at next (prepare_frame()).
	uint32_t surface_sweep_chunk = 0;
	uint32_t material_sweep_chunk = 0;
	uint32_t allocate_material_slot();

	// Internal methods.
	RTSurfaceData *process_surface(
			const void *p_surf,
			void *p_mesh_surface,
			uint32_t p_surface_invalidation_counter,
			const Transform3D &p_transform,
			LocalVector<RID> &r_dirty_blas_list);
	RTSurfaceData *process_deformed_surface(
			const void *p_surf,
			void *p_mesh_surface,
			const struct RTDeformedGeometrySource &p_source,
			LocalVector<RID> &r_dirty_blas_list,
			LocalVector<RID> &r_dirty_blas_update_list);
	void _populate_surface_blas(
			void *p_mesh_surface,
			RID p_vertex_buffer_override,
			bool p_force_uncompressed,
			bool p_prefer_fast_build,
			bool p_allow_update,
			uint32_t p_cache_key,
			RTSurfaceData *r_surf_data,
			LocalVector<RID> &r_dirty_blas_list,
			RID p_opacity_micromap = RID(),
			uint32_t p_opacity_micromap_level = 0);

	// Opacity micromaps: the cut-outs the any-hit shaders test, baked per surface and material
	// (rt_alpha_dependencies.h, raytracing_omm_bake.glsl), the surface's BLAS built again with them.
	HashMap<RTOmmKey, RTOmmEntry, RTOmmKey> omm_entries;
	// The current key of each {mesh, surface, material, LOD} (the key with its counters at 0): a new one supersedes it.
	HashMap<RTOmmKey, RTOmmKey, RTOmmKey> omm_current_keys;
	// The entries whose bake is under way, oldest first (_omm_dispatch_bakes()).
	LocalVector<RTOmmKey> omm_bake_queue;
	// The estimates on their way back: serial -> key.
	HashMap<uint64_t, RTOmmKey> omm_estimates_pending;
	uint64_t omm_bake_serial = 0;
	RID omm_job_buffer;
	uint32_t omm_job_capacity = 0;
	uint32_t omm_sweep_frame = 0;
	// The surface's cut-out baked with the material into a micromap, when the material allows it, the surface lasted and
	// its bake is done, or null (the bake started or carried on for this frame). With p_lod, the triangles of that LOD.
	RTOmmEntry *_omm_entry(void *p_mesh_surface, RID p_mesh, uint32_t p_surface_index, uint32_t p_surface_counter, RID p_material, uint16_t p_material_counter, const RTMaterialData *p_mat_data, bool p_multimesh, uint32_t p_lod = 0);
	// Takes the bake's inputs from the live surface and material (false: the surface cannot be baked).
	bool _omm_fill_job(RTOmmEntry &r_entry, void *p_mesh_surface, const RTMaterialData *p_mat_data, uint32_t p_lod);
	static void _omm_estimate_arrived_static(const Vector<uint8_t> &p_data, uint64_t p_serial);
	void _omm_estimate_arrived(const Vector<uint8_t> &p_data, uint64_t p_serial);
	// The surface's static BLAS built again with the entry's micromap (built this frame if new), or invalid.
	RID _omm_static_blas(RTOmmEntry *p_entry, void *p_mesh_surface, LocalVector<RID> &r_dirty_blas_list);
	// Records this frame's share of the queued bakes (estimates, then ranges of micro-triangles within the GPU budget) and
	// the micromaps of the bakes done, before the BLAS builds.
	void _omm_dispatch_bakes();
	void _omm_free_entry(RTOmmEntry &p_entry);
	// Frees the entries no frame used for a while (all of them with p_all).
	void _omm_evict(bool p_all);
	RTMaterialData *process_material(RID p_material_rid, uint16_t p_material_invalidation_counter);
	// rendering/pathtracing/opacity_micromaps, on a device that has them.
	static bool _omm_enabled();
	// The material a surface is traced with while its custom shader's hit group compiles, or null to leave it out.
	RTMaterialData *_compiling_material_fallback(const RTMaterialData *p_mat_data, bool p_cut_out);
	// A surface of the raster transparent pass that the front layer rays leave out.
	static bool _front_layer_skipped(const void *p_surf);
	// A surface of the raster transparent pass that the TLAS holds for the front layer rays only.
	static bool _front_layer_surface(const void *p_surf);
	// Whether a MultiMesh surface goes into a merged BLAS (_build_merged_mm_blas()), or one TLAS instance per instance.
	bool _mm_fits_merged_blas(void *p_mesh_surface, uint32_t p_mm_count, const RTLodView *p_lod_view) const;
	bool _build_merged_mm_blas(
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
			bool p_allow_omm, // The material's own hit group, not forced opaque: its cut-out can be baked.
			const Transform3D &p_transform,
			const Transform3D &p_prev_transform,
			uint32_t p_instance_uniforms_ofs,
			RD::ComputeListID p_compute_list,
			LocalVector<RID> &r_dirty_blas_list,
			LocalVector<RID> &r_dirty_blas_update_list,
			RTSurfaceData *r_surf_data);
	void update_procedural_blas(RTProceduralState *p_state, LocalVector<RID> &r_dirty_blas_list);
	void build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list);
	void finalize_buffers(RTViewportState *p_state);
	void prepare_frame();

public:
	void initialize(RenderForwardClustered *p_owner);

	void cleanup_caches();
	// Frees every cache cleanup_caches() frees once no view has traced for a few frames: a game that turns path tracing
	// off gets the memory back for raster's effects.
	void free_caches_if_unused();

	RTViewportState *build_tlas(const RenderDataRD *p_render_data, uint32_t p_rt_flags);
	// Queues the hit groups of these materials' shaders (duplicates and freed materials allowed), whether or not their
	// instances are in a TLAS yet: compiled by the next pipeline build.
	void precache_hit_groups(const LocalVector<RID> &p_materials);
	// A BaseMaterial3D's generated shader, traced by hit group 0 from its parameters (p_albedo: its "albedo").
	static bool is_base_material(RID p_material, const Variant &p_albedo);
	uint32_t gather_lights(const RenderDataRD *p_render_data, RT_LightData *r_light_data, uint32_t p_max_lights);
	// A hashed grid of the lights in world space for the choice of light at each hit; uploads it to p_state's buffer.
	void build_light_grid(RTViewportState *p_state, const RT_LightData *p_lights, uint32_t p_count);
	// The RD texture of the directional light projector gather_lights() found (binding 38), or invalid.
	RID rt_directional_projector;
	// The decals around the camera, in world space, and a hashed grid of them for the hit shaders; uploads them to
	// p_state's buffers. Returns their number.
	uint32_t gather_decals(RTViewportState *p_state, const RenderDataRD *p_render_data);
	// `p_background_color` (linear) is what the camera rays that miss see where the raster clears to it (no sky drawn);
	// with `p_background_is_color` false they see the sky, as the light of any other ray. With
	// `p_reflection_shows_background` (REFLECTION_SOURCE_BG), the rays that miss along a specular lobe see it too.
	RID update_uniform_set(RTViewportState *p_state, const RenderDataRD *p_render_data, uint32_t p_rt_flags, bool p_background_is_color, const Color &p_background_color, bool p_reflection_shows_background);
	// The probes of the rasterized instances (RendererRD::PTProbes::get_rays_buffer()), binding 31: read and written by the
	// raygen's probe dispatch.
	RID probe_buffer;

	// Where the camera rays missed, `p_keep_background` keeps the color buffer's pixels (BG_CANVAS, BG_KEEP), else the
	// miss color goes there with an alpha of 0, as the raster's clear color.
	void copy_output_texture(const RenderDataRD *p_render_data, bool p_keep_background);
	void apply_volumetric_fog(const RenderDataRD *p_render_data, bool p_legacy_blending);
	void dlss_rr_write_background_albedo(const RenderDataRD *p_render_data);
	void free_viewport_state(RenderSceneBuffersRD *p_render_buffers);

	// Raytracing output textures (stored on the render buffers via named scopes).
	void rt_ensure_textures(RenderSceneBuffersRD *p_render_buffers);
	bool rt_has_texture(RenderSceneBuffersRD *p_render_buffers) const;
	RID rt_get_texture(RenderSceneBuffersRD *p_render_buffers) const;
	bool rt_has_depth_texture(RenderSceneBuffersRD *p_render_buffers) const;
	RID rt_get_depth_texture(RenderSceneBuffersRD *p_render_buffers) const;
	RID rt_get_front_layer_texture(RenderSceneBuffersRD *p_render_buffers) const;
	// Transparent surfaces in the TLAS this frame: the raygen traces their reflections.
	uint32_t get_front_layer_instance_count() const { return front_layer_instance_count; }

	// DLSS Ray Reconstruction guide buffers (stored on the render buffers).
	void dlss_rr_ensure_buffers(RenderSceneBuffersRD *p_render_buffers);
	void dlss_rr_free_buffers(RenderSceneBuffersRD *p_render_buffers);
	bool dlss_rr_has_buffers(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_diffuse_albedo(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_specular_albedo(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_normal_roughness(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_specular_hit_dist(RenderSceneBuffersRD *p_render_buffers) const;
	// Ray Reconstruction's depth guide (NDC, as rt_depth): the opaque surface's, or the transparent one in front where
	// its reflection dominates. The raster depth buffer keeps the opaque one, which the transparent pass reads.
	RID dlss_rr_get_depth(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_color_before_transparency(RenderSceneBuffersRD *p_render_buffers) const;

	void register_raytracing_buffer_dependencies(RD::RaytracingListID p_list);
	// The buffers written per frame that the shaders read through the addresses in RT_GeometryData and RT_MaterialData.
	void get_buffers_read_by_address(LocalVector<RID> &r_buffers);

	// The material textures, for the shadow rays' alpha test in the transparent pass (USE_RT_SHADOWS), in a set of
	// p_shader's layout.
	RID get_fragment_texture_set(RID p_shader, uint32_t p_set_index) const {
		return bindless_block ? bindless_block->get_secondary_uniform_set(p_shader, p_set_index, BindlessBlock::SECONDARY_LAYOUT_FRAGMENT) : RID();
	}

	SceneShaderRaytracing *get_shader() const { return shader; }

	RID get_bindless_uniform_set() const { return bindless_uniform_set; }
	RID get_bindless_array_uniform_set() const { return bindless_array_uniform_set; }
	RID get_mat_ubo_pool_buffer() const { return mat_ubo_pool_buffer; }

	~RenderRaytracing();
};

} // namespace RendererSceneRenderImplementation
