// Shared defines and common bindings for all RT shader stages.
// Include AFTER raytracing_inc.glsl and scene_data_inc.glsl.
// The includer must set exactly one of RT_STAGE_{RAYGEN,MISS,CLOSEST_HIT,ANY_HIT,INTERSECTION}.

// Specialization constant: the flags that change the code (SceneShaderRaytracing::RaytracingFlags), one pipeline each.
layout(constant_id = 0) const uint RT_FLAGS = 0u;

#define RT_FLAG_DLSS_RR_ENABLED (1u << 1)

// The Environment's fog, from the scene data as raster reads it: uniform across the dispatch, so switching it keeps the
// pipeline and its hit groups.
#define RT_FOG_ENABLED() bool(scene_data_block.data.flags & SCENE_DATA_FLAGS_USE_FOG)

// From the parameters buffer (not in the any-hit stage): changing them does not build another pipeline.
#define RT_GET_SAMPLE_COUNT() max(1u, uint(get_rt_param(RT_PARAM_SAMPLE_COUNT)))
#define RT_GET_MAX_BOUNCES() clamp(uint(get_rt_param(RT_PARAM_MAX_BOUNCES)), 1u, 8u)

// Cull back faces by default; double-sided instances override via CULL_DISABLE flag.
#define RT_RAY_FLAGS gl_RayFlagsCullBackFacingTrianglesEXT
// A shadow ray goes toward the light: the faces raster's shadow pass draws, turned to the light, are hit from behind.
#define RT_SHADOW_RAY_FLAGS gl_RayFlagsCullFrontFacingTrianglesEXT

// TLAS instance masks (RT_INSTANCE_MASK_* in render_raytracing.h).
#define RT_MASK_CAMERA 0x01
#define RT_MASK_SHADOW 0x02
#define RT_MASK_FRONT_LAYER 0x04
// Procedural geometry only, next to SHADOW and CAMERA: what a ray query loop traces apart (RT_PARAM_PROCEDURAL_MASKS).
#define RT_MASK_PROCEDURAL_SHADOW 0x08
#define RT_MASK_PROCEDURAL_CAMERA 0x10
// Seen by every ray but the camera's (render_mode pathtracing_rasterized: drawn by the raster transparent pass instead).
#define RT_MASK_SECONDARY 0x20
// What a ray from a surface sees: the camera's instances and the ones only bounces see.
#define RT_MASK_BOUNCE (RT_MASK_CAMERA | RT_MASK_SECONDARY)

layout(set = 0, binding = 2, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

layout(set = 0, binding = 14, std430) readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;

#ifndef RT_STAGE_ANY_HIT

layout(set = 0, binding = 6, std140) uniform RaytracingParams {
	vec4 rt_params[4];
	mat4 prev_vp_unjittered;
	mat4 curr_vp_unjittered;
};

float get_rt_param(uint idx) {
	return rt_params[idx >> 2u][idx & 3u];
}

/// Project a world-space point to UV through an unjittered VP.
vec2 project_uv(vec3 world_pos, mat4 vp) {
	vec4 clip = vp * vec4(world_pos, 1.0);
	return clip.xy / clip.w * 0.5 + 0.5;
}

// The guides of the primary surfaces: DLSS Ray Reconstruction's, which NRD (NRD_ENABLED) also takes: its normal and
// roughness, and the albedos the diffuse and specular light were divided by (raytracing_nrd_composite.glsl).
#if defined(DLSS_RR_ENABLED) || defined(NRD_ENABLED)
layout(set = 0, binding = 9, rgba16f) uniform image2D dlss_rr_diffuse_albedo;
layout(set = 0, binding = 10, rgba16f) uniform image2D dlss_rr_specular_albedo;
layout(set = 0, binding = 11, rgba16f) uniform image2D dlss_rr_normal_roughness;
#endif

#ifdef DLSS_RR_ENABLED
layout(set = 0, binding = 12, r16f) uniform image2D dlss_rr_specular_hit_dist;
// NDC depth for Ray Reconstruction (RenderRaytracing::dlss_rr_get_depth()), written by the raygen.
layout(set = 0, binding = 39, r32f) uniform image2D dlss_rr_depth;
#endif

// Binding 14 is reserved for GlobalShaderUniformData (declared above).
// Samplers occupy 16-27 (see raytracing_samplers_inc.glsl); velocity sits at
// the first free slot past them so we do not collide with either.
layout(set = 0, binding = 28, rg16f) uniform image2D rt_velocity_image;
layout(set = 0, binding = 15, r32f) uniform image2D rt_depth_image;

#endif // !RT_STAGE_ANY_HIT

// Hit attributes: a triangle's barycentrics (BuiltInTriangleIntersectionAttributes, required in triangle hit groups by
// the DXR spec, "Intersection attributes structure"), or with ENABLE_INTERSECTION_SHADERS, which only procedural hit
// groups' shaders get, the structure their intersection shader reports. Every shader of a hit group uses the same one.
#if defined(RT_STAGE_CLOSEST_HIT) || defined(RT_STAGE_ANY_HIT) || defined(RT_STAGE_INTERSECTION)
struct HitAttribs {
	vec2 bary_or_uv;
#ifdef ENABLE_INTERSECTION_SHADERS
	uint packed_normal; // xyz: snorm8 normal,  w: delta.x FP16 low byte.
	uint packed_tangent; // xyz: snorm8 tangent, w: delta.x FP16 high byte.
	uint prev_pos_delta_yz; // packHalf2x16(delta.y, delta.z).
#endif
};
hitAttributeEXT HitAttribs hit_attribs;
#endif
