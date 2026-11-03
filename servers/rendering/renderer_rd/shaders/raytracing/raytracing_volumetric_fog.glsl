#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// The volumetric fog over the path traced color, as raster's scene shader applies it to each opaque fragment
// (volumetric_fog_process()). The environment's depth fog is already in the traced color. The sky (depth 0) is left
// alone: the sky shader applies the volumetric fog itself.

layout(rgba16f, set = 0, binding = 0) uniform restrict image2D color_image;
layout(r32f, set = 0, binding = 1) uniform restrict readonly image2D depth_image;
layout(set = 0, binding = 2) uniform texture3D volumetric_fog_texture;
layout(set = 0, binding = 3) uniform sampler linear_sampler;

layout(push_constant, std430) uniform Params {
	mat4 inv_projection;
	ivec2 size;
	float inv_length;
	float detail_spread;
	uint legacy_blending;
	uint pad[3]; // MODE_DEBUG: pad[0] is the debug view.
}
params;

#ifdef MODE_DEBUG
// Environment.RT_DEBUG_VOLUMETRIC_FOG (25) and RT_DEBUG_VOLUMETRIC_FOG_TRANSMITTANCE (26): the fog this pass applies, or its
// transmittance, instead of the color; 1: either of them where no volumetric fog is bound (magenta).
#define DEBUG_FOG 25u
#define DEBUG_FOG_TRANSMITTANCE 26u
#define DEBUG_NO_FOG 1u
#endif

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) {
		return;
	}

	float depth = imageLoad(depth_image, pixel).r;
	if (depth == 0.0) {
		return;
	}
#ifdef MODE_DEBUG
	if (params.pad[0] == DEBUG_NO_FOG) {
		imageStore(color_image, pixel, vec4(1.0, 0.0, 1.0, 1.0));
		return;
	}
#endif

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.size);
	vec4 view = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 fog_pos = vec3(uv, -view.z / view.w * params.inv_length);
	if (fog_pos.z < 0.0) {
#ifdef MODE_DEBUG
		// Nearer than the froxels: no fog.
		imageStore(color_image, pixel, vec4(vec3(params.pad[0] == DEBUG_FOG_TRANSMITTANCE ? 1.0 : 0.0), 1.0));
#endif
		return;
	} else if (fog_pos.z < 1.0) {
		fog_pos.z = pow(fog_pos.z, params.detail_spread);
	}

	vec4 fog = textureLod(sampler3D(volumetric_fog_texture, linear_sampler), fog_pos, 0.0);
	if (params.legacy_blending != 0) {
		fog.rgb *= 1.0 - fog.a;
	}

	vec4 color = imageLoad(color_image, pixel);
#ifdef MODE_DEBUG
	color.rgb = params.pad[0] == DEBUG_FOG_TRANSMITTANCE ? vec3(fog.a) : fog.rgb;
#else
	color.rgb = color.rgb * fog.a + fog.rgb;
#endif
	imageStore(color_image, pixel, color);
}
