#[compute]

#version 450

#VERSION_DEFINES

#define GLSL 1
#include "../raytracing/brdf_inc.glsl"

// RendererRD::PTRasterizedGuides: DLSS-RR's guides where the path tracer's raster pass drew a pathtracing_rasterized card.
// The path tracer wrote the guides of what is behind it (its camera rays leave the cards out). Read here: the cards'
// material pass (albedo, view normal, roughness and metallic, as SDFGI's material pass writes them) and the depth buffer
// that holds their depth. Written, as the raygen writes them for a surface: diffuse and specular albedo, world normal and
// roughness, specular hit distance, depth.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D albedo_texture; // RGB albedo, A coverage (0 where no card).
layout(set = 0, binding = 1) uniform sampler2D normal_texture; // encode24(view normal) * 0.5 + 0.5.
layout(set = 0, binding = 2) uniform sampler2D orm_texture; // ao, roughness, metallic.
layout(set = 0, binding = 3) uniform sampler2D depth_texture; // The depth buffer: NDC depth, the cards' included, cut out.
layout(set = 0, binding = 9) uniform sampler2D view_depth_texture; // The material pass' nearest card: its view depth.

layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D diffuse_albedo;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D specular_albedo;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D normal_roughness;
layout(r16f, set = 0, binding = 7) uniform restrict writeonly image2D specular_hit_dist;
layout(r32f, set = 0, binding = 8) uniform restrict writeonly image2D rr_depth;

layout(push_constant, std430) uniform Params {
	mat4 inv_projection;
	vec4 inv_view_rows[3]; // The camera's rotation, view to world, as three rows.
	ivec2 size;
	ivec2 pad;
}
params;

// The raygen's dielectric reflectance of Godot's default specular (0.5): 0.16 * specular^2.
#define DEFAULT_DIELECTRIC_F0 0.04

void main() {
	const ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	const vec4 albedo = texelFetch(albedo_texture, pos, 0);
	if (albedo.a <= 0.0) {
		return; // Not a card's pixel: the path tracer's guides stay.
	}

	const vec3 view_normal = normalize(texelFetch(normal_texture, pos, 0).xyz * 2.0 - 1.0);
	const vec3 orm = texelFetch(orm_texture, pos, 0).rgb;
	const float roughness = orm.g;
	const float metallic = orm.b;
	const float depth = texelFetch(depth_texture, pos, 0).r;

	const vec2 ndc_xy = (vec2(pos) + 0.5) / vec2(params.size) * 2.0 - 1.0;
	const vec4 view_h = params.inv_projection * vec4(ndc_xy, depth, 1.0);
	const vec3 view_pos = view_h.xyz / view_h.w;
	// The card is seen here only where the scene's depth is its own: elsewhere the depth pass cut it out (alpha scissor) or
	// something nearer hides it.
	const float card_view_depth = texelFetch(view_depth_texture, pos, 0).r;
	if (abs(card_view_depth - (-view_pos.z)) > 1e-3 * card_view_depth + 1e-4) {
		return;
	}
	const float NdotV = max(dot(view_normal, normalize(-view_pos)), 0.0001);

	const mat3 inv_view_basis = transpose(mat3(params.inv_view_rows[0].xyz, params.inv_view_rows[1].xyz, params.inv_view_rows[2].xyz));
	const vec3 world_normal = normalize(inv_view_basis * view_normal);

	imageStore(diffuse_albedo, pos, vec4(DLSSRR_encodeDiffuseAlbedo(DLSSRR_computeDiffuseAlbedo(albedo.rgb, metallic)), 1.0));
	imageStore(specular_albedo, pos, vec4(clamp(DLSSRR_computeSpecularAlbedo(albedo.rgb, metallic, DEFAULT_DIELECTRIC_F0, roughness, NdotV), vec3(0.04), vec3(1.0)), 1.0));
	imageStore(normal_roughness, pos, vec4(world_normal, roughness));
	// No reflection ray is traced from a card: as the raygen writes for a rough surface.
	imageStore(specular_hit_dist, pos, vec4(-1.0));
	imageStore(rr_depth, pos, vec4(depth));
}
