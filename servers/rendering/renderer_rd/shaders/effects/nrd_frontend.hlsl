// NRD's front end for the path tracer (PT_DENOISER_NRD_REBLUR, effects/nrd_denoiser.cpp): the light of each lobe the raygen
// wrote (radiance over the lobe's albedo, hit distance) and its normal and roughness guide, into the encodings NRD's
// denoisers take, through NRD's own front-end functions (NRD.hlsli, "NOISY INPUTS" in NRD's README).
//
// Compiled to SPIR-V by misc/scripts/install_nrd.py with NRD's DXC and NRD.hlsli, next to the NRD library it builds; the
// engine embeds that SPIR-V (never this source with NRD's header, which stays outside the repository: NVIDIA RTX SDKs licence).

#include "NRD.hlsli"

struct Constants {
	float4 hit_distance_parameters; // ReblurHitDistanceParameters A, B, C; w unused.
	uint2 size;
	uint relax; // 1: ReLAX's encoding, else ReBLUR's.
	uint pad;
};
[[vk::push_constant]] Constants constants;

[[vk::binding(0, 0)]] Texture2D<float4> in_diffuse; // rgb: diffuse light over the diffuse albedo, a: hit distance.
[[vk::binding(1, 0)]] Texture2D<float4> in_specular; // Same for the specular lobe.
[[vk::binding(2, 0)]] Texture2D<float4> in_normal_roughness; // World space normal, linear roughness.
[[vk::binding(3, 0)]] Texture2D<float> in_view_z;
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_diffuse; // IN_DIFF_RADIANCE_HITDIST
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_specular; // IN_SPEC_RADIANCE_HITDIST
[[vk::binding(6, 0)]] [[vk::image_format("rgb10a2")]] RWTexture2D<float4> out_normal_roughness; // IN_NORMAL_ROUGHNESS

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
	if (any(pixel >= constants.size)) {
		return;
	}

	const float4 normal_roughness = in_normal_roughness[pixel];
	const float view_z = in_view_z[pixel];
	const float4 diffuse = in_diffuse[pixel];
	const float4 specular = in_specular[pixel];

	out_normal_roughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normalize(normal_roughness.xyz), normal_roughness.w, 0.0);
	if (constants.relax != 0) {
		out_diffuse[pixel] = RELAX_FrontEnd_PackRadianceAndHitDist(diffuse.rgb, diffuse.a, true);
		out_specular[pixel] = RELAX_FrontEnd_PackRadianceAndHitDist(specular.rgb, specular.a, true);
	} else {
		// The diffuse lobe's hit distance is normalized with a roughness of 1, as NRD's README does ("isDiffusePath ? 1.0").
		const float diffuse_hit = REBLUR_FrontEnd_GetNormHitDist(diffuse.a, view_z, constants.hit_distance_parameters.xyz, 1.0);
		const float specular_hit = REBLUR_FrontEnd_GetNormHitDist(specular.a, view_z, constants.hit_distance_parameters.xyz, normal_roughness.w);
		out_diffuse[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse.rgb, diffuse_hit, true);
		out_specular[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular.rgb, specular_hit, true);
	}
}
