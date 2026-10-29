#[compute]

#version 450

#VERSION_DEFINES

// A copy of a layer of premultiplied contributions (the path tracer's DLSS-RR transparency layer) where a texel holding a
// NaN or an Inf adds and covers nothing (0). With REPORT_NON_FINITE, only those texels touch the statistics: how many,
// and the screen rectangle they cover, read back by RendererRD::FiniteLayer to name the effects that wrote them.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_layer;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D dest_layer;
#ifdef REPORT_NON_FINITE
layout(set = 0, binding = 2, std430) restrict buffer Stats {
	uint count;
	uint min_x;
	uint min_y;
	uint max_x;
	uint max_y;
}
stats;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 pad;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}

	vec4 color = texelFetch(source_layer, pos, 0);
	if (any(isnan(color)) || any(isinf(color))) {
		color = vec4(0.0);
#ifdef REPORT_NON_FINITE
		atomicAdd(stats.count, 1u);
		atomicMin(stats.min_x, uint(pos.x));
		atomicMin(stats.min_y, uint(pos.y));
		atomicMax(stats.max_x, uint(pos.x));
		atomicMax(stats.max_y, uint(pos.y));
#endif
	}
	imageStore(dest_layer, pos, color);
}
