// Light sampling and Next Event Estimation (NEE) for raytracing.
// Requires: raytracing_inc.glsl, brdf_inc.glsl, tlas at binding 1, payload at location 0, bindless_textures[].
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
	vec3 spot_direction; // The light's -Z (normalized, world space): a spot's cone axis, an omni light's with a projector.
	float shadow_opacity; // 0 when the light casts no shadow (no shadow ray), as raster.
	// Directional: world position (w = 1) to the UV of rt_directional_light_projector (raster's projector). Omni and spot
	// with projector_texture: xyz the light's X and Y axes, projector_uv_x.w a spot's cot(spot angle).
	vec4 projector_uv_x;
	vec4 projector_uv_y;
	uint projector_enabled;
	uint projector_texture; // Omni and spot: the projector in bindless_textures (sRGB view), 0 for none.
#ifdef RT_SHADOW_GROUPS
	uint shadow_caster_mask; // Light3D.shadow_caster_mask: the layers whose casters shadow this light.
#else
	uint pad1;
#endif
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

// Raster's light_projector_sampler (rendering/textures/light_projectors/filter, no repeat).
layout(set = 0, binding = 45) uniform sampler rt_light_projector_sampler;

// The projector of an omni or spot light (projector_texture, white when none) toward p_dir, the direction from the light's
// position to the point, as raster's light_process_omni() and light_process_spot() map it (scene_forward_lights_inc.glsl,
// through the light's shadow_matrix): rgb * a multiplies the light. Level 0: a ray has no screen derivatives.
vec3 lights_positional_projector(RTLightData p_light, vec3 p_dir) {
	if (p_light.projector_texture == 0u) {
		return vec3(1.0);
	}
	const vec3 local = vec3(dot(p_dir, p_light.projector_uv_x.xyz), dot(p_dir, p_light.projector_uv_y.xyz), -dot(p_dir, p_light.spot_direction));
	vec2 uv;
	if (p_light.type == RT_LIGHT_TYPE_SPOT) {
		// The spot's shadow camera, looking down -Z with a field of view of twice the spot angle, its NDC to [0, 1]
		// (set_light_bias()); the projector rectangle flips v (light_storage.cpp: "flip because shadow is flipped").
		const float w = -local.z >= 0.0 ? max(-local.z, 1e-6) : min(-local.z, -1e-6);
		const vec2 ndc = local.xy * (p_light.projector_uv_x.w / w);
		uv = vec2(ndc.x, -ndc.y) * 0.5 + 0.5;
	} else {
		// The decal atlas holds an omni light's projector as the dual paraboloid copy_to_fb.glsl's MODE_PANORAMA_TO_DP makes
		// of it, the texture read as an equirectangular panorama: the point of that panorama the copy put at the paraboloid
		// texel raster samples for p_dir.
		uv = vec2(fract(atan(-local.x, -local.z) * (0.5 / PI)), acos(clamp(-local.y, -1.0, 1.0)) * (1.0 / PI));
	}
	const vec4 color = textureLod(sampler2D(bindless_textures[nonuniformEXT(p_light.projector_texture)], rt_light_projector_sampler), uv, 0.0);
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
	// Not clamped to 1, as raster's get_omni_attenuation() (scene_forward_lights_inc.glsl): nearer than 1 m, a light is
	// brighter than at 1 m.
	float atten = pow(max(distance, 0.0001), -decay);

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

#ifdef RT_SHADOW_GROUPS
// rendering/pathtracing/shadow_groups: whether the receiver is SELF_SHADOWING, a parameter of the light evaluation in this
// variant only, so that the others' code is unchanged.
#define RT_SHADOW_RECEIVER_PARAM , bool p_self_shadowing_receiver
#define RT_SHADOW_RECEIVER_ARG , p_self_shadowing_receiver
#define RT_SHADOW_RECEIVER_OF(m_geometry_idx) , ((geometries[m_geometry_idx].flags & FLAG_SELF_SHADOWING) != 0u)

// The light the shadow group casters let through toward p_light (ray_query_shadow_group_transmittance()), when the TLAS
// holds some this frame.
vec3 lights_trace_shadow_group(RTLightData p_light, vec3 p_origin, vec3 p_direction, float p_max_dist, bool p_self_shadowing_receiver) {
	if (!(p_max_dist > 0.002) || (uint(get_rt_param(RT_PARAM_FLAGS)) & RT_PARAM_FLAG_SHADOW_GROUPS) == 0u) {
		return vec3(1.0);
	}
	return ray_query_shadow_group_transmittance(p_origin, p_direction, p_max_dist - 0.001, RT_SHADOW_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT, p_light.shadow_caster_mask, p_self_shadowing_receiver);
}
#else
#define RT_SHADOW_RECEIVER_PARAM
#define RT_SHADOW_RECEIVER_ARG
#define RT_SHADOW_RECEIVER_OF(m_geometry_idx)
#endif

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

#ifdef RT_TRANSPARENT_SHADOWS
// rendering/pathtracing/transparent_shadows: the light that reaches the end of the ray, 0 when an opaque surface blocks
// it, filtered by the shadow rays' copies of the transparent surfaces in between (RT_GEOM_FLAG_TRANSPARENT_SHADOW), whose
// any-hit multiplies the transmittance the payload carries (shadow_transmittance_filter()) and lets the ray go on. The
// copies are not forced opaque and their BLAS has NO_DUPLICATE_ANY_HIT_INVOCATION: each triangle filters it once.
vec3 lights_trace_shadow_transmittance_pipeline(vec3 origin, vec3 direction, float max_dist, uint p_mask) {
	payload.packed_bounces_flags = set_shadow_ray(0u);
	shadow_transmittance_store(payload, vec3(1.0));
#if defined(USE_SER)
	hitObjectEXT hitObject;
	hitObjectTraceRayEXT(hitObject, tlas,
			RT_SHADOW_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			p_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	return hitObjectIsHitEXT(hitObject) ? vec3(0.0) : shadow_transmittance_load(payload);
#else
	traceRayEXT(tlas,
			RT_SHADOW_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			p_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	return is_path_terminated(payload.packed_bounces_flags) ? shadow_transmittance_load(payload) : vec3(0.0);
#endif
}

vec3 lights_trace_shadow_transmittance(vec3 origin, vec3 direction, float max_dist, inout uint rng_state) {
	if (!(max_dist > 0.002)) {
		return vec3(1.0);
	}
#ifdef USE_RAY_QUERY_SHADOWS
	// As lights_trace_shadow_ray(), hit group 0's albedo for any material.
	vec3 transmittance = vec3(1.0);
	rayQueryEXT shadow_rq;
	rayQueryInitializeEXT(shadow_rq, tlas,
			RT_SHADOW_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT,
			RT_MASK_SHADOW, origin, 0.001, direction, max_dist - 0.001);

	while (rayQueryProceedEXT(shadow_rq)) {
		if (rayQueryGetIntersectionTypeEXT(shadow_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			const uint geometry_idx = rayQueryGetIntersectionInstanceCustomIndexEXT(shadow_rq, false);
			const uint primitive_id = rayQueryGetIntersectionPrimitiveIndexEXT(shadow_rq, false);
			const vec2 bary = rayQueryGetIntersectionBarycentricsEXT(shadow_rq, false);
			if ((geometries[geometry_idx].flags & FLAG_TRANSPARENT_SHADOW) != 0u) {
				const vec4 color = ray_query_shadow_color(geometry_idx, primitive_id, bary);
				if (!shadow_transmittance_filter(transmittance, color.rgb, color.a)) {
					rayQueryConfirmIntersectionEXT(shadow_rq);
				}
			} else if (ray_query_alpha_test(geometry_idx, primitive_id, bary)) {
				rayQueryConfirmIntersectionEXT(shadow_rq);
			}
		}
	}

	if (rayQueryGetIntersectionTypeEXT(shadow_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
		return vec3(0.0);
	}
	if ((uint(get_rt_param(RT_PARAM_PROCEDURAL_MASKS)) & RT_MASK_PROCEDURAL_SHADOW) != 0u) {
		return lights_trace_shadow_ray_pipeline(origin, direction, max_dist, RT_MASK_PROCEDURAL_SHADOW) ? transmittance : vec3(0.0);
	}
	return transmittance;
#else
	return lights_trace_shadow_transmittance_pipeline(origin, direction, max_dist, RT_MASK_SHADOW);
#endif
}
#endif

// ============================================================================
// Next Event Estimation (NEE) - Direct Light Sampling
// ============================================================================

#ifdef RT_TRANSPARENT_SHADOWS
#define RT_SHADOW_VISIBILITY vec3
#define RT_SHADOW_BLOCKED(m_visibility) all(lessThanEqual(m_visibility, vec3(0.0)))

// The light reaching the point, through the transparent surfaces in between (rendering/pathtracing/transparent_shadows):
// no shadow ray for a light without shadows, and its shadow opacity, as raster.
vec3 lights_shadow_visibility(RTLightData p_light, vec3 p_origin, vec3 p_direction, float p_max_dist, inout uint rng_state RT_SHADOW_RECEIVER_PARAM) {
	if (p_light.shadow_opacity <= 0.0) {
		return vec3(1.0);
	}
#ifdef RT_SHADOW_GROUPS
	vec3 transmittance = lights_trace_shadow_transmittance(p_origin, p_direction, p_max_dist, rng_state);
	if (any(greaterThan(transmittance, vec3(0.0)))) {
		transmittance *= lights_trace_shadow_group(p_light, p_origin, p_direction, p_max_dist, p_self_shadowing_receiver);
	}
	return mix(vec3(1.0), transmittance, p_light.shadow_opacity);
#else
	return mix(vec3(1.0), lights_trace_shadow_transmittance(p_origin, p_direction, p_max_dist, rng_state), p_light.shadow_opacity);
#endif
}
#else
#define RT_SHADOW_VISIBILITY float
#define RT_SHADOW_BLOCKED(m_visibility) (m_visibility <= 0.0)

// The light reaching the point: no shadow ray for a light without shadows, and its shadow opacity, as raster.
float lights_shadow_visibility(RTLightData p_light, vec3 p_origin, vec3 p_direction, float p_max_dist, inout uint rng_state RT_SHADOW_RECEIVER_PARAM) {
	if (p_light.shadow_opacity <= 0.0) {
		return 1.0;
	}
#ifdef RT_SHADOW_GROUPS
	const bool visible = lights_trace_shadow_ray(p_origin, p_direction, p_max_dist, rng_state) &&
			lights_trace_shadow_group(p_light, p_origin, p_direction, p_max_dist, p_self_shadowing_receiver).x > 0.0;
	return visible ? 1.0 : 1.0 - p_light.shadow_opacity;
#else
	return lights_trace_shadow_ray(p_origin, p_direction, p_max_dist, rng_state) ? 1.0 : 1.0 - p_light.shadow_opacity;
#endif
}
#endif

// BACKLIGHT as raster's light_compute() adds it: light_color * (1 / PI - diffuse_brdf_NL) * backlight, times the
// diffuse albedo raster multiplies all its diffuse light by.
vec3 lights_backlight_term(vec3 p_brdf_diffuse, MaterialProperties p_material, vec3 p_backlight) {
	vec3 diffuse_reflectance = baseColorToDiffuseReflectance(p_material.baseColor, p_material.metalness);
	return max(diffuse_reflectance * ONE_OVER_PI - p_brdf_diffuse, vec3(0.0)) * p_backlight;
}

// Whether a light behind the shading normal can light the surface: through BACKLIGHT (m_backlight), and in the
// rendering/pathtracing/raster_brdf variant through the wrap and toon modes too (godotShadesBackfacingLight()). Their shadow
// ray still leaves from the front: the surface shadows such a light as raster's shadow map does.
#ifdef RT_RASTER_BRDF
#define RT_LIT_FROM_BEHIND(m_backlight, m_material) ((m_backlight) || godotShadesBackfacingLight(m_material))
#else
#define RT_LIT_FROM_BEHIND(m_backlight, m_material) m_backlight
#endif

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

vec3 lights_evaluate_light(RTLightData light, vec3 hit_pos, vec3 hit_pos_back, vec3 geometry_normal, vec3 N, vec3 V, MaterialProperties material, vec3 backlight, bool specular_disabled, inout uint rng_state, bool is_indirect_bounce RT_SHADOW_RECEIVER_PARAM NRD_SPECULAR_PARAM NRD_SHADOW_RAY_PARAM);

// Evaluate direct lighting using NEE: one light, picked among those that can light the point in proportion to
// lights_target_weight() (resampled importance sampling with each of them as a candidate: Talbot et al. 2005, "Importance
// Resampling for Global Illumination"; Bitterli et al. 2020, "Spatiotemporal reservoir resampling", section 3), and one
// shadow ray toward it. The estimate f(y) * sum(w) / w(y) is unbiased: the weight is above 0 wherever the light's
// contribution can be (measured: the mean matches every light summed, each with its shadow ray, within the noise). Without
// rendering/pathtracing/light_sampling_ris (RT_PARAM_FLAG_LIGHT_SAMPLING_RIS), the light is picked uniformly among those
// in range, as before: a strong light among many weak ones is then sampled now and then, and its light is noise (and
// that choice came out 0.6 % darker than every light summed on a sunlit wall, a cause not looked into).
// A light behind the surface reaches it only through BACKLIGHT, and its shadow ray then leaves from hit_pos_back, on
// that side of the surface. With NRD, r_specular: the part of the returned light the specular lobe reflects; p_skip_light
// is left out of the choice (SIGMA's sun, lit apart at a primary hit: the choice among the others estimates their sum,
// unbiased as before); with NRD_DIRECT_LIGHT, r_shadow_ray: the sampled light's shadow ray (lights_evaluate_light()), 0
// when none was picked.
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
		uint layer_mask RT_SHADOW_RECEIVER_PARAM NRD_DIRECT_LIGHTING_PARAMS NRD_SHADOW_RAY_PARAM) {
#ifdef NRD_ENABLED
	r_specular = vec3(0.0);
#endif
#ifdef NRD_DIRECT_LIGHT
	r_shadow_ray = vec4(0.0);
#endif
	// Wrap and toon materials take lights behind the shading normal too: weighed as for BACKLIGHT, so that the light choice
	// stays unbiased.
	bool has_backlight = RT_LIT_FROM_BEHIND(any(greaterThan(backlight, vec3(0.0))), material);
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
#ifdef NRD_ENABLED
			if (idx == p_skip_light) {
				continue;
			}
#endif
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
#ifdef NRD_ENABLED
			// Drawn but left out: the count of the others in range is still estimated over all the draws.
			if (idx == p_skip_light) {
				continue;
			}
#endif
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
	const vec3 light = lights_evaluate_light(rt_lights[selected_idx], hit_pos, hit_pos_back, geometry_normal, N, V, material, backlight, specular_disabled, rng_state, is_indirect_bounce RT_SHADOW_RECEIVER_ARG, r_specular NRD_SHADOW_RAY_ARG);
	r_specular /= max(light_select_pdf, 1e-10);
	return light / max(light_select_pdf, 1e-10);
#else
	return lights_evaluate_light(rt_lights[selected_idx], hit_pos, hit_pos_back, geometry_normal, N, V, material, backlight, specular_disabled, rng_state, is_indirect_bounce RT_SHADOW_RECEIVER_ARG) / max(light_select_pdf, 1e-10);
#endif
}

// The light one light brings to the point, before the probability of having picked it: one shadow ray toward it.
// With NRD, r_specular: the part of it the specular lobe reflects; with NRD_DIRECT_LIGHT, r_shadow_ray: the shadow ray's
// direction and length
// (NRD_MISS_HIT_T for a directional light that reaches the point), the length negative when the ray is blocked, 0 when no
// ray was traced. NRD's direct light signal takes its hit distance from it: the distance to the light, or to the nearest
// occluder (shade_and_bounce(): NRD's README, "Other": "For direct lighting a distance to an occluder or a light source is
// needed"). The occluder search is not here: this function is inlined at every bounce.
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
		bool is_indirect_bounce RT_SHADOW_RECEIVER_PARAM NRD_SPECULAR_PARAM NRD_SHADOW_RAY_PARAM) {
#ifdef NRD_ENABLED
	r_specular = vec3(0.0);
#endif
#ifdef NRD_DIRECT_LIGHT
	r_shadow_ray = vec4(0.0);
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

		// From the light's center, as raster (a light with a size keeps a sharp projection); before the shadow ray, which
		// has nothing to find where the projector is black.
		vec3 projector = lights_positional_projector(light, -to_light / max(dist, 0.0001));
		if (all(lessThanEqual(projector, vec3(0.0)))) {
			return vec3(0.0);
		}

		bool through = has_backlight && dot(geometry_normal, L) < 0.0;
		RT_SHADOW_VISIBILITY visibility = lights_shadow_visibility(light, through ? hit_pos_back : hit_pos, L, shadow_dist, rng_state RT_SHADOW_RECEIVER_ARG);
#ifdef NRD_DIRECT_LIGHT
		r_shadow_ray = vec4(L, RT_SHADOW_BLOCKED(visibility) ? -shadow_dist : shadow_dist);
#endif
		if (RT_SHADOW_BLOCKED(visibility)) {
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
		r_specular = brdf_specular * spec_mul * light.emission * projector * atten * indirect_mul * visibility;
#endif
		return brdf_value * light.emission * projector * atten * indirect_mul * visibility;
	}
	// === CONE LIGHT PATH (directional) ===
	else {
		LightSample ls = lights_prepare_sample(hit_pos, light);
		float light_pdf;
		vec3 L = lights_sample_cone(ls, u, light_pdf);

		if (!RT_LIT_FROM_BEHIND(has_backlight, material) && dot(N, L) <= 0.0) {
			return vec3(0.0);
		}

		// Before the shadow ray: where the projector is black (a cloud's full shadow) there is nothing to trace.
		vec3 projector = lights_directional_projector(light, hit_pos);
		if (all(lessThanEqual(projector, vec3(0.0)))) {
			return vec3(0.0);
		}

		bool through = has_backlight && dot(geometry_normal, L) < 0.0;
		RT_SHADOW_VISIBILITY visibility = lights_shadow_visibility(light, through ? hit_pos_back : hit_pos, L, ls.max_distance, rng_state RT_SHADOW_RECEIVER_ARG);
#ifdef NRD_DIRECT_LIGHT
		r_shadow_ray = vec4(L, RT_SHADOW_BLOCKED(visibility) ? -ls.max_distance : NRD_MISS_HIT_T);
#endif
		if (RT_SHADOW_BLOCKED(visibility)) {
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

#if defined(NRD_ENABLED) && defined(RT_STAGE_RAYGEN)
// SIGMA's sun (RenderRaytracing::update_uniform_set()): its index in rt_lights, NRD_NO_LIGHT when none.
uint nrd_sigma_sun_index() {
	return ((uint(get_rt_param(RT_PARAM_FLAGS)) >> RT_PARAM_FLAG_NRD_SUN_SHIFT) & RT_PARAM_FLAG_NRD_SUN_MASK) - 1u;
}

// The distance to the nearest opaque occluder along a ray toward a light, as NRD's README asks of SIGMA's input ("Distance
// to occluder (SIGMA)": cast from the point toward the light, and "ACCEPT_FIRST_HIT_AND_END_SEARCH ray flag can't be used",
// so the search runs to the nearest hit) and of the direct light signal's hit distance. Cut-outs through the alpha test;
// procedural geometry is not found (its intersection code runs in its hit group only): 0, a hard edge.
float nrd_sigma_occluder_distance(vec3 p_origin, vec3 p_direction, float p_max_dist) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, RT_SHADOW_RAY_FLAGS, RT_MASK_SHADOW, p_origin, 0.001, p_direction, p_max_dist - 0.001);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
				ray_query_alpha_test(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), rayQueryGetIntersectionPrimitiveIndexEXT(rq, false), rayQueryGetIntersectionBarycentricsEXT(rq, false))) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT ? rayQueryGetIntersectionTEXT(rq, true) : 0.0;
}

// SIGMA's sun at a camera path's primary hit, lit apart from the other lights (NRD's README: "SIGMA - per-light shadow-only
// denoiser"): one direction in its cone, as lights_evaluate_light() takes it, and one shadow ray along it, as
// lights_shadow_visibility() traces it, its opacity apart. r_shadowable: the light its shadow can take, without that
// shadow (raytracing_nrd_composite.glsl multiplies it by SIGMA's denoised shadow). r_occluder_t: SIGMA's distance to the
// occluder ("NoL <= 0 - 0", "hit - hit distance", "miss - >= NRD_FP16_MAX"); where the light has no size the penumbra is 0
// whatever the distance (SIGMA_FrontEnd_PackPenumbra()), so no search for it. Returned, r_specular its specular part: what
// the shadow cannot take (shadow_opacity below 1), into the lobes as the other lights' light.
// Through transparent surfaces (rendering/pathtracing/transparent_shadows) the sun counts as reached, their transmittance
// stays in r_shadowable: SIGMA denoises the opaque occluders' shadow only.
vec3 lights_evaluate_sigma_sun(
		RTLightData light,
		vec3 hit_pos,
		vec3 hit_pos_back,
		vec3 geometry_normal,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		vec3 backlight,
		bool specular_disabled,
		inout uint rng_state RT_SHADOW_RECEIVER_PARAM,
		out vec3 r_specular,
		out vec3 r_shadowable,
		out float r_occluder_t) {
	r_specular = vec3(0.0);
	r_shadowable = vec3(0.0);
	r_occluder_t = 0.0;
	bool has_backlight = any(greaterThan(backlight, vec3(0.0)));
	vec2 u = rand2(rng_state);

	LightSample ls = lights_prepare_sample(hit_pos, light);
	float light_pdf;
	vec3 L = lights_sample_cone(ls, u, light_pdf);
	if (!RT_LIT_FROM_BEHIND(has_backlight, material) && dot(N, L) <= 0.0) {
		return vec3(0.0);
	}
	vec3 projector = lights_directional_projector(light, hit_pos);
	if (all(lessThanEqual(projector, vec3(0.0)))) {
		r_occluder_t = NRD_MISS_HIT_T; // No light to shadow.
		return vec3(0.0);
	}

	vec3 brdf_diffuse, brdf_specular;
	evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);
	float spec_mul = specular_disabled ? 0.0 : lights_get_specular_multiplier(light.specular_amount, material.roughness);
	vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul;
	if (has_backlight) {
		brdf_value += lights_backlight_term(brdf_diffuse, material, backlight);
	}
	const vec3 light_value = brdf_value * light.emission * projector;
	const vec3 specular_value = brdf_specular * spec_mul * light.emission * projector;

	bool through = has_backlight && dot(geometry_normal, L) < 0.0;
	const vec3 origin = through ? hit_pos_back : hit_pos;
#ifdef RT_TRANSPARENT_SHADOWS
	vec3 transmittance = lights_trace_shadow_transmittance(origin, L, ls.max_distance, rng_state);
#ifdef RT_SHADOW_GROUPS
	if (any(greaterThan(transmittance, vec3(0.0)))) {
		transmittance *= lights_trace_shadow_group(light, origin, L, ls.max_distance, p_self_shadowing_receiver);
	}
#endif
#else
	bool visible = lights_trace_shadow_ray(origin, L, ls.max_distance, rng_state);
#ifdef RT_SHADOW_GROUPS
	visible = visible && lights_trace_shadow_group(light, origin, L, ls.max_distance, p_self_shadowing_receiver).x > 0.0;
#endif
	const vec3 transmittance = vec3(visible ? 1.0 : 0.0);
#endif

	const float opacity = light.shadow_opacity;
	if (all(lessThanEqual(transmittance, vec3(0.0)))) {
		r_shadowable = light_value * opacity;
		r_occluder_t = light.radius > 0.0 ? nrd_sigma_occluder_distance(origin, L, ls.max_distance) : 1.0;
	} else {
		r_shadowable = light_value * opacity * transmittance;
		r_occluder_t = NRD_MISS_HIT_T;
	}
	r_specular = specular_value * (1.0 - opacity);
	return light_value * (1.0 - opacity);
}
#endif
