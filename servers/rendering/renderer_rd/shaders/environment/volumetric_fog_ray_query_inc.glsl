// rendering/pathtracing/volumetric_fog_shadows: the density pass of a path traced view shadows the fog's lights with ray
// queries through the path tracer's TLAS instead of reading shadow maps (USE_RAY_QUERY_SHADOWS, RTFogShadows). NVIDIA's
// reservoir (49a825dc52): the shadowed directional lights share one slot, the omni, spot and area lights
// FOG_RQ_POSITIONAL_SHADOW_RAYS slots, each slot keeps one light by its weight and traces one ray, scaled by
// weight_sum / weight. The rays are the path tracer's shadow rays: its shadow mask, face culling and alpha test, and the
// colored copies of rendering/pathtracing/transparent_shadows. Procedural geometry casts no shadow here: its intersection
// code is only in its hit group. The extensions are enabled at the top of volumetric_fog_process.glsl.

#define RT_NO_HIT_BUILTINS
// The transparent shadow copies' flag and filter (only set on geometry when the project setting is on).
#define RT_TRANSPARENT_SHADOWS

// clang-format off
#include "../raytracing/raytracing_inc.glsl"
#include "../raytracing/raytracing_hit_inc.glsl"

layout(set = 1, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 1, binding = 1, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 1, binding = 2, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

layout(set = 1, binding = 3) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;

layout(set = 1, binding = 4, std140) uniform FogRayQueryParams {
	mat4 view_to_world;
	uint frame_index;
	uint pad0;
	uint pad1;
	uint pad2;
}
fog_ray_query;

layout(set = 2, binding = 0) uniform texture2D bindless_textures[];

#include "../raytracing/raytracing_alpha_test_inc.glsl"

layout(constant_id = 0) const uint FOG_RQ_POSITIONAL_SHADOW_RAYS = 2u;

// Must match RTFogShadows::MAX_POSITIONAL_SHADOW_RAYS.
#define FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS 4
// As the path tracer's directional shadow rays.
#define FOG_RQ_DIRECTIONAL_DISTANCE 10000.0
// The path tracer's shadow rays (RT_MASK_SHADOW, RT_SHADOW_RAY_FLAGS): the faces turned away from the light do not shadow.
#define FOG_RQ_SHADOW_MASK 0x02
#define FOG_RQ_SHADOW_RAY_FLAGS (gl_RayFlagsCullFrontFacingTrianglesEXT | gl_RayFlagsTerminateOnFirstHitEXT)

struct FogShadowSlot {
	vec3 light;
	float weight;
	vec3 to_light;
	float weight_sum;
	float distance;
};

struct FogShadowReservoir {
	vec3 view_pos;
	uint rng;
	uint positional_count;
	FogShadowSlot directional;
	FogShadowSlot positional[FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS];
};

FogShadowReservoir fog_shadows_begin(vec3 p_view_pos, ivec3 p_froxel) {
	const FogShadowSlot empty_slot = FogShadowSlot(vec3(0.0), 0.0, vec3(0.0), 0.0, 0.0);

	FogShadowReservoir reservoir;
	reservoir.view_pos = p_view_pos;
	reservoir.rng = init_rng(uvec2(p_froxel.xy), fog_ray_query.frame_index, uint(p_froxel.z));
	reservoir.positional_count = 0u;
	reservoir.directional = empty_slot;
	[[unroll]] for (uint i = 0u; i < FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS; i++) {
		reservoir.positional[i] = empty_slot;
	}
	return reservoir;
}

// Weighted reservoir sampling: each slot keeps one light with probability proportional to its weight.
void fog_shadows_slot_add(inout FogShadowSlot r_slot, vec3 p_light, vec3 p_to_light, float p_distance, float p_weight, inout uint r_rng) {
	r_slot.weight_sum += p_weight;
	if (rand(r_rng) * r_slot.weight_sum < p_weight) {
		r_slot.light = p_light;
		r_slot.weight = p_weight;
		r_slot.to_light = p_to_light;
		r_slot.distance = p_distance;
	}
}

vec3 fog_shadows_scatter(vec3 p_view_pos, vec3 p_light, vec3 p_to_light) {
	return p_light * henyey_greenstein(dot(safe_normalize(p_to_light), safe_normalize(p_view_pos)), params.phase_g);
}

float fog_shadows_weight(vec3 p_light) {
	return abs(dot(p_light, vec3(0.2126, 0.7152, 0.0722)));
}

// p_light: the light's part its shadow attenuates (color, energy, fog energy, shadow opacity); p_direction towards it.
void fog_shadows_add_directional(inout FogShadowReservoir r_reservoir, vec3 p_light, vec3 p_direction) {
	vec3 light = fog_shadows_scatter(r_reservoir.view_pos, p_light, p_direction);
	float weight = fog_shadows_weight(light);
	if (weight > 0.0) {
		fog_shadows_slot_add(r_reservoir.directional, light, p_direction, FOG_RQ_DIRECTIONAL_DISTANCE, weight, r_reservoir.rng);
	}
}

// p_to_light: from the froxel to the light's point, p_distance long.
void fog_shadows_add_positional(inout FogShadowReservoir r_reservoir, vec3 p_light, vec3 p_to_light, float p_distance) {
	vec3 light = fog_shadows_scatter(r_reservoir.view_pos, p_light, p_to_light);
	float weight = fog_shadows_weight(light);
	if (weight <= 0.0) {
		return;
	}

	uint slot = r_reservoir.positional_count % FOG_RQ_POSITIONAL_SHADOW_RAYS;
	r_reservoir.positional_count++;
	[[unroll]] for (uint i = 0u; i < FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS; i++) {
		if (i == slot) {
			fog_shadows_slot_add(r_reservoir.positional[i], light, p_to_light, p_distance, weight, r_reservoir.rng);
		}
	}
}

// The light that reaches the end of the ray (world space), as the path tracer's ray query shadows let it through
// (lights_trace_shadow_transmittance()): 0 when an opaque or alpha tested surface blocks it, filtered by the transparent
// shadow copies in between.
vec3 fog_shadows_transmittance(vec3 p_origin, vec3 p_direction, float p_distance) {
	vec3 transmittance = vec3(1.0);
	rayQueryEXT shadow_rq;
	rayQueryInitializeEXT(shadow_rq, tlas, FOG_RQ_SHADOW_RAY_FLAGS, FOG_RQ_SHADOW_MASK, p_origin, 0.001, p_direction, p_distance - 0.001);

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

	return rayQueryGetIntersectionTypeEXT(shadow_rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? transmittance : vec3(0.0);
}

vec3 fog_shadows_trace(vec3 p_view_pos, FogShadowSlot p_slot) {
	vec3 visibility = vec3(1.0);
	// A light within 2 mm of the froxel would give the ray an empty interval (tmax below tmin): visible.
	if (p_slot.distance > 0.002) {
		vec3 origin = (fog_ray_query.view_to_world * vec4(p_view_pos, 1.0)).xyz;
		vec3 direction = normalize(mat3(fog_ray_query.view_to_world) * p_slot.to_light);
		visibility = fog_shadows_transmittance(origin, direction, p_slot.distance);
	}
	return p_slot.light * visibility * (p_slot.weight_sum / p_slot.weight);
}

vec3 fog_shadows_resolve(inout FogShadowReservoir r_reservoir) {
	vec3 light = vec3(0.0);
	if (r_reservoir.directional.weight_sum > 0.0) {
		light += fog_shadows_trace(r_reservoir.view_pos, r_reservoir.directional);
	}
	[[unroll]] for (uint i = 0u; i < FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS; i++) {
		if (i < FOG_RQ_POSITIONAL_SHADOW_RAYS && r_reservoir.positional[i].weight_sum > 0.0) {
			light += fog_shadows_trace(r_reservoir.view_pos, r_reservoir.positional[i]);
		}
	}
	return light;
}
