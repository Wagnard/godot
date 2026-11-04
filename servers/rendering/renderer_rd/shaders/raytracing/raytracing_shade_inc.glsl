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

#ifdef DLSS_RR_SPECULAR_MOTION_VECTORS
// ============================================================================
// SPECULAR MOTION VECTORS (DLSS Ray Reconstruction)
// ============================================================================
// NVIDIA, Streamline's ProgrammingGuideDLSS_RR.md, 4.1.8: "DLSS-RR uses Specular Motion Vectors to improve Image Quality of
// Reflections during motion", a "Dense motion vector field for Reflections (Virtually Reflected Geometries)"; the specular
// hit distance (4.1.9) is "Only needed if Specular Motion Vectors are not provided". In rt_velocity_image's convention.
// Required bindings: motion_indices[], motion_transforms[] (the raygen declares them with this define).

/// The screen motion of a point from the previous frame, as the closest hit writes the velocity (write_primary_hit_velocity()).
vec2 rt_motion_vector(vec3 p_pos, vec3 p_prev_pos) {
	return project_uv(p_prev_pos, prev_vp_unjittered) - project_uv(p_pos, curr_vp_unjittered);
}

/// Where p_pos, on triangle p_primitive of geometry p_geometry_idx that a ray query met, was in the previous frame: as
/// write_primary_hit_velocity() finds it in the closest hit (the instance's previous transform, a deformed mesh's previous
/// vertices, a merged MultiMesh's previous instance transforms).
vec3 rt_previous_position(uint p_geometry_idx, uint p_primitive, vec2 p_barycentrics, mat4 p_object_to_world, mat4 p_world_to_object, vec3 p_pos) {
	const GeometryData geom = geometries[p_geometry_idx];
	const int mi = motion_indices[p_geometry_idx];
	mat4 prev_model = p_object_to_world;
	if (mi >= 0) {
		const InstanceMotionData m = motion_transforms[mi];
		prev_model = transpose(mat4(
				vec4(m.prev_xform[0], m.prev_xform[1], m.prev_xform[2], m.prev_xform[3]),
				vec4(m.prev_xform[4], m.prev_xform[5], m.prev_xform[6], m.prev_xform[7]),
				vec4(m.prev_xform[8], m.prev_xform[9], m.prev_xform[10], m.prev_xform[11]),
				vec4(0.0, 0.0, 0.0, 1.0)));
	}
	const vec3 obj_pos = (p_world_to_object * vec4(p_pos, 1.0)).xyz;
	vec3 prev_obj_pos = obj_pos;
	if ((geom.flags & FLAG_DEFORMED) != 0u) {
		const uint64_t prev_addr = packUint2x32(uvec2(geom.prev_vertex_address_lo, geom.prev_vertex_address_hi));
		if (prev_addr != 0ul) {
			uint i0, i1, i2;
			get_triangle_indices_ex(geom, p_primitive, i0, i1, i2);
			const vec3 bary = vec3(1.0 - p_barycentrics.x - p_barycentrics.y, p_barycentrics.x, p_barycentrics.y);
			FloatBuffer prev_vb = FloatBuffer(prev_addr);
			const uint stride_floats = geom.position_stride >> 2;
			const vec3 p0 = vec3(prev_vb.v[i0 * stride_floats + 0u], prev_vb.v[i0 * stride_floats + 1u], prev_vb.v[i0 * stride_floats + 2u]);
			const vec3 p1 = vec3(prev_vb.v[i1 * stride_floats + 0u], prev_vb.v[i1 * stride_floats + 1u], prev_vb.v[i1 * stride_floats + 2u]);
			const vec3 p2 = vec3(prev_vb.v[i2 * stride_floats + 0u], prev_vb.v[i2 * stride_floats + 1u], prev_vb.v[i2 * stride_floats + 2u]);
			prev_obj_pos = bary.x * p0 + bary.y * p1 + bary.z * p2;
		}
	} else if (geom.multimesh_previous_offset != geom.multimesh_offset) {
		uint i0, i1, i2;
		get_triangle_indices_ex(geom, p_primitive, i0, i1, i2);
		prev_obj_pos = multimesh_previous_position(geom, i0, obj_pos);
	}
	return (prev_model * vec4(prev_obj_pos, 1.0)).xyz;
}

/// Where the hit r of the ray p_ray_origin + t * p_ray_dir, at p_hit_pos, was in the previous frame. A geometry that did
/// not move is found from its data alone; one that moved is met again by a ray query along the same ray, kept to that
/// geometry and to a short interval around the hit, which gives what the closest hit had: its triangle and transforms.
/// Procedural geometry, which a ray query cannot intersect, is taken as still. Not through the payload: two more fields
/// written by the closest hit broke the path tracer on NVIDIA's Vulkan driver with SER (no light but the albedo, black
/// areas), the driver defect of PT-VK-SER-PAYLOAD-WA.md.
vec3 rt_primary_previous_position(HitRecord r, vec3 p_ray_origin, vec3 p_ray_dir, vec3 p_hit_pos) {
	const GeometryData geom = geometries[r.geometry_idx];
	const bool deformed = (geom.flags & FLAG_DEFORMED) != 0u && (geom.prev_vertex_address_lo | geom.prev_vertex_address_hi) != 0u;
	const bool moved = motion_indices[r.geometry_idx] >= 0 || deformed || geom.multimesh_previous_offset != geom.multimesh_offset;
	if (!moved || (geom.flags & FLAG_PROCEDURAL) != 0u) {
		return p_hit_pos;
	}
	const float margin = r.hit_t * 1e-3 + 1e-3;
	rayQueryEXT rq;
	// Every candidate of the interval comes to the loop (no opaque shortcut), so the hit's own triangle is the one kept.
	rayQueryInitializeEXT(rq, tlas, RT_RAY_FLAGS | gl_RayFlagsNoOpaqueEXT, RT_MASK_BOUNCE, p_ray_origin, max(r.hit_t - margin, 0.0), p_ray_dir, r.hit_t + margin);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT && uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) == r.geometry_idx) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		return p_hit_pos;
	}
	return rt_previous_position(r.geometry_idx, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)), rayQueryGetIntersectionBarycentricsEXT(rq, true),
			mat4(rayQueryGetIntersectionObjectToWorldEXT(rq, true)), mat4(rayQueryGetIntersectionWorldToObjectEXT(rq, true)), p_hit_pos);
}

/// The specular motion vector of the primary hit r of the ray p_ray_origin + t * p_ray_dir, at p_hit_pos, whose own motion
/// vector is p_surface_motion_vector.
/// NVIDIA's design (dlss_rr_specular_motion_vector(), raytracing_closest_hit_common_inc.glsl:703-743 at 49a825dc52): the
/// surface is a mirror plane, whose virtual image of the reflected point lies on the view ray at the reflection's distance
/// and moves with the mirror, plus the reflected point's own motion mirrored through the plane. Past the roughness of
/// MAX_DENOISER_SPECULAR_HIT_THRESHOLD the reflection is too blurred to follow: the surface's own motion. With the
/// geometry normal: the waves of a normal map would scatter the virtual image per pixel. The reflected point is the
/// closest hit of the reflection ray (NVIDIA's query stopped at the first hit found, a farther one at times). r_hit_dist:
/// the reflection's distance, -1 where it reaches the sky, as dlss_rr_specular_hit_dist holds it.
vec2 rt_specular_motion_vector(HitRecord r, float p_roughness, vec3 p_ray_origin, vec3 p_ray_dir, vec3 p_hit_pos, vec2 p_surface_motion_vector, out float r_hit_dist) {
	r_hit_dist = -1.0;
	if (p_roughness >= MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
		return p_surface_motion_vector;
	}
	const vec3 V = -p_ray_dir;
	const vec3 prev_hit_pos = rt_primary_previous_position(r, p_ray_origin, p_ray_dir, p_hit_pos);
	const vec3 N = r.geometry_normal;
	const vec3 spec_dir = reflect(-V, N);
	const vec3 spec_origin = offset_ray_origin(p_hit_pos, N);

	rayQueryEXT spec_rq;
	rayQueryInitializeEXT(spec_rq, tlas, RT_RAY_FLAGS, RT_MASK_BOUNCE, spec_origin, 0.001, spec_dir, 10000.0);
	while (rayQueryProceedEXT(spec_rq)) {
		if (rayQueryGetIntersectionTypeEXT(spec_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
				ray_query_alpha_test(
						rayQueryGetIntersectionInstanceCustomIndexEXT(spec_rq, false),
						rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, false),
						rayQueryGetIntersectionBarycentricsEXT(spec_rq, false))) {
			rayQueryConfirmIntersectionEXT(spec_rq);
		}
	}
	bool hit = rayQueryGetIntersectionTypeEXT(spec_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
	float spec_t = hit ? rayQueryGetIntersectionTEXT(spec_rq, true) : 10000.0;
	vec3 reflected_motion = vec3(0.0);
	if (hit) {
		const vec3 reflected_pos = spec_origin + spec_dir * spec_t;
		const vec3 reflected_prev_pos = rt_previous_position(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(spec_rq, true)),
				uint(rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, true)), rayQueryGetIntersectionBarycentricsEXT(spec_rq, true),
				mat4(rayQueryGetIntersectionObjectToWorldEXT(spec_rq, true)), mat4(rayQueryGetIntersectionWorldToObjectEXT(spec_rq, true)), reflected_pos);
		reflected_motion = reflected_prev_pos - reflected_pos;
	}
#ifdef USE_SER
	// Procedural geometry, found by its hit group's intersection shader only (see the specular hit distance below): nearer
	// than the triangles, it is what the reflection shows, taken as still.
	if ((uint(get_rt_param(RT_PARAM_PROCEDURAL_MASKS)) & RT_MASK_PROCEDURAL_CAMERA) != 0u) {
		hitObjectEXT spec_hit_object;
		hitObjectTraceRayEXT(spec_hit_object, tlas, RT_RAY_FLAGS, RT_MASK_PROCEDURAL_CAMERA, 0, 0, 0, spec_origin, 0.001, spec_dir, spec_t, 0);
		if (hitObjectIsHitEXT(spec_hit_object)) {
			hit = true;
			spec_t = hitObjectGetRayTMaxEXT(spec_hit_object);
			reflected_motion = vec3(0.0);
		}
	}
#endif
	if (!hit) {
		// The sky, at infinity: only the camera's rotation moves it, as the velocity of a camera ray that misses.
		const vec3 far_pos = p_hit_pos - V * 10000.0;
		return rt_motion_vector(far_pos, far_pos);
	}
	r_hit_dist = spec_t;
	const vec3 reflector_motion = prev_hit_pos - p_hit_pos;
	const vec3 virtual_pos = p_hit_pos - V * spec_t;
	const vec3 virtual_prev_pos = virtual_pos + reflector_motion + reflect(reflected_motion - reflector_motion, N);
	return rt_motion_vector(virtual_pos, virtual_prev_pos);
}
#endif // DLSS_RR_SPECULAR_MOTION_VECTORS

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
#ifdef NRD_ENABLED
	// The camera path's primary hit: what came so far, its own emission included, stays out of the denoised light (NRD's
	// README, "Noisy inputs": "Direct emission from primary surfaces should be decoupled from incoming radiance").
	const bool nrd_primary = nrd_path_stage(ps) == NRD_PATH_PRIMARY;
	if (nrd_primary) {
		ps.nrd_emission = ps.radiance;
		ps.nrd_state = NRD_PATH_SHADED;
	}
#endif

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
	// DLSS Ray Reconstruction output (primary ray, sample 0 only); NRD takes its albedos, normal and roughness
	// =================================================================
#if defined(DLSS_RR_ENABLED) || defined(NRD_ENABLED)
	if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
		ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

		vec3 diffuse_albedo = DLSSRR_encodeDiffuseAlbedo(DLSSRR_computeDiffuseAlbedo(r.albedo, metalness));
		imageStore(dlss_rr_diffuse_albedo, pixel, vec4(diffuse_albedo, 1.0));

		vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(r.albedo, metalness, brdf_mat.dielectricF0, roughness, NdotV);
		imageStore(dlss_rr_specular_albedo, pixel, vec4(clamp(specular_albedo, vec3(0.04), vec3(1.0)), 1.0)); // match UNORM8 like before - fixes some issues with garbling..

		imageStore(dlss_rr_normal_roughness, pixel, vec4(N, roughness));

#ifdef DLSS_RR_SPECULAR_MOTION_VECTORS
		// Ray Reconstruction takes the specular motion vectors instead of the hit distance (dlss.cpp), which stays written
		// for its debug view, from the same reflection ray.
		float spec_hit_dist;
		// The surface's own motion vector, which the closest hit wrote to the velocity in this sample's camera ray
		// (write_primary_hit_velocity()), as the raygen reads its depth back for the depth guides (rt_depth_image).
		const vec2 surface_motion_vector = imageLoad(rt_velocity_image, pixel).xy;
		const vec2 specular_motion_vector = rt_specular_motion_vector(r, roughness, p_ray_origin, p_ray_dir, hit_pos, surface_motion_vector, spec_hit_dist);
		imageStore(dlss_rr_specular_motion_vectors, pixel, vec4(specular_motion_vector, 0.0, 0.0));
		imageStore(dlss_rr_specular_hit_dist, pixel, vec4(spec_hit_dist));
#elif defined(DLSS_RR_ENABLED)
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
#endif
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
#ifdef NRD_ENABLED
		vec3 direct_specular;
		vec3 direct_light = lights_evaluate_direct_lighting(
				hit_pos_offset, hit_pos_back, r.geometry_normal, N, V, brdf_mat, r.backlight, (r.flags & HIT_FLAG_SPECULAR_DISABLED) != 0u, ps.rng_state, is_indirect, rt_light_count, geometries[r.geometry_idx].layer_mask RT_SHADOW_RECEIVER_OF(r.geometry_idx), direct_specular);
#else
		vec3 direct_light = lights_evaluate_direct_lighting(
				hit_pos_offset, hit_pos_back, r.geometry_normal, N, V, brdf_mat, r.backlight, (r.flags & HIT_FLAG_SPECULAR_DISABLED) != 0u, ps.rng_state, is_indirect, rt_light_count, geometries[r.geometry_idx].layer_mask RT_SHADOW_RECEIVER_OF(r.geometry_idx));
#endif
		ps.radiance += ps.throughput * direct_light * mix(1.0, r.ao, r.ao_light_affect);
#ifdef NRD_ENABLED
		if (nrd_primary) {
			const vec3 scale = ps.throughput * mix(1.0, r.ao, r.ao_light_affect);
			ps.nrd_diffuse_direct += (direct_light - direct_specular) * scale;
			ps.nrd_specular_direct += direct_specular * scale;
		}
#endif
	}

	// =================================================================
	// BRDF importance sampling for next bounce
	// =================================================================
	float specularLum = luminance(specularF0);
	float diffuseLum = luminance(diffuseReflectance);
	// The chance of sampling the specular lobe, as picked below. NRD's choice at the primary hit clamps it to [1/4, 3/4] (see
	// nrd_lobe_threshold()); the sky's sampling below weighs its directions with this same chance.
#ifdef NRD_ENABLED
	const vec2 lobe_probability_range = nrd_primary ? vec2(0.25, 0.75) : vec2(0.01, 0.99);
	const float specular_probability = diffuseLum < 0.0001 ? 1.0 : (specularLum < 0.0001 ? 0.0 : clamp(specularLum / (specularLum + diffuseLum), lobe_probability_range.x, lobe_probability_range.y));
#else
	const float specular_probability = diffuseLum < 0.0001 ? 1.0 : (specularLum < 0.0001 ? 0.0 : clamp(specularLum / (specularLum + diffuseLum), 0.01, 0.99));
#endif

	// =================================================================
	// NEE of the Environment: one direction from the sky's distribution, combined with the BRDF sampling below by
	// multiple importance sampling (raytracing_environment_inc.glsl)
	// =================================================================
	RTEnvSampling env_sampling = rt_env_sampling_setup(specular_probability, roughness, !is_unextended(ps.packed_bounces_flags), dot(N, V));
	if (env_sampling.diffuse || env_sampling.specular) {
#ifdef NRD_ENABLED
		vec3 env_specular;
		vec3 env_light = rt_environment_nee(env_sampling, offset_ray_origin(hit_pos, r.geometry_normal), r.geometry_normal, N, V, brdf_mat, r.ao, ps.rng_state, env_specular);
#else
		vec3 env_light = rt_environment_nee(env_sampling, offset_ray_origin(hit_pos, r.geometry_normal), r.geometry_normal, N, V, brdf_mat, r.ao, ps.rng_state);
#endif
		ps.radiance += ps.throughput * env_light;
#ifdef NRD_ENABLED
		if (nrd_primary) {
			ps.nrd_diffuse_direct += (env_light - env_specular) * ps.throughput;
			ps.nrd_specular_direct += env_specular * ps.throughput;
		}
#endif
	}

	int brdfType;
	if (diffuseLum < 0.0001) {
		brdfType = SPECULAR_TYPE;
	} else if (specularLum < 0.0001) {
		brdfType = DIFFUSE_TYPE;
	} else {
		float brdfProbability = specular_probability;
#ifdef NRD_ENABLED
		// NRD's lobe choice at the primary hit: against a Bayer threshold (nrd_lobe_threshold()), so that the hit distance
		// NRD rebuilds for the skipped lobe has neighbors to come from.
		const float lobe_random = nrd_primary ? nrd_lobe_threshold(gl_LaunchIDEXT.xy, uint(get_rt_param(RT_PARAM_FRAME_INDEX))) : rand(ps.rng_state);
		if (lobe_random < brdfProbability) {
#else
		if (rand(ps.rng_state) < brdfProbability) {
#endif
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
	if (brdfType == DIFFUSE_TYPE || r.roughness > SINGULAR_ROUGHNESS_MAX) {
		ps.packed_bounces_flags |= NON_SINGULAR_PATH_FLAG;
	}
	// If the next ray leaves the scene, it takes the Environment's light by this lobe: as raster's ambient_light for the
	// diffuse lobe and indirect_specular_light for the specular one (scene_forward_clustered.glsl).
	ps.packed_bounces_flags = set_specular_lobe(ps.packed_bounces_flags, brdfType == SPECULAR_TYPE);
	// The share of that light next event estimation of the sky took for this direction, which the miss leaves to it.
	ps.packed_bounces_flags = set_env_mis_share(ps.packed_bounces_flags, rt_env_bounce_share(env_sampling, brdfType == SPECULAR_TYPE, N, V, next_dir, recovered));
#ifdef NRD_ENABLED
	// The rest of the camera path's light is this lobe's, and the next ray's hit distance its hit distance.
	if (nrd_primary) {
		ps.nrd_state = NRD_PATH_BOUNCE | (brdfType == SPECULAR_TYPE ? NRD_PATH_SPECULAR : 0u);
	}
#endif

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

/// Whether a camera ray sees the triangle p_primitive of the geometry p_geometry_idx that a shadow ray met p_t along
/// p_direction: an instance the camera rays see, of the same triangles (the shadow rays' own copies, for double-sided
/// casters and transparent shadows, have their own records over the same vertices), at that distance. NVIDIA's test
/// (49a825dc52 raytracing_closest_hit_common_inc.glsl:319-327) compared the geometry index, which those copies change.
bool debug_camera_sees(uint p_geometry_idx, uint p_primitive, vec3 p_origin, vec3 p_direction, float p_t) {
	const uint64_t vertices = geometries[p_geometry_idx].vertex_address;
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoOpaqueEXT, RT_MASK_CAMERA | RT_MASK_SECONDARY, p_origin, max(p_t - 0.01, 0.0), p_direction, p_t + 0.01);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
				rayQueryGetIntersectionPrimitiveIndexEXT(rq, false) == p_primitive &&
				geometries[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)].vertex_address == vertices) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

/// View 27 (NVIDIA's, 49a825dc52 raytracing_closest_hit_common_inc.glsl:329-379): from the primary hit toward the first
/// directional light, against the shadow casters as the shadow rays see them (faces turned to the light culled, cut-outs
/// alpha tested; procedural geometry, which only its hit group can intersect, left out). White: the light is visible.
/// Yellow: only transparent casters (rendering/pathtracing/transparent_shadows) in the way, darker with less light through.
/// Red: an opaque caster the camera rays do not see blocks it. Blue: one they see. Darker: farther. Dark gray: the surface
/// faces away from the light. Magenta: no directional light.
vec3 debug_directional_shadow_blocker(vec3 p_hit_pos, vec3 p_geometry_normal) {
	const uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	int sun = -1;
	for (uint i = 0u; i < light_count; i++) {
		if (rt_lights[i].type == RT_LIGHT_TYPE_DIRECTIONAL) {
			sun = int(i);
			break;
		}
	}
	if (sun < 0) {
		return vec3(1.0, 0.0, 1.0);
	}
	const vec3 to_sun = -normalize(rt_lights[sun].position);
	if (dot(p_geometry_normal, to_sun) <= 0.0) {
		return vec3(0.1);
	}

	const vec3 origin = offset_ray_origin(p_hit_pos, p_geometry_normal);
	vec3 transmittance = vec3(1.0);
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, RT_SHADOW_RAY_FLAGS, RT_MASK_SHADOW, origin, 0.001, to_sun, 10000.0);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) {
			continue;
		}
		const uint geometry_idx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false);
		const uint primitive_id = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
		const vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
#ifdef RT_TRANSPARENT_SHADOWS
		if ((geometries[geometry_idx].flags & FLAG_TRANSPARENT_SHADOW) != 0u) {
			// Every transparent caster on the way filters the light (shadow_transmittance_filter()), none ends the query.
			const vec4 color = ray_query_shadow_color(geometry_idx, primitive_id, bary);
			shadow_transmittance_filter(transmittance, color.rgb, color.a);
			continue;
		}
#endif
		if (ray_query_alpha_test(geometry_idx, primitive_id, bary)) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}

	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		return transmittance == vec3(1.0) ? vec3(1.0) : vec3(1.0, 1.0, 0.0) * luminance(transmittance);
	}
	const float t = rayQueryGetIntersectionTEXT(rq, true);
	const uint blocker = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
	const uint blocker_primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
	const float brightness = clamp(1.0 - log2(1.0 + t) / 8.0, 0.25, 1.0);
	const vec3 color = debug_camera_sees(blocker, blocker_primitive, origin, to_sun, t) ? vec3(0.0, 0.3, 1.0) : vec3(1.0, 0.0, 0.0);
	return color * brightness;
}

/// View 27 at the camera ray's hit; its miss shows the sky. False when the view's color is final.
bool debug_shadow_blocker_view(inout PathState ps, HitRecord r, bool p_missed, inout vec3 ray_origin, vec3 ray_dir) {
	if (p_missed) {
		ps.radiance = r.emissive;
		return false;
	}
	const vec3 hit_pos = ray_origin + ray_dir * r.hit_t;
	if ((r.flags & HIT_FLAG_PASS_THROUGH) != 0u) {
		ray_origin = offset_ray_origin(hit_pos, ray_dir);
		return true;
	}
	ps.radiance = debug_directional_shadow_blocker(hit_pos, r.geometry_normal);
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

	// Whether the ray hit, and where, come from the hit object, not from the payload fields this shader wrote before the
	// call (packed_bounces_flags, hit_t): NVIDIA's Vulkan driver gave the raygen those fields back as it had written them,
	// in most runs, instead of what the closest hit and miss shaders wrote; the rest of the payload, which only they
	// write, came back right. The haze of PT-VK-SER-PAYLOAD-WA.md: hit_t 0, every bounce left from the camera.
	const bool missed = !hitObjectIsHitEXT(hitObject);
	HitRecord r = hit_unpack(payload);
	r.hit_t = missed ? 0.0 : hitObjectGetRayTMaxEXT(hitObject);
#else
	traceRayEXT(tlas, RT_RAY_FLAGS, mask, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);

	const bool missed = is_path_terminated(payload.packed_bounces_flags);
	const HitRecord r = hit_unpack(payload);
#endif
	r_hit_t = r.hit_t;
#ifdef RT_DEBUG_ENABLED
	{
		const int vis_mode = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (vis_mode == 1 || vis_mode == 13) {
			return debug_reflection_view(vis_mode, ps, r, missed, ray_origin, ray_dir);
		}
		if (vis_mode == RT_VIS_DIRECTIONAL_SHADOW_BLOCKER) {
			return debug_shadow_blocker_view(ps, r, missed, ray_origin, ray_dir);
		}
	}
#endif
	if ((r.flags & HIT_FLAG_DEBUG) != 0u) {
		ps.radiance = r.emissive;
		return false;
	}
#ifdef NRD_ENABLED
	// The hit distance of the ray the primary hit's bounce traced, NRD's "hitT for 1st bounce (recommended baseline)", in
	// its lobe. Where vertex() removed the point, the ray goes on from it and the next hit's distance counts from there.
	if (nrd_path_stage(ps) == NRD_PATH_BOUNCE && (missed || (r.flags & HIT_FLAG_PASS_THROUGH) == 0u)) {
		ps.nrd_hit_t = missed ? NRD_MISS_HIT_T : r.hit_t;
		ps.nrd_state = (ps.nrd_state & ~NRD_PATH_STAGE_MASK) | NRD_PATH_DONE;
	}
#endif
	if (missed) {
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

#ifdef RT_TRANSPARENCY_PEELING
// ============================================================================
// TRANSPARENCY PEELING (rendering/pathtracing/max_transparency_layers)
// ============================================================================
// NVIDIA's transparency peeling (49a825dc52 scene_raytracing_raygen.glsl:258-386, its peel hit
// raytracing_closest_hit_common_inc.glsl:843-915) on this raygen's shading: the camera ray's transparent surfaces, the front
// layer ones (RT_MASK_FRONT_LAYER), are traced front to back as the closest hits of rays that each start one float step past
// the last layer (the instances are opaque to traversal unless they cut out). Each layer is lit as an opaque hit is
// (shade_and_bounce(): emission, direct light, the sky's next event estimation), weighed by its coverage and the light the
// layers in front let through; one bounce goes on, among the layers' and the opaque hit's, picked in proportion to its
// throughput (NVIDIA's continuation reservoir), so a pixel still traces one path. The raster transparent pass leaves these
// surfaces out (RenderForwardClusteredPT).

/// The layers the camera rays peel; 0: none (the front layer is traced for its reflection, as without the variant).
uint peel_layer_budget() {
	if (get_rt_param(RT_PARAM_FRONT_LAYER_COUNT) <= 0.0) {
		return 0u;
	}
	return min((uint(get_rt_param(RT_PARAM_FLAGS)) >> RT_PARAM_FLAG_PEEL_LAYERS_SHIFT) & RT_PARAM_FLAG_PEEL_LAYERS_MASK, 64u);
}

/// The nearest front layer surface along p_dir from p_origin between p_tmin and p_tmax (the opaque surface behind, which the
/// hit shaders read as DEPTH_TEXTURE). False where there is none.
bool peel_trace_layer(vec3 p_origin, vec3 p_dir, float p_tmin, float p_tmax, out HitRecord r_layer, out float r_alpha) {
	payload.packed_bounces_flags = set_front_layer_ray(0u);
	payload.hit_t = p_tmax;
	r_alpha = 0.0;
#ifdef USE_SER
	// Whether the ray hit, and where, from the hit object, as trace_path_ray() reads them: NVIDIA's Vulkan driver gave the
	// raygen back the payload fields it wrote before the call (PT-VK-SER-PAYLOAD-WA.md), and the layers flickered in and
	// out between frames. A miss runs no shader.
	hitObjectEXT hit_object;
	hitObjectTraceRayEXT(hit_object, tlas, RT_RAY_FLAGS, RT_MASK_FRONT_LAYER, 0, 0, 0, p_origin, p_tmin, p_dir, p_tmax, 0);
	if (!hitObjectIsHitEXT(hit_object)) {
		r_layer = hit_unpack(payload);
		return false;
	}
	hitObjectExecuteShaderEXT(hit_object, 0);
	r_layer = hit_unpack(payload);
	r_layer.hit_t = hitObjectGetRayTMaxEXT(hit_object);
#else
	traceRayEXT(tlas, RT_RAY_FLAGS, RT_MASK_FRONT_LAYER, 0, 0, 0, p_origin, p_tmin, p_dir, p_tmax, 0);
	r_layer = hit_unpack(payload);
	if (is_path_terminated(payload.packed_bounces_flags)) {
		return false;
	}
#endif
	r_alpha = clamp(hit_unpack_peel_alpha(payload, r_layer), 0.0, 1.0);
	return true;
}

/// Where the next peel ray starts: one representable float past the layer, which it would hit again otherwise (NVIDIA's step,
/// scene_raytracing_raygen.glsl:332-334).
float peel_next_tmin(float p_t) {
	return uintBitsToFloat(floatBitsToUint(p_t) + 1u);
}

/// The blend mode the layer r was put in the TLAS with (build_tlas()).
uint peel_blend(HitRecord r) {
	return (geometries[r.geometry_idx].flags >> FLAG_PEEL_BLEND_SHIFT) & FLAG_PEEL_BLEND_MASK;
}

/// How much the layer r of alpha p_alpha lets through of what is behind it: Mix (and OIT) and premultiplied alpha cover by
/// their alpha, Add and Subtract cover nothing, Multiply filters by its color (NVIDIA's luminance, per channel here).
vec3 peel_layer_transmittance(HitRecord r, uint p_blend, float p_alpha) {
	if (p_blend == PEEL_BLEND_MUL) {
		return clamp(r.albedo, vec3(0.0), vec3(1.0));
	}
	return vec3(p_blend == PEEL_BLEND_ADD || p_blend == PEEL_BLEND_SUB ? 1.0 : 1.0 - p_alpha);
}

/// Peels the camera ray's transparent layers in front of its first hit, p_opaque_t away (10000 where it missed). ps is the
/// camera path with that hit shaded (at full throughput), r_origin and r_dir its next ray where p_goes_on; on return, ps holds
/// the layers' light over the hit's, attenuated by what they cover, and r_origin / r_dir the ray the path goes on with, if it
/// does (the return value). r_layers: the layers peeled; r_first, r_first_alpha: the nearest one and its coverage (-1: none).
bool peel_camera_segment(inout PathState ps, inout vec3 r_origin, inout vec3 r_dir, bool p_goes_on, vec3 p_camera_origin, vec3 p_camera_dir, float p_opaque_t, uint p_budget, out uint r_layers, out HitRecord r_first, out float r_first_alpha) {
	r_layers = 0u;
	r_first_alpha = -1.0;
	vec3 transmittance = vec3(1.0);
	vec3 layers_light = vec3(0.0);
	// The continuation: one of the bounces offered, kept with a chance proportional to its throughput's largest component;
	// the kept one's throughput is then divided by that chance (total_weight / chosen_weight).
	vec3 chosen_throughput = vec3(0.0);
	uint chosen_flags = 0u;
	vec3 chosen_origin = r_origin;
	vec3 chosen_dir = r_dir;
	float chosen_weight = 0.0;
	float total_weight = 0.0;
	bool layer_chosen = false;

	float tmin = 0.001;
	for (uint l = 0u; l < p_budget && tmin < p_opaque_t && max(transmittance.r, max(transmittance.g, transmittance.b)) > 1.0 / 256.0; l++) {
		HitRecord r;
		float alpha;
		if (!peel_trace_layer(p_camera_origin, p_camera_dir, tmin, p_opaque_t, r, alpha)) {
			break;
		}
		tmin = peel_next_tmin(r.hit_t);
		if ((r.flags & HIT_FLAG_PASS_THROUGH) != 0u) {
			continue; // vertex() removed the point.
		}
		const uint blend = peel_blend(r);
		if (r_layers == 0u) {
			r_first = r;
			r_first_alpha = blend == PEEL_BLEND_MIX || blend == PEEL_BLEND_PREMUL_ALPHA ? alpha : 0.0;
		}
		r_layers++;
		if (blend != PEEL_BLEND_MUL) {
			// Its light as raster blends it: by its alpha (Mix, Add, Subtract), whole (premultiplied alpha).
			PathState ls;
			ls.radiance = vec3(0.0);
			ls.throughput = transmittance * (blend == PEEL_BLEND_PREMUL_ALPHA ? 1.0 : alpha);
			// Not sample zero: the per pixel writes are the opaque hit's.
			ls.packed_bounces_flags = pack_bounces(0u, 0u);
			ls.rng_state = ps.rng_state;
#ifdef NRD_ENABLED
			nrd_path_init(ls, false);
#endif
			vec3 offset_normal;
			vec3 next_dir;
			const bool bounced = shade_and_bounce(ls, r, p_camera_origin, p_camera_dir, offset_normal, next_dir);
			ps.rng_state = ls.rng_state;
			layers_light += blend == PEEL_BLEND_SUB ? -ls.radiance : ls.radiance;
			// Subtract takes light away: its bounce is not a continuation.
			const float weight = max(ls.throughput.r, max(ls.throughput.g, ls.throughput.b));
			if (bounced && blend != PEEL_BLEND_SUB && weight > 1e-6) {
				total_weight += weight;
				if (rand(ps.rng_state) * total_weight < weight) {
					chosen_throughput = ls.throughput;
					chosen_flags = ls.packed_bounces_flags;
					chosen_origin = offset_ray_origin(p_camera_origin + p_camera_dir * r.hit_t, offset_normal);
					chosen_dir = next_dir;
					chosen_weight = weight;
					layer_chosen = true;
				}
			}
		}
		transmittance *= peel_layer_transmittance(r, blend, alpha);
	}
	if (r_layers == 0u) {
		return p_goes_on;
	}

	// The opaque hit, behind the layers.
	ps.radiance = layers_light + transmittance * ps.radiance;
#ifdef NRD_ENABLED
	// What NRD denoises is the opaque hit's light; the layers' is not denoised.
	ps.nrd_emission = layers_light + transmittance * ps.nrd_emission;
	ps.nrd_diffuse_direct *= transmittance;
	ps.nrd_specular_direct *= transmittance;
#endif
	if (p_goes_on) {
		const vec3 throughput = ps.throughput * transmittance;
		const float weight = max(throughput.r, max(throughput.g, throughput.b));
		if (weight > 1e-6) {
			total_weight += weight;
			if (rand(ps.rng_state) * total_weight < weight) {
				chosen_throughput = throughput;
				chosen_flags = ps.packed_bounces_flags;
				chosen_origin = r_origin;
				chosen_dir = r_dir;
				chosen_weight = weight;
				layer_chosen = false;
			}
		}
	}
	if (chosen_weight <= 0.0) {
		return false;
	}
	ps.throughput = chosen_throughput * (total_weight / chosen_weight);
	if (layer_chosen) {
		ps.packed_bounces_flags = chosen_flags | (ps.packed_bounces_flags & SAMPLE_ZERO_FLAG);
#ifdef NRD_ENABLED
		// The rest of the path is a layer's light: not denoised.
		if (nrd_path_stage(ps) != NRD_PATH_PRIMARY) {
			ps.nrd_state = NRD_PATH_SHADED;
		}
#endif
	}
	r_origin = chosen_origin;
	r_dir = chosen_dir;
	return true;
}
#endif // RT_TRANSPARENCY_PEELING
