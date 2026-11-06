#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_buffer_reference : require
#extension GL_ARB_gpu_shader_int64 : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// One TLAS instance per instance of a MultiMesh, written from its buffer: the instances raster draws this frame, with no
// copy to the CPU. Per instance (one thread): its TLAS instance, its geometry and material records (the MultiMesh's,
// pointing at the instance's COLOR and INSTANCE_CUSTOM), its previous transform (motion vectors).
//
// The instances follow the ones the CPU wrote: slot = params.first_slot + thread, the TLAS instance's custom index and
// the index of its records, as for every other instance.

layout(buffer_reference, std430) readonly buffer MultiMeshBuffer {
	float v[];
};

struct ExpandJob {
	vec4 node[3]; // The MultiMesh node's transform, three rows.
	vec4 prev_node[3]; // In the previous frame.
	vec4 aabb[3]; // A compressed mesh's AABB transform (its positions are in the AABB's unit cube), identity otherwise.
	uvec4 instance_tail; // Words 12-15 of the instance: custom index | mask, hit group offset | flags, BLAS address.
	uint mm_address_lo;
	uint mm_address_hi;
	uint mm_stride; // In floats.
	uint mm_offset; // The instances drawn this frame start there in the buffer.
	uint mm_prev_offset; // Their previous transforms; mm_offset when they did not move.
	uint first_slot; // Of the job's first instance.
	uint first_motion; // Its motion transform.
	uint template_index; // Of its geometry and material records.
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Jobs {
	ExpandJob jobs[];
};

layout(set = 0, binding = 1, std430) restrict readonly buffer GeometryTemplates {
	uint geometry_templates[];
};

layout(set = 0, binding = 2, std430) restrict readonly buffer MaterialTemplates {
	uint material_templates[];
};

// The TLAS instances, 16 words each (API_TRAIT_ACCELERATION_STRUCTURE_INSTANCE_STANDARD_LAYOUT).
layout(set = 0, binding = 3, std430) restrict writeonly buffer Instances {
	uint instances[];
};

layout(set = 0, binding = 4, std430) restrict writeonly buffer Geometries {
	uint geometries[];
};

layout(set = 0, binding = 5, std430) restrict writeonly buffer Materials {
	uint materials[];
};

layout(set = 0, binding = 6, std430) restrict writeonly buffer MotionIndices {
	int motion_indices[];
};

// RT_InstanceMotionData: the previous object-to-world transform, three rows.
layout(set = 0, binding = 7, std430) restrict writeonly buffer MotionTransforms {
	vec4 motion_transforms[];
};

#ifdef PARTICLES
// The variant of a frame with GPUParticles3D jobs (rendering/pathtracing/trace_gpu_particles), one record per job (zero
// for a MultiMesh's): a particle's instance buffer has the 3D MultiMesh layout (transform, color, custom: 20 floats).
#define EXPAND_PARTICLES 1u // Its dead particles are inactive instances.
#define EXPAND_BILLBOARD 2u // Billboard render modes: the basis faces the camera (billboard_inc.glsl).
#define EXPAND_BILLBOARD_ROTATION 4u // billboard_particles: turned by INSTANCE_CUSTOM.x.
#define EXPAND_BILLBOARD_KEEP_SCALE 8u
#define EXPAND_FIXED_SIZE 16u
#define PARTICLE_CUSTOM_OFFSET 16u // INSTANCE_CUSTOM, in floats.

struct ExpandParticles {
	vec4 basis[3]; // The facing basis of the billboard mode for the camera, three columns (RenderingBillboard::facing_basis()).
	vec4 prev_basis[3]; // For the previous frame's camera.
	vec4 fixed_size; // fixed_size: the scale at an origin, dot(xyz, origin) + w (RenderingBillboard::fixed_size_scale()).
	vec4 prev_fixed_size;
	uvec4 flags; // x: EXPAND_*.
};

layout(set = 0, binding = 8, std430) restrict readonly buffer Particles {
	ExpandParticles particles[];
};

// An instance's transform with the render modes the rasterizer applies to MODELVIEW_MATRIX (billboard_inc.glsl, its CPU
// mirror RenderingBillboard::compute_world_transform()), before a compressed mesh's AABB transform.
mat4 particle_world_transform(mat4 p_world, uint p_flags, mat3 p_facing, vec4 p_fixed_size, float p_rotation) {
	if (bool(p_flags & EXPAND_BILLBOARD)) {
		mat3 basis = p_facing;
		if (bool(p_flags & EXPAND_BILLBOARD_ROTATION)) {
			const float c = cos(p_rotation);
			const float s = sin(p_rotation);
			basis = basis * mat3(vec3(c, -s, 0.0), vec3(s, c, 0.0), vec3(0.0, 0.0, 1.0));
		}
		if (bool(p_flags & EXPAND_BILLBOARD_KEEP_SCALE)) {
			basis[0] *= length(p_world[0].xyz);
			basis[1] *= length(p_world[1].xyz);
			basis[2] *= length(p_world[2].xyz);
		}
		p_world[0] = vec4(basis[0], 0.0);
		p_world[1] = vec4(basis[1], 0.0);
		p_world[2] = vec4(basis[2], 0.0);
	}
	if (bool(p_flags & EXPAND_FIXED_SIZE)) {
		const float scale = dot(p_fixed_size.xyz, p_world[3].xyz) + p_fixed_size.w;
		p_world[0] *= scale;
		p_world[1] *= scale;
		p_world[2] *= scale;
	}
	return p_world;
}

// particles_copy.glsl writes a dead particle with a zero basis at -INF: no instance may carry that transform. The exponent
// bits, not isinf(): a compiler that assumes finite floats may fold isinf() and isnan() away.
bool particle_is_dead(mat4 p_transform) {
	const uvec3 exponent = floatBitsToUint(p_transform[3].xyz) & uvec3(0x7F800000u);
	if (any(equal(exponent, uvec3(0x7F800000u)))) {
		return true;
	}
	return all(equal(p_transform[0].xyz, vec3(0.0))) && all(equal(p_transform[1].xyz, vec3(0.0))) && all(equal(p_transform[2].xyz, vec3(0.0)));
}
#endif

layout(push_constant, std430) uniform Params {
	uint job_count;
	uint first_slot; // Of the first instance written here: the instances the CPU wrote come first.
	uint instance_count; // Written here.
	uint geometry_words; // Per record.
	uint material_words;
	uint mm_offset_word; // RT_GeometryData::multimesh_offset.
	uint mm_prev_offset_word; // RT_GeometryData::multimesh_previous_offset.
	uint pad;
}
params;

mat4 rows_to_mat4(vec4 p_row0, vec4 p_row1, vec4 p_row2) {
	return transpose(mat4(p_row0, p_row1, p_row2, vec4(0.0, 0.0, 0.0, 1.0)));
}

// An instance's transform in the MultiMesh buffer, as raster reads it: three rows.
mat4 multimesh_transform(MultiMeshBuffer p_mm, uint p_float_offset) {
	return rows_to_mat4(
			vec4(p_mm.v[p_float_offset + 0u], p_mm.v[p_float_offset + 1u], p_mm.v[p_float_offset + 2u], p_mm.v[p_float_offset + 3u]),
			vec4(p_mm.v[p_float_offset + 4u], p_mm.v[p_float_offset + 5u], p_mm.v[p_float_offset + 6u], p_mm.v[p_float_offset + 7u]),
			vec4(p_mm.v[p_float_offset + 8u], p_mm.v[p_float_offset + 9u], p_mm.v[p_float_offset + 10u], p_mm.v[p_float_offset + 11u]));
}

void main() {
	if (gl_GlobalInvocationID.x >= params.instance_count) {
		return;
	}
	const uint slot = params.first_slot + gl_GlobalInvocationID.x;

	// The job holding this slot: the last one starting at or before it.
	uint lo = 0u;
	uint hi = params.job_count - 1u;
	while (lo < hi) {
		uint mid = (lo + hi + 1u) >> 1u;
		if (jobs[mid].first_slot <= slot) {
			lo = mid;
		} else {
			hi = mid - 1u;
		}
	}
	const uint j = lo;
	const uint index = slot - jobs[j].first_slot;

	MultiMeshBuffer mm = MultiMeshBuffer(packUint2x32(uvec2(jobs[j].mm_address_lo, jobs[j].mm_address_hi)));
	const mat4 aabb = rows_to_mat4(jobs[j].aabb[0], jobs[j].aabb[1], jobs[j].aabb[2]);
#ifdef PARTICLES
	const uint flags = particles[j].flags.x;
	const uint current_float = (jobs[j].mm_offset + index) * jobs[j].mm_stride;
	uint previous_float = (jobs[j].mm_prev_offset + index) * jobs[j].mm_stride;
	const mat4 current_instance = multimesh_transform(mm, current_float);
	mat4 previous_instance = multimesh_transform(mm, previous_float);
	bool dead = false;
	if (bool(flags & EXPAND_PARTICLES)) {
		dead = particle_is_dead(current_instance);
		if (particle_is_dead(previous_instance)) {
			// Born this frame (or dead): no motion.
			previous_instance = current_instance;
			previous_float = current_float;
		}
	}
	mat4 current = rows_to_mat4(jobs[j].node[0], jobs[j].node[1], jobs[j].node[2]) * current_instance;
	mat4 previous = rows_to_mat4(jobs[j].prev_node[0], jobs[j].prev_node[1], jobs[j].prev_node[2]) * previous_instance;
	if (bool(flags & (EXPAND_BILLBOARD | EXPAND_FIXED_SIZE)) && !dead) {
		// Each with its frame's camera: turning with the camera is motion too.
		const ExpandParticles p = particles[j];
		current = particle_world_transform(current, flags, mat3(p.basis[0].xyz, p.basis[1].xyz, p.basis[2].xyz), p.fixed_size, mm.v[current_float + PARTICLE_CUSTOM_OFFSET]);
		previous = particle_world_transform(previous, flags, mat3(p.prev_basis[0].xyz, p.prev_basis[1].xyz, p.prev_basis[2].xyz), p.prev_fixed_size, mm.v[previous_float + PARTICLE_CUSTOM_OFFSET]);
	}
	current = current * aabb;
	previous = previous * aabb;
	if (dead) {
		// An inactive instance: "one whose acceleration structure reference is 0" (Vulkan, Inactive Primitives and
		// Instances), "NULL bottom level acceleration structure pointers" (DXR functional spec, Inactive primitives and
		// instances). Its mask is 0 too and its transform finite.
		current = mat4(0.0);
		previous = mat4(0.0);
	}
#else
	const mat4 current = rows_to_mat4(jobs[j].node[0], jobs[j].node[1], jobs[j].node[2]) * multimesh_transform(mm, (jobs[j].mm_offset + index) * jobs[j].mm_stride) * aabb;
	const mat4 previous = rows_to_mat4(jobs[j].prev_node[0], jobs[j].prev_node[1], jobs[j].prev_node[2]) * multimesh_transform(mm, (jobs[j].mm_prev_offset + index) * jobs[j].mm_stride) * aabb;
#endif

	// The instance: the transform's rows, then the job's words with this slot as the custom index.
	const mat4 current_rows = transpose(current);
	const uint instance_word = slot * 16u;
	for (uint r = 0u; r < 3u; r++) {
		instances[instance_word + r * 4u + 0u] = floatBitsToUint(current_rows[r].x);
		instances[instance_word + r * 4u + 1u] = floatBitsToUint(current_rows[r].y);
		instances[instance_word + r * 4u + 2u] = floatBitsToUint(current_rows[r].z);
		instances[instance_word + r * 4u + 3u] = floatBitsToUint(current_rows[r].w);
	}
#ifdef PARTICLES
	if (dead) {
		instances[instance_word + 12u] = slot & 0x00FFFFFFu;
		instances[instance_word + 13u] = jobs[j].instance_tail.y;
		instances[instance_word + 14u] = 0u;
		instances[instance_word + 15u] = 0u;
	} else {
		instances[instance_word + 12u] = (jobs[j].instance_tail.x & 0xFF000000u) | (slot & 0x00FFFFFFu);
		instances[instance_word + 13u] = jobs[j].instance_tail.y;
		instances[instance_word + 14u] = jobs[j].instance_tail.z;
		instances[instance_word + 15u] = jobs[j].instance_tail.w;
	}
#else
	instances[instance_word + 12u] = (jobs[j].instance_tail.x & 0xFF000000u) | (slot & 0x00FFFFFFu);
	instances[instance_word + 13u] = jobs[j].instance_tail.y;
	instances[instance_word + 14u] = jobs[j].instance_tail.z;
	instances[instance_word + 15u] = jobs[j].instance_tail.w;
#endif

	// The records: the MultiMesh's, at this instance in its buffer.
	const uint geometry_src = jobs[j].template_index * params.geometry_words;
	const uint geometry_dst = slot * params.geometry_words;
	for (uint w = 0u; w < params.geometry_words; w++) {
		geometries[geometry_dst + w] = geometry_templates[geometry_src + w];
	}
	geometries[geometry_dst + params.mm_offset_word] = jobs[j].mm_offset + index;
	geometries[geometry_dst + params.mm_prev_offset_word] = jobs[j].mm_offset + index;

	const uint material_src = jobs[j].template_index * params.material_words;
	const uint material_dst = slot * params.material_words;
	for (uint w = 0u; w < params.material_words; w++) {
		materials[material_dst + w] = material_templates[material_src + w];
	}

	// Its motion, as raster's motion vectors see it (its previous transform equals the current one when it did not move).
	const uint motion = jobs[j].first_motion + index;
	motion_indices[slot] = int(motion);
	const mat4 previous_rows = transpose(previous);
	motion_transforms[motion * 3u + 0u] = previous_rows[0];
	motion_transforms[motion * 3u + 1u] = previous_rows[1];
	motion_transforms[motion * 3u + 2u] = previous_rows[2];
}
