// ============================================================================
// Inline Alpha Test (shared by all ray query proceed loops)
// ============================================================================
// Requires: raytracing_hit_inc.glsl, geometries[], materials[], bindless_textures[] and SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT
// declared, GL_EXT_nonuniform_qualifier.

/// Inline alpha test for ray query candidates. Returns true if the hit is opaque (alpha >= 0.5).
/// Mirrors the any-hit shader logic for use with inline ray queries.
bool ray_query_alpha_test(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);

	GeometryData geom = geometries[geometry_idx];
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	vec2 uv = fetch_uv(geom, i0, i1, i2, bary);

	MaterialData mat = materials[geometry_idx];
	uv = uv * mat.uv1_scale + mat.uv1_offset;
	// The base level, as the any-hit shaders and the opacity micromap bake test it: zero gradients take level 0 whatever
	// the sampler's mip bias (texture() took a mip from the shaded pixel's derivatives in a fragment shader).
	float alpha = textureGrad(sampler2D(bindless_textures[nonuniformEXT(mat.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, vec2(0.0), vec2(0.0)).a;
	alpha *= mat.albedo_color.a;
	if ((mat.flags & MAT_FLAG_VERTEX_COLOR_ALBEDO) != 0u) {
		alpha *= fetch_color(geom, i0, i1, i2, bary).a;
	}

	return alpha >= 0.5;
}

#ifdef RT_TRANSPARENT_SHADOWS
/// The color and alpha the any-hit shader of hit group 0 filters a shadow ray with, for a ray query candidate of the shadow
/// rays' copy of a transparent surface (rendering/pathtracing/transparent_shadows).
vec4 ray_query_shadow_color(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);

	GeometryData geom = geometries[geometry_idx];
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	vec2 uv = fetch_uv(geom, i0, i1, i2, bary);

	MaterialData mat = materials[geometry_idx];
	uv = uv * mat.uv1_scale + mat.uv1_offset;
	vec4 color = textureGrad(sampler2D(bindless_textures[nonuniformEXT(mat.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, vec2(0.0), vec2(0.0)) * mat.albedo_color;
	if ((mat.flags & MAT_FLAG_VERTEX_COLOR_ALBEDO) != 0u) {
		color *= material_vertex_color(fetch_color(geom, i0, i1, i2, bary), mat.flags);
	}
	return color;
}
#endif
