#[vertex]

#version 450

#VERSION_DEFINES

layout(push_constant, std430) uniform Params {
	float z_far;
	float z_near;
	vec2 texel_size;
#ifdef WATCH
	uint watch_offset;
	uint watch_stamp;
	uint pad0;
	uint pad1;
#endif
}
params;

layout(location = 0) out vec2 uv_interp;

void main() {
	vec2 base_arr[4] = vec2[](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
	uv_interp = base_arr[gl_VertexIndex];
	gl_Position = vec4(uv_interp * 2.0 - 1.0, 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) in vec2 uv_interp;

layout(set = 0, binding = 0) uniform samplerCube source_cube;

layout(push_constant, std430) uniform Params {
	float z_far;
	float z_near;
	vec2 texel_size;
#ifdef WATCH
	uint watch_offset;
	uint watch_stamp;
	uint pad0;
	uint pad1;
#endif
}
params;

#ifdef WATCH
// Wagnard/godot#1 (ShadowWatch): on a 32x32 grid of the region, what this copy read from the cubemap and the depth it
// wrote, as the copy itself sees them. Header: the stamp of the copy.
layout(set = 0, binding = 1, std430) restrict writeonly buffer WatchRecord {
	uint data[];
}
watch_record;
#endif

void main() {
	vec2 uv = uv_interp;
	vec2 texel_size = abs(params.texel_size);

	uv = clamp(uv * (1.0 + 2.0 * texel_size) - texel_size, vec2(0.0), vec2(1.0));

	vec3 normal = vec3(uv * 2.0 - 1.0, 0.0);
	normal.z = 0.5 * (1.0 - dot(normal.xy, normal.xy)); // z = 1/2 - 1/2 * (x^2 + y^2)
	normal = normalize(normal);

	normal.y = -normal.y; //needs to be flipped to match projection matrix
	if (params.texel_size.x >= 0.0) { // Sign is used to encode Z flip
		normal.z = -normal.z;
	}

	float depth = texture(source_cube, normal).r;
#ifdef WATCH
	float sampled = depth;
#endif

	// absolute values for direction cosines, bigger value equals closer to basis axis
	vec3 unorm = abs(normal);

	if ((unorm.x >= unorm.y) && (unorm.x >= unorm.z)) {
		// x code
		unorm = normal.x > 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(-1.0, 0.0, 0.0);
	} else if ((unorm.y > unorm.x) && (unorm.y >= unorm.z)) {
		// y code
		unorm = normal.y > 0.0 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, -1.0, 0.0);
	} else if ((unorm.z > unorm.x) && (unorm.z > unorm.y)) {
		// z code
		unorm = normal.z > 0.0 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 0.0, -1.0);
	} else {
		// oh-no we messed up code
		// has to be
		unorm = vec3(1.0, 0.0, 0.0);
	}

	float depth_fix = 1.0 / dot(normal, unorm);

	depth = 2.0 * depth - 1.0;
	float linear_depth = 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + depth * (params.z_far - params.z_near));
	// linear_depth equal to view space depth
	depth = (params.z_far - linear_depth * depth_fix) / params.z_far;
	gl_FragDepth = depth;

#ifdef WATCH
	// The grid of ShadowWatch: local pixel g * (size / 32) + size / 64.
	ivec2 size = ivec2(round(1.0 / texel_size));
	ivec2 local_px = ivec2(floor(uv_interp * vec2(size)));
	ivec2 step = size / 32;
	if (all(greaterThanEqual(step, ivec2(2))) && all(equal(local_px % step, step / 2))) {
		ivec2 g = local_px / step;
		uint index = uint(g.y * 32 + g.x);
		watch_record.data[params.watch_offset + 2u + index * 2u] = floatBitsToUint(sampled);
		watch_record.data[params.watch_offset + 3u + index * 2u] = floatBitsToUint(clamp(depth, 0.0, 1.0));
		if (index == 0u) {
			watch_record.data[params.watch_offset] = params.watch_stamp;
		}
	}
#endif
}
