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
#include "core/templates/bit_field.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/bindless_block.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_debug.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/multimesh_merge.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_lod_compact.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_nrd_composite.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_output_copy.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_sky_guide.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_sky_sampling.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_tlas_expand.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_volumetric_fog.glsl.gen.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"

// The path tracer's own textures, in a scope of their own so a view that stops path tracing frees them.
#define RB_SCOPE_RAYTRACING SNAME("raytracing")
#define RB_TEX_RAYTRACING SNAME("raytracing")
#define RB_TEX_RT_DEPTH SNAME("rt_depth")
#define RB_TEX_RT_FRONT_LAYER SNAME("rt_front_layer")

// Environment.RT_DEBUG_TIMING_HEATMAP: the device clock ticks of each pixel's raygen (RT_FLAG_TIMING_ENABLED, binding 46).
#define RB_SCOPE_RT_TIMING SNAME("rt_timing")
#define RB_TEX_RT_TIMING SNAME("ticks")

#define RB_SCOPE_DLSS_RR SNAME("dlss_rr")
#define RB_TEX_DLSS_RR_DIFFUSE_ALBEDO SNAME("diffuse_albedo")
#define RB_TEX_DLSS_RR_SPECULAR_ALBEDO SNAME("specular_albedo")
#define RB_TEX_DLSS_RR_NORMAL_ROUGHNESS SNAME("normal_roughness")
#define RB_TEX_DLSS_RR_SPECULAR_HIT_DIST SNAME("specular_hit_dist")
#define RB_TEX_DLSS_RR_COLOR_BEFORE_TRANSPARENCY SNAME("color_before_transparency")
#define RB_TEX_DLSS_RR_DEPTH SNAME("rr_depth")
#define RB_TEX_DLSS_RR_SPECULAR_MOTION_VECTORS SNAME("specular_motion_vectors")

// NRD's inputs the raygen writes (RenderRaytracing::nrd_ensure_buffers()), in the guides' layout of DLSS Ray Reconstruction
// (bindings 9-11), apart from them: what reads DLSS-RR's scope takes it for Ray Reconstruction.
#define RB_SCOPE_NRD SNAME("nrd")
#define RB_TEX_NRD_DIFFUSE SNAME("diffuse")
#define RB_TEX_NRD_SPECULAR SNAME("specular")
#define RB_TEX_NRD_DIFFUSE_ALBEDO SNAME("diffuse_albedo")
#define RB_TEX_NRD_SPECULAR_ALBEDO SNAME("specular_albedo")
#define RB_TEX_NRD_NORMAL_ROUGHNESS SNAME("normal_roughness")
#define RB_TEX_NRD_VIEW_Z SNAME("view_z")
#define RB_TEX_NRD_SUN SNAME("sun")
#define RB_TEX_NRD_DIRECT SNAME("direct")

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
	// Environment.RT_DEBUG_DYNAMIC_GEOMETRY only: RTDynamicGeometryDebug::pack() of the BLAS's recent work (multimesh_pad0
	// in GLSL), 0 otherwise.
	uint32_t debug_state;
	// rendering/visibility_range/dithered_fade: 0, or RT_VISIBILITY_FADE_DITHERED | the fade (8 bits, as the rasterizer's
	// instance flags hold it) of an instance fading through its visibility range in a view that dithers such fades
	// (RenderForwardClustered::_instance_fade_alpha()): the camera rays keep or drop it against a noise threshold.
	uint32_t visibility_fade;
	uint32_t multimesh_pad;
};
static_assert(sizeof(RT_GeometryData) == 176, "RT_GeometryData must be 176 bytes for std430");

static constexpr uint32_t RT_CUSTOM_FORMAT_PRESENT = 8u;
// RT_GeometryData::visibility_fade: the instance's fade is dithered (VISIBILITY_FADE_DITHERED in raytracing_shade_inc.glsl).
static constexpr uint32_t RT_VISIBILITY_FADE_DITHERED = 0x100u;

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
	float emission_indirect; // BaseMaterial3D.emission_indirect_energy: the emission's weight after a diffuse bounce.
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
	uint32_t metallic_texture_idx; // BaseMaterial3D's metallic texture, or its ORM texture (RT_MAT_METALLIC_CHANNEL_SHIFT).
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

// A job's GPUParticles3D data (raytracing_tlas_expand.glsl PARTICLES variant, ExpandParticles, std430); zero for a
// MultiMesh's job. rendering/pathtracing/trace_gpu_particles.
struct RT_TlasExpandParticles {
	float basis[12]; // The billboard mode's facing basis for the camera, three columns of 4 floats.
	float prev_basis[12]; // For the previous frame's camera.
	float fixed_size[4]; // The fixed_size scale at an origin: dot(xyz, origin) + w.
	float prev_fixed_size[4];
	uint32_t flags[4]; // [0]: RT_EXPAND_*.
};
static_assert(sizeof(RT_TlasExpandParticles) == 144, "RT_TlasExpandParticles must be 144 bytes for std430");

// RT_TlasExpandParticles::flags[0], as raytracing_tlas_expand.glsl EXPAND_*.
static constexpr uint32_t RT_EXPAND_PARTICLES = 1u; // Dead particles are inactive instances.
static constexpr uint32_t RT_EXPAND_BILLBOARD = 2u;
static constexpr uint32_t RT_EXPAND_BILLBOARD_ROTATION = 4u; // billboard_particles: by INSTANCE_CUSTOM.x.
static constexpr uint32_t RT_EXPAND_BILLBOARD_KEEP_SCALE = 8u;
static constexpr uint32_t RT_EXPAND_FIXED_SIZE = 16u;
// A 3D particle's record in the instance buffer (particles_copy.glsl): 3 rows of transform, COLOR, INSTANCE_CUSTOM.
static constexpr uint32_t RT_PARTICLE_INSTANCE_FLOATS = 20u;

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
	float spot_direction[3]; // The light's -Z: a spot's cone axis, and an omni light's with a projector.
	float shadow_opacity; // 0 without shadows: no shadow ray, as raster.
	// Directional: world position (w = 1) to the UV of rt_directional_light_projector, when projector_enabled (as raster's
	// DirectionalLightData, in world space). Omni and spot with projector_texture: xyz the light's X and Y axes, and
	// projector_uv_x[3] a spot's cot(spot angle), its projector's perspective.
	float projector_uv_x[4];
	float projector_uv_y[4];
	uint32_t projector_enabled;
	uint32_t projector_texture; // Omni and spot: Light3D.light_projector in the bindless textures (sRGB), 0 for none.
	uint32_t shadow_caster_mask; // Light3D.shadow_caster_mask: the layers whose instances cast its shadows (RT_SHADOW_GROUPS).
	uint32_t pad;
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
	// Build-time only: a transparent surface gets a copy for the shadow rays (rendering/pathtracing/transparent_shadows).
	RT_INSTANCE_MASK_TRANSPARENT_SHADOW = 0x40u,
	RT_INSTANCE_MASK_DOUBLE_SIDED_SHADOW = 0x80u, // Build-time only: the shadow rays need both faces, see build_tlas().
	// In place of SHADOW, assigned after the build-time bits above are cleared (the same bit as DOUBLE_SIDED_SHADOW): a
	// caster that shadows only some of the frame's lights or receivers (rendering/pathtracing/shadow_groups,
	// Light3D.shadow_caster_mask, SHADOW_CASTING_SETTING_SELF_SHADOWING). The shadow rays meet these in a ray query that
	// tests each candidate (RT_SHADOW_GROUPS): the 8-bit instance mask cannot hold a group per caster mask.
	RT_INSTANCE_MASK_SHADOW_GROUP = 0x80u,
};

// The 20 render layers of VisualInstance3D.layers, which light cull and shadow caster masks select.
static constexpr uint32_t RT_LAYER_MASK_ALL = 0xFFFFFu;

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

// The channels BaseMaterial3D reads its roughness (orm_texture_idx) and metallic (metallic_texture_idx) textures in, in
// the material flags (an RTTextureChannel each, 3 bits): GREEN and BLUE of the ORM texture.
enum RTTextureChannel : uint32_t {
	RT_TEXTURE_CHANNEL_RED = 0u,
	RT_TEXTURE_CHANNEL_GREEN = 1u,
	RT_TEXTURE_CHANNEL_BLUE = 2u,
	RT_TEXTURE_CHANNEL_ALPHA = 3u,
	RT_TEXTURE_CHANNEL_GRAYSCALE = 4u, // The mean of red, green and blue, as BaseMaterial3D's mask (0.333333 each).
};
static constexpr uint32_t RT_MAT_ROUGHNESS_CHANNEL_SHIFT = 16u;
static constexpr uint32_t RT_MAT_METALLIC_CHANNEL_SHIFT = 19u;

// The material's diffuse and specular modes (raster's ShaderData::DiffuseMode / SpecularMode values), in the material flags,
// read by the rendering/pathtracing/raster_brdf variant (RT_RASTER_BRDF). NVIDIA's bits and values (49a825dc52
// render_raytracing.h:451-464).
enum {
	RT_MAT_DIFFUSE_MODE_SHIFT = 28u,
	RT_MAT_SPECULAR_MODE_SHIFT = 30u,
	RT_MAT_DIFFUSE_LAMBERT = 0u,
	RT_MAT_DIFFUSE_BURLEY = 1u,
	RT_MAT_DIFFUSE_LAMBERT_WRAP = 2u,
	RT_MAT_DIFFUSE_TOON = 3u,
	RT_MAT_SPECULAR_SCHLICK_GGX = 0u,
	RT_MAT_SPECULAR_TOON = 1u,
	RT_MAT_SPECULAR_DISABLED = 2u,
};

// Packs an RT_MAT_DIFFUSE_* and an RT_MAT_SPECULAR_* value into material flag bits.
constexpr uint32_t rt_material_shading_mode_flags(uint32_t p_diffuse_mode, uint32_t p_specular_mode) {
	return (p_diffuse_mode << RT_MAT_DIFFUSE_MODE_SHIFT) | (p_specular_mode << RT_MAT_SPECULAR_MODE_SHIFT);
}

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
	// Per TLAS instance (rt_triangle_facing()): a triangle the ray tracer reports front facing is a raster back face.
	RT_GEOM_FLAG_FACING_INVERTED = 32u,
	// Per TLAS instance: the material turns the normal of back faces, as raster's DO_SIDE_CHECK (cull_front, cull_disabled).
	RT_GEOM_FLAG_SIDE_CHECK = 64u,
	// Per TLAS instance: the shadow rays' copy of a transparent surface, whose any-hit filters their light
	// (rendering/pathtracing/transparent_shadows, RT_TRANSPARENT_SHADOWS).
	RT_GEOM_FLAG_TRANSPARENT_SHADOW = 128u,
	// Bits 8-10, per TLAS instance: a front layer surface's blend mode, as the raygen peels it
	// (rendering/pathtracing/max_transparency_layers, RT_TRANSPARENCY_PEELING: FLAG_PEEL_BLEND_* in the shaders).
	RT_GEOM_PEEL_BLEND_SHIFT = 8u,
	RT_GEOM_PEEL_BLEND_ADD = 1u,
	RT_GEOM_PEEL_BLEND_SUB = 2u,
	RT_GEOM_PEEL_BLEND_MUL = 3u,
	RT_GEOM_PEEL_BLEND_PREMUL_ALPHA = 4u,
	// Per TLAS instance, rendering/pathtracing/shadow_groups only (RT_SHADOW_GROUPS): an instance with
	// SHADOW_CASTING_SETTING_SELF_SHADOWING. Its shadows fall only on such instances; it receives every shadow.
	RT_GEOM_FLAG_SELF_SHADOWING = 1u << 11,
	// Per TLAS instance, on RT_INSTANCE_MASK_SHADOW_GROUP casters only: not forced opaque, alpha tested in the ray query.
	RT_GEOM_FLAG_SHADOW_GROUP_CUTOUT = 1u << 12,
};

// The facing of the traced triangles of one TLAS instance.
struct RTTriangleFacing {
	uint32_t instance_flags = 0; // TRIANGLE_FACING_CULL_DISABLE and TRIANGLE_FLIP_FACING.
	uint32_t geometry_flags = 0; // RT_GEOM_FLAG_FACING_INVERTED and RT_GEOM_FLAG_SIDE_CHECK.
};

// The facing of triangles drawn with p_cull_mode, as raster has it (RenderForwardClustered::_render_list_template()):
// raster culls by the winding in mesh space (a mirrored instance is drawn with the reversed cull mode), and its
// FRONT_FACING follows the winding on screen, or in mesh space with preserve_front_facing_when_mirrored.
// The ray tracer decides facing in BLAS space, which the instance transform does not change (Vulkan "Ray Triangle Culling",
// DXR D3D12_RAYTRACING_INSTANCE_FLAGS), and its front face, without TRIANGLE_FLIP_FACING, is raster's back face.
// p_blas_mirrored: the BLAS space is mirrored from the space raster culls in (the TLAS instance transform's determinant
// has the other sign than the one of the transform raster takes the instance's mirror flag from: a MultiMesh instance's
// own mirroring, which raster does not undo). p_screen_mirrored: raster mirrors the instance's winding on screen (the
// instance or the camera is mirrored, without preserve_front_facing_when_mirrored).
constexpr RTTriangleFacing rt_triangle_facing(RSE::CullMode p_cull_mode, bool p_blas_mirrored, bool p_screen_mirrored) {
	const bool cull_front = p_cull_mode == RSE::CULL_MODE_FRONT;
	RTTriangleFacing facing;
	facing.instance_flags = (cull_front == p_blas_mirrored ? uint32_t(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT) : 0u) |
			(p_cull_mode == RSE::CULL_MODE_DISABLED ? uint32_t(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT) : 0u);
	facing.geometry_flags = (cull_front != p_screen_mirrored ? uint32_t(RT_GEOM_FLAG_FACING_INVERTED) : 0u) |
			(p_cull_mode != RSE::CULL_MODE_BACK ? uint32_t(RT_GEOM_FLAG_SIDE_CHECK) : 0u);
	return facing;
}

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

// Must match GLSL RTCaptureDraw (forward_clustered/rt_capture_inc.glsl, std430): a vertex capture request's draw.
struct RT_VertexCaptureDraw {
	float view_to_local[12]; // Rows of the 3x4 transform from view space to the surface's mesh space.
	uint32_t vertex_count;
	uint32_t tbn_words;
	uint32_t flags; // RT_CAPTURE_FLAG_*.
	uint32_t guard_index;
};
static_assert(sizeof(RT_VertexCaptureDraw) == 64, "RT_VertexCaptureDraw must be 64 bytes for std430");
static constexpr uint32_t RT_CAPTURE_FLAG_REFIT_GUARD = 1u << 0;
static constexpr uint32_t RT_CAPTURE_FLAG_COMPRESSED = 1u << 1;

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
	// Built with ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT: a transparent shadow caster's any-hit
	// multiplies the shadow ray's transmittance, which a second call for the same triangle would apply twice.
	bool no_duplicate_any_hit = false;
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
	bool rebuild_only = false; ///< RT_BLAS_UPDATE_REBUILD: as force_rebuild, the update still waiting for its turn (defer_update).
	uint32_t prev_offset = UINT32_MAX; ///< When set and the data changed: previous positions read from `prev_vb` there.
	bool defer_update = false; ///< A change of `current_vb` waits for a later frame (out of view and far): the BLAS keeps the positions it has.
	bool no_duplicate_any_hit = false; ///< The BLAS needs RTSurfaceData::no_duplicate_any_hit (kept once built with it).
};

struct RTMaterialData {
	alignas(16) RT_MaterialData data = {};
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

// The uniforms vertex() reads from the global uniform buffer, hashed (RenderRaytracing::_vertex_uniforms_hash()): the
// instance uniforms and the global shader parameters. Hashed again only once the buffer's values, the instance's slice or
// the material changed, so a surface whose uniforms stay as they are reads nothing per frame.
struct RTVertexUniformsHash {
	uint64_t buffer_version = UINT64_MAX; // MaterialStorage::global_shader_uniforms_get_version() when hashed.
	uint64_t instance_version = 0; // MaterialStorage::global_shader_parameters_instance_get_version() when hashed.
	int32_t instance_uniforms_ofs = -1;
	uint16_t material_counter = 0;
	uint32_t hash = 0;
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
	uint32_t evaluated_frame = 0; // Last frame a view evaluated vertex() on it: the later views of that frame share it.
	uint64_t displaced_stamp = 0; // Bumped on each evaluation.
	bool displaced_rebuild = false; // Evaluated this frame and the BLAS is to be built again, not refit (_displace_surfaces()).
	uint32_t displaced_refits = 0; // Since the last rebuild.
	bool displaced_guard_rebuild = false; // A refit kept vertices vertex() turned on or off (raytracing_displace.glsl).
	uint32_t displaced_slot = 0;
	uint32_t displaced_prev_offset = 0; // Bytes to the previous frame's positions in displaced_vb.
	uint16_t displaced_material_counter = 0;
	uint32_t displaced_instance_uniforms_hash = 0; // The instance uniforms and global shader parameters vertex() was evaluated with.
	RTVertexUniformsHash vertex_uniforms; // Their current hash.
	bool displaced_deferred = false; // A change waits for the surface's next update frame (far from the camera).
	Transform3D displaced_transform;
	uint64_t displaced_deformation = 0; // A skinned surface's: the skinning output it was evaluated from.
	// Last evaluated by a vertex capture (rt_capture_inc.glsl), which leaves the previous positions' section out: the
	// previous positions are then the last evaluation's (process_deformed_surface()).
	bool displaced_captured = false;

	// The surface's cut-out baked into an opacity micromap (build_tlas() sets it, _omm_entry()), and the one the BLAS
	// was built with: a change builds the BLAS again.
	RID omm_micromap;
	uint32_t omm_level = 0;
	RID built_omm_micromap;

	uint32_t updated_frame = 0; // The last frame its BLAS was built or refit; 0 = never.
	uint32_t budget_held_frame = 0; // The last frame the dynamic BLAS budgets held its update (_schedule_dynamic_updates()).
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
	uint64_t displaced_build = 0; // The build_tlas() call (RenderRaytracing::tlas_build_serial) that displaced it last.
	uint32_t displaced_inputs = 0; // Hash of the material's parameters and the uniforms vertex() was evaluated with.
	RTVertexUniformsHash vertex_uniforms; // The uniforms' current hash.
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

	// The frame a user (a view, or another node of the same MultiMesh) chose it in build_tlas(), and the LOD it is traced at
	// that frame: the other users of the frame trace it as built, and those that need another LOD take another entry
	// (RenderRaytracing::MMSurfaceHandles).
	uint32_t claimed_frame = 0;
	uint32_t claimed_lod = 0;
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
	// With NRD, the light buffer index of the sun whose shadow SIGMA denoises (update_uniform_set()), -1: none; the direction
	// to it and the tangent of its angular radius.
	int32_t nrd_sigma_light = -1;
	Vector3 nrd_sigma_light_direction;
	float nrd_sigma_tan_angular_radius = 0.0f;
	// With NRD, whether the raygen writes the direct light signal (the RT_FLAG_NRD_DIRECT_LIGHT variant).
	bool nrd_direct_light = false;
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
	// The view's parameters of the volumetric fog's ray queries (RTFogShadows, rendering/pathtracing/volumetric_fog_shadows).
	RID fog_shadows_params_buffer;
	// rendering/pathtracing/shadow_groups: the last TLAS holds RT_INSTANCE_MASK_SHADOW_GROUP casters (RT_PARAM_FLAG_SHADOW_GROUPS).
	bool shadow_groups_used = false;

	uint32_t frame_counter = 0;
};

// GeometryInstance3D.rt_blas_update, else the shader's rt_blas render mode (NVIDIA's rt_resolve_blas_update()).
constexpr RSE::RTBlasUpdate rt_resolve_blas_update(RSE::RTBlasUpdate p_instance, RSE::RTBlasUpdate p_shader) {
	return p_instance != RSE::RT_BLAS_UPDATE_AUTO ? p_instance : p_shader;
}

// rendering/pathtracing/dynamic_blas_*: per-frame budgets of the updates of deforming geometry that can wait (refits,
// periodic and RT_BLAS_UPDATE_REBUILD builds), ranked by apparent size and frames waited (NVIDIA's RTDynamicScheduler).
// Off by default. A new or reshaped BLAS is built at once and not counted. Shared by the views of a frame
// (RenderRaytracing::_schedule_dynamic_updates()).
struct RTDynamicBudgets {
	int64_t max_operations = 0; // 0: no limit, as the triangle budgets.
	int64_t build_triangles = 0;
	int64_t update_triangles = 0;
	uint32_t max_stale_frames = 30; // dynamic_max_stale_frames: past it, an update ranks first.
	float offscreen_weight = 0.25f; // dynamic_offscreen_size_weight.
};

struct RTDynamicCandidate {
	RTDeformedCacheEntry *entry = nullptr;
	AABB aabb; // World space.
	uint64_t key = 0;
	float score = 0.0f;
	uint32_t order = 0;
	uint32_t triangles = 0;
	bool in_view = false;
	uint32_t pending = UINT32_MAX; // A displaced surface's pending job in _displace_surfaces(); UINT32_MAX: skinned.
	bool rebuild = false; // The update is a build.
	bool rebuild_only = false; // RT_BLAS_UPDATE_REBUILD: it cannot become a refit.
	bool selected = false;
	bool downgraded = false; // A periodic build past the build budget, refit instead.
};

struct RTDynamicBudgetUsage {
	uint32_t frame = UINT32_MAX;
	uint32_t operations = 0;
	uint64_t build_triangles = 0;
	uint64_t update_triangles = 0;
	uint32_t cursor = 0; // Rotates equal scores between frames.
};

// The apparent size NVIDIA's RTCpuVisibilityEstimator ranks by: (radius / max(distance, radius))^2 of the bounding sphere
// of p_aabb, times p_offscreen_weight out of view.
float rt_dynamic_visibility(const AABB &p_aabb, bool p_in_view, const Vector3 &p_camera_origin, float p_offscreen_weight);
// p_visibility times 1 + the frames waited, at most p_max_stale_frames, past which the update ranks first.
float rt_dynamic_score(float p_visibility, uint32_t p_waited, uint32_t p_max_stale_frames);
// Ranks r_candidates by score, equal scores by key rotated by r_usage's cursor, and selects those within p_budgets less
// what r_usage spent of them this frame; the first update of each kind passes whatever its size. Returns the number of
// candidates selected and advances the cursor by it.
uint32_t rt_dynamic_select(LocalVector<RTDynamicCandidate> &r_candidates, const RTDynamicBudgets &p_budgets, RTDynamicBudgetUsage &r_usage);

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
		// The PARTICLES variant, for the frames with GPUParticles3D jobs (rendering/pathtracing/trace_gpu_particles).
		RID particles_version_shader;
		RID particles_pipeline;
	} tlas_expand_shader;
	bool tlas_expand_supported = false;
	struct TlasExpandJob {
		RT_TlasExpandJob data;
		RT_TlasExpandParticles particles = {}; // A GPUParticles3D's job: its flags are not 0.
		RID blas;
		RID mm_gpu_buffer;
		uint32_t count = 0;
		uint32_t sbt_offset = 0;
		uint32_t flags = 0;
		uint8_t mask = 0;
	};
	RID tlas_expand_particles_buffer; // RT_TlasExpandParticles, one per job, in the frames with particle jobs.
	uint32_t tlas_expand_particles_capacity = 0;
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
	// The frame's BLAS updates of deforming geometry waiting for their turn out of view (out_of_view_update_distance), or
	// held by the dynamic BLAS budgets.
	uint32_t out_of_view_deferred = 0;
	// The most frames a held update of deforming geometry waited since its last BLAS update, since the previous log
	// (GODOT_RT_LOG_CACHES).
	uint32_t held_max_age = 0;

	// out_of_view_update_distance's turns: what is in view is updated every frame, what is out of view and far every N
	// frames (build_tlas()).
	struct UpdateTurns {
		Vector<Plane> view_planes; // Empty: everything counts as in view.
		Vector3 view_points[8];
		Vector3 camera_origin;
		float update_distance = 0.0f; // 0: no turns.
		uint32_t frame = 0;
		bool in_view(const AABB &p_aabb) const;
		bool turn(const AABB &p_aabb, uint64_t p_key) const; // False when the update waits for a later frame.
	};

	// rendering/pathtracing/dynamic_blas_* (RTDynamicBudgets).
	RTDynamicBudgetUsage dynamic_budget_usage;
	LocalVector<RTDynamicCandidate> dynamic_candidates;
	static bool _dynamic_budgets_enabled();
	// Adds the skinned and blend shape surfaces whose update is due, ranks every candidate and selects those within the
	// budgets; the others keep their BLAS this frame (RTDeformedCacheEntry::budget_held_frame).
	void _schedule_dynamic_updates(const RenderDataRD *p_render_data, const UpdateTurns &p_turns);
	// What a build_tlas() call did, for its view's Viewport.RENDER_INFO_RT_* counters: counted on the CPU where the work is
	// decided (no readback). Later views of a frame count what they do themselves (the first one updates shared entries).
	struct RenderInfoCounters {
		uint32_t rebakes = 0; // Merged MultiMeshes whose vertices were merged again from their instances.
		uint64_t rebake_vertices = 0;
		uint32_t captured = 0; // Merged MultiMeshes whose material's vertex() was evaluated (surfaces: displaced_evaluated).
		uint64_t captured_vertices = 0;
		uint64_t captured_bytes = 0; // Written to the surfaces' displaced vertex buffers.
		uint32_t dynamic_updated = 0; // Deforming geometry (skinned, blend shapes, displaced, merged) whose BLAS was built or refit.
		uint32_t budget_limited = 0; // Merged MultiMeshes kept at their LOD by the LOD switch budget, updates held by the dynamic BLAS budgets.
		uint32_t pending_surfaces = 0; // Surfaces whose hit group is still compiling: left out, or traced with a stand-in.
	} render_info_counters;
	uint64_t displaced_vb_bytes = 0; // The capacity of every surface's displaced vertex buffer.
	// What the frame not_ready_frame traced at an older state (RenderingServer.get_rendering_not_ready_reasons()).
	uint64_t not_ready_frame = UINT64_MAX;
	BitField<RSE::RenderingNotReadyReason> frame_not_ready_reasons = 0;
	void _flag_not_ready(RSE::RenderingNotReadyReason p_reason);
	void _record_render_info(const RenderDataRD *p_render_data);
	// Environment.RT_DEBUG_DYNAMIC_GEOMETRY: each instance's recent BLAS work, in RT_GeometryData::debug_state.
	RTDynamicGeometryDebug dynamic_geometry_debug;
	LocalVector<uint32_t> merged_displaced_records; // This build's records of merged MultiMeshes displaced by vertex().
	void _annotate_dynamic_geometry(const LocalVector<RID> &p_built, const LocalVector<RID> &p_refit);
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
			uint32_t pad[3]; // The debug variant's view in pad[0] (raytracing_volumetric_fog.glsl MODE_DEBUG).
		};
		RaytracingVolumetricFogShaderRD shader;
		RID version;
		RID version_shader;
		RID pipeline;
		// Environment.RT_DEBUG_VOLUMETRIC_FOG and RT_DEBUG_VOLUMETRIC_FOG_TRANSMITTANCE: the fog instead of the color.
		RID debug_version_shader;
		RID debug_pipeline;
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
			float alpha_multiplier;
		};
		RaytracingOutputCopyShaderRD shader;
		RID version;
		RID version_shader;
		RID pipeline;
	} output_copy_shader;

	// Each lobe's light of the primary surfaces multiplied back by its albedo onto the path traced output (nrd_composite()).
	struct NRDCompositeShader {
		struct PushConstant {
			int32_t size[2];
			uint32_t sun; // 1: add the sun's light times SIGMA's shadow (binding 6).
			uint32_t direct; // 1: add the direct light signal (binding 7).
		};
		RaytracingNrdCompositeShaderRD shader;
		RID version;
		RID version_shader;
		RID pipeline;
	} nrd_composite_shader;

	struct MMSurfaceHandles {
		// Grown on demand; per touched surface index, its merged entries (RTMergedMMEntry): one per LOD the users of a frame
		// trace it at, usually one. A BLAS a view's TLAS references is never built or refit again in place for another LOD
		// in the same frame: that TLAS tracks it only through itself, so the render graph does not order the write after the
		// view's trace, and DXR and Vulkan require the TLAS to be built again after it.
		LocalVector<LocalVector<RID>> per_surface;
		uint32_t mm_validator = 0; // High 32 bits of MM RID id at last access.

		// Resolve / grow storage for `p_surface_index`.
		LocalVector<RID> &surface_slots(uint32_t p_surface_index) {
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
	void _displace_surfaces(const RenderDataRD *p_render_data, const UpdateTurns &p_turns);
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
	// rendering/pathtracing/vertex_capture_mode: the surfaces whose vertex() output comes from the raster vertex shader
	// (RenderForwardClustered::_rt_record_vertex_captures()) instead of the compute pass, where the device has vertex
	// pipeline stores (Vulkan). Its parameters per request (RT_VertexCaptureDraw) and NaN guard counts.
	enum VertexCaptureMode {
		VERTEX_CAPTURE_AUTOMATIC,
		VERTEX_CAPTURE_OPT_IN,
		VERTEX_CAPTURE_DISABLED,
	};
	RID vertex_capture_params_buffer;
	uint32_t vertex_capture_params_capacity = 0;
	RID vertex_capture_guard_buffer;
	uint32_t vertex_capture_guard_capacity = 0;
	RTMergedMMEntry *_access_merged_mm_slot(RID &r_handle);
	void _free_merged_mm_entry(RID p_handle);
	// The merged entries of p_mm_rid's surface (MMSurfaceHandles), released first when its RID index was recycled.
	LocalVector<RID> &_merged_mm_slots(RID p_mm_rid, uint32_t p_surface_index);

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
	// Counts the build_tlas() calls: a front layer surface the raygen peels is stamped with the build that put it in the TLAS
	// (GeometryInstanceSurfaceDataCache::rt_peeled_build).
	uint64_t tlas_build_serial = 0;

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
			LocalVector<RID> &r_dirty_blas_list,
			bool p_no_duplicate_any_hit = false);
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
	// A front layer surface whose alpha is a coverage of its color (mix and OIT blending), which
	// rendering/pathtracing/transparent_shadows lets filter the shadow rays.
	static bool _transparent_shadow_blend(const void *p_surf);
	// Whether a MultiMesh surface goes into a merged BLAS (_build_merged_mm_blas()), or one TLAS instance per instance.
	bool _mm_fits_merged_blas(void *p_mesh_surface, uint32_t p_mm_count, const RTLodView *p_lod_view) const;
	bool _build_merged_mm_blas(
			RID p_mm_rid,
			RID p_mm_gpu_buffer,
			void *p_mesh_surface,
			uint32_t p_mm_count,
			uint32_t p_surface_index,
			uint32_t p_slot, // Of _merged_mm_slots(), claimed by build_tlas() for p_lod.
			uint32_t p_surface_counter,
			uint32_t p_lod,
			bool p_displace,
			bool p_reevaluate,
			bool p_camera_dependent, // Its vertex() reads the camera: each view evaluates it with its own.
			bool p_rebuild_only, // RT_BLAS_UPDATE_REBUILD: every update is a build.
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
	// miss color goes there with an alpha of 0, as the raster's clear color. Where they hit, `p_apply_alpha_multiplier`
	// writes an alpha of 0, as the raster's opaque pass does when the color's alpha is the upscaler's reactive mask
	// (RenderForwardClustered::_render_scene(), apply_alpha_multiplier): the raygen writes 1.
	void copy_output_texture(const RenderDataRD *p_render_data, bool p_keep_background, bool p_apply_alpha_multiplier);
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
	// rendering/pathtracing/max_transparency_layers: the layers the camera rays peel, 0 when the raster transparent pass
	// draws the transparent surfaces (RT_FLAG_TRANSPARENCY_PEELING).
	static uint32_t get_transparency_peel_layers();
	// A raster transparent pass surface the raygen peels when get_transparency_peel_layers() is above 0, if the TLAS holds
	// it (peeled_in_last_build()): a front layer one.
	static bool is_peel_candidate(const void *p_surf);
	// Whether the last build_tlas() put p_surf in the TLAS to be peeled: the raster transparent pass then leaves it out.
	bool peeled_in_last_build(const void *p_surf) const;
	// Environment.RT_DEBUG_TIMING_HEATMAP's ticks (R32F), made by update_uniform_set() for the RT_FLAG_TIMING_ENABLED
	// variant and freed once another one traces.
	bool rt_timing_has_texture(RenderSceneBuffersRD *p_render_buffers) const;
	RID rt_timing_get_texture(RenderSceneBuffersRD *p_render_buffers) const;

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
	// Ray Reconstruction's specular motion vectors (RT_FLAG_DLSS_RR_SPECULAR_MOTION_VECTORS, binding 44), RG16F in the
	// velocity's convention. Made at their first use, freed with the other guides.
	void dlss_rr_ensure_specular_motion_vectors(RenderSceneBuffersRD *p_render_buffers);
	bool dlss_rr_has_specular_motion_vectors(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_specular_motion_vectors(RenderSceneBuffersRD *p_render_buffers) const;

	// NRD's inputs (PT_DENOISER_NRD_REBLUR, RT_FLAG_NRD_ENABLED): per lobe, the primary surfaces' light divided by the lobe's
	// albedo with the hit distance of their bounce (bindings 41 and 43), the albedo, normal and roughness guides (9-11) and
	// the view depth (39).
	void nrd_ensure_buffers(RenderSceneBuffersRD *p_render_buffers);
	void nrd_free_buffers(RenderSceneBuffersRD *p_render_buffers);
	bool nrd_has_buffers(RenderSceneBuffersRD *p_render_buffers) const;
	// One of the RB_TEX_NRD_* textures.
	RID nrd_get_texture(RenderSceneBuffersRD *p_render_buffers, const StringName &p_name) const;
	// The sun SIGMA denoises this frame in the view, if any: the direction to it, the tangent of its angular radius.
	bool nrd_get_sigma_sun(const RenderDataRD *p_render_data, Vector3 &r_direction, float &r_tan_angular_radius) const;
	// Multiplies the denoised light of each lobe (or, before a denoiser runs, the raygen's own) back by its albedo onto the
	// path tracer's output, which holds the light that is not denoised (raytracing_nrd_composite.glsl). p_sun: SIGMA ran, the
	// sun's light times its denoised shadow is added too (RB_TEX_NRD_SUN).
	// p_direct: the raygen wrote the direct light signal this frame (RB_TEX_NRD_DIRECT), which is added too.
	void nrd_composite(RenderSceneBuffersRD *p_render_buffers, RID p_diffuse, RID p_specular, bool p_sun, bool p_direct);
	// Whether the raygen writes the direct light signal in this view this frame (rendering/pathtracing/
	// nrd_direct_light_denoising, update_uniform_set()).
	bool nrd_get_direct_light(const RenderDataRD *p_render_data) const;

	void register_raytracing_buffer_dependencies(RD::RaytracingListID p_list);
	// The buffers written per frame that the shaders read through the addresses in RT_GeometryData and RT_MaterialData.
	void get_buffers_read_by_address(LocalVector<RID> &r_buffers);

	// The material textures, for the shadow rays' alpha test in the transparent pass (USE_RT_SHADOWS), in a set of
	// p_shader's layout.
	RID get_fragment_texture_set(RID p_shader, uint32_t p_set_index) const {
		return bindless_block ? bindless_block->get_secondary_uniform_set(p_shader, p_set_index, BindlessBlock::SECONDARY_LAYOUT_FRAGMENT) : RID();
	}
	// The same table for the volumetric fog's ray queries (RTFogShadows), which owns its set.
	BindlessBlock *get_bindless_block() const { return bindless_block; }

	SceneShaderRaytracing *get_shader() const { return shader; }

	// RenderingServer.get_rendering_not_ready_reasons(): what the last traced frame left out or traced at an older state,
	// and RT_SHADER_COMPILATION while hit groups or programs compile.
	BitField<RSE::RenderingNotReadyReason> get_not_ready_reasons() const;

	RID get_bindless_uniform_set() const { return bindless_uniform_set; }
	RID get_bindless_array_uniform_set() const { return bindless_array_uniform_set; }
	RID get_mat_ubo_pool_buffer() const { return mat_ubo_pool_buffer; }

	~RenderRaytracing();
};

} // namespace RendererSceneRenderImplementation
