// Path tracer vertex capture (rendering/pathtracing/vertex_capture_mode): the raster vertex shader stores what vertex()
// made of each vertex in the surface's displaced vertex buffer, in the layout raytracing_displace.glsl writes for the
// deformed path (RenderRaytracing::_displace_surfaces()): [displaced float3 positions][rest TBN block][rest float3
// positions]. The previous positions are the last capture's (process_deformed_surface() keeps them).

// Mirrors RT_VertexCaptureDraw (render_raytracing.h).
struct RTCaptureDraw {
	vec4 view_to_local[3]; // Rows: from the view space vertex() ends in to the space of the BLAS.
	uint vertex_count;
	uint tbn_words; // 0, 1 (normal) or 2 (normal and tangent).
	uint flags;
	uint guard_index;
};

#define RT_CAPTURE_FLAG_REFIT_GUARD (1u << 0) // The BLAS is refit: a vertex keeps its triangles' active state.
#define RT_CAPTURE_FLAG_COMPRESSED (1u << 1) // The mesh's attributes are compressed: the rest TBN is encoded again.

// Indexed by draw_call.instance_index, the request's element: the push constant stays raster's.
layout(set = RT_CAPTURE_UNIFORM_SET, binding = 0, std430) restrict readonly buffer RTCaptureParams {
	RTCaptureDraw data[];
}
rt_capture_params;

layout(set = RT_CAPTURE_UNIFORM_SET, binding = 1, std430) restrict writeonly buffer RTCaptureOutput {
	uint data[];
}
rt_capture_output;

layout(set = RT_CAPTURE_UNIFORM_SET, binding = 2, std430) restrict readonly buffer RTCaptureBlasPositions {
	uint data[];
}
rt_capture_blas_positions;

// Per request: the vertices a refit kept as the BLAS has them; any makes the CPU build that BLAS again.
layout(set = RT_CAPTURE_UNIFORM_SET, binding = 3, std430) restrict buffer RTCaptureGuardCounts {
	uint data[];
}
rt_capture_guard_counts;

// As raytracing_displace.glsl encodes compressed meshes (Vector3::octahedron_encode(), octahedron_tangent_encode()).
uint rt_capture_pack_unorm16x2(vec2 p_value) {
	uvec2 u = uvec2(clamp(p_value, 0.0, 1.0) * 65535.0);
	return u.x | (u.y << 16);
}

vec2 rt_capture_vec3_to_oct(vec3 p_n) {
	p_n /= abs(p_n.x) + abs(p_n.y) + abs(p_n.z);
	vec2 o = p_n.z >= 0.0 ? p_n.xy : (1.0 - abs(p_n.yx)) * vec2(p_n.x >= 0.0 ? 1.0 : -1.0, p_n.y >= 0.0 ? 1.0 : -1.0);
	return o * 0.5 + 0.5;
}

// The words of an uncompressed mesh's attribute, as raytracing_displace.glsl copies them.
uint rt_capture_unorm16x2_words(vec2 p_value) {
	uvec2 u = uvec2(round(clamp(p_value, 0.0, 1.0) * 65535.0));
	return u.x | (u.y << 16);
}

// p_vertex: vertex()'s output in view space, as raster draws it. p_rest_*: the vertex vertex() was given;
// p_axis_tangent: its attribute (uncompressed: the octahedral normal and tangent words).
void rt_capture_store(vec3 p_vertex, vec3 p_rest_vertex, vec3 p_rest_normal, vec3 p_rest_tangent, vec3 p_rest_binormal, vec4 p_axis_tangent) {
	RTCaptureDraw draw = rt_capture_params.data[draw_call.instance_index];
	uint index = uint(gl_VertexIndex);
	gl_PointSize = 1.0;
	if (index >= draw.vertex_count) {
		return;
	}

	vec4 v = vec4(p_vertex, 1.0);
	vec3 displaced = vec3(dot(draw.view_to_local[0], v), dot(draw.view_to_local[1], v), dot(draw.view_to_local[2], v));
	// A point vertex() removed (NaN) makes its triangles inactive in the BLAS, which needs a NaN x; an infinite one too.
	if (any(isnan(displaced)) || any(isinf(displaced))) {
		displaced = vec3(uintBitsToFloat(0x7fc00000u));
	}
	// A refit may not turn a triangle on or off (raytracing_displace.glsl): a vertex that would change its state stays as
	// the BLAS has it, and the count makes the CPU build the BLAS again.
	if (bool(draw.flags & RT_CAPTURE_FLAG_REFIT_GUARD)) {
		vec3 built = vec3(uintBitsToFloat(rt_capture_blas_positions.data[index * 3u]), uintBitsToFloat(rt_capture_blas_positions.data[index * 3u + 1u]), uintBitsToFloat(rt_capture_blas_positions.data[index * 3u + 2u]));
		if (isnan(built.x) != isnan(displaced.x)) {
			displaced = built;
			atomicAdd(rt_capture_guard_counts.data[draw.guard_index], 1u);
		}
	}

	uint vertex_count = draw.vertex_count;
	rt_capture_output.data[index * 3u] = floatBitsToUint(displaced.x);
	rt_capture_output.data[index * 3u + 1u] = floatBitsToUint(displaced.y);
	rt_capture_output.data[index * 3u + 2u] = floatBitsToUint(displaced.z);

	if (draw.tbn_words > 0u) {
		uint tbn_base = vertex_count * 3u + index * draw.tbn_words;
		if (!bool(draw.flags & RT_CAPTURE_FLAG_COMPRESSED)) {
			rt_capture_output.data[tbn_base] = rt_capture_unorm16x2_words(p_axis_tangent.xy);
			if (draw.tbn_words > 1u) {
				rt_capture_output.data[tbn_base + 1u] = rt_capture_unorm16x2_words(p_axis_tangent.zw);
			}
		} else {
			rt_capture_output.data[tbn_base] = rt_capture_pack_unorm16x2(rt_capture_vec3_to_oct(p_rest_normal));
			if (draw.tbn_words > 1u) {
				vec2 t = rt_capture_vec3_to_oct(p_rest_tangent);
				t.y = max(t.y, 1.0 / 32767.0) * 0.5 + 0.5;
				t.y = dot(cross(p_rest_normal, p_rest_tangent), p_rest_binormal) >= 0.0 ? t.y : 1.0 - t.y;
				rt_capture_output.data[tbn_base + 1u] = rt_capture_pack_unorm16x2(t);
			}
		}
	}

	uint rest_base = vertex_count * (3u + draw.tbn_words) + index * 3u;
	rt_capture_output.data[rest_base] = floatBitsToUint(p_rest_vertex.x);
	rt_capture_output.data[rest_base + 1u] = floatBitsToUint(p_rest_vertex.y);
	rt_capture_output.data[rest_base + 2u] = floatBitsToUint(p_rest_vertex.z);
}
