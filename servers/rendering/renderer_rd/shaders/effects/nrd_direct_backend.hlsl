// NRD's back end for the path tracer's direct light signal (effects/nrd_denoiser.cpp): the denoised light out of NRD's
// encoding, through NRD's own back-end function (NRD.hlsli), into the raygen's layout (light over the diffuse albedo),
// which raytracing_nrd_composite.glsl multiplies back by the albedo.
//
// Compiled to SPIR-V by misc/scripts/install_nrd.py with NRD's DXC and NRD.hlsli, as nrd_frontend.hlsl.

#include "NRD.hlsli"

struct Constants {
	uint2 size;
	uint2 pad;
};
[[vk::push_constant]] Constants constants;

[[vk::binding(0, 0)]] Texture2D<float4> in_direct; // OUT_DIFF_RADIANCE_HITDIST of the second ReBLUR.
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_direct;

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
	if (any(pixel >= constants.size)) {
		return;
	}
	out_direct[pixel] = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(in_direct[pixel]);
}
