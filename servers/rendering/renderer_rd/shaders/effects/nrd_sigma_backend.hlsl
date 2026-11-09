// NRD SIGMA's back end for the path tracer's sun shadow (effects/nrd_denoiser.cpp): the denoised shadow out of SIGMA's
// encoding, through NRD's own back-end function (NRD.hlsli, SIGMA_BackEnd_UnpackShadow), into nrd_sun_image's alpha, which
// raytracing_nrd_composite.glsl multiplies the sun's light by.
//
// Compiled to SPIR-V by misc/scripts/install_nrd.py with NRD's DXC and NRD.hlsli, as nrd_frontend.hlsl.

#include "NRD.hlsli"

struct Constants {
	uint2 size;
	uint2 pad;
};
[[vk::push_constant]] Constants constants;

[[vk::binding(0, 0)]] Texture2D<float4> in_shadow; // OUT_SHADOW_TRANSLUCENCY
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> sun; // rgb: the sun's light, kept.

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
	if (any(pixel >= constants.size)) {
		return;
	}
	const float shadow = SIGMA_BackEnd_UnpackShadow(in_shadow[pixel]).x;
	sun[pixel] = float4(sun[pixel].rgb, shadow);
}
