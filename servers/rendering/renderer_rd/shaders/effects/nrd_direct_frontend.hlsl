// NRD's front end for the path tracer's direct light signal (rendering/pathtracing/nrd_direct_light_denoising,
// effects/nrd_denoiser.cpp): the light choice's diffuse light at the primary hits the raygen wrote (radiance over the
// diffuse albedo, distance to the occluder or the light) into the encoding of NRD's second ReBLUR (REBLUR_DIFFUSE), through
// NRD's own front-end functions (NRD.hlsli, "NOISY INPUTS" in NRD's README).
//
// Compiled to SPIR-V by misc/scripts/install_nrd.py with NRD's DXC and NRD.hlsli, as nrd_frontend.hlsl.

#include "NRD.hlsli"

struct Constants {
	float4 hit_distance_parameters; // ReblurHitDistanceParameters A, B, C; w unused.
	uint2 size;
	uint2 pad;
};
[[vk::push_constant]] Constants constants;

[[vk::binding(0, 0)]] Texture2D<float4> in_direct; // rgb: the direct diffuse light over the diffuse albedo, a: hit distance.
[[vk::binding(1, 0)]] Texture2D<float> in_view_z;
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_direct; // IN_DIFF_RADIANCE_HITDIST

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
	if (any(pixel >= constants.size)) {
		return;
	}
	const float4 direct = in_direct[pixel];
	// A diffuse signal: normalized with a roughness of 1, as nrd_frontend.hlsl's diffuse lobe.
	const float hit = REBLUR_FrontEnd_GetNormHitDist(direct.a, in_view_z[pixel], constants.hit_distance_parameters.xyz, 1.0);
	out_direct[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(direct.rgb, hit, true);
}
