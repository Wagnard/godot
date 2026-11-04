#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

// Bakes a mesh surface's cut-out with its material into an opacity micromap: one entry per triangle, 4-state, at one
// subdivision level. Each micro-triangle is evaluated as the any-hit shaders evaluate a hit (raytracing_omm_bake.glsl's
// custom variant runs the material's vertex() at the corners and its fragment code; the standard one, hit group 0's
// alpha) at points spaced at most half a texel apart in every texture the decision samples. All of them pass: opaque;
// none: transparent; otherwise, or when one sits on the threshold (RT_OMM_ALPHA_TIE), unknown, and the any-hit shader
// decides as before. The decision is taken at the textures' base level, which the any-hit shaders test at any distance
// too (NVIDIA OMM SDK integration guide: "Pick a texture mip and always use that version").
//
// Output: the entries' 2-bit states in micro-triangle order (the space-filling curve of VK_EXT_opacity_micromap and
// D3D12's opacity micromaps), LSB first, and per triangle its 8-byte description (byte offset, level, format).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define GLSL 1
#define RT_NO_HIT_BUILTINS 1

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_hit_inc.glsl"
// clang-format on

layout(set = 0, binding = 2, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

layout(set = 0, binding = 14, std430) readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;

#include "raytracing_samplers_inc.glsl"

struct OmmBakeJob {
	GeometryData geom; // The mesh surface as stored.
	uint64_t material_address; // Its uniforms (custom shaders).
	uint triangle_count;
	uint level;
	uint data_offset; // In words, in the data buffer.
	uint desc_offset; // In words, in the description buffer.
	// Hit group 0 (BaseMaterial3D): the albedo texture's alpha times the color's, and the vertex color's.
	uint albedo_texture_idx;
	float albedo_alpha;
	vec2 uv1_scale;
	vec2 uv1_offset;
	uint use_vertex_color;
	uint max_samples; // Per micro-triangle edge.
	uint pad[2];
};

layout(set = 0, binding = 30, std430) readonly buffer OmmBakeJobs {
	OmmBakeJob jobs[];
};

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];
layout(set = 2, binding = 0) uniform texture2DArray bindless_texture_arrays[];

layout(set = 3, binding = 0, std430) restrict buffer OmmData {
	uint v[];
}
omm_data;

layout(set = 3, binding = 1, std430) restrict writeonly buffer OmmDescriptions {
	uint v[];
}
omm_descs;

// Bound so the graph orders the material buffer's upload before this pass; read through its address.
layout(set = 3, binding = 2, std430) restrict readonly buffer MaterialDependency {
	uint v[];
}
material_dependency;

// The estimate pass's output: per triangle, the samples its bake takes, in units of RT_OMM_COST_UNIT.
layout(set = 3, binding = 3, std430) restrict writeonly buffer OmmCost {
	uint v[];
}
omm_cost;

// render_raytracing.h's RT_OMM_COST_UNIT.
#define RT_OMM_COST_UNIT 1024u

layout(push_constant, std430) uniform Params {
	uint job_index;
	uint never; // Always 0: keeps every binding statically used, so all slots share the set layouts.
	uint first; // The dispatch's first triangle (estimate) or micro-triangle (bake).
	uint estimate; // 1: one thread per triangle estimates its bake's samples (omm_cost); 0: one per micro-triangle bakes.
}
params;

// The texel footprint of the sampled textures, measured while rt_omm_measuring is set: the largest distance in texels
// between two evaluations the gradients span.
bool rt_omm_measuring = false;
float rt_omm_footprint = 0.0;

// The material's texture() calls become textureGrad() (scene_shader_raytracing.cpp). A sampler built in the call cannot be
// passed to a function: the macro below leaves the call as it is and passes its x gradient through rt_omm_record(),
// which records the footprint (the texture's size by the gradients) and returns it. Overloads by the size's and the
// gradients' types: 2D, 2D array (its layers ignored), 3D; a cube map (a direction) has no footprint to measure.
vec2 rt_omm_record(ivec2 p_size, vec2 p_dx, vec2 p_dy) {
	if (rt_omm_measuring) {
		rt_omm_footprint = max(rt_omm_footprint, max(length(p_dx * vec2(p_size)), length(p_dy * vec2(p_size))));
	}
	return p_dx;
}
vec2 rt_omm_record(ivec3 p_size, vec2 p_dx, vec2 p_dy) {
	return rt_omm_record(p_size.xy, p_dx, p_dy);
}
vec3 rt_omm_record(ivec3 p_size, vec3 p_dx, vec3 p_dy) {
	if (rt_omm_measuring) {
		rt_omm_footprint = max(rt_omm_footprint, max(length(p_dx * vec3(p_size)), length(p_dy * vec3(p_size))));
	}
	return p_dx;
}
vec3 rt_omm_record(ivec2 p_size, vec3 p_dx, vec3 p_dy) {
	return p_dx;
}
// Not expanded again inside its own expansion: the inner textureGrad() is the built-in.
#define textureGrad(m_sampler, m_coord, m_dx, m_dy) textureGrad(m_sampler, m_coord, rt_omm_record(textureSize(m_sampler, 0), m_dx, m_dy), m_dy)

#ifndef RT_OMM_BAKE_STANDARD
#include "raytracing_custom_globals_inc.glsl"
#endif

// The micro-triangle index of discrete barycentrics (u, v, w), u + v + w = 2^level - 1 for an upright micro-triangle and
// 2^level - 2 for an inverted one: VK_EXT_opacity_micromap's reference conversion.
uint rt_omm_prefix_eor(uint p_x) {
	uint x = p_x;
	x ^= (x >> 1) & 0x7fff7fffu;
	x ^= (x >> 2) & 0x3fff3fffu;
	x ^= (x >> 4) & 0x0fff0fffu;
	x ^= (x >> 8) & 0x00ff00ffu;
	return x;
}

// The low 16 bits of p_x in the even bits, those of p_y in the odd bits.
uint rt_omm_interleave_bits(uint p_x, uint p_y) {
	uint x = p_x & 0xffffu;
	uint y = p_y & 0xffffu;
	x = (x | (x << 8)) & 0x00ff00ffu;
	x = (x | (x << 4)) & 0x0f0f0f0fu;
	x = (x | (x << 2)) & 0x33333333u;
	x = (x | (x << 1)) & 0x55555555u;
	y = (y | (y << 8)) & 0x00ff00ffu;
	y = (y | (y << 4)) & 0x0f0f0f0fu;
	y = (y | (y << 2)) & 0x33333333u;
	y = (y | (y << 1)) & 0x55555555u;
	return x | (y << 1);
}

uint rt_omm_dbary_to_index(uint p_u, uint p_v, uint p_w, uint p_level) {
	const uint coord_mask = (1u << p_level) - 1u;
	uint b0 = ~(p_u ^ p_w) & coord_mask;
	uint t = (p_u ^ p_v) & b0;
	uint c = (((p_u & p_v & p_w) | (~p_u & ~p_v & ~p_w)) & coord_mask) << 16;
	uint f = rt_omm_prefix_eor(t | c) ^ p_u;
	uint b1 = (f & ~b0) | t;
	return rt_omm_interleave_bits(b0, b1);
}

GeometryData rt_omm_geom;
uint rt_omm_i0;
uint rt_omm_i1;
uint rt_omm_i2;

#ifdef RT_OMM_BAKE_STANDARD
vec2 rt_omm_uv[3];
vec4 rt_omm_color[3];
#endif

// Prepares the triangle's corners (vertex() for a custom shader).
void rt_omm_prepare_triangle(OmmBakeJob p_job) {
#ifdef RT_OMM_BAKE_STANDARD
	uint corner_index[3] = uint[3](rt_omm_i0, rt_omm_i1, rt_omm_i2);
	for (int corner = 0; corner < 3; corner++) {
		uint index = corner_index[corner];
		rt_omm_uv[corner] = fetch_uv(rt_omm_geom, index, index, index, vec3(1.0, 0.0, 0.0)) * p_job.uv1_scale + p_job.uv1_offset;
		rt_omm_color[corner] = fetch_color(rt_omm_geom, index, index, index, vec3(1.0, 0.0, 0.0));
	}
#else
	material = CustomMaterialUniforms(p_job.material_address);
	// Nothing the decision reads depends on the instance or the view (rt_alpha_dependencies.cpp): identity.
	read_model_matrix = mat4(1.0);
	model_normal_matrix = mat3(1.0);
	read_view_matrix = mat4(1.0);
	inv_view_matrix = mat4(1.0);
	rt_modelview_matrix = mat4(1.0);
	rt_modelview_normal_matrix = mat3(1.0);
	uint rt_corner_index[3] = uint[3](rt_omm_i0, rt_omm_i1, rt_omm_i2);
#ifdef ENABLE_INSTANCE_CUSTOM
	instance_custom = vec4(0.0);
#endif
	for (int rt_corner = 0; rt_corner < 3; rt_corner++) {
		uint rt_index = rt_corner_index[rt_corner];
		vec3 rt_rest = fetch_mesh_position(rt_omm_geom, rt_index);
		TBNResult rt_tbn;
		rt_tbn.normal = vec3(0.0, 1.0, 0.0);
		rt_tbn.tangent = vec3(1.0, 0.0, 0.0);
		rt_tbn.binormal = vec3(0.0, 0.0, 1.0);
		if (rt_omm_geom.normal_byte_offset != OFFSET_NONE) {
			rt_tbn = (rt_omm_geom.flags & FLAG_COMPRESSED) != 0u ? fetch_tbn_compressed_vertex(rt_omm_geom, rt_index) : fetch_tbn_uncompressed_vertex(rt_omm_geom, rt_index);
		}
		vertex = rt_rest;
		normal = rt_tbn.normal;
		tangent = rt_tbn.tangent;
		binormal = rt_tbn.binormal;
		uv_interp = fetch_uv(rt_omm_geom, rt_index, rt_index, rt_index, vec3(1.0, 0.0, 0.0));
		uv2_interp = fetch_uv2(rt_omm_geom, rt_index, rt_index, rt_index, vec3(1.0, 0.0, 0.0));
		color_interp = fetch_color(rt_omm_geom, rt_index, rt_index, rt_index, vec3(1.0, 0.0, 0.0));
#ifdef CUSTOM0_USED
		custom0_attrib = fetch_custom(rt_omm_geom, 0u, rt_index);
#endif
#ifdef CUSTOM1_USED
		custom1_attrib = fetch_custom(rt_omm_geom, 1u, rt_index);
#endif
#ifdef CUSTOM2_USED
		custom2_attrib = fetch_custom(rt_omm_geom, 2u, rt_index);
#endif
#ifdef CUSTOM3_USED
		custom3_attrib = fetch_custom(rt_omm_geom, 3u, rt_index);
#endif
		rt_vertex_id = int(rt_index);

		/* RT_CUSTOM_VERTEX_CALL */

		rt_corner_vertex[rt_corner] = vertex;
		rt_corner_normal[rt_corner] = normal;
		rt_corner_tangent[rt_corner] = tangent;
		rt_corner_binormal[rt_corner] = binormal;
		rt_corner_uv[rt_corner] = uv_interp;
		rt_corner_uv2[rt_corner] = uv2_interp;
		rt_corner_color[rt_corner] = color_interp;
		/* RT_CUSTOM_VARYING_SAVE */
	}
	rt_derivatives_ready = true;
#endif
}

// An alpha this close to the threshold decides nothing: the hit's barycentrics, interpolated by the hardware, differ from
// the bake's by rounding, and a cut-out's edge filtered between alpha 0 and 1 runs exactly along the micro-triangles'
// edges when its texels align with them (the corner of a checkerboard square at 0.5).
#define RT_OMM_ALPHA_TIE 1e-3

#define RT_OMM_SAMPLE_TRANSPARENT 0u
#define RT_OMM_SAMPLE_OPAQUE 1u
#define RT_OMM_SAMPLE_TIE 2u

uint rt_omm_compare(float p_alpha, float p_threshold) {
	if (abs(p_alpha - p_threshold) < RT_OMM_ALPHA_TIE) {
		return RT_OMM_SAMPLE_TIE;
	}
	return p_alpha >= p_threshold ? RT_OMM_SAMPLE_OPAQUE : RT_OMM_SAMPLE_TRANSPARENT;
}

// How a ray fares in the any-hit test at barycentrics p_bary (weights of the triangle's vertices 0, 1, 2), the texture
// footprint spanning p_bary_dx and p_bary_dy: RT_OMM_SAMPLE_*.
uint rt_omm_sample(vec3 p_bary, vec3 p_bary_dx, vec3 p_bary_dy) {
#ifdef RT_OMM_BAKE_STANDARD
	OmmBakeJob job = jobs[params.job_index];
	vec2 uv = rt_omm_uv[0] * p_bary.x + rt_omm_uv[1] * p_bary.y + rt_omm_uv[2] * p_bary.z;
	vec2 uv_dx = rt_omm_uv[0] * p_bary_dx.x + rt_omm_uv[1] * p_bary_dx.y + rt_omm_uv[2] * p_bary_dx.z;
	vec2 uv_dy = rt_omm_uv[0] * p_bary_dy.x + rt_omm_uv[1] * p_bary_dy.y + rt_omm_uv[2] * p_bary_dy.z;
	float alpha = textureGrad(sampler2D(bindless_textures[nonuniformEXT(job.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, uv_dx, uv_dy).a;
	alpha *= job.albedo_alpha;
	if (job.use_vertex_color != 0u) {
		alpha *= (rt_omm_color[0] * p_bary.x + rt_omm_color[1] * p_bary.y + rt_omm_color[2] * p_bary.z).a;
	}
	return rt_omm_compare(alpha, 0.5);
#else
	rt_hit_bary = p_bary;
	rt_hit_bary_ddx = p_bary_dx;
	rt_hit_bary_ddy = p_bary_dy;
	rt_set_fragment_inputs(p_bary);
	rt_discarded = false;

	// Fragment outputs with raster's defaults, as raytracing_custom_fragment_inc.glsl declares them.
	vec3 albedo = vec3(1.0);
	float alpha = 1.0;
	float metallic = 0.0;
	float roughness = 1.0;
	float specular = 0.5;
	vec3 emission = vec3(0.0);
	float emission_indirect = 1.0;
	vec3 normal_map = vec3(0.5, 0.5, 1.0);
	float normal_map_depth = 1.0;
	float ao = 1.0;
	float ao_light_affect = 0.0;
	vec3 backlight = vec3(0.0);
	float sss_strength = 0.0;
	float rim = 0.0;
	float rim_tint = 0.0;
	float clearcoat = 0.0;
	float clearcoat_roughness = 0.0;
	float anisotropy = 0.0;
	vec2 anisotropy_flow = vec2(1.0, 0.0);
	float alpha_scissor_threshold = 0.0;
	float alpha_hash_scale = 1.0;
	float alpha_antialiasing_edge = 0.0;
	vec2 alpha_texture_coordinate = vec2(0.0);
	float premul_alpha = 1.0;
	float rt_depth = 0.0;
	vec4 custom_radiance = vec4(0.0);
	vec4 custom_irradiance = vec4(0.0);
	vec4 transmittance_color = vec4(0.0);
	float transmittance_depth = 0.0;
	float transmittance_boost = 0.0;
	vec3 light_vertex = vertex;

	{
		/* RT_CUSTOM_FRAGMENT_CODE */
	}
	// The any-hit shader's test.
	if (rt_discarded) {
		return RT_OMM_SAMPLE_TRANSPARENT;
	}
	return alpha_scissor_threshold > 0.0 ? rt_omm_compare(alpha, alpha_scissor_threshold) : RT_OMM_SAMPLE_OPAQUE;
#endif
}

void main() {
	OmmBakeJob job = jobs[params.job_index];
	const uint micro_count = 1u << (job.level * 2u);
	// _rt_dispatch_linear(): the groups beyond the device's X limit go on Y.
	const uint thread = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * gl_WorkGroupSize.x;
	// The estimate's threads are triangles from params.first on; the bake's, micro-triangles (all the triangles' in a row).
	const uint item = params.first + thread;
	const uint triangle = params.estimate != 0u ? item : item / micro_count;
	const uint micro = item % micro_count;

	if (params.never != 0u) {
		float value = global_shader_uniforms.data[0].x + scene_data_block.data.time;
		value += texelFetch(sampler2D(bindless_textures[nonuniformEXT(triangle)], SAMPLER_NEAREST_CLAMP), ivec2(0), 0).r;
		value += texelFetch(sampler2DArray(bindless_texture_arrays[nonuniformEXT(triangle)], SAMPLER_NEAREST_CLAMP), ivec3(0), 0).r;
		omm_data.v[0] = material_dependency.v[0] + floatBitsToUint(value);
		omm_cost.v[0] = floatBitsToUint(value);
	}

	if (triangle >= job.triangle_count) {
		return;
	}

	const uint n = 1u << job.level;
	if (params.estimate != 0u) {
		// The samples the bake below takes for this triangle, from the footprint of a micro-triangle's edges at its centre
		// (the same measure, over the whole triangle's edges divided by 2^level): its micro-triangles beyond max_samples
		// stay unknown after one evaluation.
		rt_omm_geom = job.geom;
		get_triangle_indices_ex(rt_omm_geom, triangle, rt_omm_i0, rt_omm_i1, rt_omm_i2);
		rt_omm_prepare_triangle(job);
		rt_omm_measuring = true;
		rt_omm_footprint = 0.0;
		rt_omm_sample(vec3(1.0 / 3.0), vec3(-1.0, 1.0, 0.0) / float(n), vec3(-1.0, 0.0, 1.0) / float(n));
		rt_omm_measuring = false;
		const uint steps = max(1u, uint(ceil(rt_omm_footprint * 2.0)));
		const uint64_t samples = steps <= job.max_samples ? uint64_t(steps + 1u) * uint64_t(steps + 2u) / 2u + 1u : 1u;
		const uint64_t units = (samples * uint64_t(micro_count) + uint64_t(RT_OMM_COST_UNIT - 1u)) / uint64_t(RT_OMM_COST_UNIT);
		omm_cost.v[triangle] = uint(min(units, uint64_t(0xFFFFFFFFu)));
		return;
	}

	const uint bits_per_entry = micro_count * 2u;
	if (micro == 0u) {
		// D3D12_RAYTRACING_OPACITY_MICROMAP_DESC / VkMicromapTriangleEXT: byte offset, subdivision level, format (4-state).
		omm_descs.v[job.desc_offset + triangle * 2u] = (triangle * bits_per_entry) / 8u;
		omm_descs.v[job.desc_offset + triangle * 2u + 1u] = job.level | (2u << 16);
	}

	rt_omm_geom = job.geom;
	get_triangle_indices_ex(rt_omm_geom, triangle, rt_omm_i0, rt_omm_i1, rt_omm_i2);
	rt_omm_prepare_triangle(job);

	// The micro-triangle, enumerated row by row: row v holds 2^level - v upright micro-triangles and one inverted fewer.
	uint row = 0u;
	uint first = 0u;
	while (row < n && micro >= first + 2u * (n - row) - 1u) {
		first += 2u * (n - row) - 1u;
		row++;
	}
	const uint position = micro - first;
	const bool upright = (position & 1u) == 0u;
	const uint column = position >> 1;
	// Its corners in (u, v), the weights of the vertices 1 and 2 (the hit's barycentrics), in units of 1 / 2^level.
	vec2 c0, c1, c2;
	if (upright) {
		c0 = vec2(column, row);
		c1 = vec2(column + 1u, row);
		c2 = vec2(column, row + 1u);
	} else {
		c0 = vec2(column + 1u, row + 1u);
		c1 = vec2(column, row + 1u);
		c2 = vec2(column + 1u, row);
	}
	const uint index = rt_omm_dbary_to_index(column, row, upright ? n - 1u - column - row : n - 2u - column - row, job.level);
	c0 /= float(n);
	c1 /= float(n);
	c2 /= float(n);
	const vec3 b0 = vec3(1.0 - c0.x - c0.y, c0.x, c0.y);
	const vec3 b1 = vec3(1.0 - c1.x - c1.y, c1.x, c1.y);
	const vec3 b2 = vec3(1.0 - c2.x - c2.y, c2.x, c2.y);

	// The texel footprint of the micro-triangle's edges, from the textures the decision samples: half a texel between
	// evaluations. Beyond max_samples (a bound on one thread's work), the micro-triangle stays unknown.
	rt_omm_measuring = true;
	rt_omm_footprint = 0.0;
	rt_omm_sample((b0 + b1 + b2) / 3.0, b1 - b0, b2 - b0);
	rt_omm_measuring = false;
	const uint steps = max(1u, uint(ceil(rt_omm_footprint * 2.0)));

	uint state = 3u; // Unknown opaque.
	if (steps <= job.max_samples) {
		const vec3 step_dx = (b1 - b0) / float(steps);
		const vec3 step_dy = (b2 - b0) / float(steps);
		bool any_opaque = false;
		bool any_transparent = false;
		for (uint j = 0u; j <= steps && !(any_opaque && any_transparent); j++) {
			for (uint i = 0u; i + j <= steps; i++) {
				// Level 0, as the any-hit shaders test it: zero gradients, whatever the sampler's mip bias.
				const uint sample_state = rt_omm_sample(b0 + step_dx * float(i) + step_dy * float(j), vec3(0.0), vec3(0.0));
				any_opaque = any_opaque || sample_state != RT_OMM_SAMPLE_TRANSPARENT;
				any_transparent = any_transparent || sample_state != RT_OMM_SAMPLE_OPAQUE;
				if (any_opaque && any_transparent) {
					break;
				}
			}
		}
		state = any_opaque && any_transparent ? 3u : (any_opaque ? 1u : 0u);
	}

	if (state != 0u) {
		const uint bit = job.data_offset * 32u + triangle * bits_per_entry + index * 2u;
		atomicOr(omm_data.v[bit >> 5], state << (bit & 31u));
	}
}
