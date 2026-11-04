#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

// Runs a material's vertex() on every vertex of a mesh surface, so the BLAS holds the geometry raster draws.
// The material code is injected per hit group slot, as in the hit shaders.
//
// Output, per job: [float3 displaced position x N] [rest TBN block x N, the mesh's uncompressed layout]
// [float3 rest position x N] [float3 previous frame's displaced position x N]. The first two sections are the
// deformed-path vertex buffer; the hit shaders run vertex() again on the rest data at the triangle's vertices to get
// raster's varyings; the last one gives the motion vectors.

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

struct DisplaceJob {
	mat4 model_matrix;
	mat4 inv_model_matrix;
	mat4 prev_model_matrix; // The current one for a teleported instance, as raster's motion vectors.
	mat4 prev_inv_model_matrix;
	// The mesh surface as stored (possibly compressed). A skinned or blend shape surface's: its skinning output (this
	// frame's at vertex_address, the previous frame's at prev_vertex_address), uncompressed, as raster's vertex input.
	GeometryData geom;
	uint64_t material_address;
	uint tbn_words; // 0, 1 (normal) or 2 (normal and tangent) per vertex.
	uint instance_uniforms_ofs;
	// A merged MultiMesh when mm_instance_count is not 0: one thread per vertex of each instance, MODEL_MATRIX = model_matrix
	// * the instance's transform, the positions written over the merged ones (in the MultiMesh's space, the BLAS's).
	uint mm_instance_count;
	uint mm_vertex_count; // Per instance, in the merged buffers.
	uint mm_stride; // Of the MultiMesh buffer, in floats.
	uint mm_offset; // Instances to skip in it.
	uint mm_use_remap; // 1: the merged vertices are a LOD view's, mm_remap maps them to the mesh's.
	uint mm_color_offset; // In floats in an instance, or 0xFFFFFFFF without colors.
	uint mm_custom_offset; // Same for the custom data.
	// 1: the BLAS is refit with this output, which must keep every vertex's activity of its last build (main()).
	uint refit_guard;
};

layout(set = 0, binding = 30, std430) readonly buffer DisplaceJobs {
	DisplaceJob jobs[];
};

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];
layout(set = 2, binding = 0) uniform texture2DArray bindless_texture_arrays[];

layout(set = 3, binding = 0, std430) restrict writeonly buffer DisplaceOutput {
	uint v[];
}
dst;

// Bound so the graph orders the material buffer's upload before this pass; read through its address.
layout(set = 3, binding = 1, std430) restrict readonly buffer MaterialDependency {
	uint v[];
}
material_dependency;

layout(set = 3, binding = 2, std430) restrict readonly buffer MultiMeshTransforms {
	float v[];
}
mm_transforms;

layout(set = 3, binding = 3, std430) restrict readonly buffer MultiMeshRemap {
	uint v[];
}
mm_remap;

// The previous frame's displaced positions of a merged MultiMesh, for its motion vectors.
layout(set = 3, binding = 4, std430) restrict writeonly buffer MultiMeshPrevious {
	uint v[];
}
mm_previous;

// The positions the BLAS was last built or refit from (the deformed path's own copy), read by a refit job.
layout(set = 3, binding = 5, std430) restrict readonly buffer BlasPositions {
	uint v[];
}
blas_positions;

// Per job: the vertices a refit job kept as the BLAS has them; any makes the CPU rebuild that BLAS a few frames later.
layout(set = 3, binding = 6, std430) restrict buffer GuardCounts {
	uint v[];
}
guard_counts;

// Bound so the graph orders the skinning of this frame (MeshStorage::update_mesh_instances()) before this pass; read
// through the job's addresses.
layout(set = 3, binding = 7, std430) restrict readonly buffer DeformedVertices {
	uint v[];
}
deformed_vertices;

layout(set = 3, binding = 8, std430) restrict readonly buffer DeformedPreviousVertices {
	uint v[];
}
deformed_previous_vertices;

layout(push_constant, std430) uniform Params {
	uint job_index;
	uint never; // Always 0: keeps every binding statically used, so all slots share the set layouts.
	uint pad[2];
}
params;

#include "raytracing_custom_globals_inc.glsl"

vec3 fetch_position(in GeometryData p_geom, uint p_index) {
	if ((p_geom.flags & FLAG_COMPRESSED) != 0u) {
		Uint32Buffer vb = Uint32Buffer(p_geom.vertex_address);
		uint w0 = vb.v[p_index * 2u];
		uint w1 = vb.v[p_index * 2u + 1u];
		vec3 unorm = vec3(float(w0 & 0xFFFFu), float(w0 >> 16), float(w1 & 0xFFFFu)) / 65535.0;
		return unorm * vec3(p_geom.aabb_size_x, p_geom.aabb_size_y, p_geom.aabb_size_z) + vec3(p_geom.aabb_pos_x, p_geom.aabb_pos_y, p_geom.aabb_pos_z);
	}
	FloatBuffer vb = FloatBuffer(p_geom.vertex_address);
	return vec3(vb.v[p_index * 3u], vb.v[p_index * 3u + 1u], vb.v[p_index * 3u + 2u]);
}

// A MultiMesh instance's color, multiplied into COLOR as raster does.
vec4 rt_displace_instance_color = vec4(1.0);

uint pack_unorm16x2(vec2 p_value) {
	uvec2 u = uvec2(clamp(p_value, 0.0, 1.0) * 65535.0);
	return u.x | (u.y << 16);
}

// Runs vertex() on a rest vertex with this frame's or the previous frame's data; returns the position in the mesh
// space of that frame's transform.
vec3 displace_vertex(DisplaceJob p_job, vec3 p_rest_position, TBNResult p_tbn, uint p_index, bool p_previous) {
	mat4 model = p_previous ? p_job.prev_model_matrix : p_job.model_matrix;
	mat4 inv_model = p_previous ? p_job.prev_inv_model_matrix : p_job.inv_model_matrix;
	read_model_matrix = model;
	model_normal_matrix = transpose(mat3(inv_model));
	if (p_previous) {
		read_view_matrix = transpose(mat4(scene_data_block.prev_data.view_matrix[0], scene_data_block.prev_data.view_matrix[1], scene_data_block.prev_data.view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
		inv_view_matrix = transpose(mat4(scene_data_block.prev_data.inv_view_matrix[0], scene_data_block.prev_data.inv_view_matrix[1], scene_data_block.prev_data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
		projection_matrix = scene_data_block.prev_data.projection_matrix;
		inv_projection_matrix = scene_data_block.prev_data.inv_projection_matrix;
		read_viewport_size = scene_data_block.prev_data.viewport_size;
		global_time = scene_data_block.prev_data.time;
	} else {
		read_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0], scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
		inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0], scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
		projection_matrix = scene_data_block.data.projection_matrix;
		inv_projection_matrix = scene_data_block.data.inv_projection_matrix;
		read_viewport_size = scene_data_block.data.viewport_size;
		global_time = scene_data_block.data.time;
	}
	rt_modelview_matrix = read_view_matrix * read_model_matrix;
	rt_modelview_normal_matrix = mat3(rt_modelview_matrix);

#ifdef VERTEX_WORLD_COORDS_USED
	vertex = (read_model_matrix * vec4(p_rest_position, 1.0)).xyz;
	normal = normalize(model_normal_matrix * p_tbn.normal);
	tangent = normalize(mat3(read_model_matrix) * p_tbn.tangent);
	binormal = normalize(mat3(read_model_matrix) * p_tbn.binormal);
#else
	vertex = p_rest_position;
	normal = p_tbn.normal;
	tangent = p_tbn.tangent;
	binormal = p_tbn.binormal;
#endif
	uv_interp = fetch_uv(p_job.geom, p_index, p_index, p_index, vec3(1.0, 0.0, 0.0));
	uv2_interp = fetch_uv2(p_job.geom, p_index, p_index, p_index, vec3(1.0, 0.0, 0.0));
	color_interp = fetch_color(p_job.geom, p_index, p_index, p_index, vec3(1.0, 0.0, 0.0)) * rt_displace_instance_color;
#ifdef CUSTOM0_USED
	custom0_attrib = fetch_custom(p_job.geom, 0u, p_index);
#endif
#ifdef CUSTOM1_USED
	custom1_attrib = fetch_custom(p_job.geom, 1u, p_index);
#endif
#ifdef CUSTOM2_USED
	custom2_attrib = fetch_custom(p_job.geom, 2u, p_index);
#endif
#ifdef CUSTOM3_USED
	custom3_attrib = fetch_custom(p_job.geom, 3u, p_index);
#endif
	rt_vertex_id = int(p_index);

	/* RT_CUSTOM_VERTEX_CALL */

	// Back to mesh space, the space of the BLAS.
#if defined(SKIP_TRANSFORM_USED)
	vec3 displaced = (inv_model * (inv_view_matrix * vec4(vertex, 1.0))).xyz;
#elif defined(VERTEX_WORLD_COORDS_USED)
	vec3 displaced = (inv_model * vec4(vertex, 1.0)).xyz;
#elif defined(RT_MODELVIEW_WRITTEN)
	// Through the MODELVIEW_MATRIX vertex() wrote (a billboard), as raster draws it, then back from view space.
	vec3 displaced = (inv_model * (inv_view_matrix * (rt_modelview_matrix * vec4(vertex, 1.0)))).xyz;
#else
	vec3 displaced = vertex;
#endif
	// A point vertex() removed (NaN) makes its triangles inactive in the BLAS, which needs a NaN x. An infinite one too
	// (an effect dividing by a zero duration): raster clips it, but a BLAS built from it is invalid.
	if (any(isnan(displaced)) || any(isinf(displaced))) {
		displaced = vec3(uintBitsToFloat(0x7fc00000u));
	}
	return displaced;
}

void main() {
	DisplaceJob job = jobs[params.job_index];
	// _rt_dispatch_linear(): the groups beyond the device's X limit go on Y.
	uint index = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * gl_WorkGroupSize.x;

	if (params.never != 0u) {
		float value = global_shader_uniforms.data[0].x + scene_data_block.data.time;
		value += texelFetch(sampler2D(bindless_textures[nonuniformEXT(index)], SAMPLER_NEAREST_CLAMP), ivec2(0), 0).r;
		value += texelFetch(sampler2DArray(bindless_texture_arrays[nonuniformEXT(index)], SAMPLER_NEAREST_CLAMP), ivec3(0), 0).r;
		dst.v[0] = material_dependency.v[0] + floatBitsToUint(value) + floatBitsToUint(mm_transforms.v[0]) + mm_remap.v[0] + blas_positions.v[0] + deformed_vertices.v[0] + deformed_previous_vertices.v[0];
		mm_previous.v[0] = 0u;
		guard_counts.v[0] = 0u;
	}

	if (job.mm_instance_count != 0u) {
		if (index >= job.mm_instance_count * job.mm_vertex_count) {
			return;
		}
		uint instance = index / job.mm_vertex_count;
		uint merged_vertex = index % job.mm_vertex_count;
		uint source_vertex = job.mm_use_remap != 0u ? mm_remap.v[merged_vertex] : merged_vertex;

		// The instance's transform: three rows of the MultiMesh buffer.
		uint base = (job.mm_offset + instance) * job.mm_stride;
		mat4 instance_transform = transpose(mat4(
				vec4(mm_transforms.v[base + 0u], mm_transforms.v[base + 1u], mm_transforms.v[base + 2u], mm_transforms.v[base + 3u]),
				vec4(mm_transforms.v[base + 4u], mm_transforms.v[base + 5u], mm_transforms.v[base + 6u], mm_transforms.v[base + 7u]),
				vec4(mm_transforms.v[base + 8u], mm_transforms.v[base + 9u], mm_transforms.v[base + 10u], mm_transforms.v[base + 11u]),
				vec4(0.0, 0.0, 0.0, 1.0)));
		if (job.mm_color_offset != 0xFFFFFFFFu) {
			uint o = base + job.mm_color_offset;
			rt_displace_instance_color = vec4(mm_transforms.v[o], mm_transforms.v[o + 1u], mm_transforms.v[o + 2u], mm_transforms.v[o + 3u]);
		}
		if (job.mm_custom_offset != 0xFFFFFFFFu) {
			uint o = base + job.mm_custom_offset;
			instance_custom = vec4(mm_transforms.v[o], mm_transforms.v[o + 1u], mm_transforms.v[o + 2u], mm_transforms.v[o + 3u]);
		}
		DisplaceJob instance_job = job;
		instance_job.model_matrix = job.model_matrix * instance_transform;
		instance_job.inv_model_matrix = inverse(instance_job.model_matrix);
		instance_job.prev_model_matrix = job.prev_model_matrix * instance_transform;
		instance_job.prev_inv_model_matrix = inverse(instance_job.prev_model_matrix);

		GeometryData mm_geom = job.geom;
		vec3 mm_rest = fetch_position(mm_geom, source_vertex);
		TBNResult mm_tbn;
		mm_tbn.normal = vec3(0.0, 1.0, 0.0);
		mm_tbn.tangent = vec3(1.0, 0.0, 0.0);
		mm_tbn.binormal = vec3(0.0, 0.0, 1.0);
		mm_tbn.bitangent_sign = 1.0;
		if (mm_geom.normal_byte_offset != OFFSET_NONE) {
			mm_tbn = (mm_geom.flags & FLAG_COMPRESSED) != 0u ? fetch_tbn_compressed_vertex(mm_geom, source_vertex) : fetch_tbn_uncompressed_vertex(mm_geom, source_vertex);
		}
		material = CustomMaterialUniforms(job.material_address);
		rt_instance_uniforms_ofs = job.instance_uniforms_ofs;
		rt_instance_id = int(instance);

		vec3 mm_previous_position = displace_vertex(instance_job, mm_rest, mm_tbn, source_vertex, true);
		vec3 mm_position = displace_vertex(instance_job, mm_rest, mm_tbn, source_vertex, false);
		// A removed point (NaN) collapses onto the instance's origin instead: the BLAS is refit every frame, and a
		// refit cannot turn triangles on or off.
		mm_previous_position = any(isnan(mm_previous_position)) ? vec3(0.0) : mm_previous_position;
		mm_position = any(isnan(mm_position)) ? vec3(0.0) : mm_position;
		mm_previous_position = (instance_transform * vec4(mm_previous_position, 1.0)).xyz;
		mm_position = (instance_transform * vec4(mm_position, 1.0)).xyz;

		dst.v[index * 3u] = floatBitsToUint(mm_position.x);
		dst.v[index * 3u + 1u] = floatBitsToUint(mm_position.y);
		dst.v[index * 3u + 2u] = floatBitsToUint(mm_position.z);
		mm_previous.v[index * 3u] = floatBitsToUint(mm_previous_position.x);
		mm_previous.v[index * 3u + 1u] = floatBitsToUint(mm_previous_position.y);
		mm_previous.v[index * 3u + 2u] = floatBitsToUint(mm_previous_position.z);
		return;
	}

	GeometryData geom = job.geom;
	if (index >= geom.vertex_count) {
		return;
	}

	// The rest vertex, decoded as the hit shaders decode it.
	vec3 rest_position = fetch_position(geom, index);
	TBNResult tbn;
	tbn.normal = vec3(0.0, 1.0, 0.0);
	tbn.tangent = vec3(1.0, 0.0, 0.0);
	tbn.binormal = vec3(0.0, 0.0, 1.0);
	tbn.bitangent_sign = 1.0;
	if (geom.normal_byte_offset != OFFSET_NONE) {
		tbn = (geom.flags & FLAG_COMPRESSED) != 0u ? fetch_tbn_compressed_vertex(geom, index) : fetch_tbn_uncompressed_vertex(geom, index);
	}

	material = CustomMaterialUniforms(job.material_address);
	rt_instance_uniforms_ofs = job.instance_uniforms_ofs;

	// As raster computes motion vectors: vertex() again with the previous frame's time, camera and transform (the
	// uniforms stay current), for the motion of the previous position; on the previous frame's skinning output for a
	// skinned or blend shape surface (raster's previous vertex attributes).
	vec3 previous_rest_position = rest_position;
	TBNResult previous_tbn = tbn;
	const uint64_t previous_address = packUint2x32(uvec2(geom.prev_vertex_address_lo, geom.prev_vertex_address_hi));
	if (previous_address != 0ul) {
		GeometryData previous_geom = geom;
		previous_geom.vertex_address = previous_address;
		previous_rest_position = fetch_position(previous_geom, index);
		if (geom.normal_byte_offset != OFFSET_NONE) {
			previous_tbn = (geom.flags & FLAG_COMPRESSED) != 0u ? fetch_tbn_compressed_vertex(previous_geom, index) : fetch_tbn_uncompressed_vertex(previous_geom, index);
		}
	}
	vec3 previous = displace_vertex(job, previous_rest_position, previous_tbn, index, true);
	vec3 displaced = displace_vertex(job, rest_position, tbn, index, false);

	// A refit may not turn a triangle on or off: DXR "Inactive primitives and instances" (inactive primitives may not
	// become active in an update, active ones may not become inactive), Vulkan "Inactive Primitives and Instances" (any
	// transition between the active and inactive states requires a full rebuild). A triangle is inactive when a vertex
	// has a NaN x, and displace_vertex() makes a removed point all NaN: a vertex keeping its NaN state keeps its
	// triangles' state. One that would change it stays as the BLAS has it (its position, or NaN), and the count makes
	// the CPU rebuild the BLAS, from vertex()'s output, a few frames later (_displace_guard_arrived()).
	if (job.refit_guard != 0u) {
		vec3 built = vec3(uintBitsToFloat(blas_positions.v[index * 3u]), uintBitsToFloat(blas_positions.v[index * 3u + 1u]), uintBitsToFloat(blas_positions.v[index * 3u + 2u]));
		if (isnan(built.x) != isnan(displaced.x)) {
			displaced = built;
			atomicAdd(guard_counts.v[params.job_index], 1u);
		}
	}

	uint vertex_count = geom.vertex_count;
	dst.v[index * 3u] = floatBitsToUint(displaced.x);
	dst.v[index * 3u + 1u] = floatBitsToUint(displaced.y);
	dst.v[index * 3u + 2u] = floatBitsToUint(displaced.z);

	uint previous_base = vertex_count * (6u + job.tbn_words) + index * 3u;
	dst.v[previous_base] = floatBitsToUint(previous.x);
	dst.v[previous_base + 1u] = floatBitsToUint(previous.y);
	dst.v[previous_base + 2u] = floatBitsToUint(previous.z);

	if (job.tbn_words > 0u) {
		uint tbn_base = vertex_count * 3u + index * job.tbn_words;
		if ((geom.flags & FLAG_COMPRESSED) == 0u) {
			// Same layout: copy the words.
			Uint32Buffer vb = Uint32Buffer(geom.vertex_address);
			uint src_base = (geom.normal_byte_offset >> 2) + index * (geom.normal_stride >> 2);
			for (uint w = 0u; w < job.tbn_words; w++) {
				dst.v[tbn_base + w] = vb.v[src_base + w];
			}
		} else {
			// Vector3::octahedron_encode() and octahedron_tangent_encode().
			dst.v[tbn_base] = pack_unorm16x2(vec3_to_oct(tbn.normal));
			if (job.tbn_words > 1u) {
				vec2 t = vec3_to_oct(tbn.tangent);
				t.y = max(t.y, 1.0 / 32767.0) * 0.5 + 0.5;
				t.y = tbn.bitangent_sign >= 0.0 ? t.y : 1.0 - t.y;
				dst.v[tbn_base + 1u] = pack_unorm16x2(t);
			}
		}
	}

	uint rest_base = vertex_count * (3u + job.tbn_words) + index * 3u;
	dst.v[rest_base] = floatBitsToUint(rest_position.x);
	dst.v[rest_base + 1u] = floatBitsToUint(rest_position.y);
	dst.v[rest_base + 2u] = floatBitsToUint(rest_position.z);
}
