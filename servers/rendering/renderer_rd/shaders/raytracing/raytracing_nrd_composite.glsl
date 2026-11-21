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
// The sun whose shadow NRD's SIGMA denoised: its light without its shadow (rgb), SIGMA's shadow unpacked by its back end (a).
layout(set = 0, binding = 6) uniform texture2D sun_texture;

// The direct light signal (rendering/pathtracing/nrd_direct_light_denoising): the light choice's diffuse light over the
// diffuse albedo, denoised by NRD's second ReBLUR.
layout(set = 0, binding = 7) uniform texture2D direct_texture;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	uint sun; // 1: SIGMA ran this frame, the sun's light times its shadow is added.
	uint direct; // 1: the direct light signal is added (the raygen moved it out of the diffuse lobe).
}
params;

// As NRD_ALBEDO_MIN (raytracing_inc.glsl): the floor the raygen divided by.
#define NRD_ALBEDO_MIN 0.001

// PT-NRD-COMPOSITE-NAN-WA: every value this pass adds up is read from a texture another pass wrote, and a component that is
// not finite becomes 0. Intel's D3D12 shader compiler (Arc B580, driver 32.0.101.9150) gives the raygen's NRD variant a NaN
// or Inf at ~220 random pixels per 1080p frame, in values whose terms are all finite; Vulkan on the same GPU and NVIDIA
// never do, the DXIL validates and holds no undefined value. The NaN reached the output copy, which turns it into 100.0:
// white dots. The test is on the bits of loaded values: in the raygen the DXIL is fast-math, and Intel's compiler removed
// isnan() / isinf() tests of computed values there. NRD's own front end replaces non-finite inputs with 0 the same way
// (REBLUR_FrontEnd_PackRadianceAndNormHitDist(..., true)). Remove when an Intel driver passes ptsnowtest (render-benches)
// with NRD without it: no white dot (count_dots.ps1) and no non-finite image value at this pass.
vec3 finite_or_zero(vec3 p_value) {
	const bvec3 not_finite = equal(floatBitsToUint(p_value) & uvec3(0x7F800000u), uvec3(0x7F800000u));
	return mix(p_value, vec3(0.0), not_finite);
}

void main() {
	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) {
		return;
	}
	vec4 color = imageLoad(output_image, pixel);
	const vec3 color_finite = finite_or_zero(color.rgb);
	if (imageLoad(depth_image, pixel).r == 0.0) {
		// Where the camera ray missed, the image is the sky's light alone: written back only when it was not finite.
		if (any(notEqual(floatBitsToUint(color_finite), floatBitsToUint(color.rgb)))) {
			imageStore(output_image, pixel, vec4(color_finite, color.a));
		}
		return;
	}

	const vec3 diffuse_albedo = max(finite_or_zero(texelFetch(diffuse_albedo_texture, pixel, 0).rgb), vec3(NRD_ALBEDO_MIN));
	const vec3 specular_albedo = max(finite_or_zero(texelFetch(specular_albedo_texture, pixel, 0).rgb), vec3(NRD_ALBEDO_MIN));
	color.rgb = color_finite;
	color.rgb += finite_or_zero(texelFetch(diffuse_texture, pixel, 0).rgb) * diffuse_albedo + finite_or_zero(texelFetch(specular_texture, pixel, 0).rgb) * specular_albedo;
	if (params.direct != 0u) {
		color.rgb += finite_or_zero(texelFetch(direct_texture, pixel, 0).rgb) * diffuse_albedo;
	}
	if (params.sun != 0u) {
		const vec4 sun = texelFetch(sun_texture, pixel, 0);
		color.rgb += finite_or_zero(sun.rgb) * finite_or_zero(sun.aaa).x;
	}
	imageStore(output_image, pixel, color);
}
