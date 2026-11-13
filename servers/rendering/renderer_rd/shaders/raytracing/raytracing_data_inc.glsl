// Shared data structures for raytracing shaders.
// Requires: GL_EXT_buffer_reference, GL_ARB_gpu_shader_int64, raytracing_inc.glsl (for OFFSET_NONE/FLAG_*)

// ============================================================================
// BUFFER REFERENCES
// ============================================================================
layout(buffer_reference, std430) readonly buffer FloatBuffer {
	float v[];
};
layout(buffer_reference, std430) readonly buffer Uint32Buffer {
	uint v[];
};

// ============================================================================
// GEOMETRY DATA (matches C++ RT_GeometryData, 176 bytes)
// ============================================================================
struct GeometryData {
	uint64_t vertex_address;
	uint64_t attribute_address;
	uint64_t index_address;

	uint vertex_count;
	uint position_stride;
	uint normal_byte_offset;
	uint normal_stride;
	uint tangent_byte_offset;
	uint tangent_stride;

	uint attribute_stride;
	uint uv_byte_offset;
	uint uv_scale_packed; // fp16 x, fp16 y
	uint index_format;
	uint primitive_count;
	uint flags;
	float aabb_size_x;
	float aabb_size_y;
	float aabb_size_z;

	// Byte offset of the vertex color attribute inside `attribute_stride`,
	// or OFFSET_NONE if the mesh has no vertex colors.
	uint color_byte_offset;

	float aabb_pos_x;
	float aabb_pos_y;
	float aabb_pos_z;

	// For deformed geometry: previous-frame position buffer used for motion vectors.
	uint prev_vertex_address_lo;
	uint prev_vertex_address_hi;

	// For displaced geometry: positions before vertex() moved them (float3 packed).
	uint rest_vertex_address_lo;
	uint rest_vertex_address_hi;

	uint layer_mask; // The instance's layers, against each light's cull mask.
	uint uv2_byte_offset;
	uint uv2_scale_packed; // fp16 x, fp16 y

	// CUSTOM0-3: byte offset in an attribute (8 bits each, CUSTOM0 lowest), and 4 bits each of CUSTOM_FORMAT_PRESENT
	// | the mesh's custom format. 0: none.
	uint custom_byte_offsets;
	uint custom_formats;

	// The MultiMesh buffer whose instances this geometry draws, for COLOR and INSTANCE_CUSTOM (0: not a MultiMesh).
	uint multimesh_address_lo;
	uint multimesh_address_hi;
	uint multimesh_stride; // In floats.
	uint multimesh_offset; // The geometry's first instance in the buffer.
	uint multimesh_vertex_count; // A merged BLAS's vertices per instance; 0xFFFFFFFF: one instance, at multimesh_offset.
	uint multimesh_data_offsets; // In floats in an instance: the color (low 16 bits), the custom data (high), 0xFFFF: none.
	// A merged BLAS's first instance in the buffer's previous transforms (motion vectors); multimesh_offset when its instances
	// did not move, and for one instance, whose motion is the TLAS instance's.
	uint multimesh_previous_offset;
	uint multimesh_pad0; // RT_GeometryData::debug_state: Environment.RT_DEBUG_DYNAMIC_GEOMETRY's state, 0 otherwise.
	// rendering/visibility_range/dithered_fade: 0, or VISIBILITY_FADE_DITHERED | the fade (8 bits, as the rasterizer's
	// instance flags hold it) of an instance fading through its visibility range, which the camera rays keep or drop
	// against a noise threshold (visibility_fade_drops(), raytracing_shade_inc.glsl).
	uint visibility_fade;
	uint multimesh_pad2;
};

void get_aabb_compression_xforms(GeometryData geom, out mat4 aabb_xform, out mat4 inv_aabb_xform) {
	if ((geom.flags & FLAG_COMPRESSED) == 0u) {
		aabb_xform = mat4(1.0);
		inv_aabb_xform = mat4(1.0);
		return;
	}
	vec3 aabb_sz = vec3(geom.aabb_size_x, geom.aabb_size_y, geom.aabb_size_z);
	vec3 aabb_po = vec3(geom.aabb_pos_x, geom.aabb_pos_y, geom.aabb_pos_z);
	aabb_xform = mat4(
			vec4(aabb_sz.x, 0.0, 0.0, 0.0),
			vec4(0.0, aabb_sz.y, 0.0, 0.0),
			vec4(0.0, 0.0, aabb_sz.z, 0.0),
			vec4(aabb_po, 1.0));
	vec3 inv_sz = 1.0 / aabb_sz;
	inv_aabb_xform = mat4(
			vec4(inv_sz.x, 0.0, 0.0, 0.0),
			vec4(0.0, inv_sz.y, 0.0, 0.0),
			vec4(0.0, 0.0, inv_sz.z, 0.0),
			vec4(-aabb_po * inv_sz, 1.0));
}

// ============================================================================
// PER-INSTANCE MOTION DATA (matches C++ RT_InstanceMotionData, 48 bytes)
// ============================================================================
struct InstanceMotionData {
	float prev_xform[12]; // Previous object-to-world (mat3x4, transposed 3x4)
};

// ============================================================================
// MATERIAL DATA (matches C++ layout, 96 bytes)
// ============================================================================
struct MaterialData {
	uint albedo_texture_idx;
	uint normal_texture_idx;
	uint orm_texture_idx;
	uint emission_texture_idx;

	vec4 albedo_color;
	vec3 emission_color;
	float emission_strength;

	float metallic;
	float roughness;
	float emission_indirect; // BaseMaterial3D.emission_indirect_energy: the emission's weight after a diffuse bounce.
	uint flags; // Bit 0: has_normal_map, Bit 1: has_emission

	vec2 uv1_scale; // UV1 scale (default 1,1)
	vec2 uv1_offset; // UV1 offset (default 0,0)

	float normal_map_depth; // Normal map strength (default 1.0)
	float specular; // Dielectric specular [0..1], default 0.5 -> F0 = 0.04.
	uint64_t uniform_address; // BDA for custom shader uniform buffer (0 = none)

	float uv1_scale_z; // Triplanar only.
	float uv1_offset_z;
	float uv1_blend_sharpness;
	uint instance_uniforms_ofs; // The instance's `instance uniform` values in the global shader uniform buffer.
	vec3 backlight; // BaseMaterial3D's backlight color (MAT_FLAG_BACKLIGHT).
	uint backlight_texture_idx; // Added to it (MAT_FLAG_HAS_BACKLIGHT_TEX).
	uint ao_texture_idx; // BaseMaterial3D's ambient occlusion texture (MAT_FLAG_AO_TEXTURE).
	uint ao_channel; // Its channel, 0-3.
	float ao_light_affect; // With MAT_FLAG_AO_TEXTURE or MAT_FLAG_AO_ORM.
	uint metallic_texture_idx; // BaseMaterial3D's metallic texture, or its ORM texture (material_roughness_metallic()).
};

#define MAT_FLAG_UV1_TRIPLANAR 8u
#define MAT_FLAG_UV1_WORLD_TRIPLANAR 16u
#define MAT_FLAG_EMISSION 32u
#define MAT_FLAG_EMISSION_ADD 64u
#define MAT_FLAG_UNSHADED 128u
#define MAT_FLAG_VERTEX_COLOR_ALBEDO 256u
#define MAT_FLAG_VERTEX_COLOR_SRGB 512u
#define MAT_FLAG_SPECULAR_DISABLED 1024u
#define MAT_FLAG_BACKLIGHT 2048u
#define MAT_FLAG_HAS_BACKLIGHT_TEX 4096u
#define MAT_FLAG_ANISOTROPIC_FILTER 8192u
#define MAT_FLAG_AO_TEXTURE 16384u
#define MAT_FLAG_AO_ORM 32768u
// The channels of the roughness (orm_texture_idx) and metallic (metallic_texture_idx) textures, 3 bits each:
// 0-3 red to alpha, 4 the mean of red, green and blue (RTTextureChannel).
#define MAT_ROUGHNESS_CHANNEL_SHIFT 16u
#define MAT_METALLIC_CHANNEL_SHIFT 19u
// The material's diffuse (GODOT_DIFFUSE_*) and specular (GODOT_SPECULAR_*) modes, 2 bits each, read by the
// rendering/pathtracing/raster_brdf variant (RT_RASTER_BRDF): RT_MAT_DIFFUSE_MODE_SHIFT / RT_MAT_SPECULAR_MODE_SHIFT.
#define MAT_DIFFUSE_MODE_SHIFT 28u
#define MAT_SPECULAR_MODE_SHIFT 30u
#define MAT_SHADING_MODE_MASK 3u

/// A texel's channel, as BaseMaterial3D's dot(texel, channel mask) reads it.
float material_texture_channel(vec4 p_texel, uint p_channel) {
	if (p_channel == 4u) {
		return dot(p_texel.rgb, vec3(0.333333));
	}
	return p_channel == 0u ? p_texel.r : (p_channel == 1u ? p_texel.g : (p_channel == 2u ? p_texel.b : p_texel.a));
}

/// BaseMaterial3D's vertex color, as its albedo_tex *= COLOR reads it.
vec4 material_vertex_color(vec4 p_color, uint p_flags) {
	if ((p_flags & MAT_FLAG_VERTEX_COLOR_SRGB) != 0u) {
		p_color.rgb = mix(pow((p_color.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)), p_color.rgb * (1.0 / 12.92), lessThan(p_color.rgb, vec3(0.04045)));
	}
	return p_color;
}
