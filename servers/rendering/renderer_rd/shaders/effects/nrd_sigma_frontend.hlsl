// NRD SIGMA's front end for the path tracer's sun shadow (effects/nrd_denoiser.cpp): the distance to the occluder the raygen
// wrote for the sun (nrd_sun_image's alpha: 0 where the surface faces away from the sun, NRD_FP16_MAX where its ray reached
// the sun) into SIGMA's IN_PENUMBRA, through NRD's own front-end function for an infinite light source (NRD.hlsli,
// SIGMA_FrontEnd_PackPenumbra; "Distance to occluder (SIGMA)" in NRD's README).
//
// Compiled to SPIR-V by misc/scripts/install_nrd.py with NRD's DXC and NRD.hlsli, as nrd_frontend.hlsl.

#include "NRD.hlsli"

struct Constants {
	uint2 size;
	float tan_angular_radius; // tan() of the sun's angular radius (half of DirectionalLight3D.light_angular_distance).
	uint pad;
};
[[vk::push_constant]] Constants constants;

[[vk::binding(0, 0)]] Texture2D<float4> in_sun; // rgb: the sun's light, a: the distance to the occluder.
[[vk::binding(1, 0)]] [[vk::image_format("r16f")]] RWTexture2D<float> out_penumbra; // IN_PENUMBRA

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
	if (any(pixel >= constants.size)) {
		return;
	}
	out_penumbra[pixel] = SIGMA_FrontEnd_PackPenumbra(in_sun[pixel].a, constants.tan_angular_radius);
}
