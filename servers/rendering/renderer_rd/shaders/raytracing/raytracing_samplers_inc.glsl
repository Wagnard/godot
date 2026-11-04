// 12 material samplers (filter/repeat combinations, matching samplers_inc.glsl).
layout(set = 0, binding = 16) uniform sampler SAMPLER_NEAREST_CLAMP;
layout(set = 0, binding = 17) uniform sampler SAMPLER_LINEAR_CLAMP;
layout(set = 0, binding = 18) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
layout(set = 0, binding = 19) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 0, binding = 20) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 0, binding = 21) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 0, binding = 22) uniform sampler SAMPLER_NEAREST_REPEAT;
layout(set = 0, binding = 23) uniform sampler SAMPLER_LINEAR_REPEAT;
layout(set = 0, binding = 24) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
layout(set = 0, binding = 25) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
layout(set = 0, binding = 26) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
layout(set = 0, binding = 27) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;

#ifdef RT_SPECULAR_ENERGY_COMPENSATION
// brdf_inc.glsl: raster's DFG LUT (RenderForwardClustered::dfg_lut), read as prefiltered_dfg() reads it.
layout(set = 0, binding = 47) uniform texture2D rt_dfg;
vec3 rt_specular_energy_compensation(vec3 p_f0, float p_roughness, float p_NdotV) {
	const float dfg_y = textureLod(sampler2D(rt_dfg, SAMPLER_LINEAR_CLAMP), vec2(clamp(p_NdotV, 0.0001, 1.0), 1.0 - p_roughness), 0.0).y;
	return 1.0 + p_f0 * (1.0 / max(dfg_y, 0.0001) - 1.0);
}
#endif
