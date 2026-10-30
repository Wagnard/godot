#[compute]

#version 450

#extension GL_EXT_samplerless_texture_functions : require

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// With NRD (PT_DENOISER_NRD_REBLUR), the path tracer's output holds the light that is not denoised (the primary surfaces'
// own emission, the sky, what no surface shaded) and each lobe's light of the primary surfaces is apart, divided by the
// lobe's albedo guide (scene_raytracing_raygen.glsl). This multiplies each lobe's light back by its albedo onto the output:
// the denoised light, or the raygen's own when no denoiser ran. Where the camera rays missed (depth 0) there is none, and
// NRD writes nothing there (outside CommonSettings::denoisingRange).

layout(set = 0, binding = 0) uniform texture2D diffuse_texture;
layout(set = 0, binding = 1) uniform texture2D specular_texture;
layout(set = 0, binding = 2) uniform texture2D diffuse_albedo_texture;
layout(set = 0, binding = 3) uniform texture2D specular_albedo_texture;
layout(r32f, set = 0, binding = 4) uniform restrict readonly image2D depth_image;
layout(rgba16f, set = 0, binding = 5) uniform restrict image2D output_image;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	uint pad[2];
}
params;

// As NRD_ALBEDO_MIN (raytracing_inc.glsl): the floor the raygen divided by.
#define NRD_ALBEDO_MIN 0.001

void main() {
	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size)) || imageLoad(depth_image, pixel).r == 0.0) {
		return;
	}

	const vec3 diffuse = texelFetch(diffuse_texture, pixel, 0).rgb * max(texelFetch(diffuse_albedo_texture, pixel, 0).rgb, vec3(NRD_ALBEDO_MIN));
	const vec3 specular = texelFetch(specular_texture, pixel, 0).rgb * max(texelFetch(specular_albedo_texture, pixel, 0).rgb, vec3(NRD_ALBEDO_MIN));
	vec4 color = imageLoad(output_image, pixel);
	color.rgb += diffuse + specular;
	imageStore(output_image, pixel, color);
}
