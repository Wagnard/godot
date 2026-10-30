// Light sampling and Next Event Estimation (NEE) for raytracing.
// Requires: raytracing_inc.glsl, brdf_inc.glsl, tlas at binding 1, payload at location 0.
// Note: ray_query_alpha_test() still requires GL_EXT_ray_query (used by DLSS-RR path or when USE_RAY_QUERY_SHADOWS is defined).

// ============================================================================
// Light Types and Constants
// ============================================================================

#define RT_LIGHT_TYPE_OMNI 0 // Point light with radius (soft shadows)
#define RT_LIGHT_TYPE_DIRECTIONAL 1 // Sun/moon with angular size
#define RT_LIGHT_TYPE_SPOT 3 // Spot light with cone falloff

// ============================================================================
// Light Data (matches C++ RT_LightData, 128 bytes, std430)
// ============================================================================

struct RTLightData {
	vec3 position; // World pos (omni/spot) or direction (directional).
	uint type; // RT_LIGHT_TYPE_*.
	vec3 emission; // HDR emission color (color * energy).
	float radius; // Light size (omni/spot) or angular radius (directional).
	float attenuation; // Attenuation exponent (2=inverse-square, 1=linear).
	float inv_max_range; // 1/range (-1 = infinite).
	float max_range_squared; // range^2 (0 = infinite).
	float specular_amount; // Godot specular multiplier [0..1].
	float indirect_energy; // Godot indirect energy multiplier.
	float inv_spot_attenuation; // Spot cone softness.
	float cos_spot_angle; // Cosine of spot cone half-angle.
	uint cull_mask; // Layers this light lights (Light3D.light_cull_mask).
	vec3 spot_direction; // Spot direction (normalized, world space).
	float shadow_opacity; // 0 when the light casts no shadow (no shadow ray), as raster.
	// Directional: world position (w = 1) to the UV of rt_directional_light_projector (raster's projector).
	vec4 projector_uv_x;
	vec4 projector_uv_y;
	uint projector_enabled;
	uint pad0;
	uint pad1;
	uint pad2;
};

// Light buffer SSBO (binding provided by the including shader via RT_LIGHT_BUFFER_BINDING).
#ifndef RT_LIGHT_BUFFER_BINDING
#define RT_LIGHT_BUFFER_BINDING 13
#endif

layout(set = 0, binding = RT_LIGHT_BUFFER_BINDING, std430) readonly buffer LightBuffer {
	RTLightData rt_lights[];
};

// RenderRaytracing::build_light_grid(): [0] 1 / cell size (float bits), [1] buckets - 1, [2] G, the lights weighed at
// every hit, [3] unused; [4, 4 + G) those lights; [4 + G, 4 + G + buckets] the first entry of each bucket in this buffer
// (the last one ends the previous), then the entries: the lights whose range reaches one of the bucket's cells.
// clang-format off
layout(set = 0, binding = 42, std430) readonly buffer RTLightGrid {
	uint rt_light_grid[];
};
// clang-format on

// Same as _rt_decal_grid_hash() in render_raytracing.cpp, which the light grid shares.
uint rt_light_grid_hash(ivec3 p_cell) {
	return (uint(p_cell.x) * 73856093u) ^ (uint(p_cell.y) * 19349663u) ^ (uint(p_cell.z) * 83492791u);
}

// The projector of the directional light whose projector_enabled is set (white when none): it multiplies the light,
// rgb * a, repeated beyond its size, as raster's directional_light_projector.
layout(set = 0, binding = 38) uniform texture2D rt_directional_light_projector;

vec3 lights_directional_projector(RTLightData light, vec3 hit_pos) {
	if (light.projector_enabled == 0u) {
		return vec3(1.0);
	}
	vec2 uv = vec2(dot(light.projector_uv_x, vec4(hit_pos, 1.0)), dot(light.projector_uv_y, vec4(hit_pos, 1.0)));
	vec4 color = textureLod(sampler2D(rt_directional_light_projector, SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, 0.0);
	return color.rgb * color.a;
}

// ============================================================================
// Unified Cone Sampling (for sphere, directional, spot lights)
// ============================================================================

struct LightSample {
	vec3 cone_axis; // Direction to sample around (normalized).
	float cos_theta_max; // Cone half-angle cosine (1.0 = point, 0.0 = hemisphere).
	vec3 emission; // Light radiance.
	float distance_sq; // Squared distance to light (0 = directional).
	float max_distance; // Max shadow ray distance.
};

// Build orthonormal basis from a single direction.
void lights_build_basis(vec3 dir, out vec3 tangent, out vec3 bitangent) {
	vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(up, dir));
	bitangent = cross(dir, tangent);
}

// Transform local direction to world space around axis.
vec3 lights_local_to_world(vec3 local_dir, vec3 axis) {
	vec3 tangent, bitangent;
	lights_build_basis(axis, tangent, bitangent);
	return local_dir.x * tangent + local_dir.y * bitangent + local_dir.z * axis;
}

// Prepare unified cone sample from any light type.
LightSample lights_prepare_sample(vec3 hit_pos, RTLightData light) {
	LightSample s;

	vec3 to_light = light.position - hit_pos;
	float dist_sq = dot(to_light, to_light);
	float inv_dist = inversesqrt(dist_sq + 1e-10);
	float dist = dist_sq * inv_dist;

	float is_directional = (light.type == RT_LIGHT_TYPE_DIRECTIONAL) ? 1.0 : 0.0;

	// Cone axis.
	vec3 sphere_axis = to_light * inv_dist;
	vec3 dir_axis = -normalize(light.position);
	s.cone_axis = mix(sphere_axis, dir_axis, is_directional);

	// Distance (0 for directional = no falloff).
	s.distance_sq = mix(dist_sq, 0.0, is_directional);

	// Cone angle from subtended solid angle.
	float sin_theta_sphere = clamp(light.radius * inv_dist, 0.0, 1.0);
	float cos_theta_sphere = sqrt(max(0.0, 1.0 - sin_theta_sphere * sin_theta_sphere));
	float cos_theta_dir = cos(light.radius);
	s.cos_theta_max = mix(cos_theta_sphere, cos_theta_dir, is_directional);
	s.cos_theta_max = min(s.cos_theta_max, 0.999999);

	s.emission = light.emission;

	// Max shadow ray distance.
	float sphere_max = dist + light.radius;
	float dir_max = 10000.0;
	s.max_distance = mix(sphere_max, dir_max, is_directional);

	return s;
}

// Sample a direction within the light's cone.
vec3 lights_sample_cone(LightSample ls, vec2 u, out float pdf) {
	float cos_theta = 1.0 - u.x * (1.0 - ls.cos_theta_max);
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = 2.0 * PI * u.y;

	vec3 local_dir = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	vec3 L = lights_local_to_world(local_dir, ls.cone_axis);

	float solid_angle = 2.0 * PI * (1.0 - ls.cos_theta_max);
	pdf = 1.0 / max(solid_angle, 1e-10);

	return L;
}

// ============================================================================
// Attenuation
// ============================================================================

// Godot-style windowed distance attenuation.
// window = (1 - (d/range)^4)^2, combined with pow(d, -decay).
float lights_get_attenuation(LightSample ls, float inv_max_range, float decay) {
	if (ls.distance_sq <= 0.0) {
		return 1.0; // Directional: no distance attenuation.
	}

	float distance = sqrt(ls.distance_sq);
	float atten = min(pow(max(distance, 0.0001), -decay), 1.0);

	// Windowed falloff if range is finite (inv_max_range >= 0).
	if (inv_max_range >= 0.0) {
		float nd = distance * inv_max_range;
		nd *= nd;
		nd *= nd; // nd^4
		nd = max(1.0 - nd, 0.0);
		nd *= nd; // nd^2 window
		return atten * nd;
	}

	return atten;
}

// Per-light specular multiplier.
float lights_get_specular_multiplier(float specular_amount, float roughness) {
	if (specular_amount >= 0.0) {
		return specular_amount;
	} else {
		float r3 = roughness * roughness * roughness;
		return mix(0.0, r3, -specular_amount);
	}
}

#include "raytracing_alpha_test_inc.glsl"

// ============================================================================
// Shadow Ray (traceRayEXT pipeline)
// ============================================================================

/// Returns true if nothing p_mask selects blocks the ray.
/// Uses SkipClosestHitShader so only intersection, any_hit (alpha test) and miss are invoked.
/// TerminateOnFirstHit causes early exit on first confirmed opaque hit.
bool lights_trace_shadow_ray_pipeline(vec3 origin, vec3 direction, float max_dist, uint p_mask) {
#if defined(USE_SER)
	hitObjectEXT hitObject;
	hitObjectTraceRayEXT(hitObject, tlas,
			RT_SHADOW_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			p_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	return !(hitObjectIsHitEXT(hitObject));
#else
	// The miss shader ends a shadow ray (visible); an opaque hit calls no miss, and the flag stays clear. Only the raygen
	// traces, and it reads the payload of a path ray before shading its hit.
	payload.packed_bounces_flags = set_shadow_ray(0u);

	traceRayEXT(tlas,
			RT_SHADOW_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			p_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	return is_path_terminated(payload.packed_bounces_flags);
#endif
}

/// Returns true if light is visible.
bool lights_trace_shadow_ray(vec3 origin, vec3 direction, float max_dist, inout uint rng_state) {
	// A light within 2 mm of the surface would give the ray an empty interval (tmax below tmin), which Vulkan leaves
	// undefined: it is visible.
	if (!(max_dist > 0.002)) {
		return true;
	}
#ifdef USE_RAY_QUERY_SHADOWS
	// Ray queries are significantly faster, but can not handle complex alpha materials
	rayQueryEXT shadow_rq;
	rayQueryInitializeEXT(shadow_rq, tlas,
			RT_SHADOW_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT,
			RT_MASK_SHADOW, origin, 0.001, direction, max_dist - 0.001);

	while (rayQueryProceedEXT(shadow_rq)) {
		if (rayQueryGetIntersectionTypeEXT(shadow_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			// quick and dirty way to check transparency by sampling the alpha texture
			// this completely ignores the actual material, so might not be accurate
			if (ray_query_alpha_test(
						rayQueryGetIntersectionInstanceCustomIndexEXT(shadow_rq, false),
						rayQueryGetIntersectionPrimitiveIndexEXT(shadow_rq, false),
						rayQueryGetIntersectionBarycentricsEXT(shadow_rq, false))) {
				rayQueryConfirmIntersectionEXT(shadow_rq);
			}
		}
	}

	if (rayQueryGetIntersectionTypeEXT(shadow_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
		return false;
	}
	// The query skipped the AABB candidates: the shader must find their hit itself (GL_EXT_ray_query
	// rayQueryGenerateIntersectionEXT, DXR spec RayQuery::CommitProceduralPrimitiveHit), and it is the material's
	// intersection code, which only its hit group has. The procedural shadow casters are traced through the pipeline, when the TLAS holds any.
	if ((uint(get_rt_param(RT_PARAM_PROCEDURAL_MASKS)) & RT_MASK_PROCEDURAL_SHADOW) != 0u) {
		return lights_trace_shadow_ray_pipeline(origin, direction, max_dist, RT_MASK_PROCEDURAL_SHADOW);
	}
	return true;
#else
	return lights_trace_shadow_ray_pipeline(origin, direction, max_dist, RT_MASK_SHADOW);
#endif
}

// ============================================================================
// Next Event Estimation (NEE) - Direct Light Sampling
// ============================================================================

// The light reaching the point: no shadow ray for a light without shadows, and its shadow opacity, as raster.
float lights_shadow_visibility(RTLightData p_light, vec3 p_origin, vec3 p_direction, float p_max_dist, inout uint rng_state) {
	if (p_light.shadow_opacity <= 0.0) {
		return 1.0;
	}
	return lights_trace_shadow_ray(p_origin, p_direction, p_max_dist, rng_state) ? 1.0 : 1.0 - p_light.shadow_opacity;
}

// BACKLIGHT as raster's light_compute() adds it: light_color * (1 / PI - diffuse_brdf_NL) * backlight, times the
// diffuse albedo raster multiplies all its diffuse light by.
vec3 lights_backlight_term(vec3 p_brdf_diffuse, MaterialProperties p_material, vec3 p_backlight) {
	vec3 diffuse_reflectance = baseColorToDiffuseReflectance(p_material.baseColor, p_material.metalness);
	return max(diffuse_reflectance * ONE_OVER_PI - p_brdf_diffuse, vec3(0.0)) * p_backlight;
}

// The weight a light is picked by (resampled importance sampling, below): its unshadowed contribution at the point,
// estimated from the light's center (luminance of the emission, distance attenuation, spot cone, cosine). It is 0 only
// where the light cannot light the point (other layers, out of range, the whole light outside the spot cone, behind a
// surface without BACKLIGHT), so the estimate stays unbiased; elsewhere a floor on the cosine keeps it above 0 where a
// light with a size can still reach the point while its center cannot.
float lights_target_weight(RTLightData p_light, vec3 p_hit_pos, vec3 p_N, bool p_has_backlight, bool p_is_indirect, uint p_layer_mask) {
	if ((p_light.cull_mask & p_layer_mask) == 0u) {
		return 0.0;
	}
	// A negative light (Light3D.light_negative) takes light away: weighted by its magnitude.
	float weight = abs(dot(p_light.emission, vec3(0.2126, 0.7152, 0.0722))) * (p_is_indirect ? p_light.indirect_energy : 1.0);
	vec3 L;
	float sin_size;
	if (p_light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
		L = -normalize(p_light.position);
		sin_size = sin(p_light.radius);
	} else {
		vec3 to_light = p_light.position - p_hit_pos;
		float dist_sq = dot(to_light, to_light);
		if (p_light.max_range_squared != 0.0 && dist_sq > p_light.max_range_squared) {
			return 0.0;
		}
		float dist = sqrt(dist_sq);
		L = to_light / max(dist, 0.0001);
		sin_size = p_light.radius > 0.01 ? clamp(p_light.radius / max(dist, 0.0001), 0.0, 1.0) : 0.0;
		LightSample ls_atten;
		ls_atten.distance_sq = dist_sq;
		weight *= lights_get_attenuation(ls_atten, p_light.inv_max_range, p_light.attenuation);
		if (p_light.type == RT_LIGHT_TYPE_SPOT) {
			float scos = dot(-L, p_light.spot_direction);
			if (sin_size == 0.0) {
				if (scos <= p_light.cos_spot_angle) {
					return 0.0;
				}
				float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - p_light.cos_spot_angle));
				weight *= 1.0 - pow(spot_rim, p_light.inv_spot_attenuation);
			} else if (acos(clamp(scos, -1.0, 1.0)) - asin(sin_size) >= acos(p_light.cos_spot_angle)) {
				return 0.0; // Every point of the sphere is outside the cone.
			}
		}
	}
	float cos_theta = dot(p_N, L);
	if (p_has_backlight) {
		cos_theta = abs(cos_theta);
	} else if (sin_size == 0.0 && cos_theta <= 0.0) {
		return 0.0;
	}
	return weight * max(cos_theta, 0.05);
}

vec3 lights_evaluate_light(RTLightData light, vec3 hit_pos, vec3 hit_pos_back, vec3 geometry_normal, vec3 N, vec3 V, MaterialProperties material, vec3 backlight, bool specular_disabled, inout uint rng_state, bool is_indirect_bounce NRD_SPECULAR_PARAM);

// Evaluate direct lighting using NEE: one light, picked among those that can light the point in proportion to
// lights_target_weight() (resampled importance sampling with each of them as a candidate: Talbot et al. 2005, "Importance
// Resampling for Global Illumination"; Bitterli et al. 2020, "Spatiotemporal reservoir resampling", section 3), and one
// shadow ray toward it. The estimate f(y) * sum(w) / w(y) is unbiased: the weight is above 0 wherever the light's
// contribution can be (measured: the mean matches every light summed, each with its shadow ray, within the noise). Without
// rendering/pathtracing/light_sampling_ris (RT_PARAM_FLAG_LIGHT_SAMPLING_RIS), the light is picked uniformly among those
// in range, as before: a strong light among many weak ones is then sampled now and then, and its light is noise (and
// that choice came out 0.6 % darker than every light summed on a sunlit wall, a cause not looked into).
// A light behind the surface reaches it only through BACKLIGHT, and its shadow ray then leaves from hit_pos_back, on
// that side of the surface. With NRD, r_specular: the part of the returned light the specular lobe reflects.
vec3 lights_evaluate_direct_lighting(
		vec3 hit_pos,
		vec3 hit_pos_back,
		vec3 geometry_normal,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		vec3 backlight,
		bool specular_disabled,
		inout uint rng_state,
		bool is_indirect_bounce,
		uint light_count,
		uint layer_mask NRD_SPECULAR_PARAM) {
#ifdef NRD_ENABLED
	r_specular = vec3(0.0);
#endif
	bool has_backlight = any(greaterThan(backlight, vec3(0.0)));
	if (light_count == 0u) {
		return vec3(0.0);
	}

	uint selected_idx = 0u;
	float light_select_pdf; // The probability the light was picked with.
	if ((uint(get_rt_param(RT_PARAM_FLAGS)) & RT_PARAM_FLAG_LIGHT_SAMPLING_RIS) != 0u) {
		// One reservoir (weighted reservoir sampling, Bitterli et al. 2020, algorithm 2; one random number per candidate)
		// over the lights that can light the point (RenderRaytracing::build_light_grid()): the global ones, then those of
		// its cell's bucket. Any other light weighs 0 here, so the choice is the one over every light, at every bounce, for
		// the cost of the few lights of a cell: every light at every bounce without the grid cost 3 ms more with 64 lights
		// at 3 bounces (godot3d's Drowned Ossuary), with it 1 ms less than the uniform choice below.
		const ivec3 cell = ivec3(floor(hit_pos * uintBitsToFloat(rt_light_grid[0])));
		const uint global_count = rt_light_grid[2];
		const uint bucket = 4u + global_count + (rt_light_grid_hash(cell) & rt_light_grid[1]);
		const uint bucket_start = rt_light_grid[bucket];
		const uint candidates = global_count + rt_light_grid[bucket + 1u] - bucket_start;
		float weight_sum = 0.0;
		float selected_weight = 0.0;
		for (uint i = 0u; i < candidates; i++) {
			uint idx = i < global_count ? rt_light_grid[4u + i] : rt_light_grid[bucket_start + i - global_count];
			float w = lights_target_weight(rt_lights[idx], hit_pos, N, has_backlight, is_indirect_bounce, layer_mask);
			if (w <= 0.0) {
				continue;
			}
			weight_sum += w;
			if (rand(rng_state) * weight_sum < w) {
				selected_idx = idx;
				selected_weight = w;
			}
		}
		if (selected_weight <= 0.0) {
			return vec3(0.0);
		}
		light_select_pdf = selected_weight / weight_sum;
	} else {
		// rendering/pathtracing/light_sampling_ris off (for comparison): k lights drawn uniformly, one of those in range
		// kept uniformly, the count of lights in range estimated from the draws.
		const uint k = min(16u, light_count);
		uint valid_found = 0u;
		for (uint i = 0u; i < k; i++) {
			uint idx = min(uint(rand(rng_state) * float(light_count)), light_count - 1u);
			RTLightData test_light = rt_lights[idx];

			// Range check for positional lights; a light lights only the layers of its cull mask.
			bool is_positional = (test_light.type == RT_LIGHT_TYPE_OMNI || test_light.type == RT_LIGHT_TYPE_SPOT);
			bool is_valid = !is_positional;
			if ((test_light.cull_mask & layer_mask) == 0u) {
				continue;
			}
			if (!is_valid) {
				vec3 to_l = test_light.position - hit_pos;
				float d2 = dot(to_l, to_l);
				is_valid = (test_light.max_range_squared == 0.0 || d2 <= test_light.max_range_squared);
			}

			if (is_valid) {
				valid_found++;
				if (rand(rng_state) < 1.0 / float(valid_found)) {
					selected_idx = idx;
				}
			}
		}
		if (valid_found == 0u) {
			return vec3(0.0);
		}
		light_select_pdf = 1.0 / max(float(light_count) * (float(valid_found) / float(k)), 1.0);
	}

#ifdef NRD_ENABLED
	const vec3 light = lights_evaluate_light(rt_lights[selected_idx], hit_pos, hit_pos_back, geometry_normal, N, V, material, backlight, specular_disabled, rng_state, is_indirect_bounce, r_specular);
	r_specular /= max(light_select_pdf, 1e-10);
	return light / max(light_select_pdf, 1e-10);
#else
	return lights_evaluate_light(rt_lights[selected_idx], hit_pos, hit_pos_back, geometry_normal, N, V, material, backlight, specular_disabled, rng_state, is_indirect_bounce) / max(light_select_pdf, 1e-10);
#endif
}

// The light one light brings to the point, before the probability of having picked it: one shadow ray toward it.
// With NRD, r_specular: the part of it the specular lobe reflects.
vec3 lights_evaluate_light(
		RTLightData light,
		vec3 hit_pos,
		vec3 hit_pos_back,
		vec3 geometry_normal,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		vec3 backlight,
		bool specular_disabled,
		inout uint rng_state,
		bool is_indirect_bounce NRD_SPECULAR_PARAM) {
#ifdef NRD_ENABLED
	r_specular = vec3(0.0);
#endif
	bool has_backlight = any(greaterThan(backlight, vec3(0.0)));
	vec2 u = rand2(rng_state);

	// === POSITIONAL LIGHT PATH (omni + spot) ===
	if (light.type == RT_LIGHT_TYPE_OMNI || light.type == RT_LIGHT_TYPE_SPOT) {
		vec3 to_light = light.position - hit_pos;
		float dist_sq = dot(to_light, to_light);

		// Early out: outside max range.
		if (light.max_range_squared != 0.0 && dist_sq > light.max_range_squared) {
			return vec3(0.0);
		}

		float dist = sqrt(dist_sq);
		vec3 L;
		float shadow_dist;

		if (light.radius <= 0.01) {
			// True point light: exact direction.
			L = to_light / max(dist, 0.0001);
			shadow_dist = dist;
		} else {
			// Sphere light: cone sampling for soft shadows.
			LightSample ls = lights_prepare_sample(hit_pos, light);
			float light_pdf;
			L = lights_sample_cone(ls, u, light_pdf);
			float t_center = dot(to_light, L);
			vec3 perp = to_light - t_center * L;
			float perp_sq = dot(perp, perp);
			float dt = sqrt(max(0.0, light.radius * light.radius - perp_sq));
			shadow_dist = max(0.0, t_center - dt);
		}

		// Spot cone early-out.
		float spot_atten = 1.0;
		if (light.type == RT_LIGHT_TYPE_SPOT) {
			float scos = dot(-L, light.spot_direction);
			if (scos <= light.cos_spot_angle) {
				return vec3(0.0);
			}
			float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - light.cos_spot_angle));
			spot_atten = 1.0 - pow(spot_rim, light.inv_spot_attenuation);
		}

		bool through = has_backlight && dot(geometry_normal, L) < 0.0;
		float visibility = lights_shadow_visibility(light, through ? hit_pos_back : hit_pos, L, shadow_dist, rng_state);
		if (visibility <= 0.0) {
			return vec3(0.0);
		}

		// Evaluate BRDF (diffuse + specular separately for specular_amount control).
		vec3 brdf_diffuse, brdf_specular;
		evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

		// Distance attenuation.
		LightSample ls_atten;
		ls_atten.distance_sq = dist_sq;
		float atten = lights_get_attenuation(ls_atten, light.inv_max_range, light.attenuation) * spot_atten;

		float spec_mul = specular_disabled ? 0.0 : lights_get_specular_multiplier(light.specular_amount, material.roughness);
		vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul;
		if (has_backlight) {
			brdf_value += lights_backlight_term(brdf_diffuse, material, backlight);
		}

		float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

		// NdotL is already included in brdf_value (evalLambertian/evalMicrofacet bake it in).
#ifdef NRD_ENABLED
		r_specular = brdf_specular * spec_mul * light.emission * atten * indirect_mul * visibility;
#endif
		return brdf_value * light.emission * atten * indirect_mul * visibility;
	}
	// === CONE LIGHT PATH (directional) ===
	else {
		LightSample ls = lights_prepare_sample(hit_pos, light);
		float light_pdf;
		vec3 L = lights_sample_cone(ls, u, light_pdf);

		if (!has_backlight && dot(N, L) <= 0.0) {
			return vec3(0.0);
		}

		// Before the shadow ray: where the projector is black (a cloud's full shadow) there is nothing to trace.
		vec3 projector = lights_directional_projector(light, hit_pos);
		if (all(lessThanEqual(projector, vec3(0.0)))) {
			return vec3(0.0);
		}

		bool through = has_backlight && dot(geometry_normal, L) < 0.0;
		float visibility = lights_shadow_visibility(light, through ? hit_pos_back : hit_pos, L, ls.max_distance, rng_state);
		if (visibility <= 0.0) {
			return vec3(0.0);
		}

		vec3 brdf_diffuse, brdf_specular;
		evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

		float spec_mul = specular_disabled ? 0.0 : lights_get_specular_multiplier(light.specular_amount, material.roughness);
		vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul;
		if (has_backlight) {
			brdf_value += lights_backlight_term(brdf_diffuse, material, backlight);
		}

		float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

#ifdef NRD_ENABLED
		r_specular = brdf_specular * spec_mul * light.emission * projector * indirect_mul * visibility;
#endif
		return brdf_value * light.emission * projector * indirect_mul * visibility;
	}
}
