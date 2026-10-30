// The light the Environment gives the rays that leave the scene, and its next event estimation in the raygen.
// Requires raytracing_inc.glsl, scene_data_inc.glsl, brdf_inc.glsl, raytracing_common_inc.glsl and radiance_octmap_sample();
// the raygen also tlas (binding 1) and payload (location 0).

#include "raytracing_sky_sampling_inc.glsl"

// World directions to the radiance octmap's (Environment.sky_rotation), as the miss shader has always turned them.
mat3 rt_world_to_sky() {
	return scene_data_block.data.radiance_inverse_xform * mat3(scene_data_block.data.inv_view_matrix);
}

vec2 rt_sky_uv(vec3 p_sky_dir) {
	const vec2 border = vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0);
	return vec3_to_oct_with_border(p_sky_dir, border);
}

// The fog over what a ray that leaves the scene sees (fog_sky_affect), as raster's over its sky.
vec3 rt_environment_fog(vec3 p_color, vec2 p_sky_uv) {
	if (!RT_FOG_ENABLED()) {
		return p_color;
	}
	vec3 fog_color = scene_data_block.data.fog_light_color;
	if (scene_data_block.data.fog_aerial_perspective > 0.0) {
		vec3 sky_fog = radiance_octmap_sample(p_sky_uv, 1.0 / MAX_ROUGHNESS_LOD);
		sky_fog *= scene_data_block.data.radiance_normalization;
		fog_color = mix(fog_color, sky_fog, scene_data_block.data.fog_aerial_perspective);
	}
	return mix(p_color, fog_color, scene_data_block.data.fog_sky_affect);
}

// What a ray that left a surface sees when it leaves the scene: the light the Environment gives the scene by the lobe it
// left along (SPECULAR_LOBE_FLAG), as raster's indirect_specular_light and ambient_light (scene_forward_clustered.glsl),
// under the fog. Camera rays see the background instead (miss shader).
vec3 rt_environment_light(vec3 p_sky_dir, bool p_specular_lobe) {
	const vec2 sky_uv = rt_sky_uv(p_sky_dir);
	vec3 color = vec3(0.0);
	if (p_specular_lobe) {
		// The reflected light (reflected_light_source): the sky; the background color for REFLECTION_SOURCE_BG with a color
		// background; nothing with REFLECTION_SOURCE_DISABLED.
		if (bool(scene_data_block.data.flags & SCENE_DATA_FLAGS_USE_REFLECTION_CUBEMAP)) {
			color = radiance_octmap_sample(sky_uv, 0.0) * scene_data_block.data.radiance_normalization;
		} else if (get_rt_param(RT_PARAM_REFLECTION_BACKGROUND) != 0.0) {
			color = vec3(get_rt_param(RT_PARAM_BACKGROUND_COLOR), get_rt_param(RT_PARAM_BACKGROUND_COLOR + 1), get_rt_param(RT_PARAM_BACKGROUND_COLOR + 2));
		}
	} else if (bool(scene_data_block.data.flags & SCENE_DATA_FLAGS_USE_AMBIENT_LIGHT)) {
		// The ambient light (ambient_light_source): the ambient color, or the background color for AMBIENT_SOURCE_BG, mixed
		// with the sky by ambient_light_sky_contribution when the sky lights the scene; nothing with AMBIENT_SOURCE_DISABLED.
		color = scene_data_block.data.ambient_light_color_energy.rgb;
		if (bool(scene_data_block.data.flags & SCENE_DATA_FLAGS_USE_AMBIENT_CUBEMAP)) {
			vec3 sky_radiance = radiance_octmap_sample(sky_uv, 0.0) * scene_data_block.data.radiance_normalization;
			color = mix(color, sky_radiance, scene_data_block.data.ambient_color_sky_mix);
		}
	}
	return rt_environment_fog(color, sky_uv);
}

#ifdef RT_STAGE_RAYGEN

// ============================================================================
// NEXT EVENT ESTIMATION OF THE ENVIRONMENT
// ============================================================================
// Outdoors, most of a surface's light comes from the sky, which BRDF sampling alone finds only by chance: one direction
// toward the sky per surface the path meets, light or dark. As PBRT v4's ImageInfiniteLight and Unreal's path tracer
// SkyLight, each surface also takes one direction from the sky's luminance distribution (raytracing_sky_sampling.glsl),
// traced like a BRDF ray, and the two estimators are combined by multiple importance sampling (Veach 1997, power
// heuristic). A lobe's environment differs (raster's ambient and reflection split): each lobe is combined on its own,
// with the light its own misses take, so both estimate the same integral the BRDF sampling alone did. Only where the sky
// lights that lobe; elsewhere the BRDF sampling keeps all of it.

layout(set = 0, binding = 40, std430) readonly buffer SkySampling {
	float sky_sampling[];
};

// A specular lobe smoother than this is left to BRDF sampling: its lobe finds the sky by itself, and a sky texel is wider.
#define RT_ENV_SAMPLING_MIN_ROUGHNESS 0.1

struct RTEnvSampling {
	bool diffuse; // The diffuse lobe's misses take the sky (ambient light from it): sampled.
	bool specular; // Same for the specular lobe and the reflected light.
	float diffuse_probability; // The lobe shade_and_bounce() picks with this chance.
	float specular_probability;
	float alpha; // GGX alpha: roughness squared.
	float total; // The distribution's integral (RT_SKY_SAMPLING_MARGINAL_CDF's last value).
	mat3 world_to_sky;
};

// p_extended: the raygen traces the BRDF ray this hit picks (not is_unextended()). p_n_dot_v: the clamped shading normal's
// cosine with the view, unclamped.
RTEnvSampling rt_env_sampling_setup(float p_specular_probability, float p_roughness, bool p_extended, float p_n_dot_v) {
	RTEnvSampling s;
	s.diffuse_probability = 1.0 - p_specular_probability;
	s.specular_probability = p_specular_probability;
	s.alpha = p_roughness * p_roughness;
	s.diffuse = false;
	s.specular = false;
	s.total = 0.0;
	s.world_to_sky = mat3(1.0);
	// Without the BRDF ray, its share of the sky would be missing.
	if (get_rt_param(RT_PARAM_ENV_SAMPLING) == 0.0 || !p_extended) {
		return s;
	}
	s.total = sky_sampling[RT_SKY_SAMPLING_MARGINAL_CDF + RT_SKY_SAMPLING_SIZE - 1u];
	if (!(s.total > 0.0) || isinf(s.total)) {
		return s;
	}
	const uint flags = scene_data_block.data.flags;
	s.diffuse = s.diffuse_probability > 0.0 && bool(flags & SCENE_DATA_FLAGS_USE_AMBIENT_LIGHT) && bool(flags & SCENE_DATA_FLAGS_USE_AMBIENT_CUBEMAP) && scene_data_block.data.ambient_color_sky_mix > 0.0;
	// The density of visible normals (rt_env_lobe_pdf()) is the sampler's only for a view above the shading normal
	// (Heitz 2018): below it, at a silhouette, the specular lobe keeps all of its light.
	s.specular = s.specular_probability > 0.0 && bool(flags & SCENE_DATA_FLAGS_USE_REFLECTION_CUBEMAP) && p_roughness >= RT_ENV_SAMPLING_MIN_ROUGHNESS && p_n_dot_v > 0.0;
	s.world_to_sky = rt_world_to_sky();
	return s;
}

// The solid angle density of a direction in the octmap's space.
float rt_sky_pdf(RTEnvSampling p_env, vec3 p_sky_dir) {
	const float func = sky_sampling[RT_SKY_SAMPLING_FUNC + rt_sky_sampling_texel(rt_equal_area_sphere_to_square(p_sky_dir))];
	return func * float(RT_SKY_SAMPLING_SIZE * RT_SKY_SAMPLING_SIZE) / (p_env.total * 4.0 * PI);
}

// The first index in [p_first, p_first + SIZE) whose cumulative value exceeds p_value.
uint rt_sky_sampling_find(uint p_first, float p_value) {
	uint lo = 0u;
	uint hi = RT_SKY_SAMPLING_SIZE - 1u;
	while (lo < hi) {
		const uint mid = (lo + hi) >> 1u;
		if (sky_sampling[p_first + mid] > p_value) {
			hi = mid;
		} else {
			lo = mid + 1u;
		}
	}
	return lo;
}

// A direction in the octmap's space, from the marginal then the row's distribution (PBRT v4 PiecewiseConstant2D::Sample).
vec3 rt_sky_sample(RTEnvSampling p_env, vec2 p_u) {
	const float target_y = p_u.y * p_env.total;
	const uint y = rt_sky_sampling_find(RT_SKY_SAMPLING_MARGINAL_CDF, target_y);
	const float before_y = y > 0u ? sky_sampling[RT_SKY_SAMPLING_MARGINAL_CDF + y - 1u] : 0.0;
	const float row_sum = sky_sampling[RT_SKY_SAMPLING_MARGINAL_CDF + y] - before_y;

	const uint row = y * RT_SKY_SAMPLING_SIZE;
	const float target_x = p_u.x * sky_sampling[RT_SKY_SAMPLING_ROW_CDF + row + RT_SKY_SAMPLING_SIZE - 1u];
	const uint x = rt_sky_sampling_find(RT_SKY_SAMPLING_ROW_CDF + row, target_x);
	const float before_x = x > 0u ? sky_sampling[RT_SKY_SAMPLING_ROW_CDF + row + x - 1u] : 0.0;
	const float func = sky_sampling[RT_SKY_SAMPLING_FUNC + row + x];

	// Where in the texel, from what the target leaves past its start.
	const vec2 offset = clamp(vec2(func > 0.0 ? (target_x - before_x) / func : 0.5, row_sum > 0.0 ? (target_y - before_y) / row_sum : 0.5), vec2(0.0), vec2(1.0));
	return rt_equal_area_square_to_sphere((vec2(x, y) + offset) / float(RT_SKY_SAMPLING_SIZE));
}

// The density with which shade_and_bounce() samples a lobe's direction, its chance of picking that lobe included: the
// cosine-weighted hemisphere for the diffuse lobe, GGX visible normals for the specular one. In the shading normal's frame.
float rt_env_lobe_pdf(RTEnvSampling p_env, bool p_specular, vec3 p_v_local, vec3 p_l_local) {
	if (!p_specular) {
		return p_env.diffuse_probability * max(p_l_local.z, 0.0) * ONE_OVER_PI;
	}
	const vec3 h = normalize(p_v_local + p_l_local);
	return p_env.specular_probability * sampleGGXVNDFReflectionPdf(p_env.alpha, p_env.alpha * p_env.alpha, saturate(h.z), p_v_local.z, saturate(dot(p_l_local, h)));
}

// The sky's share of a direction, power heuristic, in 1/65535: the same quantized value on both sides, so the weights of a
// direction sum to one exactly.
uint rt_env_mis_share(float p_lobe_pdf, float p_sky_pdf) {
	const float lobe2 = p_lobe_pdf * p_lobe_pdf;
	const float sky2 = p_sky_pdf * p_sky_pdf;
	if (!(sky2 > 0.0)) {
		return 0u;
	}
	return uint(round(clamp(sky2 / (lobe2 + sky2), 0.0, 1.0) * 65535.0));
}

// The sky's share for the direction the BRDF sampling picked (SPECULAR_LOBE_FLAG gives its lobe), for the miss shader
// (set_env_mis_share()). p_recovered: mirrored above the surface (recoverBelowHemisphereSample()), a direction next
// event estimation never takes, nor one below the shading normal: those keep all of their light.
uint rt_env_bounce_share(RTEnvSampling p_env, bool p_specular, vec3 p_n, vec3 p_v, vec3 p_dir, bool p_recovered) {
	if (p_recovered || !(p_specular ? p_env.specular : p_env.diffuse) || dot(p_n, p_dir) <= 0.0) {
		return 0u;
	}
	const vec4 to_local = getRotationToZAxis(p_n);
	const vec3 v_local = rotatePoint(to_local, p_v);
	const vec3 l_local = rotatePoint(to_local, p_dir);
	return rt_env_mis_share(rt_env_lobe_pdf(p_env, p_specular, v_local, l_local), rt_sky_pdf(p_env, p_env.world_to_sky * p_dir));
}

// Nothing a camera or BRDF ray would hit is in the way: the same flags, mask and any-hit shaders (cut-outs), so a
// direction is seen by both estimators alike.
bool rt_environment_visible(vec3 p_origin, vec3 p_direction) {
	const uint flags = RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT;
#if defined(USE_SER)
	hitObjectEXT hit_object;
	hitObjectTraceRayEXT(hit_object, tlas, flags, RT_MASK_BOUNCE, 0, 0, 0, p_origin, 0.001, p_direction, 10000.0, 0);
	return !hitObjectIsHitEXT(hit_object);
#else
	// As lights_trace_shadow_ray_pipeline(): the miss shader ends a shadow ray.
	payload.packed_bounces_flags = set_shadow_ray(0u);
	traceRayEXT(tlas, flags, RT_MASK_BOUNCE, 0, 0, 0, p_origin, 0.001, p_direction, 10000.0, 0);
	return is_path_terminated(payload.packed_bounces_flags);
#endif
}

// One direction from the sky's distribution: what each lobe would take from it, as shade_and_bounce()'s BRDF sampling
// weighs that direction (its sample weight times its density), times the sky's share of the direction. p_n: the clamped
// shading normal; p_ao: the occlusion the diffuse bounce applies. r_specular: the specular lobe's part of the result.
vec3 rt_environment_nee(RTEnvSampling p_env, vec3 p_origin, vec3 p_geometry_normal, vec3 p_n, vec3 p_v, MaterialProperties p_material, float p_ao, inout uint r_rng_state, out vec3 r_specular) {
	r_specular = vec3(0.0);
	const vec3 sky_dir = rt_sky_sample(p_env, rand2(r_rng_state));
	const vec3 l = normalize(transpose(p_env.world_to_sky) * sky_dir);
	// Directions the BRDF sampling never traces as they are: below the surface or the shading normal.
	if (dot(p_geometry_normal, l) <= 0.0 || dot(p_n, l) <= 0.0) {
		return vec3(0.0);
	}
	// The density of the direction itself, as rt_env_bounce_share() reads it.
	const float sky_pdf = rt_sky_pdf(p_env, p_env.world_to_sky * l);
	if (!(sky_pdf > 0.0)) {
		return vec3(0.0);
	}

	const vec4 to_local = getRotationToZAxis(p_n);
	const vec3 v_local = rotatePoint(to_local, p_v);
	const vec3 l_local = rotatePoint(to_local, l);
	const BrdfData data = prepareBRDFData(vec3(0.0, 0.0, 1.0), l_local, v_local, p_material);

	vec3 value = vec3(0.0);
	if (p_env.diffuse) {
		// evalIndirectCombinedBRDF(DIFFUSE_TYPE): the diffuse reflectance times one minus the Fresnel of a half vector it
		// samples with the same random numbers as the direction (sampleHemisphere()), which this direction gives back.
		vec3 diffuse = data.diffuseReflectance * (l_local.z * ONE_OVER_PI);
#if COMBINE_BRDFS_WITH_FRESNEL
		float u_y = atan(l_local.y, l_local.x) * ONE_OVER_TWO_PI;
		const vec2 u = vec2(dot(l_local.xy, l_local.xy), u_y < 0.0 ? u_y + 1.0 : u_y);
		const vec3 h = sampleSpecularHalfVector(v_local, vec2(data.alpha, data.alpha), u);
		const float v_dot_h = max(0.00001, min(1.0, dot(v_local, h)));
		diffuse *= vec3(1.0) - evalFresnel(data.specularF0, shadowedF90(data.specularF0), v_dot_h);
#endif
		const float share = float(rt_env_mis_share(rt_env_lobe_pdf(p_env, false, v_local, l_local), sky_pdf)) / 65535.0;
		value += diffuse * p_ao * rt_environment_light(sky_dir, false) * share;
	}
	if (p_env.specular) {
		// sampleSpecularMicrofacet(): its weight (the multiple scattering approximation, Fresnel, G2 / G1) times the
		// density of GGX visible normals, the lobe's chance aside.
		const float n_dot_l = max(0.00001, min(1.0, l_local.z));
		const float n_dot_v = max(0.00001, min(1.0, v_local.z));
		const float h_dot_l = max(0.00001, min(1.0, data.LdotH));
		const float n_dot_h = max(0.00001, min(1.0, data.NdotH));
#if MICROFACET_DISTRIBUTION == GGX
		const vec3 ms = evalMultiScatterApproximation(data.alpha, n_dot_v, data.specularF0);
#else
		const vec3 ms = vec3(1.0);
#endif
		const vec3 f = evalFresnel(data.specularF0, shadowedF90(data.specularF0), h_dot_l);
		const float lobe_pdf = rt_env_lobe_pdf(p_env, true, v_local, l_local);
		const vec3 specular = ms * f * specularSampleWeight(data.alpha, data.alphaSquared, n_dot_l, n_dot_v, h_dot_l, n_dot_h) * (lobe_pdf / p_env.specular_probability);
		const float share = float(rt_env_mis_share(lobe_pdf, sky_pdf)) / 65535.0;
		r_specular = specular * rt_environment_light(sky_dir, true) * share;
		value += r_specular;
	}

	if (!(dot(value, vec3(1.0)) > 0.0) || !rt_environment_visible(p_origin, l)) {
		r_specular = vec3(0.0);
		return vec3(0.0);
	}
	r_specular /= sky_pdf;
	return value / sky_pdf;
}

#endif // RT_STAGE_RAYGEN
