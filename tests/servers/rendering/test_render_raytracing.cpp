/**************************************************************************/
/*  test_render_raytracing.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_render_raytracing)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_rt_material_state.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// NVIDIA's test_render_raytracing.cpp (@49a825dc52), the cases that apply to our path tracer. Those testing their
// packed material and light records, material table, strict validation and shadow group table, which our tree does not
// have, are left out.

namespace TestRenderRaytracing {

using namespace RendererSceneRenderImplementation;

TEST_CASE("[RenderRaytracing] BaseMaterial3D state survives the material parameter round trip") {
	CHECK(RTBaseMaterialState().encode() == 0);

	RTBaseMaterialState state;
	state.uv1_triplanar = true;
	state.uv2_world_triplanar = true;
	state.uv2_vertex_transform = true;
	state.distance_fade_mode = 3;
	state.emission = true;
	state.emission_on_uv2 = true;
	state.ambient_occlusion = true;
	state.roughness_texture_channel = 4;

	const int64_t packed = state.encode();
	CHECK(packed > 0);
	CHECK(packed <= INT32_MAX);

	const RTBaseMaterialState decoded = RTBaseMaterialState::decode(packed);
	CHECK(decoded.uv1_triplanar);
	CHECK_FALSE(decoded.uv1_world_triplanar);
	CHECK_FALSE(decoded.uv2_triplanar);
	CHECK(decoded.uv2_world_triplanar);
	CHECK(decoded.uv2_vertex_transform);
	CHECK_FALSE(decoded.proximity_fade);
	CHECK(decoded.distance_fade_mode == 3);
	CHECK(decoded.emission);
	CHECK_FALSE(decoded.emission_add);
	CHECK(decoded.emission_on_uv2);
	CHECK_FALSE(decoded.orm);
	CHECK(decoded.ambient_occlusion);
	CHECK(decoded.roughness_texture_channel == 4);
	CHECK(decoded.encode() == packed);
}

TEST_CASE("[RenderRaytracing] Light distance fade is full before the begin distance and smoothstepped to zero over the length") {
	using RendererRD::LightStorage;
	CHECK(LightStorage::distance_fade_factor(10.0f, 40.0f, 10.0f) == 1.0f);
	CHECK(LightStorage::distance_fade_factor(40.0f, 40.0f, 10.0f) == 1.0f);
	CHECK(LightStorage::distance_fade_factor(45.0f, 40.0f, 10.0f) == doctest::Approx(0.5f));
	CHECK(LightStorage::distance_fade_factor(42.0f, 40.0f, 10.0f) == doctest::Approx(Math::smoothstep(0.0f, 1.0f, 0.8f)));
	CHECK(LightStorage::distance_fade_factor(50.0f, 40.0f, 10.0f) == 0.0f);
	CHECK(LightStorage::distance_fade_factor(60.0f, 40.0f, 10.0f) == 0.0f);
	CHECK(LightStorage::distance_fade_factor(41.0f, 40.0f, 0.0f) == 0.0f);
}

TEST_CASE("[RenderRaytracing] Material buffer uniform sizes match the shader compiler layout") {
	using Uniform = ShaderLanguage::ShaderNode::Uniform;
	Uniform uniform;
	uniform.type = ShaderLanguage::TYPE_FLOAT;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 4);

	uniform.array_size = 3;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 48);

	uniform.type = ShaderLanguage::TYPE_MAT4;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 192);

	uniform.type = ShaderLanguage::TYPE_VEC4;
	uniform.array_size = 0;
	uniform.scope = Uniform::SCOPE_GLOBAL;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 4);
}

// From NVIDIA's test_render_raytracing_capture.cpp.
TEST_CASE("[SceneShaderRaytracing] Instance uniforms and samplers are not material buffer members") {
	using Uniform = ShaderLanguage::ShaderNode::Uniform;
	const Vector<uint32_t> offsets = { 0, 16 };
	Uniform local;
	local.type = ShaderLanguage::TYPE_VEC4;
	local.order = 1;
	Uniform global;
	global.type = ShaderLanguage::TYPE_FLOAT;
	global.scope = Uniform::SCOPE_GLOBAL;
	global.order = 0;
	// The parser leaves an instance uniform's order at 0, the first material uniform's slot.
	Uniform instance;
	instance.type = ShaderLanguage::TYPE_VEC3;
	instance.scope = Uniform::SCOPE_INSTANCE;
	Uniform sampler;
	sampler.type = ShaderLanguage::TYPE_SAMPLER2D;
	sampler.order = -1;
	Uniform past_offsets = local;
	past_offsets.order = offsets.size();

	CHECK(SceneShaderRaytracing::is_material_buffer_uniform(local, offsets));
	CHECK(SceneShaderRaytracing::is_material_buffer_uniform(global, offsets));
	CHECK_FALSE(SceneShaderRaytracing::is_material_buffer_uniform(instance, offsets));
	CHECK_FALSE(SceneShaderRaytracing::is_material_buffer_uniform(sampler, offsets));
	CHECK_FALSE(SceneShaderRaytracing::is_material_buffer_uniform(past_offsets, offsets));
}

TEST_CASE("[RenderRaytracing] Shadow renderers only apply while shadows are enabled and visible") {
	constexpr RSE::ShadowRenderer RASTER = RSE::SHADOW_RENDERER_RASTERIZER;
	constexpr RSE::ShadowRenderer PT = RSE::SHADOW_RENDERER_PATH_TRACER;
	constexpr uint32_t ALL = RSE::SHADOW_RENDERER_ALL;

	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(false, 1.0f, ALL, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(false, 1.0f, ALL, PT));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, ALL, RASTER));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, ALL, PT));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, PT, PT));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, PT, RASTER));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, RASTER, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, RASTER, PT));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, 0, RASTER));

	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 0.01f, ALL, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 0.0f, ALL, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 0.0f, ALL, PT));
}

TEST_CASE("[RenderRaytracing] Geometry shadow renderers only apply while cast shadows is not off") {
	constexpr RSE::ShadowRenderer RASTER = RSE::SHADOW_RENDERER_RASTERIZER;
	constexpr RSE::ShadowRenderer PT = RSE::SHADOW_RENDERER_PATH_TRACER;
	constexpr uint32_t ALL = RSE::SHADOW_RENDERER_ALL;

	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_OFF, ALL, RASTER));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_OFF, ALL, PT));
	CHECK(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_ON, ALL, RASTER));
	CHECK(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_ON, ALL, PT));
	CHECK(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_ON, PT, PT));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_ON, PT, RASTER));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_ON, RASTER, PT));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_ON, 0, RASTER));
	CHECK(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED, PT, PT));
	CHECK(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY, PT, PT));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY, PT, RASTER));
}

TEST_CASE("[RenderRaytracing] Self shadowing geometry only casts path traced shadows") {
	constexpr uint32_t ALL = RSE::SHADOW_RENDERER_ALL;

	CHECK(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_SELF_SHADOWING, ALL, RSE::SHADOW_RENDERER_PATH_TRACER));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_SELF_SHADOWING, ALL, RSE::SHADOW_RENDERER_RASTERIZER));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RSE::SHADOW_CASTING_SETTING_SELF_SHADOWING, RSE::SHADOW_RENDERER_RASTERIZER, RSE::SHADOW_RENDERER_PATH_TRACER));
}

// What one camera ray sees of a triangle: whether it is drawn, its FRONT_FACING and whether its normal is flipped.
struct FacingResult {
	bool visible = false;
	bool front_facing = false;
	bool normal_flipped = false;

	bool operator==(const FacingResult &p_other) const {
		return visible == p_other.visible && (!visible || (front_facing == p_other.front_facing && normal_flipped == p_other.normal_flipped));
	}
};

// The rasterizer: culls by the mesh winding (screen winding, reversed for mirrored instances); FRONT_FACING is the
// screen winding, or the mesh winding when p_preserve_front_facing.
static FacingResult raster_facing(RSE::CullMode p_cull_mode, bool p_raster_mirror, bool p_world_front, bool p_preserve_front_facing) {
	const bool mesh_front = p_world_front != p_raster_mirror;
	FacingResult result;
	result.visible = p_cull_mode == RSE::CULL_MODE_DISABLED || mesh_front == (p_cull_mode == RSE::CULL_MODE_BACK);
	result.front_facing = p_preserve_front_facing ? mesh_front : p_world_front;
	result.normal_flipped = p_cull_mode != RSE::CULL_MODE_BACK && !result.front_facing;
	return result;
}

// The ray tracer: decides facing in BLAS space, where the default front face is the rasterizer's back face, culls back
// faces unless the instance disables it, and the hit shader applies the geometry flags.
static FacingResult traced_facing(const RTTriangleFacing &p_facing, bool p_blas_front) {
	const bool flip = p_facing.instance_flags & RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
	const bool hit_front = p_blas_front == flip;
	FacingResult result;
	result.visible = hit_front || (p_facing.instance_flags & RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT);
	result.front_facing = hit_front != bool(p_facing.geometry_flags & RT_GEOM_FLAG_FACING_INVERTED);
	result.normal_flipped = !result.front_facing && (p_facing.geometry_flags & RT_GEOM_FLAG_SIDE_CHECK);
	return result;
}

// NVIDIA's rt_triangle_facing(cull mode, raster mirror, TLAS mirror, preserve front facing) on ours, which takes whether
// the BLAS space is mirrored from the one raster culls in, and whether raster mirrors the winding on screen, as
// RenderRaytracing::build_tlas() derives them (unmirrored camera).
static RTTriangleFacing triangle_facing(RSE::CullMode p_cull_mode, bool p_raster_mirror, bool p_tlas_mirror, bool p_preserve_front_facing) {
	return rt_triangle_facing(p_cull_mode, p_raster_mirror != p_tlas_mirror, p_raster_mirror && !p_preserve_front_facing);
}

TEST_CASE("[RenderRaytracing] Path traced triangle facing matches raster for every cull mode and mirroring") {
	const RSE::CullMode cull_modes[] = { RSE::CULL_MODE_BACK, RSE::CULL_MODE_FRONT, RSE::CULL_MODE_DISABLED };
	for (RSE::CullMode cull_mode : cull_modes) {
		for (int bits = 0; bits < 16; bits++) {
			const bool raster_mirror = bits & 1;
			// Billboards and MultiMesh instances can have a TLAS transform mirrored unlike the node.
			const bool tlas_mirror = bits & 2;
			const bool blas_front = bits & 4;
			const bool preserve = bits & 8;
			const bool world_front = blas_front != tlas_mirror;
			const FacingResult traced = traced_facing(triangle_facing(cull_mode, raster_mirror, tlas_mirror, preserve), blas_front);
			CHECK_MESSAGE(traced == raster_facing(cull_mode, raster_mirror, world_front, preserve),
					vformat("cull mode %d, raster mirror %d, TLAS mirror %d, BLAS front %d, preserve %d", int(cull_mode), int(raster_mirror), int(tlas_mirror), int(blas_front), int(preserve)));
		}
	}
}

TEST_CASE("[RenderRaytracing] Preserved front facing keeps double-sided normals of mirrored instances unflipped") {
	// A mesh front face of a mirrored node (BLAS front, TLAS mirrored).
	const FacingResult legacy = traced_facing(triangle_facing(RSE::CULL_MODE_DISABLED, true, true, false), true);
	const FacingResult preserved = traced_facing(triangle_facing(RSE::CULL_MODE_DISABLED, true, true, true), true);
	CHECK_FALSE(legacy.front_facing);
	CHECK(legacy.normal_flipped);
	CHECK(preserved.front_facing);
	CHECK_FALSE(preserved.normal_flipped);
}

TEST_CASE("[RenderRaytracing] Unmirrored back face culling keeps the TLAS facing bits of a plain mesh") {
	const RTTriangleFacing facing = triangle_facing(RSE::CULL_MODE_BACK, false, false, false);
	CHECK(facing.instance_flags == uint32_t(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT));
	CHECK(facing.geometry_flags == 0);
}

// ShaderLanguage reads RSG::utilities, which only a live RenderingServer keeps valid.
TEST_CASE("[SceneTree][RenderRaytracing] RAY_* built-ins are read-only fragment inputs with a raster value each") {
	const ShaderTypes *types = ShaderTypes::get_singleton();
	REQUIRE(types != nullptr);
	const ShaderLanguage::FunctionInfo &fragment = types->get_functions(RSE::SHADER_SPATIAL).get("fragment");
	HashMap<StringName, String> raster_renames;
	ShaderTypes::add_raster_ray_renames(raster_renames);

	int ray_built_ins = 0;
	for (const KeyValue<StringName, ShaderLanguage::BuiltInInfo> &built_in : fragment.built_ins) {
		const String name = built_in.key;
		if (!name.begins_with("RAY_")) {
			continue;
		}
		ray_built_ins++;
		CHECK_MESSAGE(built_in.value.constant, name);
		CHECK_MESSAGE(raster_renames.has(built_in.key), name);
	}
	CHECK(ray_built_ins == raster_renames.size());

	ShaderLanguage::ShaderCompileInfo info;
	info.functions = types->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = types->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = types->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types = types->get_types();
	const String read = "shader_type spatial;\nvoid fragment() {\n"
						"	if (!RAY_IS_PRIMARY && !RAY_IS_SHADOW && (RAY_IS_DIFFUSE || RAY_IS_SPECULAR) && !RAY_IS_SINGULAR) {\n"
						"		ALBEDO *= float(RAY_BOUNCES + RAY_DIFFUSE_BOUNCES);\n"
						"	}\n}\n";
	ShaderLanguage language;
	CHECK(language.compile(read, info) == OK);

	ERR_PRINT_OFF;
	CHECK(language.compile("shader_type spatial;\nvoid fragment() { RAY_IS_PRIMARY = false; }\n", info) != OK);
	CHECK(language.compile("shader_type spatial;\nvoid vertex() { bool p = RAY_IS_PRIMARY; }\n", info) != OK);
	ERR_PRINT_ON;
}

using DynamicDebug = RTDynamicGeometryDebug;

static DynamicDebug::State dynamic_state(DynamicDebug::Category p_category, uint32_t p_age = 0, DynamicDebug::Category p_rest = DynamicDebug::CATEGORY_STATIC) {
	DynamicDebug::State state;
	state.category = p_category;
	state.rest = p_rest;
	state.age = p_age;
	return state;
}

static DynamicDebug::Stamps dynamic_stamps(uint64_t p_build, uint64_t p_refit, bool p_dynamic = false) {
	DynamicDebug::Stamps stamps;
	stamps.last_build = p_build;
	stamps.last_refit = p_refit;
	stamps.dynamic = p_dynamic;
	return stamps;
}

TEST_CASE("[RTDynamicGeometryDebug] Geometry without BLAS work is static, or moved when its transform changed") {
	const DynamicDebug::Stamps none;
	CHECK(DynamicDebug::classify(none, false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(none, true, false, 100) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(none, false, true, 100) == dynamic_state(DynamicDebug::CATEGORY_MOVED));
}

TEST_CASE("[RTDynamicGeometryDebug] The most recent BLAS work picks the category, a build on ties") {
	CHECK(DynamicDebug::classify(dynamic_stamps(100, DynamicDebug::NEVER), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_BUILD));
	CHECK(DynamicDebug::classify(dynamic_stamps(DynamicDebug::NEVER, 100), false, true, 100) == dynamic_state(DynamicDebug::CATEGORY_REFIT));
	CHECK(DynamicDebug::classify(dynamic_stamps(96, 99), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_REFIT, 1));
	CHECK(DynamicDebug::classify(dynamic_stamps(99, 96), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_BUILD, 1));
	CHECK(DynamicDebug::classify(dynamic_stamps(100, 100), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_BUILD));
}

TEST_CASE("[RTDynamicGeometryDebug] Captured geometry gets its own build and refit categories") {
	CHECK(DynamicDebug::classify(dynamic_stamps(100, DynamicDebug::NEVER), true, false, 100) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_BUILD));
	CHECK(DynamicDebug::classify(dynamic_stamps(90, 98), true, false, 100) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_REFIT, 2));
}

TEST_CASE("[RTDynamicGeometryDebug] BLAS work fades out after FADE_FRAMES") {
	const uint64_t frame = 1000;
	const uint64_t last_visible = frame - (DynamicDebug::FADE_FRAMES - 1);
	CHECK(DynamicDebug::classify(dynamic_stamps(last_visible, DynamicDebug::NEVER), false, false, frame) == dynamic_state(DynamicDebug::CATEGORY_BUILD, DynamicDebug::FADE_FRAMES - 1));
	CHECK(DynamicDebug::classify(dynamic_stamps(last_visible - 1, DynamicDebug::NEVER), false, false, frame) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(dynamic_stamps(last_visible - 1, DynamicDebug::NEVER), false, true, frame) == dynamic_state(DynamicDebug::CATEGORY_MOVED));
}

TEST_CASE("[RTDynamicGeometryDebug] A dynamic BLAS whose last work was a build fades to a build idle category") {
	const uint64_t built = 100;
	const uint64_t idle = built + DynamicDebug::FADE_FRAMES;
	const DynamicDebug::Stamps dynamic_build = dynamic_stamps(built, DynamicDebug::NEVER, true);
	CHECK(DynamicDebug::classify(dynamic_build, false, false, built) == dynamic_state(DynamicDebug::CATEGORY_BUILD, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	CHECK(DynamicDebug::classify(dynamic_build, false, false, idle) == dynamic_state(DynamicDebug::CATEGORY_BUILT_IDLE, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	CHECK(DynamicDebug::classify(dynamic_build, true, false, idle) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE, 0, DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE));
	CHECK(DynamicDebug::classify(dynamic_build, false, true, idle) == dynamic_state(DynamicDebug::CATEGORY_MOVED, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	// A build after a refit settles; a refit after the build fades to static, as does a BLAS built once at load.
	CHECK(DynamicDebug::classify(dynamic_stamps(built, built - 1, true), false, false, idle).rest == DynamicDebug::CATEGORY_BUILT_IDLE);
	CHECK(DynamicDebug::classify(dynamic_stamps(built, built + 1, true), false, false, idle + 1) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(dynamic_stamps(built, DynamicDebug::NEVER), false, false, idle) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
}

TEST_CASE("[RTDynamicGeometryDebug] Packing keeps the category in the low bits, the rest category and the age above") {
	const uint32_t packed = DynamicDebug::pack(dynamic_state(DynamicDebug::CATEGORY_CAPTURED_BUILD, 17, DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE));
	CHECK((packed & DynamicDebug::CATEGORY_MASK) == uint32_t(DynamicDebug::CATEGORY_CAPTURED_BUILD));
	CHECK(((packed >> DynamicDebug::REST_SHIFT) & DynamicDebug::CATEGORY_MASK) == uint32_t(DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE));
	CHECK((packed >> DynamicDebug::AGE_SHIFT) == 17u);
	CHECK(DynamicDebug::pack(DynamicDebug::State()) == 0u);
	static_assert(DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE <= DynamicDebug::CATEGORY_MASK, "Categories must fit their bits.");
	static_assert(DynamicDebug::REST_SHIFT * 2 <= DynamicDebug::AGE_SHIFT, "The rest category must fit below the age.");
	static_assert(DynamicDebug::FADE_FRAMES <= (UINT32_MAX >> DynamicDebug::AGE_SHIFT), "Ages must fit above the categories.");
}

TEST_CASE("[RTDynamicGeometryDebug] The tracker stamps BLASes per frame and forgets faded ones") {
	DynamicDebug tracker;
	const RID skinned = RID::from_uint64(0x100000001ULL);
	const RID captured = RID::from_uint64(0x100000002ULL);
	const RID static_blas = RID::from_uint64(0x100000003ULL);

	tracker.begin_frame(10);
	tracker.record_build(skinned);
	tracker.record_refit(captured);
	CHECK(tracker.get_tracked_count() == 2);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_BUILD, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	CHECK(tracker.observe(captured, true, false, true) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_REFIT));
	CHECK(tracker.observe(static_blas, false, false, false) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(tracker.get_tracked_count() == 2);

	tracker.begin_frame(11);
	tracker.record_refit(skinned);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_REFIT));
	CHECK(tracker.observe(captured, true, false, true) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_REFIT, 1));

	// Repeated calls within a frame (one per viewport) keep the frame's stamps.
	tracker.begin_frame(11);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_REFIT));

	tracker.begin_frame(10 + DynamicDebug::FADE_FRAMES);
	CHECK(tracker.get_tracked_count() == 1);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_REFIT, DynamicDebug::FADE_FRAMES - 1));
	tracker.begin_frame(11 + DynamicDebug::FADE_FRAMES);
	CHECK(tracker.get_tracked_count() == 0);

	tracker.record_build(static_blas);
	tracker.clear();
	CHECK(tracker.get_tracked_count() == 0);
}

TEST_CASE("[RTDynamicGeometryDebug] The tracker keeps idle built dynamic BLASes while TLAS instances use them") {
	DynamicDebug tracker;
	const RID idle = RID::from_uint64(0x100000001ULL);
	const RID loaded = RID::from_uint64(0x100000002ULL);

	tracker.begin_frame(10);
	tracker.record_build(idle);
	tracker.record_build(loaded);
	tracker.observe(idle, false, false, true);
	tracker.observe(loaded, false, false, false);

	const uint64_t settled = 10 + 2 * DynamicDebug::FADE_FRAMES;
	for (uint64_t frame = 11; frame <= settled; frame++) {
		tracker.begin_frame(frame);
		tracker.observe(idle, false, false, true);
		tracker.observe(loaded, false, false, false);
	}
	CHECK(tracker.get_tracked_count() == 1);
	CHECK(tracker.observe(idle, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_BUILT_IDLE, 0, DynamicDebug::CATEGORY_BUILT_IDLE));

	tracker.begin_frame(settled + DynamicDebug::FADE_FRAMES - 1);
	CHECK(tracker.get_tracked_count() == 1);
	tracker.begin_frame(settled + DynamicDebug::FADE_FRAMES);
	CHECK(tracker.get_tracked_count() == 0);
}

} // namespace TestRenderRaytracing

#endif // RD_ENABLED
