#[compute]

#version 450

#VERSION_DEFINES

// RendererRD::PTProbes: the radiance the path tracer gathered around each probe (binding 31 of its set) projected into the
// 9 SH coefficients a LightmapGI capture holds (lm_compute.glsl, MODE_LIGHT_PROBES: 4 / ray count * the sum of radiance
// times basis), blended over frames. One workgroup per probe, one invocation per coefficient.

layout(local_size_x = 16, local_size_y = 1, local_size_z = 1) in;

// Per probe its position and generation (w: -1 when unused this frame), then per ray its radiance and direction.
layout(set = 0, binding = 0, std430) restrict readonly buffer Rays {
	vec4 data[];
}
rays;

// Per probe 9 coefficients and a header: x, the generation they belong to (a new one starts the blend over).
layout(set = 0, binding = 1, std430) restrict buffer SH {
	vec4 data[];
}
sh;

layout(push_constant, std430) uniform Params {
	uint max_probes;
	uint ray_count;
	float blend;
	uint pad;
}
params;

float sh_basis(uint p_index, vec3 p_dir) {
	switch (p_index) {
		case 0u:
			return 0.282095;
		case 1u:
			return 0.488603 * p_dir.y;
		case 2u:
			return 0.488603 * p_dir.z;
		case 3u:
			return 0.488603 * p_dir.x;
		case 4u:
			return 1.092548 * p_dir.x * p_dir.y;
		case 5u:
			return 1.092548 * p_dir.y * p_dir.z;
		case 6u:
			return 0.315392 * (3.0 * p_dir.z * p_dir.z - 1.0);
		case 7u:
			return 1.092548 * p_dir.x * p_dir.z;
		default:
			return 0.546274 * (p_dir.x * p_dir.x - p_dir.y * p_dir.y);
	}
}

void main() {
	const uint probe = gl_WorkGroupID.x;
	const uint coefficient = gl_LocalInvocationID.x;
	const float generation = rays.data[probe].w;
	if (generation < 0.0) {
		return; // The same for the whole workgroup.
	}

	const uint sh_base = probe * 10u;
	const bool fresh = sh.data[sh_base + 9u].x != generation;
	// Every invocation has read the header before the last one rewrites it.
	barrier();

	if (coefficient < 9u) {
		vec3 sum = vec3(0.0);
		for (uint r = 0u; r < params.ray_count; r++) {
			const uint at = params.max_probes + (probe * params.ray_count + r) * 2u;
			const vec3 radiance = rays.data[at].rgb;
			// One invalid path would stay in the blend for good.
			if (any(isnan(radiance)) || any(isinf(radiance))) {
				continue;
			}
			sum += radiance * sh_basis(coefficient, rays.data[at + 1u].xyz);
		}
		sum *= 4.0 / float(params.ray_count);
		const vec3 previous = sh.data[sh_base + coefficient].rgb;
		sh.data[sh_base + coefficient] = vec4(fresh ? sum : mix(previous, sum, params.blend), 0.0);
	} else if (coefficient == 9u) {
		sh.data[sh_base + 9u] = vec4(generation, 0.0, 0.0, 0.0);
	}
}
