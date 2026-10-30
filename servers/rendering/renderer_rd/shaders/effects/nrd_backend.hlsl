// NRD's back end for the path tracer (effects/nrd_denoiser.cpp): each lobe's denoised light out of NRD's encoding, through
// NRD's own back-end functions (NRD.hlsli), into the raygen's layout (light over the lobe's albedo), which
// raytracing_nrd_composite.glsl multiplies back by the albedo onto the path traced output.
//
// Compiled to SPIR-V by misc/scripts/install_nrd.py with NRD's DXC and NRD.hlsli, as nrd_frontend.hlsl.

#include "NRD.hlsli"

struct Constants {
	uint2 size;
	uint relax; // 1: ReLAX's encoding, else ReBLUR's.
	uint pad;
};
[[vk::push_constant]] Constants constants;

[[vk::binding(0, 0)]] Texture2D<float4> in_diffuse; // OUT_DIFF_RADIANCE_HITDIST
[[vk::binding(1, 0)]] Texture2D<float4> in_specular; // OUT_SPEC_RADIANCE_HITDIST
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_diffuse;
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_specular;

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
	if (any(pixel >= constants.size)) {
		return;
	}

	if (constants.relax != 0) {
		out_diffuse[pixel] = RELAX_BackEnd_UnpackRadiance(in_diffuse[pixel]);
		out_specular[pixel] = RELAX_BackEnd_UnpackRadiance(in_specular[pixel]);
	} else {
		out_diffuse[pixel] = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(in_diffuse[pixel]);
		out_specular[pixel] = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(in_specular[pixel]);
	}
}
