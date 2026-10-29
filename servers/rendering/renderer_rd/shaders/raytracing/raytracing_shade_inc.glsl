// Shading of a path's hits, in the raygen. The closest hit only evaluates the material into the payload (hit_pack());
// the light, the next direction and the path's state stay here. NVIDIA, "Best Practices for Using NVIDIA RTX Ray Tracing
// (Updated)", Hit shading: "Consider moving common code outside of hit and miss shaders", "Minimize live state across
// ray-trace calls", "Keep the ray payload small".
//
// Required includes (before this file):
//   raytracing_inc.glsl, brdf_inc.glsl, raytracing_hit_inc.glsl, raytracing_lights_inc.glsl, raytracing_environment_inc.glsl
//
// Required bindings (before this file):
//   tlas, payload, scene_data_block, geometries[], materials[], bindless_textures[], SAMPLER_*, rt_params,
//   radiance_octmap_sample(), DLSS-RR images (ifdef DLSS_RR_ENABLED)

// ============================================================================
// ENVIRONMENT FOG (per ray segment)
// ============================================================================

vec3 fog_get_directional_color(uint index) {
	return rt_lights[index].emission;
}

vec3 fog_get_directional_direction(uint index) {
	vec3 world_dir = -normalize(rt_lights[index].position);
	mat3 view_rot = transpose(mat3(
			scene_data_block.data.view_matrix[0].xyz,
			scene_data_block.data.view_matrix[1].xyz,
			scene_data_block.data.view_matrix[2].xyz));
	return view_rot * world_dir;
}

#define FOG_HAS_RADIANCE

vec3 fog_sample_radiance(vec3 vertex, float mip_level) {
	vec3 cube_view = scene_data_block.data.radiance_inverse_xform * vertex;
	vec2 border = vec2(scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 cube_uv = vec3_to_oct_with_border(cube_view, border);
	// mip_level is a normalized roughness (0..1); radiance_octmap_sample maps it
	// onto the prefiltered roughness array layers.
	return radiance_octmap_sample(cube_uv, mip_level);
}

#include "../fog_inc.glsl"

/// Apply environment fog for the ray segment that was just traversed.
/// Attenuates throughput and adds in-scattered fog color.
void apply_segment_fog(vec3 p_ray_origin, vec3 p_ray_dir, float segment_dist, inout vec3 radiance, inout vec3 throughput) {
	if (!RT_FOG_ENABLED()) {
		return;
	}

	// Build a view-space vertex along the ray direction at the hit distance.
	// fog_process needs view-space position for distance and height calculations.
	mat4 view_mat = transpose(mat4(
			scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 world_hit = p_ray_origin + p_ray_dir * segment_dist;
	vec3 vertex = (view_mat * vec4(world_hit, 1.0)).xyz;

	vec4 fog = fog_process(scene_data_block.data, vertex);
	radiance += throughput * fog.rgb * fog.a;
	throughput *= (1.0 - fog.a);
}

// ============================================================================
// SHADE AND BOUNCE
// ============================================================================

/// Production shading: emissive + NEE direct lighting + BRDF importance sampling + next bounce.
/// Also handles DLSS-RR G-buffer output on primary ray. False when the path ends here; else the next ray leaves the hit
/// along r_next_dir, from the hit offset along r_offset_normal.
bool shade_and_bounce(inout PathState ps, HitRecord r, vec3 p_ray_origin, vec3 p_ray_dir, out vec3 r_offset_normal, out vec3 r_next_dir) {
	r_offset_normal = r.geometry_normal;
	r_next_dir = p_ray_dir;
	const vec3 hit_pos = p_ray_origin + p_ray_dir * r.hit_t;
	const bool unshaded = (r.flags & HIT_FLAG_UNSHADED) != 0u;
	float metalness = r.metalness;
	float roughness = r.roughness;

	vec3 V = -p_ray_dir;

	// Clamp shading normal toward geometry at grazing view angles to keep BRDF above the geometry hemisphere.
	vec3 N = clampShadingNormal(r.normal, r.geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
	float NdotV = max(dot(N, V), 0.0001);

	uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(ps.packed_bounces_flags);

	// Environment fog for this ray segment (before surface contribution).
	apply_segment_fog(p_ray_origin, p_ray_dir, r.hit_t, ps.radiance, ps.throughput);

	// Emissive contribution; raster's unshaded output is the albedo alone. An unshaded surface is seen, in reflections too,
	// but lights nothing, as in raster: its albedo after a diffuse bounce made small unshaded shapes (candle flames) into
	// lights only rare paths reach, noise the denoiser turned into a shimmer.
	ps.radiance += ps.throughput * (unshaded ? (diffuse_bounces == 0u ? r.albedo : vec3(0.0)) : r.emissive);
	if (unshaded) {
		metalness = 0.0;
		roughness = 1.0;
	}

	// Bounce limit check.
	if (total_bounces >= RT_GET_MAX_BOUNCES() || diffuse_bounces >= MAX_DIFFUSE_BOUNCES) {
		return false;
	}

	// BRDF material setup.
	MaterialProperties brdf_mat;
	brdf_mat.baseColor = r.albedo;
	brdf_mat.metalness = metalness;
	brdf_mat.roughness = roughness;
	brdf_mat.dielectricF0 = specular_to_f0(r.specular);
	brdf_mat.emissive = r.emissive;
	brdf_mat.transmissivness = 0.0;
	brdf_mat.opacity = 1.0;

	vec3 specularF0 = baseColorToSpecularF0(brdf_mat.baseColor, brdf_mat.metalness, brdf_mat.dielectricF0);
	vec3 diffuseReflectance = baseColorToDiffuseReflectance(brdf_mat.baseColor, brdf_mat.metalness);

	// =================================================================
	// DLSS Ray Reconstruction output (primary ray, sample 0 only)
	// =================================================================
#ifdef DLSS_RR_ENABLED
	if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
		ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

		vec3 diffuse_albedo = DLSSRR_encodeDiffuseAlbedo(DLSSRR_computeDiffuseAlbedo(r.albedo, metalness));
		imageStore(dlss_rr_diffuse_albedo, pixel, vec4(diffuse_albedo, 1.0));

		vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(r.albedo, metalness, brdf_mat.dielectricF0, roughness, NdotV);
		imageStore(dlss_rr_specular_albedo, pixel, vec4(clamp(specular_albedo, vec3(0.04), vec3(1.0)), 1.0)); // match UNORM8 like before - fixes some issues with garbling..

		imageStore(dlss_rr_normal_roughness, pixel, vec4(N, roughness));

		// Specular hit distance via inline ray query (only for smooth surfaces).
		float spec_hit_dist = -1.0;
		if (roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
			vec3 spec_dir = reflect(-V, N);
			vec3 spec_origin = offset_ray_origin(hit_pos, spec_dir);

			rayQueryEXT spec_rq;
			rayQueryInitializeEXT(spec_rq, tlas, RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT,
					RT_MASK_BOUNCE, spec_origin, 0.001, spec_dir, 10000.0);
			while (rayQueryProceedEXT(spec_rq)) {
				if (rayQueryGetIntersectionTypeEXT(spec_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
					if (ray_query_alpha_test(
								rayQueryGetIntersectionInstanceCustomIndexEXT(spec_rq, false),
								rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, false),
								rayQueryGetIntersectionBarycentricsEXT(spec_rq, false))) {
						rayQueryConfirmIntersectionEXT(spec_rq);
					}
				}
			}
			float spec_t_max = 10000.0;
			if (rayQueryGetIntersectionTypeEXT(spec_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
				spec_hit_dist = rayQueryGetIntersectionTEXT(spec_rq, true);
				spec_t_max = spec_hit_dist;
			}
#ifdef USE_SER
			// The query skipped procedural geometry, whose hit only its hit group's intersection shader finds (see
			// lights_trace_shadow_ray()). A hit object runs it, and gives the distance without a closest hit shader.
			// Without SER, procedural geometry is missing from this guide.
			if ((uint(get_rt_param(RT_PARAM_PROCEDURAL_MASKS)) & RT_MASK_PROCEDURAL_CAMERA) != 0u) {
				hitObjectEXT spec_hit_object;
				hitObjectTraceRayEXT(spec_hit_object, tlas, RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT,
						RT_MASK_PROCEDURAL_CAMERA, 0, 0, 0, spec_origin, 0.001, spec_dir, spec_t_max, 0);
				if (hitObjectIsHitEXT(spec_hit_object)) {
					spec_hit_dist = hitObjectGetRayTMaxEXT(spec_hit_object);
				}
			}
#endif
		}
		imageStore(dlss_rr_specular_hit_dist, pixel, vec4(spec_hit_dist));
	}
#endif

	if (unshaded) {
		return false;
	}

	// =================================================================
	// NEE: Next Event Estimation (direct light sampling)
	// =================================================================
	uint rt_light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	if (rt_light_count > 0u) {
		vec3 hit_pos_offset = offset_ray_origin(hit_pos, r.geometry_normal);
		vec3 hit_pos_back = offset_ray_origin(hit_pos, -r.geometry_normal);
		bool is_indirect = (diffuse_bounces > 0u);
		vec3 direct_light = lights_evaluate_direct_lighting(
				hit_pos_offset, hit_pos_back, r.geometry_normal, N, V, brdf_mat, r.backlight, (r.flags & HIT_FLAG_SPECULAR_DISABLED) != 0u, ps.rng_state, is_indirect, rt_light_count, geometries[r.geometry_idx].layer_mask);
		ps.radiance += ps.throughput * direct_light * mix(1.0, r.ao, r.ao_light_affect);
	}

	// =================================================================
	// BRDF importance sampling for next bounce
	// =================================================================
	float specularLum = luminance(specularF0);
	float diffuseLum = luminance(diffuseReflectance);
	// The chance of sampling the specular lobe, as picked below.
	const float specular_probability = diffuseLum < 0.0001 ? 1.0 : (specularLum < 0.0001 ? 0.0 : clamp(specularLum / (specularLum + diffuseLum), 0.01, 0.99));

	// =================================================================
	// NEE of the Environment: one direction from the sky's distribution, combined with the BRDF sampling below by
	// multiple importance sampling (raytracing_environment_inc.glsl)
	// =================================================================
	RTEnvSampling env_sampling = rt_env_sampling_setup(specular_probability, roughness, !is_unextended(ps.packed_bounces_flags), dot(N, V));
	if (env_sampling.diffuse || env_sampling.specular) {
		vec3 env_light = rt_environment_nee(env_sampling, offset_ray_origin(hit_pos, r.geometry_normal), r.geometry_normal, N, V, brdf_mat, r.ao, ps.rng_state);
		ps.radiance += ps.throughput * env_light;
	}

	int brdfType;
	if (diffuseLum < 0.0001) {
		brdfType = SPECULAR_TYPE;
	} else if (specularLum < 0.0001) {
		brdfType = DIFFUSE_TYPE;
	} else {
		float brdfProbability = specular_probability;
		if (rand(ps.rng_state) < brdfProbability) {
			brdfType = SPECULAR_TYPE;
			ps.throughput /= brdfProbability;
		} else {
			brdfType = DIFFUSE_TYPE;
			ps.throughput /= (1.0 - brdfProbability);
		}
	}

	vec2 u = rand2(ps.rng_state);
	vec3 next_dir;
	vec3 brdf_weight;
	bool recovered = false;
	if (!evalIndirectCombinedBRDF(u, N, r.geometry_normal, V, brdf_mat, brdfType, next_dir, brdf_weight, vec4(0.0))) {
		// Two failure modes:
		//   1) Sample weight is zero (no contribution). Terminate.
		//   2) Sampled direction is below the geometry plane. Recover by
		//      mirroring across the geometry plane (see brdf_inc.glsl). The
		//      mirror is gated by RT_BELOW_HEMISPHERE_RECOVERY_ENABLED; when
		//      disabled, recovery returns false and we terminate the path.
		vec3 recovered_dir;
		if (luminance(brdf_weight) == 0.0 ||
				!recoverBelowHemisphereSample(next_dir, r.geometry_normal, recovered_dir)) {
			return false;
		}
		next_dir = recovered_dir;
		recovered = true;
	}

	ps.throughput *= brdf_weight;
	if (brdfType == DIFFUSE_TYPE) {
		ps.throughput *= r.ao; // As raster's AO on the ambient (indirect diffuse) light.
	}

	// Russian roulette, as pbrt-v4's PathIntegrator::Li: from the path's second vertex on, a path whose throughput has
	// fallen below 1 goes on with a probability equal to its largest component and is divided by it; the others end here.
	// Unbiased: what the surviving paths bring is scaled by one over the probability they had to survive. The paths that
	// would bring little are the ones that end. rendering/pathtracing/russian_roulette.
	if (total_bounces >= 1u && (uint(get_rt_param(RT_PARAM_FLAGS)) & RT_PARAM_FLAG_RUSSIAN_ROULETTE) != 0u) {
		const float survival = max(ps.throughput.r, max(ps.throughput.g, ps.throughput.b));
		if (survival < 1.0) {
			if (rand(ps.rng_state) >= survival) {
				return false;
			}
			ps.throughput /= survival;
		}
	}

	if (brdfType == DIFFUSE_TYPE) {
		ps.packed_bounces_flags = inc_diffuse_bounce(ps.packed_bounces_flags);
	} else {
		ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
	}
	// If the next ray leaves the scene, it takes the Environment's light by this lobe: as raster's ambient_light for the
	// diffuse lobe and indirect_specular_light for the specular one (scene_forward_clustered.glsl).
	ps.packed_bounces_flags = set_specular_lobe(ps.packed_bounces_flags, brdfType == SPECULAR_TYPE);
	// The share of that light next event estimation of the sky took for this direction, which the miss leaves to it.
	ps.packed_bounces_flags = set_env_mis_share(ps.packed_bounces_flags, rt_env_bounce_share(env_sampling, brdfType == SPECULAR_TYPE, N, V, next_dir, recovered));

	r_next_dir = next_dir;
	return true;
}

// ============================================================================
// FRONT LAYER
// ============================================================================

/// A front layer hit (a transparent surface before the opaque ones): a direction in its specular lobe, for its
/// reflection. The raster transparent pass shades the surface.
vec3 front_layer_reflection_dir(HitRecord r, vec3 p_ray_dir, inout uint r_rng_state) {
	const vec3 V = -p_ray_dir;
	MaterialProperties front_mat;
	front_mat.baseColor = r.albedo;
	front_mat.metalness = r.metalness;
	front_mat.roughness = r.roughness;
	front_mat.dielectricF0 = specular_to_f0(r.specular);
	front_mat.emissive = vec3(0.0);
	front_mat.transmissivness = 0.0;
	front_mat.opacity = 1.0;
	// The material's normal as raster reflects on it: clamping it toward the faceted geometry at grazing angles
	// would reflect the triangles of a displaced surface (waves) instead of its smooth normal.
	vec3 front_dir;
	vec3 front_weight;
	if (!evalIndirectCombinedBRDF(rand2(r_rng_state), r.normal, r.normal, V, front_mat, SPECULAR_TYPE, front_dir, front_weight, vec4(0.0))) {
		front_dir = reflect(-V, r.normal);
	}
	// Below the surface, the ray would cross it: mirror it back above.
	front_dir -= 2.0 * min(dot(front_dir, r.geometry_normal), 0.0) * r.geometry_normal;
	return front_dir;
}

// ============================================================================
// DEBUG VIEWS THAT TRACE A REFLECTION
// ============================================================================

#ifdef RT_DEBUG_ENABLED
/// View 1: the geometry normals seen in a mirror (the first hit reflects the ray about its geometry normal). View 13: the
/// specular hit distance Ray Reconstruction is given, as a heat map, on the surfaces smooth enough to get one; blue where
/// none. The closest hit returns the material as for shading. False when the view's color is final.
bool debug_reflection_view(int p_mode, inout PathState ps, HitRecord r, bool p_missed, inout vec3 ray_origin, inout vec3 ray_dir) {
	const bool first = get_total_bounces(ps.packed_bounces_flags) == 0u;
	if (p_missed) {
		// The miss gives the sky. View 13: a reflection that leaves the scene keeps the color its surface set.
		if (!(p_mode == 13 && !first)) {
			ps.radiance = r.emissive;
		}
		return false;
	}
	const vec3 hit_pos = ray_origin + ray_dir * r.hit_t;
	if ((r.flags & HIT_FLAG_PASS_THROUGH) != 0u) {
		ray_origin = offset_ray_origin(hit_pos, ray_dir);
		return true;
	}
	if (p_mode == 1) {
		if (first) {
			ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
			ray_origin = offset_ray_origin(hit_pos, r.geometry_normal);
			ray_dir = reflect(ray_dir, r.geometry_normal);
			return true;
		}
		ps.radiance = r.geometry_normal * 0.5 + 0.5;
		return false;
	}
	if (first) {
		ps.radiance = vec3(0.1, 0.1, 0.4);
		if (r.roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
			ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
			ray_origin = offset_ray_origin(hit_pos, r.normal);
			ray_dir = reflect(ray_dir, r.normal);
			return true;
		}
		return false;
	}
	const float v = clamp(log(r.hit_t + 1.0) / log(1000.0), 0.0, 1.0);
	if (v < 0.33) {
		ps.radiance = mix(vec3(0.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), v * 3.0);
	} else if (v < 0.66) {
		ps.radiance = mix(vec3(1.0, 0.0, 0.0), vec3(1.0, 1.0, 0.0), (v - 0.33) * 3.0);
	} else {
		ps.radiance = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 1.0, 1.0), (v - 0.66) * 3.0);
	}
	return false;
}
#endif // RT_DEBUG_ENABLED

// ============================================================================
// ONE RAY OF A PATH
// ============================================================================

/// Traces the path's next ray and shades its hit. False when the path ends there (a miss, a debug view, or the shading
/// ended it); else the ray to trace next. r_hit_t: the hit's distance.
bool trace_path_ray(inout PathState ps, inout vec3 ray_origin, inout vec3 ray_dir, out float r_hit_t, bool p_camera_ray) {
	payload.packed_bounces_flags = ps.packed_bounces_flags;
	payload.hit_t = 0.0;
	// The camera ray leaves out what the raster pass draws over the traced image (RT_MASK_SECONDARY); every other ray sees
	// it, the first ray of a front layer reflection included (it starts at bounce 0 too).
	const uint mask = p_camera_ray ? RT_MASK_CAMERA : RT_MASK_BOUNCE;
#ifdef USE_SER
	hitObjectEXT hitObject;
	hitObjectTraceRayEXT(hitObject, tlas, RT_RAY_FLAGS, mask, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);

	// Reorder with a coherence hint that has 8 bits
	uint hint = 0;
	if (hitObjectIsHitEXT(hitObject)) {
		// TODO: This hint barely does anything. There is a lot of untapped potential here.
		hint = hitObjectGetInstanceIdEXT(hitObject);
	}
	reorderThreadEXT(hitObject, hint, 8);

	hitObjectExecuteShaderEXT(hitObject, 0);
#else
	traceRayEXT(tlas, RT_RAY_FLAGS, mask, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);
#endif

	const HitRecord r = hit_unpack(payload);
	r_hit_t = r.hit_t;
#ifdef RT_DEBUG_ENABLED
	{
		const int vis_mode = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (vis_mode == 1 || vis_mode == 13) {
			return debug_reflection_view(vis_mode, ps, r, is_path_terminated(payload.packed_bounces_flags), ray_origin, ray_dir);
		}
	}
#endif
	if ((r.flags & HIT_FLAG_DEBUG) != 0u) {
		ps.radiance = r.emissive;
		return false;
	}
	if (is_path_terminated(payload.packed_bounces_flags)) {
		// A miss: the light the Environment brings along this ray.
		ps.radiance += ps.throughput * r.emissive;
		return false;
	}
	const vec3 hit_pos = ray_origin + ray_dir * r.hit_t;
	if ((r.flags & HIT_FLAG_PASS_THROUGH) != 0u) {
		ray_origin = offset_ray_origin(hit_pos, ray_dir);
		return true;
	}
	vec3 offset_normal;
	vec3 next_dir;
	if (!shade_and_bounce(ps, r, ray_origin, ray_dir, offset_normal, next_dir)) {
		return false;
	}
	ray_origin = offset_ray_origin(hit_pos, offset_normal);
	ray_dir = next_dir;
	return true;
}
