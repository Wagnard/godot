#[compute]

#version 450

#VERSION_DEFINES

// Wagnard/godot#1 diagnostic: reads back what an omni light in cube shadow mode left in the shadow atlas and in its
// cubemap, and what the main pass will read, so that the CPU can tell which stage goes wrong (ShadowWatch).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 3, std430) restrict buffer Results {
	uint data[];
}
results;

#ifdef MODE_LIGHT

layout(set = 0, binding = 0) uniform sampler2D shadow_atlas;
layout(set = 0, binding = 1) uniform samplerCube source_cube;

layout(set = 0, binding = 2, std430) restrict readonly buffer OmniLights {
	float data[];
}
omni_lights;

layout(push_constant, std430) uniform Params {
	ivec4 half_rect[2]; // In atlas pixels: x, y, width, height.
	float z_near;
	float z_far;
	uint flags;
	uint out_offset;
	uint entries_offset;
	uint entry_floats;
	uint pad0;
	uint pad1;
}
params;

#define FLAG_COPY_CHECK 1u
#define FLAG_FACES 2u
#define FLAG_ENTRIES 4u

// Per light record, in uints: two halves of 68 (64 block sums, count of 0, count of 1, copy mismatches, copy samples),
// then six faces of 18 (16 block sums, count of 0, count of 1).
#define HALF_STRIDE 68u
#define FACES_BASE 136u
#define FACE_STRIDE 18u

shared uint zero_count;
shared uint one_count;
shared uint mismatch_count;
shared uint checked_count;

uint quantize(float d) {
	return uint(round(clamp(d, 0.0, 1.0) * 65535.0));
}

// What cube_to_dp.glsl writes for the fragment of uv_interp, kept identical to it.
float expected_dp_depth(vec2 uv_interp, vec2 texel_size, bool flip) {
	vec2 uv = clamp(uv_interp * (1.0 + 2.0 * texel_size) - texel_size, vec2(0.0), vec2(1.0));

	vec3 normal = vec3(uv * 2.0 - 1.0, 0.0);
	normal.z = 0.5 * (1.0 - dot(normal.xy, normal.xy));
	normal = normalize(normal);

	normal.y = -normal.y;
	if (!flip) {
		normal.z = -normal.z;
	}

	float depth = textureLod(source_cube, normal, 0.0).r;

	vec3 unorm = abs(normal);
	if ((unorm.x >= unorm.y) && (unorm.x >= unorm.z)) {
		unorm = normal.x > 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(-1.0, 0.0, 0.0);
	} else if ((unorm.y > unorm.x) && (unorm.y >= unorm.z)) {
		unorm = normal.y > 0.0 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, -1.0, 0.0);
	} else if ((unorm.z > unorm.x) && (unorm.z > unorm.y)) {
		unorm = normal.z > 0.0 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 0.0, -1.0);
	} else {
		unorm = vec3(1.0, 0.0, 0.0);
	}

	float depth_fix = 1.0 / dot(normal, unorm);

	depth = 2.0 * depth - 1.0;
	float linear_depth = 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + depth * (params.z_far - params.z_near));
	return clamp((params.z_far - linear_depth * depth_fix) / params.z_far, 0.0, 1.0);
}

vec3 face_direction(uint p_face, vec2 st) {
	switch (p_face) {
		case 0u:
			return vec3(1.0, -st.y, -st.x);
		case 1u:
			return vec3(-1.0, -st.y, st.x);
		case 2u:
			return vec3(st.x, 1.0, st.y);
		case 3u:
			return vec3(st.x, -1.0, -st.y);
		case 4u:
			return vec3(st.x, -st.y, 1.0);
		default:
			return vec3(-st.x, -st.y, -1.0);
	}
}

void main() {
	uint task = gl_WorkGroupID.y;
	uint t = gl_LocalInvocationID.x;

	if (task == 8u) {
		// The omni lights as the main pass will read them.
		if ((params.flags & FLAG_ENTRIES) != 0u) {
			for (uint i = t; i < params.entry_floats; i += 64u) {
				results.data[params.entries_offset + i] = floatBitsToUint(omni_lights.data[i]);
			}
		}
		return;
	}

	if (t == 0u) {
		zero_count = 0u;
		one_count = 0u;
		mismatch_count = 0u;
		checked_count = 0u;
	}
	barrier();

	if (task < 2u) {
		// A 32x32 grid over the half, each invocation summing a 4x4 block of it.
		ivec4 rect = params.half_rect[task];
		uint bx = t % 8u;
		uint by = t / 8u;
		uint sum = 0u;
		bool copy_check = (params.flags & FLAG_COPY_CHECK) != 0u;
		vec2 texel_size = 1.0 / vec2(rect.zw);
		for (uint j = 0u; j < 4u; j++) {
			for (uint i = 0u; i < 4u; i++) {
				ivec2 g = ivec2(bx * 4u + i, by * 4u + j);
				ivec2 local_px = (g * rect.zw) / 32 + rect.zw / 64;
				ivec2 px = rect.xy + local_px;
				float d = texelFetch(shadow_atlas, px, 0).r;
				uint q = quantize(d);
				sum += q;
				if (q == 0u) {
					atomicAdd(zero_count, 1u);
				} else if (q == 65535u) {
					atomicAdd(one_count, 1u);
				}
				if (copy_check) {
					vec2 uv_interp = (vec2(local_px) + 0.5) * texel_size;
					float e = expected_dp_depth(uv_interp, texel_size, task == 1u);
					atomicAdd(checked_count, 1u);
					if (abs(e - d) > 0.02) {
						atomicAdd(mismatch_count, 1u);
					}
				}
			}
		}
		uint base = params.out_offset + task * HALF_STRIDE;
		results.data[base + t] = sum;
		barrier();
		if (t == 0u) {
			results.data[base + 64u] = zero_count;
			results.data[base + 65u] = one_count;
			results.data[base + 66u] = mismatch_count;
			results.data[base + 67u] = checked_count;
		}
		return;
	}

	// The cubemap faces: a 16x16 grid each, invocations 0 to 15 summing a 4x4 block.
	uint face = task - 2u;
	uint base = params.out_offset + FACES_BASE + face * FACE_STRIDE;
	if ((params.flags & FLAG_FACES) == 0u) {
		if (t < FACE_STRIDE) {
			results.data[base + t] = 0u;
		}
		return;
	}
	if (t < 16u) {
		uint bx = t % 4u;
		uint by = t / 4u;
		uint sum = 0u;
		for (uint j = 0u; j < 4u; j++) {
			for (uint i = 0u; i < 4u; i++) {
				vec2 st = (vec2(bx * 4u + i, by * 4u + j) + 0.5) / 16.0 * 2.0 - 1.0;
				uint q = quantize(textureLod(source_cube, face_direction(face, st), 0.0).r);
				sum += q;
				if (q == 0u) {
					atomicAdd(zero_count, 1u);
				} else if (q == 65535u) {
					atomicAdd(one_count, 1u);
				}
			}
		}
		results.data[base + t] = sum;
	}
	barrier();
	if (t == 0u) {
		results.data[base + 16u] = zero_count;
		results.data[base + 17u] = one_count;
	}
}

#endif

#ifdef MODE_SCREEN

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform sampler2D source_color;
layout(set = 0, binding = 2) uniform sampler2D shadow_atlas;

layout(push_constant, std430) uniform Params {
	mat4 ndc_to_light; // Shadow matrix times the inverse projection.
	vec4 uv_rect; // As the scene shader shrinks it.
	vec2 flip_offset;
	float inv_radius;
	float shadow_bias;
	uvec2 grid;
	uint out_lum; // 0xFFFFFFFF: not written by this dispatch.
	uint out_vis;
}
params;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.grid.x * params.grid.y) {
		return;
	}
	vec2 uv = (vec2(index % params.grid.x, index / params.grid.x) + 0.5) / vec2(params.grid);

	if (params.out_lum != 0xFFFFFFFFu) {
		vec3 color = textureLod(source_color, uv, 0.0).rgb;
		results.data[params.out_lum + index] = floatBitsToUint(dot(color, vec3(0.2126, 0.7152, 0.0722)));
	}

	// The omni shadow test of the scene shader, without its normal bias and filtering: 0 lit, 1 shadowed, 2 out of reach.
	uint vis = 2u;
	float depth = textureLod(source_depth, uv, 0.0).r;
	if (depth > 0.0) {
		vec4 p = params.ndc_to_light * vec4(uv * 2.0 - 1.0, depth, 1.0);
		vec3 local_vert = p.xyz / p.w;
		float shadow_len = length(local_vert);
		if (shadow_len * params.inv_radius < 1.0) {
			vec3 shadow_dir = normalize(local_vert);
			vec4 uv_rect = params.uv_rect;
			if (shadow_dir.z >= 0.0) {
				uv_rect.xy += params.flip_offset;
			}
			shadow_dir.z = 1.0 + abs(shadow_dir.z);
			vec2 pos = shadow_dir.xy / shadow_dir.z;
			pos = uv_rect.xy + (pos * 0.5 + 0.5) * uv_rect.zw;
			float ref = 1.0 - (shadow_len - params.shadow_bias) * params.inv_radius;
			float stored = textureLod(shadow_atlas, pos, 0.0).r;
			vis = stored > ref ? 1u : 0u;
		}
	}
	results.data[params.out_vis + index] = vis;
}

#endif
