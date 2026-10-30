// Shared definitions for raytracing shaders (raygen, miss, closest_hit, any_hit).
// Must be included before scene_data_inc.glsl since it defines MAX_VIEWS.
#include "../oct_inc.glsl"

#include "../oct_inc.glsl"

#define MAX_VIEWS 2

#ifndef PI
#define PI 3.141592653589f
#endif

// ============================================================================
// RT_PARAMS INDICES - Must match RT_PARAM_* in scene_shader_raytracing.h
// ============================================================================
// rt_params is a vec4[4] uniform buffer (16 floats total)
// Access: rt_params[idx >> 2][idx & 3] or get_rt_param(idx)
#define RT_PARAM_VIS_MODE 0 // rt_params[0].x - Debug visualization mode (0 = disabled)
#define RT_PARAM_SAMPLE_COUNT 1 // rt_params[0].y - Samples per pixel
#define RT_PARAM_MAX_BOUNCES 2 // rt_params[0].z - Maximum ray bounces
#define RT_PARAM_DENOISER 3 // rt_params[0].w - Denoiser selection (0=none, 1=DLSS RR, 2=NRD ReBLUR)
#define RT_PARAM_FRONT_LAYER_COUNT 4 // rt_params[1].x - Transparent surfaces in the TLAS (front layer reflections)
#define RT_PARAM_DECAL_COUNT 5 // rt_params[1].y - Decals in the decal buffer (raytracing_decals_inc.glsl)
#define RT_PARAM_PROCEDURAL_MASKS 6 // rt_params[1].z - RT_MASK_PROCEDURAL_* bits held by this frame's TLAS
#define RT_PARAM_ENV_SAMPLING 7 // rt_params[1].w - 1 when the sky sampling distribution was built this frame (raytracing_environment_inc.glsl)
#define RT_PARAM_BACKGROUND_COLOR 8 // rt_params[2].xyz - Background color seen by camera rays (linear, with its energy)
#define RT_PARAM_BACKGROUND_COLOR_USED 11 // rt_params[2].w - 1 where the raster clears to the background color (no sky drawn)
#define RT_PARAM_REFLECTION_BACKGROUND 12 // rt_params[3].x - 1 when reflections show the background color (REFLECTION_SOURCE_BG)
#define RT_PARAM_FLAGS 13 // rt_params[3].y - RT_PARAM_FLAG_* bits, runtime switches (a later one takes the next bit)
#define RT_PARAM_FLAG_LIGHT_SAMPLING_RIS 1u // Bit 0: rendering/pathtracing/light_sampling_ris
#define RT_PARAM_FLAG_RUSSIAN_ROULETTE 2u // Bit 1: rendering/pathtracing/russian_roulette
#define RT_PARAM_LIGHT_COUNT 14 // rt_params[3].z - Number of active lights in light buffer
#define RT_PARAM_FRAME_INDEX 15 // rt_params[3].w - Frame counter for temporal variation

// ============================================================================
// PATHTRACING PAYLOAD: the hit's material (48 bytes, fp16/unorm-packed; 40 with PT_PAYLOAD_COMPACT)
// ============================================================================
// The closest hit evaluates the material and returns it here (hit_pack()); the raygen shades the hit and keeps the path's
// state (raytracing_shade_inc.glsl). NVIDIA, "Best Practices for Using NVIDIA RTX Ray Tracing (Updated)", Hit shading:
// "Consider moving common code outside of hit and miss shaders", and "Minimize live state across ray-trace calls": no hit
// shader traces a ray. In: packed_bounces_flags (the path's flags, read by the primary writes and the front layer) and
// hit_t (a front layer ray: the opaque surface's distance behind). Normals are 16-bit unorm octahedral pairs.

// PT_PAYLOAD_COMPACT: 40 bytes, no term below 10 bits: albedo fp16 and normals 16-bit octahedral as in the 48-byte layout,
// roughness / metalness / specular / AO / AO light affect 10-12-bit unorm (finer than the 8-bit textures they come from),
// emissive and backlight in the shared exponent format RGB9E5 (9-bit mantissas, the range of fp16), at most 2^22 geometries.
// Measured on 2026-10-25: no gain over 48 bytes in the forest (PT-SHADE-IN-RAYGEN-STUDY.md, 2d). Off.
//#define PT_PAYLOAD_COMPACT

#define HIT_FLAG_UNSHADED (1u << 28u)
#define HIT_FLAG_SPECULAR_DISABLED (1u << 29u)
#define HIT_FLAG_PASS_THROUGH (1u << 30u) // vertex() removed the point: the ray goes on from the hit, the path unchanged.
#define HIT_FLAG_DEBUG (1u << 31u) // A debug view: emissive is the pixel's color.
#define HIT_FLAGS_MASK 0xF0000000u

#ifdef PT_PAYLOAD_COMPACT

struct PathPayload {
	uint packed_bounces_flags; // [flags:8][environment MIS:16][diffuse:4][total:4]; a miss sets PATH_TERMINATED_FLAG.
	float hit_t;
	uint oct_geometry_normal; // World space, flipped for back-face hits.
	uint oct_normal; // The material's shading normal, world space.
	uint albedo_rg; // fp16 pair.
	uint albedo_b_ao_light_affect_flags; // fp16 albedo b, 12-bit AO light affect, HIT_FLAG_* in bits 28-31.
	uint roughness_metalness_specular; // unorm 10 / 11 / 11 bits.
	uint emissive; // RGB9E5. A miss: the light it brings, by the multiple importance sampling weight.
	uint backlight; // RGB9E5.
	uint geometry_idx_ao; // Bits 0-21: the geometry index; 22-31: AO (unorm10).
};

#define HIT_GEOMETRY_IDX_MASK 0x003FFFFFu

uint pack_unorm_bits(float p_value, uint p_bits) {
	return uint(round(clamp(p_value, 0.0, 1.0) * float((1u << p_bits) - 1u)));
}

float unpack_unorm_bits(uint p_value, uint p_bits) {
	return float(p_value & ((1u << p_bits) - 1u)) / float((1u << p_bits) - 1u);
}

// The shared exponent format of GL_EXT_texture_shared_exponent (N = 9, B = 15), as the extension's specification encodes it.
uint pack_rgb9e5(vec3 p_c) {
	const vec3 c = clamp(p_c, vec3(0.0), vec3(65408.0));
	const float max_c = max(max(c.r, c.g), max(c.b, 1e-30));
	int exp_shared = max(-16, int(floor(log2(max_c)))) + 1 + 15;
	if (floor(max_c / exp2(float(exp_shared - 15 - 9)) + 0.5) >= 512.0) {
		exp_shared++;
	}
	const uvec3 m = uvec3(min(floor(c / exp2(float(exp_shared - 15 - 9)) + 0.5), vec3(511.0)));
	return m.r | (m.g << 9u) | (m.b << 18u) | (uint(exp_shared) << 27u);
}

vec3 unpack_rgb9e5(uint p_v) {
	return vec3(uvec3(p_v & 0x1FFu, (p_v >> 9u) & 0x1FFu, (p_v >> 18u) & 0x1FFu)) * exp2(float(int(p_v >> 27u) - 15 - 9));
}

#else

struct PathPayload {
	uint packed_bounces_flags; // [flags:8][environment MIS:16][diffuse:4][total:4]; a miss sets PATH_TERMINATED_FLAG.
	float hit_t;
	uint oct_geometry_normal; // World space, flipped for back-face hits.
	uint oct_normal; // The material's shading normal, world space.
	uint albedo_rg; // fp16 pairs from here.
	uint albedo_b_roughness;
	uint emissive_rg; // A miss: the light it brings, by the multiple importance sampling weight.
	uint emissive_b_backlight_r;
	uint backlight_gb;
	uint metalness_specular; // unorm16 pairs.
	uint ao_ao_light_affect;
	uint geometry_idx_flags; // Bits 0-27: the geometry index; 28-31: HIT_FLAG_*.
};

#define HIT_GEOMETRY_IDX_MASK 0x0FFFFFFFu

#endif

/// The payload unpacked, in the raygen.
struct HitRecord {
	float hit_t;
	vec3 geometry_normal;
	vec3 normal;
	vec3 albedo;
	float roughness;
	float metalness;
	float specular;
	vec3 emissive;
	vec3 backlight;
	float ao;
	float ao_light_affect;
	uint geometry_idx;
	uint flags;
};

/// fp16 with the range clamped, as the radiance always was.
uint pack_half2_clamped(vec2 p_value) {
	return packHalf2x16(clamp(p_value, vec2(-65504.0), vec2(65504.0)));
}

/// What the closest hit returns (hit_t aside).
void payload_write_material(inout PathPayload p, vec3 p_geometry_normal, vec3 p_normal, vec3 p_albedo, float p_roughness, float p_metalness, float p_specular, vec3 p_emissive, vec3 p_backlight, float p_ao, float p_ao_light_affect, uint p_geometry_idx, uint p_flags) {
#ifdef PT_PAYLOAD_COMPACT
	p.oct_geometry_normal = packUnorm2x16(vec3_to_oct(p_geometry_normal));
	p.oct_normal = packUnorm2x16(vec3_to_oct(p_normal));
	p.albedo_rg = pack_half2_clamped(p_albedo.rg);
	p.albedo_b_ao_light_affect_flags = (pack_half2_clamped(vec2(p_albedo.b, 0.0)) & 0xFFFFu) | (pack_unorm_bits(p_ao_light_affect, 12u) << 16u) | p_flags;
	p.roughness_metalness_specular = pack_unorm_bits(p_roughness, 10u) | (pack_unorm_bits(p_metalness, 11u) << 10u) | (pack_unorm_bits(p_specular, 11u) << 21u);
	p.emissive = pack_rgb9e5(p_emissive);
	p.backlight = pack_rgb9e5(p_backlight);
	p.geometry_idx_ao = (p_geometry_idx & HIT_GEOMETRY_IDX_MASK) | (pack_unorm_bits(p_ao, 10u) << 22u);
#else
	p.oct_geometry_normal = packUnorm2x16(vec3_to_oct(p_geometry_normal));
	p.oct_normal = packUnorm2x16(vec3_to_oct(p_normal));
	p.albedo_rg = pack_half2_clamped(p_albedo.rg);
	p.albedo_b_roughness = pack_half2_clamped(vec2(p_albedo.b, p_roughness));
	p.emissive_rg = pack_half2_clamped(p_emissive.rg);
	p.emissive_b_backlight_r = pack_half2_clamped(vec2(p_emissive.b, p_backlight.r));
	p.backlight_gb = pack_half2_clamped(p_backlight.gb);
	p.metalness_specular = packUnorm2x16(vec2(p_metalness, p_specular));
	p.ao_ao_light_affect = packUnorm2x16(vec2(p_ao, p_ao_light_affect));
	p.geometry_idx_flags = (p_geometry_idx & HIT_GEOMETRY_IDX_MASK) | p_flags;
#endif
}

/// A miss or a debug view: only the light (the color) and the flags.
void payload_write_light(inout PathPayload p, vec3 p_light, uint p_flags) {
#ifdef PT_PAYLOAD_COMPACT
	p.emissive = pack_rgb9e5(p_light);
	p.albedo_b_ao_light_affect_flags = p_flags;
#else
	p.emissive_rg = pack_half2_clamped(p_light.rg);
	p.emissive_b_backlight_r = pack_half2_clamped(vec2(p_light.b, 0.0));
	p.geometry_idx_flags = p_flags;
#endif
}

HitRecord hit_unpack(PathPayload p) {
	HitRecord r;
	r.hit_t = p.hit_t;
#ifdef PT_PAYLOAD_COMPACT
	r.geometry_normal = oct_to_vec3(unpackUnorm2x16(p.oct_geometry_normal) * 2.0 - 1.0);
	r.normal = oct_to_vec3(unpackUnorm2x16(p.oct_normal) * 2.0 - 1.0);
	r.albedo = vec3(unpackHalf2x16(p.albedo_rg), unpackHalf2x16(p.albedo_b_ao_light_affect_flags & 0xFFFFu).x);
	r.ao_light_affect = unpack_unorm_bits(p.albedo_b_ao_light_affect_flags >> 16u, 12u);
	r.roughness = unpack_unorm_bits(p.roughness_metalness_specular, 10u);
	r.metalness = unpack_unorm_bits(p.roughness_metalness_specular >> 10u, 11u);
	r.specular = unpack_unorm_bits(p.roughness_metalness_specular >> 21u, 11u);
	r.emissive = unpack_rgb9e5(p.emissive);
	r.backlight = unpack_rgb9e5(p.backlight);
	r.ao = unpack_unorm_bits(p.geometry_idx_ao >> 22u, 10u);
	r.geometry_idx = p.geometry_idx_ao & HIT_GEOMETRY_IDX_MASK;
	r.flags = p.albedo_b_ao_light_affect_flags & HIT_FLAGS_MASK;
#else
	r.geometry_normal = oct_to_vec3(unpackUnorm2x16(p.oct_geometry_normal) * 2.0 - 1.0);
	r.normal = oct_to_vec3(unpackUnorm2x16(p.oct_normal) * 2.0 - 1.0);
	const vec2 rg = unpackHalf2x16(p.albedo_rg);
	const vec2 b_roughness = unpackHalf2x16(p.albedo_b_roughness);
	r.albedo = vec3(rg, b_roughness.x);
	r.roughness = b_roughness.y;
	const vec2 e_rg = unpackHalf2x16(p.emissive_rg);
	const vec2 e_b_bl_r = unpackHalf2x16(p.emissive_b_backlight_r);
	r.emissive = vec3(e_rg, e_b_bl_r.x);
	r.backlight = vec3(e_b_bl_r.y, unpackHalf2x16(p.backlight_gb));
	const vec2 ms = unpackUnorm2x16(p.metalness_specular);
	r.metalness = ms.x;
	r.specular = ms.y;
	const vec2 ao = unpackUnorm2x16(p.ao_ao_light_affect);
	r.ao = ao.x;
	r.ao_light_affect = ao.y;
	r.geometry_idx = p.geometry_idx_flags & HIT_GEOMETRY_IDX_MASK;
	r.flags = p.geometry_idx_flags & HIT_FLAGS_MASK;
#endif
	return r;
}

/// Converts specular parameter [0..1] to dielectric F0.
float specular_to_f0(float specular) {
	return 0.16 * specular * specular;
}

/// The raygen's path state (fp32).
struct PathState {
	vec3 radiance;
	vec3 throughput;
	uint packed_bounces_flags;
	uint rng_state;
#ifdef NRD_ENABLED
	// A camera path's light split for NRD (shade_and_bounce()): what it gathered up to its primary hit's own emission (not
	// denoised), the direct light the hit's diffuse and specular lobes reflect, the hit distance of the ray its bounce
	// traced, and NRD_PATH_* with the lobe that bounce took. The rest of the path's light is that lobe's indirect light.
	vec3 nrd_emission;
	vec3 nrd_diffuse_direct;
	vec3 nrd_specular_direct;
	float nrd_hit_t;
	uint nrd_state;
#endif
};

#ifdef NRD_ENABLED
#define NRD_PATH_NONE 0u // Not a camera path: nothing to split.
#define NRD_PATH_PRIMARY 1u // A camera path before its primary hit is shaded.
#define NRD_PATH_SHADED 2u // Its primary hit shaded, no bounce (the path ends there).
#define NRD_PATH_BOUNCE 3u // Its bounce's ray leaves along the lobe, its hit distance still to come.
#define NRD_PATH_DONE 4u // The bounce's hit distance is known.
#define NRD_PATH_STAGE_MASK 0xFu
#define NRD_PATH_SPECULAR 0x10u // The bounce took the specular lobe.
// The hit distance of a ray that leaves the scene: NRD's NRD_FP16_MAX (NRD.hlsli, "distance to occluder").
#define NRD_MISS_HIT_T 65504.0
// The floor of the albedo guides each lobe's light is divided by, and multiplied back by (raytracing_nrd_composite.glsl).
#define NRD_ALBEDO_MIN 0.001

void nrd_path_init(inout PathState ps, bool p_camera_path) {
	ps.nrd_emission = vec3(0.0);
	ps.nrd_diffuse_direct = vec3(0.0);
	ps.nrd_specular_direct = vec3(0.0);
	ps.nrd_hit_t = 0.0;
	ps.nrd_state = p_camera_path ? NRD_PATH_PRIMARY : NRD_PATH_NONE;
}

uint nrd_path_stage(PathState ps) {
	return ps.nrd_state & NRD_PATH_STAGE_MASK;
}

// The threshold the primary hit's lobe choice compares its probability with: a 4x4 Bayer matrix, its values shifted each
// frame. NRD's HitDistanceReconstructionMode::AREA_3X3 rebuilds the skipped lobe's hit distance from a 3x3 area, which must
// hold a sample of each lobe: NRDSettings.h asks for a probability clamped to [1/4, 3/4] and "White noise must be
// replaced with Bayer dithering".
float nrd_lobe_threshold(uvec2 p_pixel, uint p_frame) {
	const uint bayer[16] = uint[16](0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u);
	const float value = (float(bayer[(p_pixel.y & 3u) * 4u + (p_pixel.x & 3u)]) + 0.5) / 16.0;
	return fract(value + float(p_frame) * 0.61803398875);
}
#endif

// Bounce count helpers (bits 0-3: total, bits 4-7: diffuse). A path ends at RT_MAX_BOUNCES (8) and MAX_DIFFUSE_BOUNCES.
uint get_total_bounces(uint packed) {
	return packed & 0xFu;
}
uint get_diffuse_bounces(uint packed) {
	return (packed >> 4u) & 0xFu;
}
uint pack_bounces(uint total, uint diffuse) {
	return total | (diffuse << 4u);
}
uint inc_total_bounce(uint packed) {
	return packed + 1u;
}
uint inc_diffuse_bounce(uint packed) {
	return packed + 0x11u;
} // +1 to both total and diffuse

// Bits 8-23: the share of the Environment's light that next event estimation of the sky took for this ray's direction,
// in 1/65535 (raytracing_environment_inc.glsl). A miss adds the rest, so the two estimators' weights sum to one for every
// direction (multiple importance sampling). 0, the default of every ray no bounce sampled with it, keeps all of it.
const uint ENV_MIS_SHIFT = 8u;
const uint ENV_MIS_MASK = 0xFFFFu << ENV_MIS_SHIFT;
uint set_env_mis_share(uint packed, uint share) {
	return (packed & ~ENV_MIS_MASK) | (min(share, 0xFFFFu) << ENV_MIS_SHIFT);
}
// The weight of the light a miss takes.
float get_env_mis_weight(uint packed) {
	return 1.0 - float((packed & ENV_MIS_MASK) >> ENV_MIS_SHIFT) / 65535.0;
}

// Sample 0 flag (bit 24) - only write DLSS RR outputs on first sample
const uint SAMPLE_ZERO_FLAG = (1u << 24);
uint set_sample_zero(uint packed) {
	return packed | SAMPLE_ZERO_FLAG;
}
bool is_sample_zero(uint packed) {
	return (packed & SAMPLE_ZERO_FLAG) != 0u;
}

// Shadow ray flag (bit 25) - indicates this ray is a shadow/occlusion test
const uint SHADOW_RAY_FLAG = (1u << 25);
uint set_shadow_ray(uint packed) {
	return packed | SHADOW_RAY_FLAG;
}
bool is_shadow_ray(uint packed) {
	return (packed & SHADOW_RAY_FLAG) != 0u;
}

// Front layer ray flag (bit 27): the closest hit returns the transparent surface's reflection direction, unshaded.
const uint FRONT_LAYER_RAY_FLAG = (1u << 27);
uint set_front_layer_ray(uint packed) {
	return packed | FRONT_LAYER_RAY_FLAG;
}
bool is_front_layer_ray(uint packed) {
	return (packed & FRONT_LAYER_RAY_FLAG) != 0u;
}

// Specular lobe flag (bit 28): the ray left its last surface along the specular lobe, not the diffuse one. A miss then
// takes the Environment's reflected light, else its ambient light, as raster splits them (scene_raytracing_raygen.glsl).
const uint SPECULAR_LOBE_FLAG = (1u << 28);
uint set_specular_lobe(uint packed, bool specular) {
	return specular ? (packed | SPECULAR_LOBE_FLAG) : (packed & ~SPECULAR_LOBE_FLAG);
}
bool is_specular_lobe(uint packed) {
	return (packed & SPECULAR_LOBE_FLAG) != 0u;
}

// Unextended flag (bit 29): the raygen traces no ray after this one's hit (the front layer's reflection path ends there),
// so the hit's next event estimation of the sky, which shares the light with the BRDF ray it would trace, is skipped.
const uint UNEXTENDED_FLAG = (1u << 29);
uint set_unextended(uint packed) {
	return packed | UNEXTENDED_FLAG;
}
bool is_unextended(uint packed) {
	return (packed & UNEXTENDED_FLAG) != 0u;
}

const uint PATH_TERMINATED_FLAG = (1u << 26);
uint set_path_terminated(uint packed) {
	return packed | PATH_TERMINATED_FLAG;
}
uint clear_path_terminated(uint packed) {
	return packed & ~PATH_TERMINATED_FLAG;
}
bool is_path_terminated(uint packed) {
	return (packed & PATH_TERMINATED_FLAG) != 0u;
}

// Bounce limits
#define MAX_DIFFUSE_BOUNCES 2u
#define MAX_DENOISER_SPECULAR_HIT_THRESHOLD 0.25

// ============================================================================
// CONSTANTS
// ============================================================================
#include "raytracing_geometry_consts_inc.glsl"

// ============================================================================
// RANDOM NUMBER GENERATION - PCG (Permuted Congruential Generator)
// ============================================================================

/// PCG's output permutation of a 32-bit state (O'Neill 2014, pcg-c's pcg_output_rxs_m_xs_32_32).
uint pcg_output_rxs_m_xs(uint state) {
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

/// pcg4d (Jarzynski and Olano 2020, "Hash Functions for GPU Rendering", JCGT 9(3), section 5): four inputs hashed
/// together. Their section 4.1 shows that hashing a linear combination of the inputs instead (pixel + 65536 * y +
/// 1000000 * frame...) destroys the hash's quality, with repeating patterns.
uvec4 pcg4d(uvec4 v) {
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	v ^= v >> 16u;
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	return v;
}

/// The random sequence of one path sample: its first state, from the pixel, the frame and the sample.
uint init_rng(uvec2 pixel, uint frame, uint sample_idx) {
	return pcg4d(uvec4(pixel, frame, sample_idx)).x;
}

/// Random float in [0, 1): PCG with a 32-bit state (pcg-c's pcg_oneseq_32_rxs_m_xs_32_random_r): the state advances by
/// its linear congruential step, and the number is the output permutation of the state before the step. The top 24 bits
/// make the float, exactly, never 1.0.
float rand(inout uint state) {
	uint old_state = state;
	state = state * 747796405u + 2891336453u;
	return float(pcg_output_rxs_m_xs(old_state) >> 8u) * (1.0 / 16777216.0);
}

/// Random vec2 in [0, 1)
vec2 rand2(inout uint state) {
	return vec2(rand(state), rand(state));
}

// ============================================================================
// RAY ORIGIN OFFSET (prevents self-intersection)
// Wachter-Binder method: offsets in integer float representation so the
// displacement scales naturally with position magnitude.
// ============================================================================
float _offset_component(float p, float n_comp, int of_comp) {
	const float origin = 1.0 / 32.0;
	const float float_scale = 1.0 / 65536.0;
	int shifted = floatBitsToInt(p) + ((p >= 0.0) ? of_comp : -of_comp);
	float p_i = intBitsToFloat(shifted);
	return (abs(p) < origin) ? (p + float_scale * n_comp) : p_i;
}

vec3 offset_ray_origin(vec3 p, vec3 n) {
	const float int_scale = 256.0;
	ivec3 of = ivec3(int_scale * n);
	return vec3(
			_offset_component(p.x, n.x, of.x),
			_offset_component(p.y, n.y, of.y),
			_offset_component(p.z, n.z, of.z));
}
