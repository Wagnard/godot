// Custom shader fragment setup for RT hit groups (closest-hit and any-hit).
// Include inside main() under #ifdef RT_CUSTOM_HIT_GROUP.
//
// Required variables before inclusion:
//   uint  rt_geometry_idx   -- geometry/material index
//   vec3  rt_hit_pos        -- world-space hit position
//   uint  rt_i0, rt_i1, rt_i2 -- triangle vertex indices
//   vec3  rt_bary           -- barycentrics of the hit
//   vec2  rt_uv             -- interpolated UV
//   vec3  rt_normal         -- world-space geometry normal (flipped for back-face)
//   vec3  rt_tangent        -- world-space tangent
//   vec3  rt_bitangent      -- world-space bitangent
//   bool  rt_front_face     -- true if front-face hit
//
// Required bindings/types:
//   materials[], CustomMaterialUniforms, scene_data_block

MaterialData rt_mat = materials[rt_geometry_idx];
// The raygen passes the distance of the opaque surface behind a front layer hit in the payload (rt_depth_buffer_value()).
if (is_front_layer_ray(payload.packed_bounces_flags) && payload.hit_t > 0.0 && payload.hit_t < 10000.0) {
	vec3 rt_behind_world = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * payload.hit_t;
	vec3 rt_behind_view = (transpose(mat4(scene_data_block.data.view_matrix[0], scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0))) * vec4(rt_behind_world, 1.0)).xyz;
	vec4 rt_behind_clip = scene_data_block.data.projection_matrix * vec4(rt_behind_view, 1.0);
	rt_behind_depth = rt_behind_clip.z / rt_behind_clip.w;
}
material = CustomMaterialUniforms(rt_mat.uniform_address);
rt_instance_uniforms_ofs = rt_mat.instance_uniforms_ofs;

// Matrices.
mat4 rt_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0],
		scene_data_block.data.view_matrix[1],
		scene_data_block.data.view_matrix[2],
		vec4(0.0, 0.0, 0.0, 1.0)));

GeometryData rt_geom = geometries[rt_geometry_idx];
mat4 rt_aabb_xform;
mat4 rt_inv_aabb_xform;
get_aabb_compression_xforms(rt_geom, rt_aabb_xform, rt_inv_aabb_xform);

// A merged MultiMesh: object space is the instance's, as raster's (VERTEX, MODEL_MATRIX), not the MultiMesh's of its BLAS.
bool rt_mm_merged = (rt_geom.flags & FLAG_PROCEDURAL) == 0u && is_merged_multimesh(rt_geom);
mat4 rt_mm_instance = rt_mm_merged ? merged_multimesh_instance_transform(rt_geom, rt_i0) : mat4(1.0);
mat4 rt_inv_mm_instance = rt_mm_merged ? inverse(rt_mm_instance) : mat4(1.0);
read_model_matrix = mat4(gl_ObjectToWorldEXT) * rt_inv_aabb_xform * rt_mm_instance;
model_normal_matrix = transpose(inverse(mat3(read_model_matrix)));
read_view_matrix = rt_view_matrix;
inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
		scene_data_block.data.inv_view_matrix[1],
		scene_data_block.data.inv_view_matrix[2],
		vec4(0.0, 0.0, 0.0, 1.0)));
projection_matrix = scene_data_block.data.projection_matrix;
inv_projection_matrix = scene_data_block.data.inv_projection_matrix;
read_viewport_size = scene_data_block.data.viewport_size;
global_time = scene_data_block.data.time;

mat4 rt_world_to_object_decomp = rt_inv_mm_instance * rt_aabb_xform * mat4(gl_WorldToObjectEXT);

#ifdef VERTEX_WORLD_COORDS_USED
vertex = rt_hit_pos;
normal = rt_normal;
tangent = rt_tangent;
binormal = rt_bitangent;
#else
vertex = (rt_world_to_object_decomp * vec4(rt_hit_pos, 1.0)).xyz;
normal = mat3(rt_world_to_object_decomp) * rt_normal;
tangent = mat3(rt_world_to_object_decomp) * rt_tangent;
binormal = mat3(rt_world_to_object_decomp) * rt_bitangent;
#endif
uv_interp = rt_uv;
uv2_interp = rt_uv;
color_interp = rt_color;
// In view space, as raster's VIEW and as NORMAL is given to fragment(): towards the ray's origin, the camera for a primary
// ray. A world-space VIEW against a view-space NORMAL made dot(NORMAL, VIEW) depend on the camera's orientation (Solfarer's
// rim emission lit whole hulls white at some angles).
view = normalize(mat3(rt_view_matrix) * -gl_WorldRayDirectionEXT);
rt_front_facing = rt_front_face;
rt_screen_uv = vec2(gl_LaunchIDEXT.xy) / vec2(gl_LaunchSizeEXT.xy);
rt_frag_coord = vec4(gl_LaunchIDEXT.xy, 0.0, 1.0);

rt_modelview_matrix = rt_view_matrix * read_model_matrix;
rt_modelview_normal_matrix = mat3(rt_modelview_matrix);

if ((rt_geom.flags & FLAG_PROCEDURAL) == 0u) {
	// As raster does, run vertex() at the triangle's vertices (on their rest data for displaced geometry, whose BLAS
	// holds vertex()'s output already) and interpolate what it writes: run once at the hit, a texelFetch() in vertex()
	// would snap to one texel. The vertex values also give the screen derivatives.
	// Each output is summed at the barycentrics it is read at as soon as its vertex() returns, in the order the
	// interpolation c0 * b.x + c1 * b.y + c2 * b.z evaluates, so the result is the same to the bit while only the sums,
	// not every corner, stay live through the next vertex() (NVIDIA, Ray Tracing Best Practices: live state and register
	// usage limit occupancy).
	// Vectors and one variable per point, the corner's component picked with selects: no array and no dynamic vector index in
	// the loop (see rt_p0_vertex in raytracing_custom_globals_inc.glsl; re-spirv does not handle OpVectorExtractDynamic).
#ifndef RT_STAGE_ANY_HIT
	hit_barycentric_derivatives(rt_geom, rt_i0, rt_i1, rt_i2, rt_bary, rt_hit_bary_ddx, rt_hit_bary_ddy);
#endif
	const vec3 rt_p0_bary = rt_bary;
#if RT_INTERP_POINTS > 1
	const vec3 rt_p1_bary = rt_bary + rt_hit_bary_ddx;
	const vec3 rt_p2_bary = rt_bary + rt_hit_bary_ddy;
#endif
	bool rt_removed = false;
	const uvec3 rt_corner_index = uvec3(rt_i0, rt_i1, rt_i2);
#ifdef ENABLE_INSTANCE_CUSTOM
	instance_custom = fetch_instance_custom(rt_geom, rt_i0);
#endif
	for (int rt_corner = 0; rt_corner < 3; rt_corner++) {
		uint rt_index = rt_corner == 0 ? rt_corner_index.x : (rt_corner == 1 ? rt_corner_index.y : rt_corner_index.z);
		vec3 rt_rest = (rt_geom.flags & FLAG_DISPLACED) != 0u ? fetch_rest_vertex(rt_geom, rt_index) : fetch_mesh_position(rt_geom, rt_index);
		TBNResult rt_tbn;
		rt_tbn.normal = vec3(0.0, 1.0, 0.0);
		rt_tbn.tangent = vec3(1.0, 0.0, 0.0);
		rt_tbn.binormal = vec3(0.0, 0.0, 1.0);
		if (rt_geom.normal_byte_offset != OFFSET_NONE) {
			rt_tbn = (rt_geom.flags & FLAG_COMPRESSED) != 0u ? fetch_tbn_compressed_vertex(rt_geom, rt_index) : fetch_tbn_uncompressed_vertex(rt_geom, rt_index);
		}
		if (rt_mm_merged) {
			rt_rest = (rt_inv_mm_instance * vec4(rt_rest, 1.0)).xyz;
			rt_tbn.normal = normalize(transpose(mat3(rt_mm_instance)) * rt_tbn.normal);
			rt_tbn.tangent = normalize(mat3(rt_inv_mm_instance) * rt_tbn.tangent);
			rt_tbn.binormal = normalize(mat3(rt_inv_mm_instance) * rt_tbn.binormal);
		}
#ifdef VERTEX_WORLD_COORDS_USED
		vertex = (read_model_matrix * vec4(rt_rest, 1.0)).xyz;
		normal = normalize(model_normal_matrix * rt_tbn.normal);
		tangent = normalize(mat3(read_model_matrix) * rt_tbn.tangent);
		binormal = normalize(mat3(read_model_matrix) * rt_tbn.binormal);
#else
		vertex = rt_rest;
		normal = rt_tbn.normal;
		tangent = rt_tbn.tangent;
		binormal = rt_tbn.binormal;
#endif
		uv_interp = fetch_uv(rt_geom, rt_index, rt_index, rt_index, vec3(1.0, 0.0, 0.0));
		uv2_interp = fetch_uv2(rt_geom, rt_index, rt_index, rt_index, vec3(1.0, 0.0, 0.0));
		color_interp = fetch_color(rt_geom, rt_index, rt_index, rt_index, vec3(1.0, 0.0, 0.0));
#ifdef CUSTOM0_USED
		custom0_attrib = fetch_custom(rt_geom, 0u, rt_index);
#endif
#ifdef CUSTOM1_USED
		custom1_attrib = fetch_custom(rt_geom, 1u, rt_index);
#endif
#ifdef CUSTOM2_USED
		custom2_attrib = fetch_custom(rt_geom, 2u, rt_index);
#endif
#ifdef CUSTOM3_USED
		custom3_attrib = fetch_custom(rt_geom, 3u, rt_index);
#endif
		rt_vertex_id = int(rt_index);

		/* RT_CUSTOM_VERTEX_CALL */

		rt_removed = rt_removed || any(isnan(vertex));
		const float rt_w0 = rt_corner == 0 ? rt_p0_bary.x : (rt_corner == 1 ? rt_p0_bary.y : rt_p0_bary.z);
#if RT_INTERP_POINTS > 1
		const float rt_w1 = rt_corner == 0 ? rt_p1_bary.x : (rt_corner == 1 ? rt_p1_bary.y : rt_p1_bary.z);
		const float rt_w2 = rt_corner == 0 ? rt_p2_bary.x : (rt_corner == 1 ? rt_p2_bary.y : rt_p2_bary.z);
#endif
		if (rt_corner == 0) {
			rt_p0_vertex = vertex * rt_w0;
			rt_p0_normal = normal * rt_w0;
			rt_p0_tangent = tangent * rt_w0;
			rt_p0_binormal = binormal * rt_w0;
			rt_p0_uv = uv_interp * rt_w0;
			rt_p0_uv2 = uv2_interp * rt_w0;
			rt_p0_color = color_interp * rt_w0;
			/* RT_CUSTOM_VARYING_ACCUMULATE_FIRST_0 */
#if RT_INTERP_POINTS > 1
			rt_p1_vertex = vertex * rt_w1;
			rt_p1_normal = normal * rt_w1;
			rt_p1_tangent = tangent * rt_w1;
			rt_p1_binormal = binormal * rt_w1;
			rt_p1_uv = uv_interp * rt_w1;
			rt_p1_uv2 = uv2_interp * rt_w1;
			rt_p1_color = color_interp * rt_w1;
			/* RT_CUSTOM_VARYING_ACCUMULATE_FIRST_1 */
			rt_p2_vertex = vertex * rt_w2;
			rt_p2_normal = normal * rt_w2;
			rt_p2_tangent = tangent * rt_w2;
			rt_p2_binormal = binormal * rt_w2;
			rt_p2_uv = uv_interp * rt_w2;
			rt_p2_uv2 = uv2_interp * rt_w2;
			rt_p2_color = color_interp * rt_w2;
			/* RT_CUSTOM_VARYING_ACCUMULATE_FIRST_2 */
#endif
		} else {
			rt_p0_vertex = rt_p0_vertex + vertex * rt_w0;
			rt_p0_normal = rt_p0_normal + normal * rt_w0;
			rt_p0_tangent = rt_p0_tangent + tangent * rt_w0;
			rt_p0_binormal = rt_p0_binormal + binormal * rt_w0;
			rt_p0_uv = rt_p0_uv + uv_interp * rt_w0;
			rt_p0_uv2 = rt_p0_uv2 + uv2_interp * rt_w0;
			rt_p0_color = rt_p0_color + color_interp * rt_w0;
			/* RT_CUSTOM_VARYING_ACCUMULATE_0 */
#if RT_INTERP_POINTS > 1
			rt_p1_vertex = rt_p1_vertex + vertex * rt_w1;
			rt_p1_normal = rt_p1_normal + normal * rt_w1;
			rt_p1_tangent = rt_p1_tangent + tangent * rt_w1;
			rt_p1_binormal = rt_p1_binormal + binormal * rt_w1;
			rt_p1_uv = rt_p1_uv + uv_interp * rt_w1;
			rt_p1_uv2 = rt_p1_uv2 + uv2_interp * rt_w1;
			rt_p1_color = rt_p1_color + color_interp * rt_w1;
			/* RT_CUSTOM_VARYING_ACCUMULATE_1 */
			rt_p2_vertex = rt_p2_vertex + vertex * rt_w2;
			rt_p2_normal = rt_p2_normal + normal * rt_w2;
			rt_p2_tangent = rt_p2_tangent + tangent * rt_w2;
			rt_p2_binormal = rt_p2_binormal + binormal * rt_w2;
			rt_p2_uv = rt_p2_uv + uv_interp * rt_w2;
			rt_p2_uv2 = rt_p2_uv2 + uv2_interp * rt_w2;
			rt_p2_color = rt_p2_color + color_interp * rt_w2;
			/* RT_CUSTOM_VARYING_ACCUMULATE_2 */
#endif
		}
	}
	if (rt_removed) {
		RT_CUSTOM_REMOVED_POINT
	}
	rt_hit_bary = rt_bary;
	rt_set_fragment_inputs(0);
	// Ready only after the loop: a dFdx() in vertex() is 0, as raster's vertex() reads mip 0.
#ifdef RT_STAGE_ANY_HIT
	// The alpha test reads the textures' base level, as the opacity micromap is baked (raytracing_omm_bake.glsl): no
	// screen derivatives, so dFdx() is 0 and the material's texture() calls (textureGrad()) take level 0 whatever the
	// sampler's mip bias. A mip following the ray's footprint would disagree with the micromap's known states at a
	// distance (NVIDIA OMM SDK integration guide: "Pick a texture mip and always use that version").
	rt_derivatives_ready = false;
#else
	rt_derivatives_ready = true;
#endif
} else {
	// Procedural geometry has no vertices: vertex() runs once at the hit, without screen derivatives.
	/* RT_CUSTOM_VERTEX_CALL */
	if (any(isnan(vertex))) {
		RT_CUSTOM_REMOVED_POINT
	}
#if defined(SKIP_TRANSFORM_USED)
	// vertex() already wrote view-space values.
#elif defined(VERTEX_WORLD_COORDS_USED)
	vertex = (rt_view_matrix * vec4(vertex, 1.0)).xyz;
	normal = normalize(mat3(rt_view_matrix) * normal);
	tangent = normalize(mat3(rt_view_matrix) * tangent);
	binormal = normalize(mat3(rt_view_matrix) * binormal);
#else
	vertex = (rt_modelview_matrix * vec4(vertex, 1.0)).xyz;
	normal = normalize(rt_modelview_normal_matrix * normal);
	tangent = normalize(mat3(rt_modelview_matrix) * tangent);
	binormal = normalize(mat3(rt_modelview_matrix) * binormal);
#endif
}

// Fragment outputs with raster's defaults (scene_forward_clustered.glsl): a shader that leaves ROUGHNESS alone is rough.
vec3 albedo = vec3(1.0);
float alpha = 1.0;
float metallic = 0.0;
float roughness = 1.0;
float specular = 0.5;
vec3 emission = vec3(0.0);
vec3 normal_map = vec3(0.5, 0.5, 1.0);
float normal_map_depth = 1.0;
float ao = 1.0;
float ao_light_affect = 0.0;
vec3 backlight = vec3(0.0);
float sss_strength = 0.0;
float rim = 0.0;
float rim_tint = 0.0;
float clearcoat = 0.0;
float clearcoat_roughness = 0.0;
float anisotropy = 0.0;
vec2 anisotropy_flow = vec2(1.0, 0.0);
float alpha_scissor_threshold = 0.0;
float alpha_hash_scale = 1.0;
float alpha_antialiasing_edge = 0.0;
vec2 alpha_texture_coordinate = vec2(0.0);
// Outputs of raster's the path tracer has no use for, declared so that a shader writing them is still traced (it did not
// compile): premultiplied alpha and depth change raster's blending and depth buffer, the custom radiance and
// irradiance its ambient light, the transmittance its subsurface scattering, LIGHT_VERTEX the point its lights shade.
float premul_alpha = 1.0;
float rt_depth = 0.0;
vec4 custom_radiance = vec4(0.0);
vec4 custom_irradiance = vec4(0.0);
vec4 transmittance_color = vec4(0.0);
float transmittance_depth = 0.0;
float transmittance_boost = 0.0;
vec3 light_vertex = vertex;

{
	/* RT_CUSTOM_FRAGMENT_CODE */
}
if (rt_discarded) {
	RT_CUSTOM_REMOVED_POINT
}
