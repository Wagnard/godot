// Common closest_hit utilities shared by all hit groups.
//
// Required includes (before this file):
//   raytracing_inc.glsl, brdf_inc.glsl, raytracing_hit_inc.glsl, raytracing_lights_inc.glsl,
//   raytracing_material_eval_inc.glsl
//
// Required bindings (before this file):
//   tlas, payload, scene_data_block, geometries[], motion_indices[], materials[], motion_transforms[], bindless_textures[],
//   SAMPLER_* (12 material samplers), rt_params, rt_depth_image,
//   DLSS-RR images (ifdef DLSS_RR_ENABLED)

// ============================================================================
// HIT DATA
// ============================================================================

struct HitData {
	vec3 hit_pos;
	vec3 geometry_normal; // World space, flipped for back-face hits of a material with DO_SIDE_CHECK (rt_side_checked_normal()).
	vec3 tangent; // World space.
	vec3 bitangent; // World space.
	vec3 mesh_pos; // Mesh space: the instance transform of a compressed mesh also scales by its AABB.
	vec3 mesh_normal; // Mesh space, not flipped.
	vec2 uv; // Raw UV (no material scale/offset applied).
	vec4 color; // Vertex color (white if not present).
	bool is_front_face;
	uint geometry_idx;
};

/// Fetch vertex attributes and transform to world space.
/// Requires hitAttributeEXT HitAttribs and GeometryBuffer/MaterialBuffer bindings.
HitData compute_hit_data() {
	HitData h;
	h.geometry_idx = gl_InstanceCustomIndexEXT;
	GeometryData geom = geometries[h.geometry_idx];

	// Custom hit groups may reference vertex color in their fragment code, so
	// pull all attributes; default HGs can skip color for perf.
#ifdef RT_CUSTOM_HIT_GROUP
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_ALL);
#else
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_UV | FETCH_TBN);
#endif
	h.uv = attrs.uv;
	h.color = attrs.color;

	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		h.uv = hit_attribs.bary_or_uv;
		vec3 obj_normal = normalize(unpackSnorm4x8(hit_attribs.packed_normal).xyz);
		h.mesh_normal = obj_normal;
		vec3 obj_tangent = normalize(unpackSnorm4x8(hit_attribs.packed_tangent).xyz);
		h.geometry_normal = normalize(normal_matrix * obj_normal);
		h.tangent = normalize(normal_matrix * obj_tangent);
		h.bitangent = cross(h.geometry_normal, h.tangent);

		h.is_front_face = (dot(h.geometry_normal, -gl_WorldRayDirectionEXT) > 0.0);
		if (!h.is_front_face) {
			h.geometry_normal = -h.geometry_normal;
		}
	} else
#endif
	{
		// Triangle hit: reuse `attrs` from the top-level fetch (already has
		// UV / TBN from FETCH_UV | FETCH_TBN or FETCH_ALL).
		h.mesh_normal = attrs.normal;
		h.geometry_normal = normalize(normal_matrix * attrs.normal);
		h.tangent = normalize(normal_matrix * attrs.tangent);
		h.bitangent = cross(h.geometry_normal, h.tangent) * attrs.bitangent_sign;

		// FRONT_FACING and the side of the normal as raster has them: a front-culled material, or a mirrored instance,
		// reports the other face, and only a material with DO_SIDE_CHECK turns the normal of its back faces.
		h.is_front_face = rt_triangle_front_facing(geom);
		h.geometry_normal = rt_side_checked_normal(geom, h.geometry_normal, h.is_front_face);
	}

	h.hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
	h.mesh_pos = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * gl_HitTEXT;
	if ((geom.flags & FLAG_COMPRESSED) != 0u) {
		h.mesh_pos = h.mesh_pos * vec3(geom.aabb_size_x, geom.aabb_size_y, geom.aabb_size_z) + vec3(geom.aabb_pos_x, geom.aabb_pos_y, geom.aabb_pos_z);
	}

	if ((geom.flags & FLAG_DISPLACED) != 0u) {
		// The TBN block holds the rest frame (vertex()'s input); the geometric normal comes from the displaced triangle.
		uint i0, i1, i2;
		get_triangle_indices(geom, i0, i1, i2);
		FloatBuffer vb = FloatBuffer(geom.vertex_address);
		vec3 p0 = vec3(vb.v[i0 * 3u], vb.v[i0 * 3u + 1u], vb.v[i0 * 3u + 2u]);
		vec3 p1 = vec3(vb.v[i1 * 3u], vb.v[i1 * 3u + 1u], vb.v[i1 * 3u + 2u]);
		vec3 p2 = vec3(vb.v[i2 * 3u], vb.v[i2 * 3u + 1u], vb.v[i2 * 3u + 2u]);
		vec3 face_normal = transpose(mat3(gl_WorldToObjectEXT)) * cross(p1 - p0, p2 - p0);
		if (dot(face_normal, face_normal) > 1e-20) {
			face_normal = normalize(face_normal);
			h.geometry_normal = dot(face_normal, gl_WorldRayDirectionEXT) > 0.0 ? -face_normal : face_normal;
		}
	}

	return h;
}

// ============================================================================
// HELPERS
// ============================================================================

vec4 sample_bindless_texture(uint tex_idx, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv);
}

/// Sample with point/nearest filtering (for pixel art textures).
vec4 sample_bindless_texture_point(uint tex_idx, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_REPEAT), uv);
}

/// Sample with the appropriate filter based on material flags (bit 2 = point filtering).
vec4 sample_material_texture(uint tex_idx, vec2 uv, uint mat_flags) {
	if ((mat_flags & 4u) != 0u) {
		return sample_bindless_texture_point(tex_idx, uv);
	}
	return sample_bindless_texture(tex_idx, uv);
}

/// With the screen derivatives of the coordinates, for the mip level raster picks.
vec4 sample_material_texture_grad(uint tex_idx, vec2 uv, vec2 uv_ddx, vec2 uv_ddy, uint mat_flags) {
	if ((mat_flags & 4u) != 0u) {
		return sample_bindless_texture_point(tex_idx, uv);
	}
	if ((mat_flags & MAT_FLAG_ANISOTROPIC_FILTER) != 0u) {
		return textureGrad(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT), uv, uv_ddx, uv_ddy);
	}
	return textureGrad(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, uv_ddx, uv_ddy);
}

/// BaseMaterial3D's triplanar_texture().
vec4 sample_material_triplanar(uint tex_idx, vec3 weights, vec3 triplanar_pos, vec3 pos_ddx, vec3 pos_ddy, uint mat_flags) {
	vec4 samp = sample_material_texture_grad(tex_idx, triplanar_pos.xy, pos_ddx.xy, pos_ddy.xy, mat_flags) * weights.z;
	samp += sample_material_texture_grad(tex_idx, triplanar_pos.xz, pos_ddx.xz, pos_ddy.xz, mat_flags) * weights.y;
	samp += sample_material_texture_grad(tex_idx, triplanar_pos.zy * vec2(-1.0, 1.0), pos_ddx.zy * vec2(-1.0, 1.0), pos_ddy.zy * vec2(-1.0, 1.0), mat_flags) * weights.x;
	return samp;
}

/// Apply tangent-space normal map to geometry normal.
vec3 apply_normal_map(HitData h, vec3 tangent_space_normal, float normal_map_depth) {
	vec3 mapped = h.tangent * tangent_space_normal.x + h.bitangent * tangent_space_normal.y + h.geometry_normal * tangent_space_normal.z;
	return normalize(mix(h.geometry_normal, mapped, normal_map_depth));
}

// ============================================================================
// DEPTH WRITE (primary ray only)
// ============================================================================

/// Write NDC depth for primary ray hits (bounce 0, sample 0 only).
void write_primary_hit_depth(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) == 0u && is_sample_zero(payload.packed_bounces_flags)) {
		mat4 view_mat = transpose(mat4(scene_data_block.data.view_matrix[0],
				scene_data_block.data.view_matrix[1],
				scene_data_block.data.view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));
		vec3 view_pos = (view_mat * vec4(hit_pos, 1.0)).xyz;
		vec4 clip_pos = scene_data_block.data.projection_matrix * vec4(view_pos, 1.0);
		float ndc_depth = clip_pos.z / clip_pos.w;
		imageStore(rt_depth_image, ivec2(gl_LaunchIDEXT.xy), vec4(ndc_depth));
	}
}

// ============================================================================
// VELOCITY WRITE (primary ray only, MV-gated)
// ============================================================================

#ifdef ENABLE_INTERSECTION_SHADERS
/// Decode the FP16-compressed PREV_POSITION delta from HitAttribs.
vec3 decode_prev_pos_delta() {
	uint dx_low = (hit_attribs.packed_normal >> 24u) & 0xFFu;
	uint dx_high = (hit_attribs.packed_tangent >> 24u) & 0xFFu;
	float delta_x = unpackHalf2x16(dx_low | (dx_high << 8u)).x;
	vec2 delta_yz = unpackHalf2x16(hit_attribs.prev_pos_delta_yz);
	return vec3(delta_x, delta_yz.x, delta_yz.y);
}
#endif

/// Reconstruct previous-frame mat4 from a compact motion transform entry.
mat4 decode_prev_object_to_world(int motion_idx) {
	InstanceMotionData m = motion_transforms[motion_idx];
	return transpose(mat4(
			vec4(m.prev_xform[0], m.prev_xform[1], m.prev_xform[2], m.prev_xform[3]),
			vec4(m.prev_xform[4], m.prev_xform[5], m.prev_xform[6], m.prev_xform[7]),
			vec4(m.prev_xform[8], m.prev_xform[9], m.prev_xform[10], m.prev_xform[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
}

/// Write motion vectors for primary ray hits (bounce 0, sample 0 only).
/// Uses unjittered VP matrices matching the raster motion_vectors_store convention.
void write_primary_hit_velocity(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) != 0u || !is_sample_zero(payload.packed_bounces_flags)) {
		return;
	}

	uint geom_idx = gl_InstanceCustomIndexEXT;
	int mi = motion_indices[geom_idx];

	// Resolve previous-frame model matrix: compact entry if moved, current transform otherwise.
	mat4 prev_model = (mi >= 0) ? decode_prev_object_to_world(mi) : mat4(gl_ObjectToWorldEXT);

	vec3 obj_pos = (mat4(gl_WorldToObjectEXT) * vec4(hit_pos, 1.0)).xyz;
	vec3 prev_obj_pos = obj_pos;

	GeometryData geom = geometries[geom_idx];

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		prev_obj_pos += decode_prev_pos_delta();
	}
#endif

#ifdef RT_HIT_ATTRIBS_DECLARED
	if ((geom.flags & FLAG_DEFORMED) != 0u) {
		uint64_t prev_addr = packUint2x32(uvec2(geom.prev_vertex_address_lo, geom.prev_vertex_address_hi));
		if (prev_addr != 0ul) {
			uint i0, i1, i2;
			get_triangle_indices(geom, i0, i1, i2);
			vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
			FloatBuffer prev_vb = FloatBuffer(prev_addr);
			uint stride_floats = geom.position_stride >> 2;
			vec3 p0 = vec3(prev_vb.v[i0 * stride_floats + 0u],
					prev_vb.v[i0 * stride_floats + 1u],
					prev_vb.v[i0 * stride_floats + 2u]);
			vec3 p1 = vec3(prev_vb.v[i1 * stride_floats + 0u],
					prev_vb.v[i1 * stride_floats + 1u],
					prev_vb.v[i1 * stride_floats + 2u]);
			vec3 p2 = vec3(prev_vb.v[i2 * stride_floats + 0u],
					prev_vb.v[i2 * stride_floats + 1u],
					prev_vb.v[i2 * stride_floats + 2u]);
			prev_obj_pos = bary.x * p0 + bary.y * p1 + bary.z * p2;
		}
	} else if (geom.multimesh_previous_offset != geom.multimesh_offset) {
		// A merged MultiMesh whose instances moved, as raster's motion vectors see them.
		uint i0, i1, i2;
		get_triangle_indices(geom, i0, i1, i2);
		prev_obj_pos = multimesh_previous_position(geom, i0, obj_pos);
	}
#endif

	vec3 prev_world_pos = (prev_model * vec4(prev_obj_pos, 1.0)).xyz;

	vec2 curr_uv = project_uv(hit_pos, curr_vp_unjittered);
	vec2 prev_uv = project_uv(prev_world_pos, prev_vp_unjittered);

	imageStore(rt_velocity_image, ivec2(gl_LaunchIDEXT.xy), vec4(prev_uv - curr_uv, 0.0, 0.0));
}

// ============================================================================
// DEBUG VISUALIZATION
// ============================================================================

#ifdef RT_DEBUG_ENABLED
// Environment.RT_DEBUG_DYNAMIC_GEOMETRY (NVIDIA's view, 49a825dc52 raytracing_closest_hit_common_inc.glsl:383-411): the
// instance's recent BLAS work, RTDynamicGeometryDebug::pack() in its geometry record (multimesh_pad0). Must match
// RTDynamicGeometryDebug.
#define RT_VIS_DYNAMIC_GEOMETRY 29
#define RT_DYNAMIC_GEOMETRY_CATEGORY_MAX 7u
#define RT_DYNAMIC_GEOMETRY_CATEGORY_MASK 15u
#define RT_DYNAMIC_GEOMETRY_REST_SHIFT 4u
#define RT_DYNAMIC_GEOMETRY_AGE_SHIFT 8u
#define RT_DYNAMIC_GEOMETRY_FADE_FRAMES 60.0

// Indexed by RTDynamicGeometryDebug::Category: static, moved, refit, build, captured refit, captured build, built idle,
// captured built idle.
const vec3 RT_DYNAMIC_GEOMETRY_COLORS[8] = vec3[](vec3(0.0), vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(0.05, 0.15, 1.0), vec3(1.0, 0.0, 1.0),
		vec3(0.25, 0.015, 0.015), vec3(0.22, 0.015, 0.22));
const float RT_DYNAMIC_GEOMETRY_STRENGTH[8] = float[](0.0, 0.4, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0);

// The color of the category packed at p_shift, over p_base.
vec3 debug_dynamic_geometry_tint(uint p_state, uint p_shift, vec3 p_base, float p_recency) {
	const uint category = min((p_state >> p_shift) & RT_DYNAMIC_GEOMETRY_CATEGORY_MASK, RT_DYNAMIC_GEOMETRY_CATEGORY_MAX);
	return mix(p_base, RT_DYNAMIC_GEOMETRY_COLORS[category], RT_DYNAMIC_GEOMETRY_STRENGTH[category] * p_recency);
}

// The category's color over the rest category's, itself over dim grayscale shading. This frame's work (age 0) is opaque;
// earlier work starts well below that and fades out over RT_DYNAMIC_GEOMETRY_FADE_FRAMES.
vec3 debug_dynamic_geometry(uint p_state, vec3 p_albedo, float p_NdotV) {
	const uint age = p_state >> RT_DYNAMIC_GEOMETRY_AGE_SHIFT;
	const vec3 gray = vec3((0.02 + 0.13 * luminance(p_albedo)) * (0.3 + 0.7 * p_NdotV));
	const vec3 rest = debug_dynamic_geometry_tint(p_state, RT_DYNAMIC_GEOMETRY_REST_SHIFT, gray, 1.0);
	const float recency = age == 0u ? 1.0 : mix(0.6, 0.1, float(age) / RT_DYNAMIC_GEOMETRY_FADE_FRAMES);
	return debug_dynamic_geometry_tint(p_state, 0u, rest, recency);
}

void debug_visualize(
		int vis_mode,
		vec3 geometry_normal,
		vec3 final_normal,
		vec3 tangent_space_normal,
		vec3 world_tangent,
		vec3 world_bitangent,
		vec2 uv,
		vec3 albedo,
		vec3 orm,
		float metalness,
		float roughness,
		float specular,
		vec3 emissive,
		vec3 V,
		float NdotV) {
	// The views that trace a second ray (1, 13) are the raygen's (debug_reflection_view()). The random sequence is the
	// one the raygen starts sample 0 with, which the primary hit's shading used.
	PathState ps;
	ps.radiance = vec3(0.0);
	ps.rng_state = init_rng(gl_LaunchIDEXT.xy, uint(get_rt_param(RT_PARAM_FRAME_INDEX)), 0u);

	if (vis_mode == 2) {
		ps.radiance = geometry_normal * 0.5 + 0.5;
	} else if (vis_mode == 3) {
		ps.radiance = final_normal * 0.5 + 0.5;
	} else if (vis_mode == 4) {
		ps.radiance = tangent_space_normal * 0.5 + 0.5;
	} else if (vis_mode == 5) {
		ps.radiance = world_tangent * 0.5 + 0.5;
	} else if (vis_mode == 6) {
		ps.radiance = world_bitangent * 0.5 + 0.5;
	} else if (vis_mode == 7) {
		ps.radiance = vec3(fract(uv), 0.0);
	} else if (vis_mode == 8) {
		ps.radiance = albedo;
	} else if (vis_mode == 9) {
		ps.radiance = orm;
	} else if (vis_mode == 10) {
		ps.radiance = DLSSRR_computeDiffuseAlbedo(albedo, metalness);
	} else if (vis_mode == 11) {
		ps.radiance = DLSSRR_computeSpecularAlbedo(albedo, metalness, specular_to_f0(specular), roughness, NdotV);
	} else if (vis_mode == 12) {
		ps.radiance = (final_normal * 0.5 + 0.5) * (1.0 - roughness * 0.5);
	} else if (vis_mode == 14) {
		ps.radiance = vec3(metalness);
	} else if (vis_mode == 15) {
		ps.radiance = vec3(roughness);
	} else if (vis_mode == 16) {
		mat3 world_to_view = mat3(scene_data_block.data.inv_view_matrix);
		ps.radiance = normalize(world_to_view * final_normal) * 0.5 + 0.5;
	} else if (vis_mode == 17) {
		vec3 diffuse_albedo = DLSSRR_computeDiffuseAlbedo(albedo, metalness);
		vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(albedo, metalness, specular_to_f0(specular), roughness, NdotV);
		ps.radiance = mix(diffuse_albedo, specular_albedo, metalness);
	} else if (vis_mode == 18) {
		ps.radiance = baseColorToSpecularF0(albedo, metalness, specular_to_f0(specular));
	} else if (vis_mode == 19) {
		bool is_front_face = rt_triangle_front_facing(geometries[gl_InstanceCustomIndexEXT]);
		ps.radiance = is_front_face ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	} else if (vis_mode == 20) {
		float depth_range = scene_data_block.data.z_far - scene_data_block.data.z_near;
		float d = clamp(gl_HitTEXT / depth_range, 0.0, 1.0);
		ps.radiance = vec3(d);
	} else if (vis_mode == 21) {
		ps.radiance = emissive;
	} else if (vis_mode == RT_VIS_DYNAMIC_GEOMETRY) {
		ps.radiance = debug_dynamic_geometry(geometries[gl_InstanceCustomIndexEXT].multimesh_pad0, albedo, NdotV);
	} else if (vis_mode == 22) {
		// BRDF below-hemisphere fallback visualization. Reflects the two-layer
		// recovery applied in shade_and_bounce:
		//   Layer 1 - shading-normal clamp toward geometry (energy preserving
		//             but flattens detail at grazing angles)
		//   Layer 2 - mirror rejected directions across the geometry plane
		//             (biased; reuses the rejected sample's BRDF weight)
		//
		// We sample both BRDF lobes once with the un-clamped shading normal
		// and once with the clamped shading normal, reusing the same random
		// pair so the two passes are directly comparable.
		//
		// Color legend (ordered cleanest -> most biased):
		//   green   = no rejection in either pass (no fallback needed)
		//   blue    = clamp alone resolves rejection (energy preserving)
		//   yellow  = clamp + mirror (one lobe still mirrored after clamp)
		//   red     = clamp + mirror (both lobes mirrored - most biased pixel)
		//
		// Production output is never black anymore -- yellow/red just indicate
		// where the cheaper Layer-1 clamp could not catch the bump and the
		// biased Layer-2 mirror had to step in. Raise
		// RT_SHADING_NORMAL_CLAMP_THRESHOLD if yellow/red dominate.
		MaterialProperties dbg_mat;
		dbg_mat.baseColor = albedo;
		dbg_mat.metalness = metalness;
		dbg_mat.roughness = roughness;
		dbg_mat.dielectricF0 = specular_to_f0(specular);
		dbg_mat.emissive = vec3(0.0);
		dbg_mat.transmissivness = 0.0;
		dbg_mat.opacity = 1.0;

		vec3 dbg_dir;
		vec3 dbg_weight;
		// Draw both random pairs up front so each (specular/diffuse) test in
		// the un-clamped and clamped pass uses the exact same numbers.
		vec2 u_spec_rng = rand2(ps.rng_state);
		vec2 u_diff_rng = rand2(ps.rng_state);

		// The Fresnel pair changes only the diffuse weight, not whether the direction is valid: the same pair.
		bool u_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, u_spec_rng, final_normal, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool u_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, u_diff_rng, final_normal, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		// Uses the same threshold as shade_and_bounce so the visualization
		// stays in sync with production sampling.
		vec3 N_clamped = clampShadingNormal(final_normal, geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
		bool c_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, u_spec_rng, N_clamped, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool c_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, u_diff_rng, N_clamped, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		bool all_uncl_ok = u_spec_ok && u_diff_ok;
		bool all_cl_ok = c_spec_ok && c_diff_ok;
		bool any_cl_ok = c_spec_ok || c_diff_ok;

		if (all_uncl_ok) {
			ps.radiance = vec3(0.0, 1.0, 0.0);
		} else if (all_cl_ok) {
			ps.radiance = vec3(0.0, 0.4, 1.0);
		} else if (any_cl_ok) {
			ps.radiance = vec3(1.0, 1.0, 0.0);
		} else {
			ps.radiance = vec3(1.0, 0.0, 0.0);
		}
	}

	payload.hit_t = gl_HitTEXT;
	payload_write_light(payload, ps.radiance, HIT_FLAG_DEBUG);
}
#endif // RT_DEBUG_ENABLED

// ============================================================================
// THE HIT'S MATERIAL, FOR THE RAYGEN
// ============================================================================

/// The material evaluated at the hit, which the raygen shades (raytracing_shade_inc.glsl).
void hit_pack(HitData h, MaterialResult m) {
	payload.hit_t = gl_HitTEXT;
	payload_write_material(payload, h.geometry_normal, m.normal, m.albedo, m.roughness, m.metalness, m.specular, m.emissive, m.backlight, m.ao, m.ao_light_affect,
			h.geometry_idx, (m.unshaded ? HIT_FLAG_UNSHADED : 0u) | (m.specular_disabled ? HIT_FLAG_SPECULAR_DISABLED : 0u));
#ifdef RT_TRANSPARENCY_PEELING
	if (is_front_layer_ray(payload.packed_bounces_flags)) {
		payload_write_peel_alpha(payload, m.ao, m.ao_light_affect, m.alpha);
	}
#endif
}

