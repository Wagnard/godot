// GeometryData's constants (raytracing_data_inc.glsl), apart for the raster shaders that read the path tracer's geometry.

const uint OFFSET_NONE = 0xFFFFFFFFu;
const uint CUSTOM_FORMAT_PRESENT = 8u; // In GeometryData.custom_formats, with the mesh's format.
const uint FLAG_COMPRESSED = 1u;
const uint FLAG_PROCEDURAL = 2u;
// Set when the BLAS uses a per-frame-deformed vertex buffer.
const uint FLAG_DEFORMED = 4u;
// Deformed by the material's vertex(): the rest positions are kept for the hit shaders.
const uint FLAG_DISPLACED = 8u;
// Uncompressed positions over a compressed attribute buffer (deformed copies of compressed meshes).
const uint FLAG_COMPRESSED_ATTRIBUTES = 16u;
// A triangle the ray tracer reports front facing is a raster back face (FRONT_FACING false), rt_triangle_facing().
const uint FLAG_FACING_INVERTED = 32u;
// The material turns the normal of back faces, as raster's DO_SIDE_CHECK (cull_front and cull_disabled).
const uint FLAG_SIDE_CHECK = 64u;
#ifdef RT_TRANSPARENT_SHADOWS
// The shadow rays' copy of a transparent surface (rendering/pathtracing/transparent_shadows): its any-hit filters their light.
const uint FLAG_TRANSPARENT_SHADOW = 128u;
#endif
#ifdef RT_TRANSPARENCY_PEELING
// Bits 8-10 of a front layer surface (rendering/pathtracing/max_transparency_layers): its blend mode, as the raygen peels it.
const uint FLAG_PEEL_BLEND_SHIFT = 8u;
const uint FLAG_PEEL_BLEND_MASK = 7u;
const uint PEEL_BLEND_MIX = 0u; // Mix, and OIT.
const uint PEEL_BLEND_ADD = 1u;
const uint PEEL_BLEND_SUB = 2u;
const uint PEEL_BLEND_MUL = 3u;
const uint PEEL_BLEND_PREMUL_ALPHA = 4u;
#endif
#ifdef RT_SHADOW_GROUPS
// rendering/pathtracing/shadow_groups: SHADOW_CASTING_SETTING_SELF_SHADOWING, its shadows fall on such instances only.
const uint FLAG_SELF_SHADOWING = 2048u;
// An RT_MASK_SHADOW_GROUP caster that is not forced opaque in the TLAS: alpha tested in the ray query.
const uint FLAG_SHADOW_GROUP_CUTOUT = 4096u;
#endif
