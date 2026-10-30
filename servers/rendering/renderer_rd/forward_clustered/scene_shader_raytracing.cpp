/**************************************************************************/
/*  scene_shader_raytracing.cpp                                           */
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

#include "scene_shader_raytracing.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/math/math_defs.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/string/string_builder.h"
#include "core/version.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/forward_clustered/rt_alpha_dependencies.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/environment_storage.h"

using namespace RendererSceneRenderImplementation;

static void _dump_failed_shader(const String &p_source, const String &p_label) {
	String tmp_dir = OS::get_singleton()->get_temp_path();
	String path = tmp_dir.path_join("rt_shader_" + p_label + ".glsl");
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(p_source);
		WARN_PRINT("Dumped failed shader to: " + path);
	}
}

namespace {

struct RaygenShaderOption {
	uint32_t rt_flag_mask;
	const char *define_snippet;
};

// Variant index is a uint32 bitmask: bit i selects RAYGEN_SHADER_OPTIONS[i].
// Add entries here to grow the raygen permutation set (2^N variants).
static constexpr RaygenShaderOption RAYGEN_SHADER_OPTIONS[] = {
	{ SceneShaderRaytracing::RT_FLAG_DLSS_RR_ENABLED, "#define DLSS_RR_ENABLED\n" },
	{ SceneShaderRaytracing::RT_FLAG_SER_ENABLED, "#define USE_SER\n" },
	{ SceneShaderRaytracing::RT_FLAG_RAY_QUERY_SHADOWS_ENABLED, "#define USE_RAY_QUERY_SHADOWS\n" },
	{ SceneShaderRaytracing::RT_FLAG_NRD_ENABLED, "#define NRD_ENABLED\n" },
	// The ray queries may meet opacity micromaps (D3D12 then allocates them with AllocateRayQuery2).
	{ SceneShaderRaytracing::RT_FLAG_OPACITY_MICROMAPS, "#define RT_OPACITY_MICROMAPS\n" },

};

static constexpr uint32_t RAYGEN_SHADER_OPTION_COUNT = sizeof(RAYGEN_SHADER_OPTIONS) / sizeof(RAYGEN_SHADER_OPTIONS[0]);

static uint32_t _raygen_variant_from_rt_flags(uint32_t p_rt_flags) {
	uint32_t variant = 0;
	for (uint32_t i = 0; i < RAYGEN_SHADER_OPTION_COUNT; i++) {
		if (p_rt_flags & RAYGEN_SHADER_OPTIONS[i].rt_flag_mask) {
			variant |= (1u << i);
		}
	}
	return variant;
}

// Procedural hit groups only: their shaders share the intersection shader's hit attributes.
static constexpr const char *INTERSECTION_SHADERS_DEFINE = "#define ENABLE_INTERSECTION_SHADERS\n";

// p_source with p_define on the line after its #version directive.
static String _insert_after_version(const String &p_source, const String &p_define) {
	const int version_pos = p_source.find("#version");
	ERR_FAIL_COND_V(version_pos < 0, p_source);
	const int line_end = p_source.find("\n", version_pos);
	ERR_FAIL_COND_V(line_end < 0, p_source);
	return p_source.insert(line_end + 1, p_define);
}

// Returns the GLSL preamble for the variant identified by p_variant_mask.
// Each set bit i activates RAYGEN_SHADER_OPTIONS[i].define_snippet.
static String _raygen_variant_preamble(uint32_t p_variant_mask) {
	String preamble = "\n";
	for (uint32_t opt = 0; opt < RAYGEN_SHADER_OPTION_COUNT; opt++) {
		if (p_variant_mask & (1u << opt)) {
			preamble += RAYGEN_SHADER_OPTIONS[opt].define_snippet;
		}
	}
	return preamble;
}

} // namespace

void SceneShaderRaytracing::ShaderData::set_code(const String &p_code) {
	code = p_code;

	if (raygen_version.is_null()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		raygen_version = SceneShaderRaytracing::singleton->raygen_shader.version_create();
	}

	blend_mode = BLEND_MODE_MIX;
	depth_draw = DEPTH_DRAW_OPAQUE;
	depth_test = DEPTH_TEST_ENABLED;
	cull_mode = CULL_BACK;
	alpha_antialiasing_mode = ALPHA_ANTIALIASING_OFF;

	uses_point_size = false;
	uses_alpha = false;
	uses_alpha_clip = false;
	uses_alpha_antialiasing = false;
	uses_blend_alpha = false;
	uses_depth_prepass_alpha = false;
	uses_discard = false;
	uses_roughness = false;
	uses_normal = false;
	uses_tangent = false;
	uses_normal_map = false;
	uses_vertex = false;
	uses_position = false;
	uses_sss = false;
	uses_transmittance = false;
	uses_time = false;
	uses_screen_texture = false;
	uses_screen_texture_mipmaps = false;
	uses_depth_texture = false;
	uses_normal_texture = false;
	uses_vertex_time = false;
	uses_fragment_time = false;
	writes_modelview_or_projection = false;
	uses_world_coordinates = false;
	uses_particle_trails = false;
	wireframe = false;
	unshaded = false;

	ubo_size = 0;
	uniforms.clear();
	ubo_offsets.clear();
	texture_uniforms.clear();
	_clear_vertex_input_mask_cache();
	pipeline_hash_map.clear_pipelines();
}

bool SceneShaderRaytracing::ShaderData::is_animated() const {
	return (uses_fragment_time && uses_discard) || (uses_vertex_time && uses_vertex);
}

bool SceneShaderRaytracing::ShaderData::casts_shadows() const {
	bool has_read_screen_alpha = uses_screen_texture || uses_depth_texture || uses_normal_texture;
	bool has_base_alpha = (uses_alpha && (!uses_alpha_clip || uses_alpha_antialiasing)) || has_read_screen_alpha;
	bool has_alpha = has_base_alpha || uses_blend_alpha;

	return !has_alpha || (uses_depth_prepass_alpha && !(depth_draw == DEPTH_DRAW_DISABLED || depth_test == DEPTH_TEST_DISABLED));
}

RenderingServerTypes::ShaderNativeSourceCode SceneShaderRaytracing::ShaderData::get_native_source_code() const {
	// For raytracing: return source code from raygen shader, not rasterization shader
	if (raygen_version.is_valid()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		return SceneShaderRaytracing::singleton->raygen_shader.version_get_native_source_code(raygen_version);
	} else {
		return RenderingServerTypes::ShaderNativeSourceCode();
	}
}

SceneShaderRaytracing::ShaderVersion SceneShaderRaytracing::ShaderData::_get_shader_version(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const {
	// Simplified: we only have one shader version now (index 0)
	return ShaderVersion(0);
}

void SceneShaderRaytracing::ShaderData::_create_pipeline(PipelineKey p_pipeline_key) {
#if PRINT_PIPELINE_COMPILATION_KEYS
	print_line(
			"HASH:", p_pipeline_key.hash(),
			"VERSION:", version,
			"VERTEX:", p_pipeline_key.vertex_format_id,
			"FRAMEBUFFER:", p_pipeline_key.framebuffer_format_id,
			"CULL:", p_pipeline_key.cull_mode,
			"PRIMITIVE:", p_pipeline_key.primitive_type,
			"VERSION:", p_pipeline_key.version,
			"PASS FLAGS:", p_pipeline_key.color_pass_flags,
			"SPEC PACKED #0:", p_pipeline_key.shader_specialization.packed_0,
			"WIREFRAME:", p_pipeline_key.wireframe);
#endif

	// Simplified: just create a simple opaque color blend state
	RD::PipelineColorBlendState blend_state = RD::PipelineColorBlendState::create_disabled(1);

	RD::PipelineDepthStencilState depth_stencil_state;
	if (depth_test != DEPTH_TEST_DISABLED) {
		depth_stencil_state.enable_depth_test = true;
		depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
		depth_stencil_state.enable_depth_write = depth_draw != DEPTH_DRAW_DISABLED ? true : false;
	}

	RD::RenderPrimitive primitive_rd_table[RSE::PRIMITIVE_MAX] = {
		RD::RENDER_PRIMITIVE_POINTS,
		RD::RENDER_PRIMITIVE_LINES,
		RD::RENDER_PRIMITIVE_LINESTRIPS,
		RD::RENDER_PRIMITIVE_TRIANGLES,
		RD::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
	};

	RD::RenderPrimitive primitive_rd = uses_point_size ? RD::RENDER_PRIMITIVE_POINTS : primitive_rd_table[p_pipeline_key.primitive_type];

	RD::PipelineRasterizationState raster_state;
	raster_state.cull_mode = p_pipeline_key.cull_mode;
	raster_state.wireframe = wireframe || p_pipeline_key.wireframe;

	RD::PipelineMultisampleState multisample_state;
	multisample_state.sample_count = RD::get_singleton()->framebuffer_format_get_texture_samples(p_pipeline_key.framebuffer_format_id, 0);

	// Simplified: no specialization constants
	Vector<RD::PipelineSpecializationConstant> specialization_constants;

	RID shader_rid = get_shader_variant(p_pipeline_key.version, p_pipeline_key.color_pass_flags, p_pipeline_key.ubershader);
	ERR_FAIL_COND(shader_rid.is_null());

	RID pipeline = RD::get_singleton()->render_pipeline_create(shader_rid, p_pipeline_key.framebuffer_format_id, p_pipeline_key.vertex_format_id, primitive_rd, raster_state, multisample_state, depth_stencil_state, blend_state, 0, 0, specialization_constants);
	ERR_FAIL_COND(pipeline.is_null());

	pipeline_hash_map.add_compiled_pipeline(p_pipeline_key.hash(), pipeline);
}

RD::PolygonCullMode SceneShaderRaytracing::ShaderData::get_cull_mode_from_cull_variant(CullVariant p_cull_variant) {
	const RD::PolygonCullMode cull_mode_rd_table[CULL_VARIANT_MAX][3] = {
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_FRONT, RD::POLYGON_CULL_BACK },
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_BACK, RD::POLYGON_CULL_FRONT },
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_DISABLED }
	};

	return cull_mode_rd_table[p_cull_variant][cull_mode];
}

RID SceneShaderRaytracing::ShaderData::_get_shader_variant(ShaderVersion p_shader_version) const {
	return RID();
}

void SceneShaderRaytracing::ShaderData::_clear_vertex_input_mask_cache() {
	for (uint32_t i = 0; i < VERTEX_INPUT_MASKS_SIZE; i++) {
		vertex_input_masks[i].store(0);
	}
}

RID SceneShaderRaytracing::ShaderData::get_shader_variant(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const {
	return _get_shader_variant(_get_shader_version(p_pipeline_version, p_color_pass_flags, p_ubershader));
}

RID SceneShaderRaytracing::ShaderData::get_raygen_shader_variant() const {
	if (raygen_version.is_valid()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		ERR_FAIL_NULL_V(SceneShaderRaytracing::singleton, RID());
		return SceneShaderRaytracing::singleton->raygen_shader.version_get_shader(raygen_version, 0);
	} else {
		return RID();
	}
}

uint64_t SceneShaderRaytracing::ShaderData::get_vertex_input_mask(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) {
	// Vertex input masks require knowledge of the shader. Since querying the shader can be expensive due to high contention and the necessary mutex, we cache the result instead.
	ShaderVersion shader_version = _get_shader_version(p_pipeline_version, p_color_pass_flags, p_ubershader);
	uint64_t input_mask = vertex_input_masks[shader_version].load(std::memory_order_relaxed);
	if (input_mask == 0) {
		RID shader_rid = _get_shader_variant(shader_version);
		ERR_FAIL_COND_V(shader_rid.is_null(), 0);

		input_mask = RD::get_singleton()->shader_get_vertex_input_attribute_mask(shader_rid);
		vertex_input_masks[shader_version].store(input_mask, std::memory_order_relaxed);
	}

	return input_mask;
}

bool SceneShaderRaytracing::ShaderData::is_valid() const {
	// Validity follows raygen_version.
	if (raygen_version.is_valid()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		ERR_FAIL_NULL_V(SceneShaderRaytracing::singleton, false);
		return SceneShaderRaytracing::singleton->raygen_shader.version_is_valid(raygen_version);
	} else {
		return false;
	}
}

SceneShaderRaytracing::ShaderData::ShaderData() :
		shader_list_element(this) {
	pipeline_hash_map.set_creation_object_and_function(this, &ShaderData::_create_pipeline);
	pipeline_hash_map.set_compilations(SceneShaderRaytracing::singleton->pipeline_compilations, &SceneShaderRaytracing::singleton_mutex);
}

SceneShaderRaytracing::ShaderData::~ShaderData() {
	pipeline_hash_map.clear_pipelines();

	// Tear down raygen only (no raster shader RID).
	if (raygen_version.is_valid()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		ERR_FAIL_NULL(SceneShaderRaytracing::singleton);
		SceneShaderRaytracing::singleton->raygen_shader.version_free(raygen_version);
	}
}

Pair<ShaderRD *, RID> SceneShaderRaytracing::ShaderData::get_native_shader_and_version() const {
	MutexLock lock(SceneShaderRaytracing::singleton_mutex);
	if (SceneShaderRaytracing::singleton == nullptr) {
		return Pair<ShaderRD *, RID>(nullptr, RID());
	}
	return Pair<ShaderRD *, RID>(&SceneShaderRaytracing::singleton->raygen_shader, raygen_version); // Raygen pair for tooling.
}

RendererRD::MaterialStorage::ShaderData *SceneShaderRaytracing::_create_shader_func() {
	MutexLock lock(SceneShaderRaytracing::singleton_mutex);
	ShaderData *shader_data = memnew(ShaderData);
	singleton->shader_list.add(&shader_data->shader_list_element);
	return shader_data;
}

void SceneShaderRaytracing::MaterialData::set_render_priority(int p_priority) {
	priority = p_priority - RSE::MATERIAL_RENDER_PRIORITY_MIN; //8 bits
}

void SceneShaderRaytracing::MaterialData::set_next_pass(RID p_pass) {
	next_pass = p_pass;
}

bool SceneShaderRaytracing::MaterialData::update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty) {
	// RT path does not drive MaterialStorage uniform sets here.
	return true;
}

SceneShaderRaytracing::MaterialData::~MaterialData() {
	free_parameters_uniform_set(uniform_set);
}

RendererRD::MaterialStorage::MaterialData *SceneShaderRaytracing::_create_material_func(ShaderData *p_shader) {
	MaterialData *material_data = memnew(MaterialData);
	material_data->shader_data = p_shader;
	return material_data;
}

SceneShaderRaytracing *SceneShaderRaytracing::singleton = nullptr;
Mutex SceneShaderRaytracing::singleton_mutex;

SceneShaderRaytracing::SceneShaderRaytracing() {
	// There should be only one of these, contained within our RenderForwardClustered singleton.
	singleton = this;
}

SceneShaderRaytracing::~SceneShaderRaytracing() {
	_join_lane_for_shutdown();
	while (!program_builds.is_empty()) {
		_finish_program_build(program_builds[0]); // Into the programs freed below.
	}
	invalidate_pipeline_bundles();

	if (raygen_shader_version.is_valid()) {
		raygen_shader.version_free(raygen_shader_version);
	}
	for (const DisplaceProgram &program : displace_programs) {
		if (program.shader.is_valid()) {
			RD::get_singleton()->free_rid(program.shader);
		}
	}
	if (displace_shader_version.is_valid()) {
		displace_shader.version_free(displace_shader_version);
	}
	for (const DisplaceProgram &program : omm_bake_programs) {
		if (program.shader.is_valid()) {
			RD::get_singleton()->free_rid(program.shader);
		}
	}
	if (omm_bake_shader_version.is_valid()) {
		omm_bake_shader.version_free(omm_bake_shader_version);
	}

	singleton = nullptr;
}

void SceneShaderRaytracing::invalidate_pipeline_bundles() {
	// Bundles own HG + base_shader RIDs; free_rid is deferred-safe for in-flight frames.
	for (KeyValue<uint32_t, PipelineBundle> &kv : pipeline_bundles) {
		PipelineBundle &b = kv.value;
		if (b.hit_sbt.is_valid()) {
			RD::get_singleton()->free_rid(b.hit_sbt);
		}
		if (b.pipeline.is_valid()) {
			RD::get_singleton()->free_rid(b.pipeline);
		}
		for (const RID &rid : b.per_hg_shaders) {
			if (rid.is_valid()) {
				RD::get_singleton()->free_rid(rid);
			}
		}
		if (b.base_shader.is_valid()) {
			RD::get_singleton()->free_rid(b.base_shader);
		}
	}
	pipeline_bundles.clear();
	variant_compile_contexts.clear();
	bake_compile_contexts.clear();
}

// Async bundle rebuild task (worker: SPIR-V + pipeline; main thread: SBT + swap). Single-lane globally.

struct SceneShaderRaytracing::PipelineBuildTask {
	uint32_t rt_flags = 0;

	RID base_shader;
	Vector<RD::PipelineSpecializationConstant> spec_constants;
	Vector<String> base_sources;
	String base_sources_key;
	// Compiled from base_sources by the worker, only when a hit group misses the cache.
	Vector<RD::ShaderStageSPIRVData> base_non_hit_stages; // RAYGEN + MISS.
	RD::ShaderStageSPIRVData base_any_hit_stage;
	bool base_any_hit_valid = false;
	// The same any-hit with the intersection shaders' hit attributes, for procedural hit groups without their own.
	String base_any_hit_procedural_src;
	RD::ShaderStageSPIRVData base_any_hit_procedural_stage;
	bool base_any_hit_procedural_valid = false;

	struct SlotInput {
		RID existing_per_hg_shader;
		bool needs_compile = false;
		bool uses_alpha_clip = false;
		bool is_procedural = false;
		String ch_src;
		String ah_src;
		String is_src;
		String name; // CustomShaderEntry::name, for --verbose.
		// Worker output.
		String cache_key;
		Vector<uint8_t> binary;
		RID shader;
		uint64_t glslang_usec = 0; // GLSL to SPIR-V of its own stages.
		uint64_t translate_usec = 0; // SPIR-V of every stage to the driver's binary (Mesa's DXIL on D3D12).
	};
	LocalVector<SlotInput> slots;
	LocalVector<uint32_t> compile_indices; // The slots to compile, then those missing from the cache.
	uint32_t slots_left_for_later = 0; // Pending slots this build leaves to the next one (_make_pipeline_build_task()).

	LocalVector<RID> new_per_hg_shaders;
	LocalVector<bool> new_ready_mask;
	RID new_pipeline;
	bool failed = false;
	String error;

	// The bundle's pipeline when the task was made, to which the hit groups compiled now are added when the driver allows
	// (incremental): new_pipeline is then base_pipeline plus them (or base_pipeline itself when none was compiled), and
	// new_slot_groups the bundle's next PipelineBundle::slot_groups.
	RID base_pipeline;
	LocalVector<uint32_t> base_slot_groups;
	uint32_t base_group_count = 0;
	uint32_t base_empty_group = 0;
	uint32_t base_sbt_capacity = 0;
	bool incremental = false;
	LocalVector<uint32_t> new_slot_groups;
	uint32_t new_group_count = 0;

	SafeFlag abort_requested;
	SafeFlag done;
	WorkerThreadPool::TaskID worker_id = WorkerThreadPool::INVALID_TASK_ID;
};

// The identifier ShaderCompiler gives a uniform ("__" is reserved in GLSL).
static String _uniform_identifier(const String &p_name) {
	return ("m_" + p_name.replace("__", "_dus_")).replace("__", "_dus_");
}

// Clamps every index of the material uniform array p_member in p_code. Raster reads material uniforms from a uniform
// buffer, where an out-of-bounds index is harmless (Terrain3D reads its region map that way); here they are read by
// device address, where it faults.
static String _clamp_uniform_array_indices(const String &p_code, const String &p_member, int p_size) {
	const String access = "material." + p_member + "[";
	String result;
	int from = 0;
	while (true) {
		const int start = p_code.find(access, from);
		if (start < 0) {
			result += p_code.substr(from);
			break;
		}
		const int index_begin = start + access.length();
		int depth = 1;
		int index_end = index_begin;
		while (index_end < p_code.length() && depth > 0) {
			const char32_t c = p_code[index_end];
			if (c == '[') {
				depth++;
			} else if (c == ']') {
				depth--;
			}
			index_end++;
		}
		if (depth != 0) {
			result += p_code.substr(from);
			break;
		}
		const String index = p_code.substr(index_begin, index_end - 1 - index_begin);
		result += p_code.substr(from, index_begin - from);
		result += "clamp(int(" + _clamp_uniform_array_indices(index, p_member, p_size) + "), 0, " + itos(p_size - 1) + ")]";
		from = index_end;
	}
	return result;
}

// The hit shaders run vertex() at the triangle's three vertices and interpolate the varyings as raster does (flat and
// integer varyings take the first vertex); dFdx() and dFdy() evaluate their argument again with the neighboring pixel's
// values. The code each material's varyings need for that, and its replacement in a hit group or displacement template.
// The hit groups (RT_INTERPOLATE_AT_POINTS) sum each corner into the values at the points they read as the corner's
// vertex() returns (accumulate), one variable per point (rt_p<N>_<name>: an array indexed in the corner loop became a local
// array in DXIL, in scratch memory on AMD, see raytracing_custom_globals_inc.glsl); the other templates keep the three
// corners (save) and interpolate them later (blend).
static String _apply_varying_code(const String &p_template, const Vector<ShaderCompiler::GeneratedCode::Varying> &p_varyings) {
	const int POINTS = 3;
	String storage, save, blend, push, pop;
	String accumulate_first[POINTS], accumulate[POINTS], select[POINTS];
	for (const ShaderCompiler::GeneratedCode::Varying &varying : p_varyings) {
		const String corners = "rt_corner_" + varying.name;
		const String saved = "rt_saved_" + varying.name;
		const String array = varying.array_size > 0 ? "[" + itos(varying.array_size) + "]" : String();
		storage += "#ifdef RT_INTERPOLATE_AT_POINTS\n";
		for (int p = 0; p < POINTS; p++) {
			storage += varying.type + " rt_p" + itos(p) + "_" + varying.name + array + ";\n";
		}
		storage += "#else\n" + varying.type + " " + corners + "[3]" + array + ";\n#endif\n";
		storage += varying.type + " " + saved + array + ";\n";
		save += corners + "[rt_corner] = " + varying.name + ";\n";
		push += saved + " = " + varying.name + ";\n";
		pop += varying.name + " = " + saved + ";\n";
		for (int p = 0; p < POINTS; p++) {
			const String point = "rt_p" + itos(p) + "_" + varying.name;
			const String w = "rt_w" + itos(p);
			select[p] += varying.name + " = " + point + ";\n";
			if (!varying.interpolated) {
				accumulate_first[p] += point + " = " + varying.name + ";\n";
			} else if (varying.array_size > 0) {
				const String elements = "for (int rt_element = 0; rt_element < " + itos(varying.array_size) + "; rt_element++) {\n";
				accumulate_first[p] += elements + point + "[rt_element] = " + varying.name + "[rt_element] * " + w + ";\n}\n";
				accumulate[p] += elements + point + "[rt_element] = " + point + "[rt_element] + " + varying.name + "[rt_element] * " + w + ";\n}\n";
			} else {
				accumulate_first[p] += point + " = " + varying.name + " * " + w + ";\n";
				accumulate[p] += point + " = " + point + " + " + varying.name + " * " + w + ";\n";
			}
		}
		if (!varying.interpolated) {
			blend += varying.name + " = " + corners + "[0];\n";
		} else if (varying.array_size > 0) {
			blend += "for (int rt_element = 0; rt_element < " + itos(varying.array_size) + "; rt_element++) {\n";
			blend += varying.name + "[rt_element] = " + corners + "[0][rt_element] * p_bary.x + " + corners + "[1][rt_element] * p_bary.y + " + corners + "[2][rt_element] * p_bary.z;\n}\n";
		} else {
			blend += varying.name + " = " + corners + "[0] * p_bary.x + " + corners + "[1] * p_bary.y + " + corners + "[2] * p_bary.z;\n";
		}
	}
	String result = p_template.replace("/* RT_CUSTOM_VARYING_STORAGE */", storage).replace("/* RT_CUSTOM_VARYING_SAVE */", save).replace("/* RT_CUSTOM_VARYING_BLEND */", blend).replace("/* RT_CUSTOM_VARYING_PUSH */", push).replace("/* RT_CUSTOM_VARYING_POP */", pop);
	for (int p = 0; p < POINTS; p++) {
		const String suffix = itos(p) + " */";
		result = result.replace("/* RT_CUSTOM_VARYING_ACCUMULATE_FIRST_" + suffix, accumulate_first[p]).replace("/* RT_CUSTOM_VARYING_ACCUMULATE_" + suffix, accumulate[p]).replace("/* RT_CUSTOM_VARYING_SELECT_" + suffix, select[p]);
	}
	return result;
}

// The hit shaders have no depth texture: every read of DEPTH_TEXTURE (any texture function on depth_buffer) becomes
// rt_depth_buffer_value(), the depth raster would read at this pixel (raytracing_custom_globals_inc.glsl).
static String _depth_buffer_reads(const String &p_code) {
	static const char *functions[] = { "textureGrad(", "textureLod(", "texelFetch(", "texture(" };
	String result = p_code;
	int from = 0;
	while (true) {
		const int sampler = result.find("sampler2D(depth_buffer", from);
		if (sampler < 0) {
			break;
		}
		int start = -1;
		for (const char *function : functions) {
			const int length = String(function).length();
			if (sampler >= length && result.substr(sampler - length, length) == function) {
				start = sampler - length;
				break;
			}
		}
		if (start < 0) {
			from = sampler + 1;
			continue;
		}
		// The end of the call.
		int depth = 0;
		int i = result.find("(", start);
		for (; i < result.length(); i++) {
			if (result[i] == '(') {
				depth++;
			} else if (result[i] == ')' && --depth == 0) {
				break;
			}
		}
		if (i >= result.length()) {
			break;
		}
		result = result.substr(0, start) + "vec4(rt_depth_buffer_value())" + result.substr(i + 1);
		from = start;
	}
	return result;
}

// Ray tracing stages read mip 0 with texture(): its calls in fragment code become textureGrad() with the screen
// derivatives of the coordinates (dFdx() is defined by the hit shaders). Shadow and other samplers are left as they are.
static String _texture_calls_with_gradients(const String &p_code) {
	String result;
	int from = 0;
	while (true) {
		int start = p_code.find("texture(", from);
		if (start < 0) {
			result += p_code.substr(from);
			break;
		}
		const char32_t before = start > 0 ? p_code[start - 1] : U' ';
		if (is_ascii_identifier_char(before)) {
			result += p_code.substr(from, start + 8 - from);
			from = start + 8;
			continue;
		}
		// Split the arguments at the top level.
		Vector<String> args;
		int depth = 0;
		int arg_begin = start + 8;
		int i = arg_begin;
		for (; i < p_code.length(); i++) {
			const char32_t c = p_code[i];
			if (c == '(' || c == '[') {
				depth++;
			} else if (c == ')' || c == ']') {
				if (depth == 0) {
					break;
				}
				depth--;
			} else if (c == ',' && depth == 0) {
				args.push_back(p_code.substr(arg_begin, i - arg_begin).strip_edges());
				arg_begin = i + 1;
			}
		}
		if (i >= p_code.length()) {
			result += p_code.substr(from);
			break;
		}
		args.push_back(p_code.substr(arg_begin, i - arg_begin).strip_edges());

		String grad_swizzle;
		bool supported = args.size() == 2 || args.size() == 3;
		if (args[0].begins_with("sampler2DArray(") || args[0].begins_with("usampler2DArray(") || args[0].begins_with("isampler2DArray(")) {
			grad_swizzle = ".xy";
		} else if (args[0].begins_with("sampler2D(") || args[0].begins_with("usampler2D(") || args[0].begins_with("isampler2D(") ||
				args[0].begins_with("sampler3D(") || args[0].begins_with("samplerCube(")) {
			grad_swizzle = "";
		} else {
			supported = false;
		}
		result += p_code.substr(from, start - from);
		if (!supported) {
			result += p_code.substr(start, i + 1 - start);
		} else {
			const String coord = _texture_calls_with_gradients(args[1]);
			// A bias scales the footprint the way it offsets the LOD.
			const String scale = args.size() == 3 ? " * exp2(" + _texture_calls_with_gradients(args[2]) + ")" : String();
			result += "textureGrad(" + _texture_calls_with_gradients(args[0]) + ", " + coord + ", dFdx(" + coord + ")" + grad_swizzle + scale + ", dFdy(" + coord + ")" + grad_swizzle + scale + ")";
		}
		from = i + 1;
	}
	return result;
}

// textureQueryLod() needs implicit derivatives, which ray tracing stages do not have (it only exists in fragment shaders):
// a material calling it failed to compile as a hit group (godot3d's tree impostors). Its calls on 2D, 2D array and 3D
// samplers become rt_query_lod() (raytracing_custom_globals_inc.glsl), the level of detail computed as the GLSL
// specification defines it (section 8.14.1, "Scale Factor and Level of Detail") from the screen derivatives the hit
// shaders already reconstruct for texture(). Other samplers are left as they are.
static String _texture_query_lod_calls(const String &p_code) {
	static const String function = "textureQueryLod(";
	String result;
	int from = 0;
	while (true) {
		const int start = p_code.find(function, from);
		if (start < 0) {
			result += p_code.substr(from);
			break;
		}
		const char32_t before = start > 0 ? p_code[start - 1] : U' ';
		if (is_ascii_identifier_char(before)) {
			result += p_code.substr(from, start + function.length() - from);
			from = start + function.length();
			continue;
		}
		// Split the arguments at the top level.
		Vector<String> args;
		int depth = 0;
		int arg_begin = start + function.length();
		int i = arg_begin;
		for (; i < p_code.length(); i++) {
			const char32_t c = p_code[i];
			if (c == '(' || c == '[') {
				depth++;
			} else if (c == ')' || c == ']') {
				if (depth == 0) {
					break;
				}
				depth--;
			} else if (c == ',' && depth == 0) {
				args.push_back(p_code.substr(arg_begin, i - arg_begin).strip_edges());
				arg_begin = i + 1;
			}
		}
		if (i >= p_code.length()) {
			result += p_code.substr(from);
			break;
		}
		args.push_back(p_code.substr(arg_begin, i - arg_begin).strip_edges());

		String size;
		bool supported = args.size() == 2;
		if (supported && (args[0].begins_with("sampler2D(") || args[0].begins_with("usampler2D(") || args[0].begins_with("isampler2D("))) {
			size = "vec2(textureSize(" + args[0] + ", 0))";
		} else if (supported && (args[0].begins_with("sampler2DArray(") || args[0].begins_with("usampler2DArray(") || args[0].begins_with("isampler2DArray("))) {
			size = "vec2(textureSize(" + args[0] + ", 0).xy)";
		} else if (supported && args[0].begins_with("sampler3D(")) {
			size = "vec3(textureSize(" + args[0] + ", 0))";
		} else {
			supported = false;
		}
		result += p_code.substr(from, start - from);
		if (!supported) {
			result += p_code.substr(start, i + 1 - start);
		} else {
			const String coord = _texture_query_lod_calls(args[1]);
			result += "rt_query_lod(" + size + ", dFdx(" + coord + "), dFdy(" + coord + "), textureQueryLevels(" + args[0] + "))";
		}
		from = i + 1;
	}
	return result;
}

void SceneShaderRaytracing::_strip_texture_globals(String &r_globals, const String &p_tex_name) {
	const String decl_marker = " " + _uniform_identifier(p_tex_name) + ";";
	int pos = r_globals.find(decl_marker);
	if (pos < 0) {
		return;
	}
	int line_start = r_globals.rfind("\n", pos);
	line_start = (line_start < 0) ? 0 : line_start + 1;
	int line_end = r_globals.find("\n", pos);
	if (line_end < 0) {
		line_end = r_globals.length();
	} else {
		line_end += 1;
	}
	r_globals = r_globals.substr(0, line_start) + r_globals.substr(line_end);
}

bool SceneShaderRaytracing::_finalize_uniforms_with_textures(
		CustomShaderEntry &r_entry,
		const ShaderCompiler::GeneratedCode &p_gen_code,
		const HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> &p_uniforms,
		bool p_strip_intersection_globals) {
	// ShaderCompiler rounds uniform_total_size to 16 bytes for UBO requirements,
	// but our buffer_reference struct uses std140 member layout without UBO-level padding.
	// Recompute the raw (unrounded) end of the last uniform member.
	uint32_t raw_uniform_end = 0;
	for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &kv : p_uniforms) {
		const ShaderLanguage::ShaderNode::Uniform &uu = kv.value;
		// Instance uniforms are not in the material's buffer (their order is left at 0).
		if (ShaderLanguage::is_sampler_type(uu.type) || uu.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE || uu.order < 0 || uu.order >= (int)p_gen_code.uniform_offsets.size()) {
			continue;
		}
		// Sizes as ShaderCompiler lays them out: a global is an index, an array is padded to 16 bytes per element.
		uint32_t size = ShaderLanguage::get_datatype_size(uu.type);
		if (uu.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
			size = ShaderLanguage::get_datatype_size(ShaderLanguage::TYPE_UINT);
		} else if (uu.array_size > 0) {
			const uint32_t padded = 16 * uu.array_size;
			size *= uu.array_size;
			if (size % padded != 0) {
				size += padded - (size % padded);
			}
		}
		uint32_t end = p_gen_code.uniform_offsets[uu.order] + size;
		if (end > raw_uniform_end) {
			raw_uniform_end = end;
		}
	}

	for (int ti = 0; ti < p_gen_code.texture_uniforms.size(); ti++) {
		const ShaderCompiler::GeneratedCode::Texture &tex = p_gen_code.texture_uniforms[ti];
		if (tex.name.is_empty()) {
			continue;
		}
		// The bindless tables hold float 2D textures and 2D texture arrays.
		if ((tex.type != ShaderLanguage::TYPE_SAMPLER2D && tex.type != ShaderLanguage::TYPE_SAMPLER2DARRAY) || tex.array_size > 0) {
			WARN_PRINT(vformat("RT: texture uniform '%s' has a type path tracing cannot bind; the material falls back to the default.", tex.name));
			return false;
		}

		_strip_texture_globals(r_entry.fragment_globals, tex.name);
		if (p_strip_intersection_globals) {
			_strip_texture_globals(r_entry.intersection_globals, tex.name);
		}

		// Append a bindless-index uint member to the uniform buffer.
		uint32_t offset = raw_uniform_end;
		if (offset % 4 != 0) {
			offset += 4 - (offset % 4);
		}

		TextureUniformInfo tui;
		tui.name = tex.name;
		tui.hint = tex.hint;
		tui.use_color = tex.use_color;
		tui.is_global = tex.global;
		tui.is_array = tex.type == ShaderLanguage::TYPE_SAMPLER2DARRAY;
		tui.buffer_offset = offset;
		r_entry.texture_uniforms.push_back(tui);

		if (tex.hint == ShaderLanguage::ShaderNode::Uniform::HINT_ALPHA) {
			if (r_entry.alpha_texture_buffer_offset != UINT32_MAX) {
				WARN_PRINT(vformat("Custom RT shader has multiple hint_alpha textures; '%s' will be ignored. Only one hint_alpha texture is supported for ray query alpha testing.", tex.name));
			} else {
				r_entry.alpha_texture_buffer_offset = offset;
			}
		}

		r_entry.uniform_members += "uint " + _uniform_identifier(tex.name) + ";\n";
		raw_uniform_end = offset + 4;
	}

	r_entry.uniform_total_size = raw_uniform_end;
	if (r_entry.uniform_total_size % 16 != 0) {
		r_entry.uniform_total_size += 16 - (r_entry.uniform_total_size % 16);
	}
	return true;
}

// Slot table: append-only by 128-bit source hash (stable across reloads).

uint32_t SceneShaderRaytracing::register_custom_shader(uint32_t /*p_shader_id*/, RID p_material, bool *r_failed) {
	return _register_slot(/*p_shader_id*/ 0, p_material, /*p_is_procedural=*/false, r_failed);
}

uint32_t SceneShaderRaytracing::register_procedural_shader(uint32_t /*p_shader_id*/, RID p_material) {
	return _register_slot(/*p_shader_id*/ 0, p_material, /*p_is_procedural=*/true);
}

uint32_t SceneShaderRaytracing::_register_slot(uint32_t /*p_shader_id*/, RID p_material, bool p_is_procedural, bool *r_failed) {
	if (r_failed) {
		*r_failed = false;
	}
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	SourceHash128 hash;
	hash.a = material_storage->material_get_shader_code_rt_hash(p_material);
	hash.b = material_storage->material_get_shader_code_rt_hash_b(p_material);
	if (hash.is_zero()) {
		return 0;
	}

	HashMap<SourceHash128, uint32_t, SourceHash128Hasher>::Iterator it = source_hash_to_slot.find(hash);
	if (it != source_hash_to_slot.end()) {
		if (r_failed) {
			*r_failed = hit_group_slots[it->value].state == HGState::Failed;
		}
		return it->value;
	}

	// New slot: preprocess here; heavy compile is on worker.
	CustomShaderEntry entry;
	entry.is_procedural = p_is_procedural;
	entry.name = material_storage->material_get_shader_path_hint(p_material);
	if (!_preprocess_shader(p_material, p_is_procedural, entry)) {
		// Failed slot still occupies an index so hash lookups stay stable.
		HitGroupSlot failed_slot;
		failed_slot.source_hash = hash;
		failed_slot.state = HGState::Failed;
		uint32_t failed_index = (uint32_t)hit_group_slots.size();
		hit_group_slots.push_back(failed_slot);
		source_hash_to_slot.insert(hash, failed_index);
		for (KeyValue<uint32_t, PipelineBundle> &kv : pipeline_bundles) {
			_bundle_resize_for_slots(kv.value);
			kv.value.per_hg_states[failed_index] = HGState::Failed;
		}
		if (r_failed) {
			*r_failed = true;
		}
		return 0;
	}
	// Empty custom HG source uses slot 0 (default).
	if (!p_is_procedural && entry.fragment_code.is_empty() && entry.vertex_code.is_empty()) {
		return 0;
	}
	if (p_is_procedural && entry.intersection_code.is_empty()) {
		return 0;
	}

	HitGroupSlot slot;
	slot.source_hash = hash;
	slot.state = HGState::Ready;
	slot.entry = entry;
	uint32_t slot_index = (uint32_t)hit_group_slots.size();
	hit_group_slots.push_back(slot);
	source_hash_to_slot.insert(hash, slot_index);

	for (KeyValue<uint32_t, PipelineBundle> &kv : pipeline_bundles) {
		PipelineBundle &bundle = kv.value;
		_bundle_resize_for_slots(bundle);
		bundle.dirty = true;
	}

	return slot_index;
}

bool SceneShaderRaytracing::_preprocess_shader(RID p_material, bool p_is_procedural, CustomShaderEntry &r_entry) {
	String code = RendererRD::MaterialStorage::get_singleton()->material_get_shader_code_rt(p_material);
	if (code.is_empty()) {
		return false;
	}

	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	if (p_is_procedural) {
		actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
		actions.entry_point_stages["intersection"] = ShaderCompiler::STAGE_INTERSECTION;
	}

	bool detected_alpha_clip = false;
	actions.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &detected_alpha_clip;
	actions.usage_flag_pointers["ALPHA_HASH_SCALE"] = &detected_alpha_clip;
	bool writes_vertex = false;
	actions.write_flag_pointers["VERTEX"] = &writes_vertex;
	bool writes_modelview = false;
	actions.write_flag_pointers["MODELVIEW_MATRIX"] = &writes_modelview;

	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniform_sink;
	actions.uniforms = &uniform_sink;

	ShaderCompiler::GeneratedCode gen_code;
	Error err = compiler.compile(RSE::SHADER_SPATIAL, code, &actions, String(), gen_code);
	if (err != OK) {
		WARN_PRINT(vformat("RT: Failed to preprocess %s shader; falling back to default material.",
				p_is_procedural ? String("procedural") : String("custom")));
		return false;
	}

	r_entry.is_procedural = p_is_procedural;
	r_entry.uses_alpha_clip = detected_alpha_clip;
	if (!p_is_procedural) {
		const RTAlphaDependencies alpha_dependencies = rt_alpha_dependencies(compiler.get_parsed_shader());
		r_entry.alpha_bakeable = alpha_dependencies.bakeable;
		r_entry.alpha_reads_color = alpha_dependencies.reads_color;
	}
	r_entry.writes_vertex = (writes_vertex || writes_modelview) && !p_is_procedural;
	r_entry.writes_modelview = writes_modelview && !p_is_procedural;
	r_entry.uses_vertex_time = gen_code.uses_vertex_time;
	r_entry.varyings = gen_code.varyings;
	// The render modes the hit group and displacement templates test. Without DO_SIDE_CHECK a double-sided material's
	// back face kept its normal, without SPECULAR_DISABLED and MODE_UNSHADED those render modes did nothing. The
	// CUSTOM0-3 and INSTANCE_CUSTOM defines make the hit group fetch them for vertex().
	for (const String &define : gen_code.defines) {
		if (define.contains("SKIP_TRANSFORM_USED") || define.contains("VERTEX_WORLD_COORDS_USED") || define.contains("DO_SIDE_CHECK") ||
				define.contains("SPECULAR_DISABLED") || define.contains("MODE_UNSHADED") || define.contains("ENABLE_INSTANCE_CUSTOM") ||
				define.contains("CUSTOM0_USED") || define.contains("CUSTOM1_USED") || define.contains("CUSTOM2_USED") || define.contains("CUSTOM3_USED")) {
			r_entry.defines += define;
		}
	}
	r_entry.vertex_code = gen_code.code.has("vertex") ? gen_code.code["vertex"] : String();
	r_entry.fragment_code = _texture_query_lod_calls(_texture_calls_with_gradients(_depth_buffer_reads(gen_code.code.has("fragment") ? gen_code.code["fragment"] : String())));
	// The functions vertex() calls too: their derivatives are 0 there, raster's vertex() reads mip 0 as well.
	r_entry.fragment_globals = _texture_query_lod_calls(_texture_calls_with_gradients(_depth_buffer_reads(gen_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT])));
	// Hit shaders cannot discard: a discarded hit is ignored by the any-hit shader, which every material that discards
	// gets, and passed through by the closest hit (raytracing_custom_fragment_inc.glsl).
	if (r_entry.fragment_code.contains("discard;") || r_entry.fragment_globals.contains("discard;")) {
		r_entry.uses_alpha_clip = true;
		r_entry.fragment_code = r_entry.fragment_code.replace("discard;", "{ rt_discarded = true; }");
		r_entry.fragment_globals = r_entry.fragment_globals.replace("discard;", "{ rt_discarded = true; }");
	}
	if (p_is_procedural) {
		r_entry.intersection_code = gen_code.code.has("intersection") ? gen_code.code["intersection"] : String();
		r_entry.intersection_globals = gen_code.stage_globals[ShaderCompiler::STAGE_INTERSECTION];
	}
	for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &kv : uniform_sink) {
		if (kv.value.array_size <= 0 || ShaderLanguage::is_sampler_type(kv.value.type) || kv.value.scope != ShaderLanguage::ShaderNode::Uniform::SCOPE_LOCAL) {
			continue;
		}
		const String member = _uniform_identifier(kv.key);
		r_entry.vertex_code = _clamp_uniform_array_indices(r_entry.vertex_code, member, kv.value.array_size);
		r_entry.fragment_code = _clamp_uniform_array_indices(r_entry.fragment_code, member, kv.value.array_size);
		r_entry.fragment_globals = _clamp_uniform_array_indices(r_entry.fragment_globals, member, kv.value.array_size);
		r_entry.intersection_code = _clamp_uniform_array_indices(r_entry.intersection_code, member, kv.value.array_size);
		r_entry.intersection_globals = _clamp_uniform_array_indices(r_entry.intersection_globals, member, kv.value.array_size);
	}
	// Conservative: any view, projection or scene data read in vertex() or the functions it may call.
	for (const char *camera_identifier : { "read_view_matrix", "inv_view_matrix", "projection_matrix", "rt_modelview", "read_viewport_size", "scene_data_block" }) {
		if (r_entry.vertex_code.contains(camera_identifier) || r_entry.fragment_globals.contains(camera_identifier)) {
			r_entry.vertex_reads_camera = true;
		}
	}
	r_entry.uniform_members = gen_code.uniforms;
	r_entry.uniform_total_size = gen_code.uniform_total_size;
	r_entry.uniform_offsets = gen_code.uniform_offsets;
	r_entry.uniforms = uniform_sink;
	return _finalize_uniforms_with_textures(r_entry, gen_code, uniform_sink, /*strip_intersection_globals=*/p_is_procedural);
}

void SceneShaderRaytracing::finalize_custom_shaders() {
	async_compilation_enabled = GLOBAL_GET_CACHED(bool, "rendering/pathtracing/async_shader_compilation");

	_kick_rebuild_if_idle(); // Async dispatch only; sync drains below.

	if (!async_compilation_enabled) {
		_drain_lane_inline_main_thread();
	}
}

const SceneShaderRaytracing::CustomShaderEntry *SceneShaderRaytracing::get_custom_shader_entry(uint32_t p_slot_index) const {
	if (p_slot_index == 0 || p_slot_index >= hit_group_slots.size()) {
		return nullptr;
	}
	const HitGroupSlot &slot = hit_group_slots[p_slot_index];
	if (slot.state == HGState::Failed) {
		return nullptr;
	}
	return &slot.entry;
}

// The #defines that read a material's textures from the bindless tables, by their indices in its uniform buffer.
static String _texture_defines(const Vector<SceneShaderRaytracing::TextureUniformInfo> &p_texture_uniforms) {
	String tex_defines;
	for (const SceneShaderRaytracing::TextureUniformInfo &tui : p_texture_uniforms) {
		const String id = _uniform_identifier(tui.name);
		tex_defines += "#define " + id + (tui.is_array ? " bindless_texture_arrays" : " bindless_textures") + "[nonuniformEXT(material." + id + ")]\n";
	}
	return tex_defines;
}

String SceneShaderRaytracing::_displace_source(const CustomShaderEntry &p_entry) const {
	String source = displace_template;
	source = source.replace("/* RT_CUSTOM_DEFINES */", p_entry.defines + (p_entry.writes_modelview ? "\n#define RT_MODELVIEW_WRITTEN\n" : ""));
	source = source.replace("/* RT_CUSTOM_FRAGMENT_GLOBALS */", p_entry.fragment_globals);
	source = source.replace("/* RT_CUSTOM_UNIFORM_MEMBERS */", p_entry.uniform_members.is_empty() ? String("float _rt_pad;") : p_entry.uniform_members);
	source = source.replace("/* RT_CUSTOM_TEXTURE_DEFINES */", _texture_defines(p_entry.texture_uniforms));
	source = source.replace("/* RT_CUSTOM_VERTEX_FUNCTION */", "void rt_run_vertex_shader() {\n" + p_entry.vertex_code + "\n}\n");
	source = source.replace("/* RT_CUSTOM_VERTEX_CALL */", "rt_run_vertex_shader();");
	return _apply_varying_code(source, p_entry.varyings);
}

String SceneShaderRaytracing::_omm_bake_source(const CustomShaderEntry *p_entry) const {
	String source = omm_bake_template;
	if (p_entry) {
		source = source.replace("/* RT_CUSTOM_DEFINES */", p_entry->defines);
		source = source.replace("/* RT_CUSTOM_FRAGMENT_GLOBALS */", p_entry->fragment_globals);
		source = source.replace("/* RT_CUSTOM_UNIFORM_MEMBERS */", p_entry->uniform_members.is_empty() ? String("float _rt_pad;") : p_entry->uniform_members);
		source = source.replace("/* RT_CUSTOM_TEXTURE_DEFINES */", _texture_defines(p_entry->texture_uniforms));
		source = source.replace("/* RT_CUSTOM_VERTEX_FUNCTION */", "void rt_run_vertex_shader() {\n" + p_entry->vertex_code + "\n}\n");
		source = source.replace("/* RT_CUSTOM_VERTEX_CALL */", "rt_run_vertex_shader();");
		source = source.replace("/* RT_CUSTOM_FRAGMENT_CODE */", p_entry->fragment_code);
		source = _apply_varying_code(source, p_entry->varyings);
	} else {
		// Hit group 0's alpha. The template's defines are expanded already: the define goes after #version.
		const int version_end = source.find("\n", source.find("#version"));
		source = source.insert(version_end + 1, "#define RT_OMM_BAKE_STANDARD\n");
	}
	return source;
}

bool SceneShaderRaytracing::_create_compute_program(const String &p_source, const String &p_name, RID &r_shader, RID &r_pipeline) const {
	const Vector<uint8_t> binary = _compile_compute_binary(p_source, p_name);
	if (binary.is_empty()) {
		return false;
	}
	r_shader = RD::get_singleton()->shader_create_from_bytecode(binary);
	ERR_FAIL_COND_V(!r_shader.is_valid(), false);
	r_pipeline = RD::get_singleton()->compute_pipeline_create(r_shader);
	ERR_FAIL_COND_V(!r_pipeline.is_valid(), false);
	return true;
}

static void _warn_program_failed(bool p_omm_bake, uint32_t p_slot_index) {
	if (p_omm_bake) {
		WARN_PRINT(vformat("RT: the opacity micromap bake of hit group %d failed to compile; its surfaces keep their any-hit tests.", p_slot_index));
	} else {
		WARN_PRINT(vformat("RT: the vertex() of hit group %d failed to compile for displacement; its surfaces are traced undisplaced.", p_slot_index));
	}
}

void SceneShaderRaytracing::_program_build_worker(void *p_userdata) {
	ProgramBuild *build = static_cast<ProgramBuild *>(p_userdata);
	if (singleton) {
		build->succeeded = singleton->_create_compute_program(build->source, build->name, build->shader, build->pipeline);
	}
}

void SceneShaderRaytracing::_start_program_build(LocalVector<DisplaceProgram> &r_programs, bool p_omm_bake, uint32_t p_slot_index, const String &p_source, const String &p_name) {
	if (r_programs.size() < hit_group_slots.size()) {
		r_programs.resize(hit_group_slots.size());
	}
	DisplaceProgram &program = r_programs[p_slot_index];
	if (program.pipeline.is_valid() || program.failed || program.build) {
		return;
	}
	ProgramBuild *build = memnew(ProgramBuild);
	build->source = p_source;
	build->name = p_name;
	build->omm_bake = p_omm_bake;
	build->slot = p_slot_index;
	build->task = WorkerThreadPool::get_singleton()->add_native_task(&SceneShaderRaytracing::_program_build_worker, build, false, "RT program precompile");
	program.build = build;
	program_builds.push_back(build);
}

void SceneShaderRaytracing::_finish_program_build(ProgramBuild *p_build) {
	if (WorkerThreadPool::get_singleton()) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(p_build->task);
	}
	DisplaceProgram &program = (p_build->omm_bake ? omm_bake_programs : displace_programs)[p_build->slot];
	program.build = nullptr;
	if (p_build->succeeded) {
		program.shader = p_build->shader;
		program.pipeline = p_build->pipeline;
	} else {
		program.failed = true;
		if (p_build->shader.is_valid()) {
			RD::get_singleton()->free_rid(p_build->shader);
		}
		_warn_program_failed(p_build->omm_bake, p_build->slot);
	}
	program_builds.erase(p_build);
	memdelete(p_build);
}

void SceneShaderRaytracing::precompile_slot_programs(uint32_t p_slot_index, bool p_omm_bake) {
	// Slot 0: BaseMaterial3D's, which only has a bake program.
	const CustomShaderEntry *entry = p_slot_index != 0 ? get_custom_shader_entry(p_slot_index) : nullptr;
	if (p_slot_index != 0 && !entry) {
		return;
	}
	// Called again for every surface cache built (some instances rebuild theirs every frame): test the program's state
	// before assembling its source, a dozen String::replace() over the whole template.
	auto started = [p_slot_index](const LocalVector<DisplaceProgram> &p_programs) {
		if (p_slot_index >= p_programs.size()) {
			return false;
		}
		const DisplaceProgram &program = p_programs[p_slot_index];
		return program.pipeline.is_valid() || program.failed || program.build != nullptr;
	};
	if (entry && entry->writes_vertex && !displace_template.is_empty() && !started(displace_programs)) {
		_start_program_build(displace_programs, false, p_slot_index, _displace_source(*entry), "RT displace");
	}
	if (p_omm_bake && (!entry || entry->alpha_bakeable) && !omm_bake_template.is_empty() && !started(omm_bake_programs)) {
		_start_program_build(omm_bake_programs, true, p_slot_index, _omm_bake_source(entry), "RT OMM bake");
	}
}

RID SceneShaderRaytracing::get_displace_pipeline(uint32_t p_slot_index, RID &r_shader) {
	const CustomShaderEntry *entry = get_custom_shader_entry(p_slot_index);
	if (!entry || !entry->writes_vertex || displace_template.is_empty()) {
		return RID();
	}
	if (displace_programs.size() < hit_group_slots.size()) {
		displace_programs.resize(hit_group_slots.size());
	}
	if (displace_programs[p_slot_index].build) {
		_finish_program_build(displace_programs[p_slot_index].build);
	}
	DisplaceProgram &program = displace_programs[p_slot_index];
	if (program.pipeline.is_valid() || program.failed) {
		r_shader = program.shader;
		return program.pipeline;
	}
	program.failed = true;
	if (!_create_compute_program(_displace_source(*entry), "RT displace", program.shader, program.pipeline)) {
		if (program.shader.is_valid()) {
			RD::get_singleton()->free_rid(program.shader);
			program.shader = RID();
		}
		_warn_program_failed(false, p_slot_index);
		return RID();
	}
	program.failed = false;
	r_shader = program.shader;
	return program.pipeline;
}

RID SceneShaderRaytracing::get_omm_bake_pipeline(uint32_t p_slot_index, RID &r_shader) {
	if (omm_bake_template.is_empty()) {
		return RID();
	}
	const CustomShaderEntry *entry = nullptr;
	if (p_slot_index != 0) {
		entry = get_custom_shader_entry(p_slot_index);
		if (!entry || !entry->alpha_bakeable) {
			return RID();
		}
	}
	if (omm_bake_programs.size() < hit_group_slots.size()) {
		omm_bake_programs.resize(hit_group_slots.size());
	}
	if (omm_bake_programs[p_slot_index].build) {
		_finish_program_build(omm_bake_programs[p_slot_index].build);
	}
	DisplaceProgram &program = omm_bake_programs[p_slot_index];
	if (program.pipeline.is_valid() || program.failed) {
		r_shader = program.shader;
		return program.pipeline;
	}
	program.failed = true;
	if (!_create_compute_program(_omm_bake_source(entry), "RT OMM bake", program.shader, program.pipeline)) {
		if (program.shader.is_valid()) {
			RD::get_singleton()->free_rid(program.shader);
			program.shader = RID();
		}
		_warn_program_failed(true, p_slot_index);
		return RID();
	}
	program.failed = false;
	r_shader = program.shader;
	return program.pipeline;
}

bool SceneShaderRaytracing::is_hg_ready_in_bundle(uint32_t p_slot_index, uint32_t p_rt_flags) const {
	if (p_slot_index == 0) {
		return true;
	}
	if (p_slot_index >= hit_group_slots.size()) {
		return false;
	}
	HashMap<uint32_t, PipelineBundle>::ConstIterator it = pipeline_bundles.find(p_rt_flags);
	if (it == pipeline_bundles.end()) {
		return false;
	}
	const PipelineBundle &b = it->value;
	if (p_slot_index >= b.live_hg_count) {
		return false;
	}
	if (p_slot_index >= b.live_ready_mask.size()) {
		return false;
	}
	return b.live_ready_mask[p_slot_index];
}

void SceneShaderRaytracing::request_hit_group(uint32_t p_slot_index, float p_distance) {
	if (p_slot_index == 0 || p_slot_index >= hit_group_slots.size()) {
		return;
	}
	HitGroupSlot &slot = hit_group_slots[p_slot_index];
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	if (!slot.requested || slot.requested_frame != frame || p_distance < slot.requested_distance) {
		slot.requested_distance = p_distance;
	}
	slot.requested = true;
	slot.requested_frame = frame;
}

uint32_t SceneShaderRaytracing::compute_rt_flags(RID p_environment) {
	uint32_t flags = RT_FLAG_NONE;

	if (p_environment.is_valid()) {
		RendererEnvironmentStorage *env_storage = RendererEnvironmentStorage::get_singleton();

		if (env_storage->environment_get_pathtracing_debug_mode(p_environment) != 0) {
			flags |= RT_FLAG_DEBUG_VIS_ENABLED;
		}

		if (GLOBAL_GET("rendering/pathtracing/use_simple_shadows")) {
			flags |= RT_FLAG_RAY_QUERY_SHADOWS_ENABLED;
		}

		const RSE::PathtracingDenoiser denoiser = env_storage->environment_get_pathtracing_denoiser(p_environment);
		if (denoiser == RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION) {
			flags |= RT_FLAG_DLSS_RR_ENABLED;
		} else if (denoiser == RSE::PT_DENOISER_NRD_REBLUR) {
			flags |= RT_FLAG_NRD_ENABLED;
		}
	}

	// Only where the shaders can be translated with it: a driver without it (D3D12 for now) fails the whole pipeline.
	static const bool use_ser = GLOBAL_GET("rendering/pathtracing/use_shader_execution_reordering") && RD::get_singleton()->has_feature(RD::SUPPORTS_SHADER_EXECUTION_REORDERING);
	if (use_ser) {
		flags |= RT_FLAG_SER_ENABLED;
	}

	static const bool use_opacity_micromaps = RD::get_singleton()->has_feature(RD::SUPPORTS_OPACITY_MICROMAP);
	if (use_opacity_micromaps) {
		flags |= RT_FLAG_OPACITY_MICROMAPS;
	}

	return flags;
}

SceneShaderRaytracing *SceneShaderRaytracing::get_singleton() {
	if (singleton == nullptr) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		if (singleton == nullptr) {
			memnew(SceneShaderRaytracing);
		}
	}
	return singleton;
}

// Binary cache of the programs built from material code (binary_cache_dir).

static const char *rt_binary_cache_header = "GDRT";
static const uint32_t rt_binary_cache_version = 1;

String SceneShaderRaytracing::_binary_cache_key(const String &p_name, const Vector<String> &p_sources, const String &p_extra) {
	StringBuilder key;
	key.append("[GodotVersionNumber]");
	key.append(GODOT_VERSION_NUMBER);
	key.append("[GodotVersionHash]");
	key.append(GODOT_VERSION_HASH);
	key.append("[DebugInfo]");
	key.append(Engine::get_singleton()->is_generate_spirv_debug_info_enabled() ? "1" : "0");
	key.append("[Name]");
	key.append(p_name);
	for (int i = 0; i < p_sources.size(); i++) {
		key.append("[Stage" + itos(i) + "]");
		key.append(p_sources[i]);
	}
	key.append("[Extra]");
	key.append(p_extra);
	return key.as_string().sha256_text();
}

String SceneShaderRaytracing::get_binary_cache_file_name(const String &p_key, const String &p_api) {
	return p_key + "." + p_api + ".cache";
}

Vector<uint8_t> SceneShaderRaytracing::make_binary_cache_file(const Vector<uint8_t> &p_binary) {
	Vector<uint8_t> bytes;
	bytes.resize(12 + p_binary.size());
	uint8_t *w = bytes.ptrw();
	memcpy(w, rt_binary_cache_header, 4);
	encode_uint32(rt_binary_cache_version, w + 4);
	encode_uint32(p_binary.size(), w + 8);
	memcpy(w + 12, p_binary.ptr(), p_binary.size());
	return bytes;
}

Vector<uint8_t> SceneShaderRaytracing::_binary_cache_load(const String &p_key) const {
	const String file_name = get_binary_cache_file_name(p_key, binary_cache_api);
	Ref<FileAccess> f;
	if (!binary_cache_dir.is_empty()) {
		f = FileAccess::open(binary_cache_dir.path_join(file_name), FileAccess::READ);
	}
	if (f.is_null() && !binary_cache_res_dir.is_empty()) {
		f = FileAccess::open(binary_cache_res_dir.path_join(file_name), FileAccess::READ);
	}
	if (f.is_null()) {
		return Vector<uint8_t>();
	}
	char header[5] = {};
	f->get_buffer((uint8_t *)header, 4);
	if (String(header) != rt_binary_cache_header || f->get_32() != rt_binary_cache_version) {
		return Vector<uint8_t>();
	}
	const uint32_t size = f->get_32();
	if (size == 0 || uint64_t(size) > f->get_length() - f->get_position()) {
		return Vector<uint8_t>();
	}
	Vector<uint8_t> binary;
	binary.resize(size);
	if (f->get_buffer(binary.ptrw(), size) != size) {
		return Vector<uint8_t>();
	}
	return binary;
}

void SceneShaderRaytracing::_binary_cache_save(const String &p_key, const Vector<uint8_t> &p_binary) const {
	if (binary_cache_dir.is_empty() || p_binary.is_empty()) {
		return;
	}
	const String path = binary_cache_dir.path_join(get_binary_cache_file_name(p_key, binary_cache_api));
	// Written aside, then renamed: a crash while writing leaves no truncated entry.
	const String temporary_path = path + "." + itos(Thread::get_caller_id()) + ".tmp";
	{
		Ref<FileAccess> f = FileAccess::open(temporary_path, FileAccess::WRITE);
		ERR_FAIL_COND_MSG(f.is_null(), "Can't write the ray tracing shader cache: " + temporary_path);
		f->store_buffer((const uint8_t *)rt_binary_cache_header, 4);
		f->store_32(rt_binary_cache_version);
		f->store_32(p_binary.size());
		f->store_buffer(p_binary.ptr(), p_binary.size());
	}
	Ref<DirAccess> d = DirAccess::open(binary_cache_dir);
	ERR_FAIL_COND(d.is_null());
	if (d->rename(temporary_path, path) != OK) {
		d->remove(temporary_path);
	}
}

Vector<uint8_t> SceneShaderRaytracing::_compile_compute_binary(const String &p_source, const String &p_name) const {
	Vector<String> sources;
	sources.resize(RD::SHADER_STAGE_MAX);
	sources.write[RD::SHADER_STAGE_COMPUTE] = p_source;
	const String key = _binary_cache_key(p_name, sources);
	Vector<uint8_t> binary = _binary_cache_load(key);
	if (!binary.is_empty()) {
		return binary;
	}
	const Vector<RD::ShaderStageSPIRVData> stages = ShaderRD::compile_stages(sources, {});
	if (stages.is_empty()) {
		return Vector<uint8_t>();
	}
	binary = RD::get_singleton()->shader_compile_binary_from_spirv(stages, p_name);
	_binary_cache_save(key, binary);
	return binary;
}

// Per-rt_flags base SPIR-V + HG templates (immutable cache for worker tasks).

bool SceneShaderRaytracing::_build_variant_compile_context(uint32_t p_rt_flags, VariantCompileContext &r_ctx) {
	ERR_FAIL_COND_V(!raygen_shader_version.is_valid(), false);

	int variant = (int)_raygen_variant_from_rt_flags(p_rt_flags);

	Vector<String> sources = raygen_shader.version_build_variant_stage_sources(raygen_shader_version, variant);
	ERR_FAIL_COND_V(sources.is_empty(), false);

	auto inject_define = [](Vector<String> &p_sources, const String &p_define) {
		for (int i = 0; i < p_sources.size(); i++) {
			if (p_sources[i].is_empty()) {
				continue;
			}
			int ver_pos = p_sources[i].find("#version");
			if (ver_pos >= 0) {
				int nl = p_sources[i].find("\n", ver_pos);
				if (nl >= 0) {
					p_sources.write[i] = p_sources[i].insert(nl + 1, p_define);
				}
			}
		}
	};

	// No ENABLE_INTERSECTION_SHADERS in the base: hit group 0 and the custom triangle hit groups are triangle hit groups,
	// whose any-hit and closest-hit shaders must use BuiltInTriangleIntersectionAttributes (DXR spec, "Intersection
	// attributes structure"). Procedural hit groups get it (_insert_after_version()), so that all their shaders use the
	// intersection shader's structure, as the same section requires.

	if (p_rt_flags & RT_FLAG_DEBUG_VIS_ENABLED) {
		inject_define(sources, "#define RT_DEBUG_ENABLED\n");
	}

	// Procedural templates come from intersection stage; strip from base HG0 path.
	String intersection_source_template;
	if (sources.size() > RD::SHADER_STAGE_INTERSECTION) {
		intersection_source_template = sources[RD::SHADER_STAGE_INTERSECTION];
		sources.write[RD::SHADER_STAGE_INTERSECTION] = String();
	}

	String base_ch_src = sources[RD::SHADER_STAGE_CLOSEST_HIT];
	String base_ah_src = sources[RD::SHADER_STAGE_ANY_HIT];
	ERR_FAIL_COND_V(base_ch_src.is_empty(), false);
	ERR_FAIL_COND_V(base_ah_src.is_empty(), false);

	static const String custom_hg_define = "#define RT_CUSTOM_HIT_GROUP\n";
	int base_ch_ver = base_ch_src.find("#version");
	int base_ah_ver = base_ah_src.find("#version");
	ERR_FAIL_COND_V(base_ch_ver < 0 || base_ah_ver < 0, false);

	r_ctx.ch_template = base_ch_src.insert(base_ch_src.find("\n", base_ch_ver) + 1, custom_hg_define);
	r_ctx.ah_template = base_ah_src.insert(base_ah_src.find("\n", base_ah_ver) + 1, custom_hg_define);
	r_ctx.ah_procedural_src = _insert_after_version(base_ah_src, INTERSECTION_SHADERS_DEFINE);
	if (!intersection_source_template.is_empty()) {
		int base_is_ver = intersection_source_template.find("#version");
		if (base_is_ver >= 0) {
			r_ctx.is_template = _insert_after_version(intersection_source_template.insert(intersection_source_template.find("\n", base_is_ver) + 1, custom_hg_define), INTERSECTION_SHADERS_DEFINE);
		}
	}

	// Full base stages; CH/AH SPIR-V is HG0 default; RG+miss duplicated into each HG bytecode.
	r_ctx.base_sources = sources;
	r_ctx.base_sources_key = _binary_cache_key("RT_base", sources);
	return true;
}

bool SceneShaderRaytracing::_ensure_variant_compile_context(uint32_t p_rt_flags) {
	if (variant_compile_contexts.has(p_rt_flags)) {
		return true;
	}
	VariantCompileContext ctx;
	if (!_build_variant_compile_context(p_rt_flags, ctx)) {
		return false;
	}
	const Vector<String> sources = ctx.base_sources;
	variant_compile_contexts.insert(p_rt_flags, ctx);

	// Creates layout-defining base_shader RID for uniform sets (immutable per variant).
	HashMap<uint32_t, PipelineBundle>::Iterator bit = pipeline_bundles.find(p_rt_flags);
	if (bit != pipeline_bundles.end() && bit->value.base_shader.is_valid()) {
		return true;
	}

	Vector<uint8_t> base_binary = _binary_cache_load(ctx.base_sources_key);
	if (base_binary.is_empty()) {
		const Vector<RD::ShaderStageSPIRVData> base_stages = ShaderRD::compile_stages(sources, {});
		ERR_FAIL_COND_V(base_stages.is_empty(), false);
		base_binary = RD::get_singleton()->shader_compile_binary_from_spirv(base_stages, "RT_base");
		ERR_FAIL_COND_V(base_binary.is_empty(), false);
		_binary_cache_save(ctx.base_sources_key, base_binary);
	}

	RID base_shader_rd = RD::get_singleton()->shader_create_from_bytecode(base_binary);
	ERR_FAIL_COND_V(!base_shader_rd.is_valid(), false);

	PipelineBundle &bundle = pipeline_bundles[p_rt_flags];
	bundle.base_shader = base_shader_rd;
	_bundle_resize_for_slots(bundle);
	if (!bundle.per_hg_states.is_empty()) {
		bundle.per_hg_states[0] = HGState::Ready;
	}

	return true;
}

// Shader baker.

SceneShaderRaytracing *SceneShaderRaytracing::get_singleton_if_initialized() {
	return singleton && singleton->raygen_shader_version.is_valid() ? singleton : nullptr;
}

// The flags that change a variant's sources (RAYGEN_SHADER_OPTIONS; the debug draw is left out): every combination a run of the project can trace with.
static LocalVector<uint32_t> _bake_rt_flags(bool p_simple_shadows, bool p_ser) {
	LocalVector<uint32_t> flags;
	const uint32_t shadows = p_simple_shadows ? SceneShaderRaytracing::RT_FLAG_RAY_QUERY_SHADOWS_ENABLED : 0;
	for (const uint32_t rr : { uint32_t(0), uint32_t(SceneShaderRaytracing::RT_FLAG_DLSS_RR_ENABLED), uint32_t(SceneShaderRaytracing::RT_FLAG_NRD_ENABLED) }) {
		// The hardware decides at run time (compute_rt_flags()): both when the project asks for it.
		for (const uint32_t set : { uint32_t(0), uint32_t(p_ser ? SceneShaderRaytracing::RT_FLAG_SER_ENABLED : 0) }) {
			// Opacity micromaps likewise, whatever the project: the player's device has them or not.
			for (const uint32_t omm : { uint32_t(0), uint32_t(SceneShaderRaytracing::RT_FLAG_OPACITY_MICROMAPS) }) {
				const uint32_t f = shadows | rr | set | omm;
				if (!flags.has(f)) {
					flags.push_back(f);
				}
			}
		}
	}
	return flags;
}

static SceneShaderRaytracing::BakeProgram _compute_bake_program(const String &p_name, const String &p_source, const String &p_key) {
	SceneShaderRaytracing::BakeProgram program;
	program.cache_key = p_key;
	program.name = p_name;
	program.stages.push_back(RD::SHADER_STAGE_COMPUTE);
	program.sources.push_back(p_source);
	return program;
}

void SceneShaderRaytracing::get_bake_programs(bool p_simple_shadows, bool p_ser, LocalVector<BakeProgram> &r_programs) {
	LocalVector<uint32_t> variants = _bake_rt_flags(p_simple_shadows, p_ser);
	if (!variants.has(RT_FLAG_NONE)) {
		variants.push_back(RT_FLAG_NONE); // init()'s probe builds its base.
	}
	for (const uint32_t flags : variants) {
		if (!bake_compile_contexts.has(flags)) {
			VariantCompileContext ctx;
			if (!_build_variant_compile_context(flags, ctx)) {
				continue;
			}
			bake_compile_contexts.insert(flags, ctx);
		}
		const VariantCompileContext &ctx = bake_compile_contexts[flags];
		// As _ensure_variant_compile_context() compiles it (ShaderRD::compile_stages()).
		BakeProgram program;
		program.cache_key = ctx.base_sources_key;
		program.name = "RT_base";
		for (int i = 0; i < ctx.base_sources.size(); i++) {
			if (!ctx.base_sources[i].strip_edges().is_empty()) {
				program.stages.push_back(RD::ShaderStage(i));
				program.sources.push_back(ctx.base_sources[i]);
			}
		}
		r_programs.push_back(program);
	}

	if (!omm_bake_template.is_empty()) {
		const String source = _omm_bake_source(nullptr);
		Vector<String> sources;
		sources.resize(RD::SHADER_STAGE_MAX);
		sources.write[RD::SHADER_STAGE_COMPUTE] = source;
		r_programs.push_back(_compute_bake_program("RT OMM bake", source, _binary_cache_key("RT OMM bake", sources)));
	}
}

void SceneShaderRaytracing::get_material_bake_programs(RID p_material, bool p_simple_shadows, bool p_ser, LocalVector<BakeProgram> &r_programs) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	// Only spatial materials have hit groups: the export walks every material, particle process, canvas item, sky and fog
	// ones included, which the hit group compiler would reject ("Invalid render mode").
	if (!material_storage->owns_material(p_material) || material_storage->material_get_data(p_material, RendererRD::MaterialStorage::SHADER_TYPE_3D) == nullptr || RenderRaytracing::is_base_material(p_material, material_storage->material_get_param(p_material, "albedo"))) {
		return;
	}

	// As _register_slot() makes its slot: a procedural hit group for a material with an intersection(), as
	// RTProceduralInstance3D registers it, a triangle one otherwise.
	CustomShaderEntry entry;
	bool valid = false;
	if (material_storage->material_get_shader_code_rt(p_material).contains("intersection")) {
		valid = _preprocess_shader(p_material, true, entry) && !entry.intersection_code.is_empty();
	}
	if (!valid) {
		entry = CustomShaderEntry();
		valid = _preprocess_shader(p_material, false, entry) && !(entry.fragment_code.is_empty() && entry.vertex_code.is_empty());
	}
	if (!valid) {
		return;
	}
	entry.name = material_storage->material_get_shader_path_hint(p_material);

	for (const uint32_t flags : _bake_rt_flags(p_simple_shadows, p_ser)) {
		if (!bake_compile_contexts.has(flags)) {
			VariantCompileContext ctx;
			if (!_build_variant_compile_context(flags, ctx)) {
				continue;
			}
			bake_compile_contexts.insert(flags, ctx);
		}
		const VariantCompileContext &ctx = bake_compile_contexts[flags];
		const HitGroupSources sources = _hit_group_sources(entry, ctx);

		// The stages _compile_hit_group() puts in the binary, in its order: the base's ray generation and miss stages,
		// the closest hit, the any-hit (the slot's own, else the base's), the intersection.
		BakeProgram program;
		program.cache_key = _binary_cache_key("RT_hg", { sources.ch, sources.ah, sources.is }, ctx.base_sources_key);
		program.name = entry.name.is_empty() ? String("RT_hg") : entry.name;
		for (int i = 0; i < ctx.base_sources.size(); i++) {
			if ((i == RD::SHADER_STAGE_RAYGEN || i == RD::SHADER_STAGE_MISS) && !ctx.base_sources[i].strip_edges().is_empty()) {
				program.stages.push_back(RD::ShaderStage(i));
				program.sources.push_back(ctx.base_sources[i]);
			}
		}
		program.stages.push_back(RD::SHADER_STAGE_CLOSEST_HIT);
		program.sources.push_back(sources.ch);
		const String &any_hit = entry.uses_alpha_clip ? sources.ah : (entry.is_procedural ? ctx.ah_procedural_src : ctx.base_sources[RD::SHADER_STAGE_ANY_HIT]);
		if (!any_hit.strip_edges().is_empty()) {
			program.stages.push_back(RD::SHADER_STAGE_ANY_HIT);
			program.sources.push_back(any_hit);
		}
		if (entry.is_procedural && !sources.is.is_empty()) {
			program.stages.push_back(RD::SHADER_STAGE_INTERSECTION);
			program.sources.push_back(sources.is);
		}
		r_programs.push_back(program);
	}

	if (entry.writes_vertex && !displace_template.is_empty()) {
		const String source = _displace_source(entry);
		Vector<String> sources;
		sources.resize(RD::SHADER_STAGE_MAX);
		sources.write[RD::SHADER_STAGE_COMPUTE] = source;
		r_programs.push_back(_compute_bake_program("RT displace", source, _binary_cache_key("RT displace", sources)));
	}
	if (entry.alpha_bakeable && !omm_bake_template.is_empty()) {
		const String source = _omm_bake_source(&entry);
		Vector<String> sources;
		sources.resize(RD::SHADER_STAGE_MAX);
		sources.write[RD::SHADER_STAGE_COMPUTE] = source;
		r_programs.push_back(_compute_bake_program("RT OMM bake", source, _binary_cache_key("RT OMM bake", sources)));
	}
}

void SceneShaderRaytracing::_bundle_resize_for_slots(PipelineBundle &r_bundle) {
	uint32_t n = (uint32_t)hit_group_slots.size();
	uint32_t old = (uint32_t)r_bundle.per_hg_states.size();
	if (old == n) {
		return;
	}
	r_bundle.per_hg_states.resize(n);
	r_bundle.per_hg_shaders.resize(n);
	for (uint32_t i = old; i < n; i++) {
		r_bundle.per_hg_states[i] = HGState::Empty;
		r_bundle.per_hg_shaders[i] = RID();
	}
	if (n > 0 && r_bundle.base_shader.is_valid()) {
		r_bundle.per_hg_states[0] = HGState::Ready;
	}
}

const SceneShaderRaytracing::PipelineBundle &SceneShaderRaytracing::ensure_pipeline_bundle(uint32_t p_rt_flags, bool p_mark_used) {
	static PipelineBundle EMPTY_BUNDLE;

	HashMap<uint32_t, PipelineBundle>::Iterator it = pipeline_bundles.find(p_rt_flags);
	if (it != pipeline_bundles.end() && it->value.initial_pipeline_built) {
		if (p_mark_used) {
			it->value.last_used_frame = int64_t(RSG::rasterizer->get_frame_number());
		}
		return it->value;
	}

	if (!_ensure_variant_compile_context(p_rt_flags)) {
		WARN_PRINT(vformat("RT: Failed to build base compile context for variant 0x%x.", p_rt_flags));
		return EMPTY_BUNDLE;
	}

	PipelineBundle &bundle = pipeline_bundles[p_rt_flags];
	_bundle_resize_for_slots(bundle);

	// Sync bootstrap: HG0-only pipeline + SBT on render thread; dirty if extra slots exist.
	if (!_build_initial_bundle(p_rt_flags, bundle)) {
		return EMPTY_BUNDLE;
	}
	if (hit_group_slots.size() > 1) {
		bundle.dirty = true;
	}
	bundle.initial_pipeline_built = true;
	if (p_mark_used) {
		bundle.last_used_frame = int64_t(RSG::rasterizer->get_frame_number());
	}
	return bundle;
}

// Minimal HG0 + empty sentinel pipeline/SBT for first use of a variant.

bool SceneShaderRaytracing::_build_initial_bundle(uint32_t p_rt_flags, PipelineBundle &r_bundle) {
	if (!r_bundle.base_shader.is_valid()) {
		return false;
	}

	Vector<RD::PipelineSpecializationConstant> spec_constants;
	{
		RD::PipelineSpecializationConstant sc;
		sc.constant_id = 0;
		sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
		sc.int_value = (int)p_rt_flags;
		spec_constants.push_back(sc);
	}

	RD::PipelineShader base_ps = { r_bundle.base_shader, spec_constants };
	RD::HitGroup hg0_default;
	hg0_default.closest_hit_shader = base_ps;
	hg0_default.any_hit_shader = base_ps;
	const RD::HitGroup empty_hg;

	// HG[0]=default, HG[1]=empty until rebuild maps custom slots.
	LocalVector<RD::HitGroup> hit_groups;
	hit_groups.push_back(hg0_default);
	hit_groups.push_back(empty_hg);

	RID new_pipeline = RD::get_singleton()->raytracing_pipeline_create(
			{ &base_ps, 1 }, { &base_ps, 1 },
			{ hit_groups.ptr(), (uint64_t)hit_groups.size() },
			RT_MAX_RECURSION_DEPTH);
	if (new_pipeline.is_null()) {
		WARN_PRINT(vformat("RT: initial raytracing_pipeline_create failed for variant 0x%x.", p_rt_flags));
		return false;
	}
	RD::get_singleton()->set_resource_name(new_pipeline, String("RT Pipeline initial [flags=") + itos(p_rt_flags) + "]");

	static constexpr uint32_t HIT_SBT_CAPACITY = 4096;
	uint32_t sbt_size = HIT_SBT_CAPACITY;

	RID new_sbt = RD::get_singleton()->hit_sbt_create(new_pipeline, sbt_size);
	if (new_sbt.is_null()) {
		RD::get_singleton()->free_rid(new_pipeline);
		return false;
	}
	RD::get_singleton()->set_resource_name(new_sbt, String("RT Hit SBT initial [flags=") + itos(p_rt_flags) + "]");

	RD::HitShaderBindingTableRange sbt_range = RD::get_singleton()->hit_sbt_range_alloc(new_sbt, sbt_size);
	if (!sbt_range) {
		RD::get_singleton()->free_rid(new_sbt);
		RD::get_singleton()->free_rid(new_pipeline);
		return false;
	}

	// Instance slot i maps to HG min(i,1) until full rebuild (custom TLAS-skipped meanwhile).
	LocalVector<uint32_t> indices;
	indices.resize(sbt_size);
	for (uint32_t i = 0; i < sbt_size; i++) {
		indices[i] = (i == 0) ? 0 : 1;
	}
	RD::get_singleton()->hit_sbt_range_update(new_sbt, sbt_range, 0, indices);

	r_bundle.pipeline = new_pipeline;
	r_bundle.hit_sbt = new_sbt;
	r_bundle.hit_sbt_range = sbt_range;
	r_bundle.hit_sbt_capacity = sbt_size;
	r_bundle.live_hg_count = 1;
	r_bundle.live_ready_mask.clear();
	r_bundle.live_ready_mask.push_back(true);
	r_bundle.slot_groups.clear();
	r_bundle.slot_groups.push_back(0);
	r_bundle.pipeline_group_count = hit_groups.size();
	r_bundle.empty_group = 1;
	return true;
}

SceneShaderRaytracing::PipelineBuildTask *SceneShaderRaytracing::_make_pipeline_build_task(uint32_t p_rt_flags, PipelineBundle &p_bundle) {
	if (!_ensure_variant_compile_context(p_rt_flags)) {
		return nullptr;
	}
	HashMap<uint32_t, VariantCompileContext>::ConstIterator cit = variant_compile_contexts.find(p_rt_flags);
	if (cit == variant_compile_contexts.end()) {
		return nullptr;
	}
	const VariantCompileContext &ctx = cit->value;

	PipelineBuildTask *task = memnew(PipelineBuildTask);
	task->rt_flags = p_rt_flags;
	task->base_shader = p_bundle.base_shader;
	task->base_pipeline = p_bundle.pipeline;
	task->base_slot_groups = p_bundle.slot_groups;
	task->base_group_count = p_bundle.pipeline_group_count;
	task->base_empty_group = p_bundle.empty_group;
	task->base_sbt_capacity = p_bundle.hit_sbt_capacity;
	task->base_sources = ctx.base_sources;
	task->base_sources_key = ctx.base_sources_key;
	task->base_any_hit_procedural_src = ctx.ah_procedural_src;
	{
		RD::PipelineSpecializationConstant sc;
		sc.constant_id = 0;
		sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
		sc.int_value = (int)p_rt_flags;
		task->spec_constants.push_back(sc);
	}

	uint32_t slot_count = (uint32_t)hit_group_slots.size();
	task->slots.resize(slot_count);

	task->slots[0].existing_per_hg_shader = RID();
	task->slots[0].needs_compile = false;

	// One build compiles its hit groups in parallel and adds them to the pipeline together: none is usable before the last
	// is done. Those the TLAS waits for (in or near the view) go in a build of their own, the nearest first; the ones only
	// precached follow, at most one per thread per build, so that one coming into view waits for one hit group's compile,
	// not for the whole zone's.
	LocalVector<uint32_t> pending;
	for (uint32_t i = 1; i < slot_count; i++) {
		const bool built = i < p_bundle.per_hg_states.size() && (p_bundle.per_hg_states[i] == HGState::Failed || (p_bundle.per_hg_states[i] == HGState::Ready && i < p_bundle.per_hg_shaders.size() && p_bundle.per_hg_shaders[i].is_valid()));
		if (hit_group_slots[i].state == HGState::Ready && !built) {
			pending.push_back(i);
		}
	}
	struct PendingOrder {
		const LocalVector<HitGroupSlot> *slots = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const {
			const HitGroupSlot &a = (*slots)[p_a];
			const HitGroupSlot &b = (*slots)[p_b];
			if (a.requested != b.requested) {
				return a.requested;
			}
			if (a.requested && a.requested_distance != b.requested_distance) {
				return a.requested_distance < b.requested_distance;
			}
			return p_a < p_b;
		}
	};
	SortArray<uint32_t, PendingOrder> sorter;
	sorter.compare.slots = &hit_group_slots;
	sorter.sort(pending.ptr(), pending.size());
	uint32_t batch_size = pending.size();
	// GODOT_RT_HG_BATCHES=0: every pending hit group in one build, as before (comparisons).
	static const bool batches_disabled = OS::get_singleton()->get_environment("GODOT_RT_HG_BATCHES") == "0";
	if (!async_compilation_enabled || batches_disabled) {
		// Compiled before the frame traces: all of them.
	} else if (batch_size > 0 && hit_group_slots[pending[0]].requested) {
		batch_size = 0;
		while (batch_size < pending.size() && hit_group_slots[pending[batch_size]].requested) {
			batch_size++;
		}
	} else {
		batch_size = MIN(batch_size, (uint32_t)MAX(4, WorkerThreadPool::get_singleton()->get_thread_count()));
	}
	HashSet<uint32_t> batch;
	for (uint32_t i = 0; i < batch_size; i++) {
		batch.insert(pending[i]);
	}
	task->slots_left_for_later = pending.size() - batch_size;

	for (uint32_t i = 1; i < slot_count; i++) {
		const HitGroupSlot &slot = hit_group_slots[i];
		PipelineBuildTask::SlotInput &si = task->slots[i];

		si.uses_alpha_clip = slot.entry.uses_alpha_clip;
		si.is_procedural = slot.entry.is_procedural;
		si.name = slot.entry.name;

		if (slot.state != HGState::Ready) {
			si.needs_compile = false;
			si.existing_per_hg_shader = RID();
			continue;
		}

		if (i < p_bundle.per_hg_shaders.size() && p_bundle.per_hg_shaders[i].is_valid() &&
				i < p_bundle.per_hg_states.size() && p_bundle.per_hg_states[i] == HGState::Ready) {
			si.existing_per_hg_shader = p_bundle.per_hg_shaders[i];
			si.needs_compile = false;
			continue;
		}
		if (i < p_bundle.per_hg_states.size() && p_bundle.per_hg_states[i] == HGState::Failed) {
			si.needs_compile = false;
			continue;
		}
		if (!batch.has(i)) {
			// A later build's: an empty group until then, as a slot registered after this task.
			si.needs_compile = false;
			continue;
		}

		si.needs_compile = true;
		const HitGroupSources sources = _hit_group_sources(slot.entry, ctx);
		si.ch_src = sources.ch;
		si.ah_src = sources.ah;
		si.is_src = sources.is;
	}

	return task;
}

SceneShaderRaytracing::HitGroupSources SceneShaderRaytracing::_hit_group_sources(const CustomShaderEntry &p_entry, const VariantCompileContext &p_ctx) {
	HitGroupSources r;
	const String tex_defines = _texture_defines(p_entry.texture_uniforms);
	const String uniform_members = p_entry.uniform_members.is_empty() ? String("float _rt_pad;") : p_entry.uniform_members;

	String vertex_function;
	String vertex_call;
	if (!p_entry.vertex_code.is_empty()) {
		vertex_function = "void rt_run_vertex_shader() {\n" + p_entry.vertex_code + "\n}\n";
		vertex_call = "rt_run_vertex_shader();";
	}

	// Procedural with no fragment stage: default material shading.
	String fragment_code = p_entry.fragment_code;
	if (fragment_code.is_empty() && p_entry.is_procedural) {
		fragment_code =
				"vec2 mat_uv = uv_interp * rt_mat.uv1_scale + rt_mat.uv1_offset;\n"
				"vec4 albedo_tex = sample_material_texture(rt_mat.albedo_texture_idx, mat_uv, rt_mat.flags);\n"
				"albedo = albedo_tex.rgb * rt_mat.albedo_color.rgb;\n"
				"alpha = albedo_tex.a * rt_mat.albedo_color.a;\n"
				"vec3 orm = sample_material_texture(rt_mat.orm_texture_idx, mat_uv, rt_mat.flags).rgb;\n"
				"roughness = orm.g * rt_mat.roughness;\n"
				"metallic = orm.b * rt_mat.metallic;\n"
				"if ((rt_mat.flags & 1u) != 0u) {\n"
				"    normal_map = sample_material_texture(rt_mat.normal_texture_idx, mat_uv, rt_mat.flags).rgb;\n"
				"    normal_map_depth = rt_mat.normal_map_depth;\n"
				"}\n"
				"if ((rt_mat.flags & 2u) != 0u) {\n"
				"    emission = sample_material_texture(rt_mat.emission_texture_idx, mat_uv, rt_mat.flags).rgb\n"
				"             * rt_mat.emission_color * rt_mat.emission_strength;\n"
				"}\n";
	}

	{
		String s = p_ctx.ch_template;
		s = s.replace("/* RT_CUSTOM_DEFINES */", p_entry.defines);
		s = s.replace("/* RT_CUSTOM_FRAGMENT_GLOBALS */", p_entry.fragment_globals);
		s = s.replace("/* RT_CUSTOM_FRAGMENT_CODE */", fragment_code);
		s = s.replace("/* RT_CUSTOM_UNIFORM_MEMBERS */", uniform_members);
		s = s.replace("/* RT_CUSTOM_TEXTURE_DEFINES */", tex_defines);
		s = s.replace("/* RT_CUSTOM_VERTEX_FUNCTION */", vertex_function);
		s = s.replace("/* RT_CUSTOM_VERTEX_CALL */", vertex_call);
		r.ch = _apply_varying_code(s, p_entry.varyings);
	}
	if (p_entry.uses_alpha_clip) {
		String s = p_ctx.ah_template;
		s = s.replace("/* RT_CUSTOM_DEFINES */", p_entry.defines);
		s = s.replace("/* RT_CUSTOM_FRAGMENT_GLOBALS */", p_entry.fragment_globals);
		s = s.replace("/* RT_CUSTOM_FRAGMENT_CODE */", fragment_code);
		s = s.replace("/* RT_CUSTOM_UNIFORM_MEMBERS */", uniform_members);
		s = s.replace("/* RT_CUSTOM_TEXTURE_DEFINES */", tex_defines);
		s = s.replace("/* RT_CUSTOM_VERTEX_FUNCTION */", vertex_function);
		s = s.replace("/* RT_CUSTOM_VERTEX_CALL */", vertex_call);
		r.ah = _apply_varying_code(s, p_entry.varyings);
	}
	if (p_entry.is_procedural) {
		r.ch = _insert_after_version(r.ch, INTERSECTION_SHADERS_DEFINE);
		if (!r.ah.is_empty()) {
			r.ah = _insert_after_version(r.ah, INTERSECTION_SHADERS_DEFINE);
		}
	}
	if (p_entry.is_procedural && !p_ctx.is_template.is_empty()) {
		String s = p_ctx.is_template;
		s = s.replace("/* RT_CUSTOM_INTERSECTION_GLOBALS */", p_entry.intersection_globals);
		s = s.replace("/* RT_CUSTOM_INTERSECTION_CODE */", p_entry.intersection_code);
		s = s.replace("/* RT_CUSTOM_UNIFORM_MEMBERS */", uniform_members);
		s = s.replace("/* RT_CUSTOM_TEXTURE_DEFINES */", tex_defines);
		r.is = s;
	}
	return r;
}

void SceneShaderRaytracing::_load_hit_group_from_cache(uint32_t p_index, PipelineBuildTask *p_task) {
	PipelineBuildTask::SlotInput &si = p_task->slots[p_task->compile_indices[p_index]];
	Vector<String> sources;
	sources.push_back(si.ch_src);
	sources.push_back(si.ah_src);
	sources.push_back(si.is_src);
	// The base's sources too: the binary holds its ray generation and miss stages (and its any-hit without one here).
	si.cache_key = _binary_cache_key("RT_hg", sources, p_task->base_sources_key);
	si.binary = _binary_cache_load(si.cache_key);
}

void SceneShaderRaytracing::_compile_hit_group(uint32_t p_index, PipelineBuildTask *p_task) {
	const uint32_t i = p_task->compile_indices[p_index];
	PipelineBuildTask::SlotInput &si = p_task->slots[i];
	String error;
	const uint64_t glslang_begin = OS::get_singleton()->get_ticks_usec();

	Vector<RD::ShaderStageSPIRVData> custom_stages = p_task->base_non_hit_stages;
	{
		RD::ShaderStageSPIRVData sd;
		sd.shader_stage = RD::SHADER_STAGE_CLOSEST_HIT;
		sd.spirv = RD::get_singleton()->shader_compile_spirv_from_source(RD::SHADER_STAGE_CLOSEST_HIT, si.ch_src, RD::SHADER_LANGUAGE_GLSL, &error);
		if (sd.spirv.is_empty()) {
			_dump_failed_shader(si.ch_src, vformat("hg%d_v%x_closest_hit", i, p_task->rt_flags));
			return;
		}
		custom_stages.push_back(sd);
	}
	if (si.uses_alpha_clip) {
		RD::ShaderStageSPIRVData sd;
		sd.shader_stage = RD::SHADER_STAGE_ANY_HIT;
		sd.spirv = RD::get_singleton()->shader_compile_spirv_from_source(RD::SHADER_STAGE_ANY_HIT, si.ah_src, RD::SHADER_LANGUAGE_GLSL, &error);
		if (sd.spirv.is_empty()) {
			_dump_failed_shader(si.ah_src, vformat("hg%d_v%x_any_hit", i, p_task->rt_flags));
			return;
		}
		custom_stages.push_back(sd);
	} else if (si.is_procedural) {
		// The hit group takes its any-hit from this binary: without it, the slot fails.
		if (!p_task->base_any_hit_procedural_valid) {
			return;
		}
		custom_stages.push_back(p_task->base_any_hit_procedural_stage);
	} else if (p_task->base_any_hit_valid) {
		custom_stages.push_back(p_task->base_any_hit_stage);
	}
	if (si.is_procedural && !si.is_src.is_empty()) {
		RD::ShaderStageSPIRVData sd;
		sd.shader_stage = RD::SHADER_STAGE_INTERSECTION;
		sd.spirv = RD::get_singleton()->shader_compile_spirv_from_source(RD::SHADER_STAGE_INTERSECTION, si.is_src, RD::SHADER_LANGUAGE_GLSL, &error);
		if (sd.spirv.is_empty()) {
			_dump_failed_shader(si.is_src, vformat("hg%d_v%x_intersection", i, p_task->rt_flags));
			return;
		}
		custom_stages.push_back(sd);
	}

	const uint64_t translate_begin = OS::get_singleton()->get_ticks_usec();
	si.glslang_usec = translate_begin - glslang_begin;
	si.binary = RD::get_singleton()->shader_compile_binary_from_spirv(custom_stages, vformat("RT_hg%d_v%x", i, p_task->rt_flags));
	si.translate_usec = OS::get_singleton()->get_ticks_usec() - translate_begin;
	_binary_cache_save(si.cache_key, si.binary);
}

void SceneShaderRaytracing::_build_pipeline_worker_static(void *p_userdata) {
	PipelineBuildTask *t = static_cast<PipelineBuildTask *>(p_userdata);
	if (singleton) {
		singleton->_build_pipeline_worker(t);
	} else {
		t->done.set();
	}
}

void SceneShaderRaytracing::_build_pipeline_worker(PipelineBuildTask *p_task) {
	auto check_abort = [p_task]() {
		return p_task->abort_requested.is_set();
	};
	auto finish = [p_task]() {
		p_task->done.set();
	};

	uint32_t n = (uint32_t)p_task->slots.size();
	p_task->new_per_hg_shaders.resize(n);
	p_task->new_ready_mask.resize(n);

	LocalVector<RID> created_here;
	auto cleanup_created = [&]() {
		for (const RID &r : created_here) {
			if (r.is_valid()) {
				RD::get_singleton()->free_rid(r);
			}
		}
		created_here.clear();
		for (uint32_t i = 0; i < n; i++) {
			if (p_task->slots[i].existing_per_hg_shader.is_null()) {
				p_task->new_per_hg_shaders[i] = RID();
			}
		}
	};

	if (n > 0) {
		p_task->new_per_hg_shaders[0] = RID();
		p_task->new_ready_mask[0] = true;
	}

	p_task->compile_indices.clear();
	for (uint32_t i = 1; i < n; i++) {
		const PipelineBuildTask::SlotInput &si = p_task->slots[i];
		if (si.existing_per_hg_shader.is_valid()) {
			p_task->new_per_hg_shaders[i] = si.existing_per_hg_shader;
			p_task->new_ready_mask[i] = true;
			continue;
		}
		p_task->new_per_hg_shaders[i] = RID();
		p_task->new_ready_mask[i] = false;
		if (si.needs_compile) {
			p_task->compile_indices.push_back(i);
		}
	}

	// The hit groups from the cache, then those missing compiled, each on its own thread: glslang and the SPIR-V
	// translation (Mesa's on D3D12, seconds per hit group) run in parallel as ShaderRD's variants do.
	const LocalVector<uint32_t> to_compile(p_task->compile_indices);
	if (!to_compile.is_empty()) {
		WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &SceneShaderRaytracing::_load_hit_group_from_cache, p_task, to_compile.size(), -1, true, SNAME("RT hit group cache"));
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
	}
	p_task->compile_indices.clear();
	for (uint32_t i : to_compile) {
		if (p_task->slots[i].binary.is_empty()) {
			p_task->compile_indices.push_back(i);
		}
	}
	print_verbose(vformat("RT: variant 0x%x: %d hit groups to build, %d from the shader cache.", p_task->rt_flags, to_compile.size(), to_compile.size() - p_task->compile_indices.size()));
	const uint64_t batch_begin = OS::get_singleton()->get_ticks_usec();
	uint64_t base_usec = 0;
	uint64_t hit_groups_usec = 0;
	if (!p_task->compile_indices.is_empty() && !check_abort()) {
		// The variant's ray generation and miss stages, which every hit group's binary holds.
		const Vector<RD::ShaderStageSPIRVData> base_stages = ShaderRD::compile_stages(p_task->base_sources, {});
		base_usec = OS::get_singleton()->get_ticks_usec() - batch_begin;
		for (const RD::ShaderStageSPIRVData &sd : base_stages) {
			if (sd.shader_stage == RD::SHADER_STAGE_RAYGEN || sd.shader_stage == RD::SHADER_STAGE_MISS) {
				p_task->base_non_hit_stages.push_back(sd);
			} else if (sd.shader_stage == RD::SHADER_STAGE_ANY_HIT) {
				p_task->base_any_hit_stage = sd;
				p_task->base_any_hit_valid = true;
			}
		}
		for (uint32_t i : p_task->compile_indices) {
			const PipelineBuildTask::SlotInput &si = p_task->slots[i];
			if (si.is_procedural && !si.uses_alpha_clip) {
				String error;
				p_task->base_any_hit_procedural_stage.shader_stage = RD::SHADER_STAGE_ANY_HIT;
				p_task->base_any_hit_procedural_stage.spirv = RD::get_singleton()->shader_compile_spirv_from_source(RD::SHADER_STAGE_ANY_HIT, p_task->base_any_hit_procedural_src, RD::SHADER_LANGUAGE_GLSL, &error);
				p_task->base_any_hit_procedural_valid = !p_task->base_any_hit_procedural_stage.spirv.is_empty();
				if (!p_task->base_any_hit_procedural_valid) {
					_dump_failed_shader(p_task->base_any_hit_procedural_src, vformat("v%x_any_hit_procedural", p_task->rt_flags));
				}
				break;
			}
		}
		if (!base_stages.is_empty()) {
			const uint64_t hit_groups_begin = OS::get_singleton()->get_ticks_usec();
			WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &SceneShaderRaytracing::_compile_hit_group, p_task, p_task->compile_indices.size(), -1, true, SNAME("RT hit group compile"));
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
			hit_groups_usec = OS::get_singleton()->get_ticks_usec() - hit_groups_begin;
		}
		if (OS::get_singleton()->is_stdout_verbose()) {
			for (uint32_t i : p_task->compile_indices) {
				const PipelineBuildTask::SlotInput &si = p_task->slots[i];
				print_line(vformat("RT: variant 0x%x: hit group %d (%s): GLSL to SPIR-V %d ms, to the driver's binary %d ms%s.", p_task->rt_flags, i,
						si.name.is_empty() ? String("no path") : si.name, int(si.glslang_usec / 1000), int(si.translate_usec / 1000), si.binary.is_empty() ? ", FAILED" : ""));
			}
		}
	}

	for (uint32_t i : to_compile) {
		PipelineBuildTask::SlotInput &si = p_task->slots[i];
		if (si.binary.is_empty()) {
			continue;
		}
		const RID rid = RD::get_singleton()->shader_create_from_bytecode(si.binary);
		si.binary = Vector<uint8_t>();
		if (!rid.is_valid()) {
			continue;
		}
		p_task->new_per_hg_shaders[i] = rid;
		p_task->new_ready_mask[i] = true;
		created_here.push_back(rid);
	}

	if (check_abort()) {
		cleanup_created();
		p_task->failed = true;
		finish();
		return;
	}

	// The hit groups compiled now added to the bundle's pipeline when the driver allows it: the driver compiles them
	// alone instead of the whole pipeline again (seconds that grew with every material). Each new slot's SBT record then
	// points to the group appended for it; a slot still pending to the empty group.
	if (p_task->base_pipeline.is_valid() && RD::get_singleton()->raytracing_pipeline_can_add_hit_groups(p_task->base_pipeline)) {
		// Within the SBT range the bundle has: beyond it, a full build makes a larger one.
		bool mapped = p_task->base_slot_groups.size() <= n && n <= p_task->base_sbt_capacity;
		p_task->new_slot_groups = p_task->base_slot_groups;
		p_task->new_slot_groups.resize(n);
		for (uint32_t i = p_task->base_slot_groups.size(); i < n; i++) {
			p_task->new_slot_groups[i] = p_task->base_empty_group;
		}
		LocalVector<RD::HitGroup> added;
		LocalVector<uint32_t> added_slots;
		for (uint32_t i = 1; i < n && mapped; i++) {
			const PipelineBuildTask::SlotInput &si = p_task->slots[i];
			if (si.existing_per_hg_shader.is_valid()) {
				// Compiled by an earlier build: it must be in the base pipeline already.
				mapped = i < p_task->base_slot_groups.size() && p_task->base_slot_groups[i] != p_task->base_empty_group;
				continue;
			}
			if (!p_task->new_ready_mask[i] || p_task->new_per_hg_shaders[i].is_null()) {
				continue;
			}
			RD::PipelineShader custom_ps = { p_task->new_per_hg_shaders[i], p_task->spec_constants };
			RD::PipelineShader base_any_hit_ps = { p_task->base_shader, p_task->spec_constants };
			RD::HitGroup hg;
			hg.closest_hit_shader = custom_ps;
			// A procedural hit group's any-hit is in its own binary, with the intersection shader's hit attributes.
			hg.any_hit_shader = (si.uses_alpha_clip || si.is_procedural) ? custom_ps : base_any_hit_ps;
			if (si.is_procedural) {
				hg.intersection_shader = custom_ps;
			}
			added.push_back(hg);
			added_slots.push_back(i);
		}
		if (mapped) {
			const uint64_t pipeline_begin = OS::get_singleton()->get_ticks_usec();
			RID new_pipeline = p_task->base_pipeline;
			if (!added.is_empty()) {
				new_pipeline = RD::get_singleton()->raytracing_pipeline_add_hit_groups(p_task->base_pipeline, { added.ptr(), (uint64_t)added.size() });
			}
			if (new_pipeline.is_valid()) {
				for (uint32_t j = 0; j < added_slots.size(); j++) {
					p_task->new_slot_groups[added_slots[j]] = p_task->base_group_count + j;
				}
				p_task->new_group_count = p_task->base_group_count + added.size();
				p_task->new_pipeline = new_pipeline;
				p_task->incremental = true;
				const uint64_t batch_end = OS::get_singleton()->get_ticks_usec();
				print_verbose(vformat("RT: variant 0x%x: %d hit groups compiled (base stages %d ms, hit groups in parallel %d ms), %d added to the pipeline (now %d hit groups) in %d ms, %d ms in all.",
						p_task->rt_flags, p_task->compile_indices.size(), int(base_usec / 1000), int(hit_groups_usec / 1000), added.size(), p_task->new_group_count,
						int((batch_end - pipeline_begin) / 1000), int((batch_end - batch_begin) / 1000)));
				finish();
				return;
			}
		}
	}

	// raytracing_pipeline_create may run off render thread (RD uses internal sync).
	RD::PipelineShader base_ps = { p_task->base_shader, p_task->spec_constants };
	RD::HitGroup hg0_default;
	hg0_default.closest_hit_shader = base_ps;
	hg0_default.any_hit_shader = base_ps;

	// No-op HG: failed / pending slots + trailing SBT sentinel (never default material).
	const RD::HitGroup empty_hg;

	// Layout: [0] default [1..n-1] custom or empty [n] sentinel.
	LocalVector<RD::HitGroup> hit_groups;
	hit_groups.resize(n + 1);
	hit_groups[0] = hg0_default;
	for (uint32_t i = 1; i < n; i++) {
		if (!p_task->new_ready_mask[i] || p_task->new_per_hg_shaders[i].is_null()) {
			hit_groups[i] = empty_hg;
			continue;
		}
		RD::PipelineShader custom_ps = { p_task->new_per_hg_shaders[i], p_task->spec_constants };
		RD::HitGroup hg;
		hg.closest_hit_shader = custom_ps;
		hg.any_hit_shader = (p_task->slots[i].uses_alpha_clip || p_task->slots[i].is_procedural) ? custom_ps : base_ps;
		if (p_task->slots[i].is_procedural) {
			hg.intersection_shader = custom_ps;
		}
		hit_groups[i] = hg;
	}
	hit_groups[n] = empty_hg;

	const uint64_t pipeline_begin = OS::get_singleton()->get_ticks_usec();
	RID new_pipeline = RD::get_singleton()->raytracing_pipeline_create(
			{ &base_ps, 1 }, { &base_ps, 1 },
			{ hit_groups.ptr(), (uint64_t)hit_groups.size() },
			RT_MAX_RECURSION_DEPTH);
	const uint64_t batch_end = OS::get_singleton()->get_ticks_usec();
	print_verbose(vformat("RT: variant 0x%x: %d hit groups compiled (base stages %d ms, hit groups in parallel %d ms), pipeline of %d hit groups created in %d ms, %d ms in all.",
			p_task->rt_flags, p_task->compile_indices.size(), int(base_usec / 1000), int(hit_groups_usec / 1000), hit_groups.size(),
			int((batch_end - pipeline_begin) / 1000), int((batch_end - batch_begin) / 1000)));
	if (new_pipeline.is_null()) {
		cleanup_created();
		p_task->failed = true;
		p_task->error = "raytracing_pipeline_create returned null";
		finish();
		return;
	}
	p_task->new_pipeline = new_pipeline;
	finish();
}

void SceneShaderRaytracing::_finalize_pipeline_build(PipelineBuildTask *p_task) {
	HashMap<uint32_t, PipelineBundle>::Iterator bit = pipeline_bundles.find(p_task->rt_flags);
	if (bit == pipeline_bundles.end()) {
		_free_unused_build_outputs(p_task);
		return;
	}
	PipelineBundle &bundle = bit->value;
	_bundle_resize_for_slots(bundle);

	if (p_task->failed) {
		WARN_PRINT(vformat("RT: pipeline rebuild failed for variant 0x%x: %s",
				p_task->rt_flags, p_task->error));
		return;
	}

	// Install worker outputs into bundle arrays.
	uint32_t n = (uint32_t)p_task->slots.size();
	for (uint32_t i = 1; i < n; i++) {
		const PipelineBuildTask::SlotInput &si = p_task->slots[i];
		if (i >= bundle.per_hg_shaders.size()) {
			break;
		}
		if (si.existing_per_hg_shader.is_valid()) {
			continue;
		}
		if (si.needs_compile && i < p_task->new_per_hg_shaders.size() && p_task->new_per_hg_shaders[i].is_valid()) {
			bundle.per_hg_shaders[i] = p_task->new_per_hg_shaders[i];
			bundle.per_hg_states[i] = HGState::Ready;
		} else if (si.needs_compile) {
			bundle.per_hg_states[i] = HGState::Failed;
		}
	}

	if (p_task->incremental) {
		if (bundle.pipeline != p_task->base_pipeline || !bundle.hit_sbt.is_valid()) {
			// Swapped since the task was made: its pipeline extends one no longer in use; built again from the bundle.
			if (p_task->new_pipeline != p_task->base_pipeline && p_task->new_pipeline.is_valid()) {
				RD::get_singleton()->free_rid(p_task->new_pipeline);
			}
			bundle.dirty = true;
			return;
		}
		if (p_task->new_pipeline != bundle.pipeline) {
			// The same SBT, its records written again from the new pipeline (its groups are a superset of the old one's).
			RD::get_singleton()->hit_sbt_set_pipeline(bundle.hit_sbt, p_task->new_pipeline);
			RD::get_singleton()->set_resource_name(p_task->new_pipeline, String("RT Pipeline [flags=") + itos(p_task->rt_flags) + "]");
			RD::get_singleton()->free_rid(bundle.pipeline);
			bundle.pipeline = p_task->new_pipeline;
			bundle.pipeline_group_count = p_task->new_group_count;
			pipeline_swap_count++;
		}
		RD::get_singleton()->hit_sbt_range_update(bundle.hit_sbt, bundle.hit_sbt_range, 0, { p_task->new_slot_groups.ptr(), (uint64_t)p_task->new_slot_groups.size() });
		bundle.slot_groups = p_task->new_slot_groups;
		bundle.live_hg_count = n;
		bundle.live_ready_mask = p_task->new_ready_mask;
		return;
	}

	static constexpr uint32_t HIT_SBT_CAPACITY = 4096;
	uint32_t sbt_size = MAX(n, HIT_SBT_CAPACITY);

	RID new_sbt = RD::get_singleton()->hit_sbt_create(p_task->new_pipeline, sbt_size);
	if (new_sbt.is_null()) {
		WARN_PRINT(vformat("RT: hit_sbt_create failed for variant 0x%x.", p_task->rt_flags));
		RD::get_singleton()->free_rid(p_task->new_pipeline);
		return;
	}
	RD::get_singleton()->set_resource_name(new_sbt, String("RT Hit SBT [flags=") + itos(p_task->rt_flags) + "]");

	RD::HitShaderBindingTableRange sbt_range = RD::get_singleton()->hit_sbt_range_alloc(new_sbt, sbt_size);
	if (!sbt_range) {
		WARN_PRINT(vformat("RT: hit_sbt_range_alloc failed for variant 0x%x.", p_task->rt_flags));
		RD::get_singleton()->free_rid(new_sbt);
		RD::get_singleton()->free_rid(p_task->new_pipeline);
		return;
	}

	// Slots i >= n map to sentinel HG n.
	LocalVector<uint32_t> indices;
	indices.resize(sbt_size);
	for (uint32_t i = 0; i < sbt_size; i++) {
		indices[i] = (i < n) ? i : n;
	}
	RD::get_singleton()->hit_sbt_range_update(new_sbt, sbt_range, 0, indices);

	if (bundle.hit_sbt.is_valid()) {
		RD::get_singleton()->free_rid(bundle.hit_sbt);
	}
	if (bundle.pipeline.is_valid()) {
		RD::get_singleton()->free_rid(bundle.pipeline);
	}
	RD::get_singleton()->set_resource_name(p_task->new_pipeline, String("RT Pipeline [flags=") + itos(p_task->rt_flags) + "]");
	bundle.pipeline = p_task->new_pipeline;
	bundle.hit_sbt = new_sbt;
	bundle.hit_sbt_range = sbt_range;
	bundle.hit_sbt_capacity = sbt_size;
	bundle.live_hg_count = n;
	bundle.live_ready_mask = p_task->new_ready_mask;
	// A full build: slot i is hit group i, the trailing sentinel n has no shader.
	bundle.slot_groups.resize(n);
	for (uint32_t i = 0; i < n; i++) {
		bundle.slot_groups[i] = i;
	}
	bundle.pipeline_group_count = n + 1;
	bundle.empty_group = n;
	pipeline_swap_count++;
}

// The pipeline and hit group shaders a task created, when no bundle takes them; the pipeline first, it uses the shaders.
// An incremental task that added nothing returns its base pipeline, which is the bundle's.
void SceneShaderRaytracing::_free_unused_build_outputs(PipelineBuildTask *p_task) {
	if (p_task->new_pipeline.is_valid() && p_task->new_pipeline != p_task->base_pipeline) {
		RD::get_singleton()->free_rid(p_task->new_pipeline);
	}
	p_task->new_pipeline = RID();
	for (uint32_t i = 0; i < p_task->slots.size() && i < p_task->new_per_hg_shaders.size(); i++) {
		if (p_task->slots[i].existing_per_hg_shader.is_null() && p_task->new_per_hg_shaders[i].is_valid()) {
			RD::get_singleton()->free_rid(p_task->new_per_hg_shaders[i]);
		}
		p_task->new_per_hg_shaders[i] = RID();
	}
}

// Single-lane rebuild dispatcher.

void SceneShaderRaytracing::_enqueue_build(PipelineBuildTask *p_task) {
	MutexLock lock(compile_lane.mutex);
	compile_lane.queue.push_back(p_task);
	_dispatch_next_locked();
}

void SceneShaderRaytracing::_dispatch_next_locked() {
	if (!async_compilation_enabled) {
		return;
	}
	if (compile_lane.current != nullptr || compile_lane.queue.is_empty()) {
		return;
	}
	PipelineBuildTask *t = compile_lane.queue[0];
	compile_lane.queue.remove_at(0);
	compile_lane.current = t;
	// High priority: as a low priority task it waited behind the precached displacement and bake programs and the raster
	// shaders' compiles (0.75 s in a zone of 42 new materials). It mostly waits for its own high priority group tasks.
	t->worker_id = WorkerThreadPool::get_singleton()->add_native_task(
			&SceneShaderRaytracing::_build_pipeline_worker_static, t, /*high_priority=*/true,
			"RT Pipeline Build");
}

void SceneShaderRaytracing::_kick_rebuild_if_idle() {
	bool lane_idle;
	{
		MutexLock lock(compile_lane.mutex);
		lane_idle = (compile_lane.current == nullptr) && compile_lane.queue.is_empty();
	}
	if (!lane_idle) {
		return;
	}
	// The variant a view traced with last first; one unused for RT_UNUSED_BUNDLE_FRAMES (init()'s probe, a setting
	// changed since) stays dirty, rebuilt if a view traces with it again. The one traced last is rebuilt at any age: hit
	// groups precached under a loading screen that hides the 3D view are compiled for it (precache_hit_groups()).
	static constexpr int64_t RT_UNUSED_BUNDLE_FRAMES = 120;
	const int64_t frame = int64_t(RSG::rasterizer->get_frame_number());
	const PipelineBundle *last_traced = _get_last_traced_bundle();
	uint32_t next_flags = 0;
	PipelineBundle *next = nullptr;
	for (KeyValue<uint32_t, PipelineBundle> &kv : pipeline_bundles) {
		PipelineBundle &bundle = kv.value;
		if (!bundle.dirty || !bundle.initial_pipeline_built || bundle.last_used_frame < 0 ||
				(frame - bundle.last_used_frame > RT_UNUSED_BUNDLE_FRAMES && &bundle != last_traced)) {
			continue;
		}
		if (!next || bundle.last_used_frame > next->last_used_frame) {
			next = &bundle;
			next_flags = kv.key;
		}
	}
	if (next) {
		PipelineBuildTask *task = _make_pipeline_build_task(next_flags, *next);
		if (task) {
			next->dirty = task->slots_left_for_later > 0;
			_enqueue_build(task);
		}
	}
}

const SceneShaderRaytracing::PipelineBundle *SceneShaderRaytracing::_get_last_traced_bundle() const {
	const PipelineBundle *last = nullptr;
	for (const KeyValue<uint32_t, PipelineBundle> &kv : pipeline_bundles) {
		const PipelineBundle &bundle = kv.value;
		if (bundle.initial_pipeline_built && bundle.last_used_frame >= 0 && (!last || bundle.last_used_frame > last->last_used_frame)) {
			last = &bundle;
		}
	}
	return last;
}

uint32_t SceneShaderRaytracing::get_pending_hit_group_count() const {
	const PipelineBundle *bundle = _get_last_traced_bundle();
	if (!bundle) {
		return 0;
	}
	// Displacement and opacity micromap bake programs too: built at first use, they stall that frame.
	uint32_t pending = program_builds.size();
	for (uint32_t i = 1; i < hit_group_slots.size(); i++) {
		if (hit_group_slots[i].state != HGState::Ready) {
			continue;
		}
		if (i < bundle->per_hg_states.size() && bundle->per_hg_states[i] == HGState::Failed) {
			continue;
		}
		if (i < bundle->live_hg_count && i < bundle->live_ready_mask.size() && bundle->live_ready_mask[i]) {
			continue;
		}
		pending++;
	}
	return pending;
}

void SceneShaderRaytracing::drain_completed_compiles() {
	for (uint32_t i = 0; i < program_builds.size();) {
		if (WorkerThreadPool::get_singleton()->is_task_completed(program_builds[i]->task)) {
			_finish_program_build(program_builds[i]); // Removes it.
		} else {
			i++;
		}
	}

	while (true) {
		PipelineBuildTask *finished = nullptr;
		{
			MutexLock lock(compile_lane.mutex);
			if (compile_lane.current && compile_lane.current->done.is_set()) {
				finished = compile_lane.current;
				compile_lane.current = nullptr;
			}
		}
		if (!finished) {
			break;
		}
		if (finished->worker_id != WorkerThreadPool::INVALID_TASK_ID) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(finished->worker_id);
		}
		_finalize_pipeline_build(finished);
		memdelete(finished);

		{
			MutexLock lock(compile_lane.mutex);
			_dispatch_next_locked();
		}
	}
}

void SceneShaderRaytracing::_drain_lane_inline_main_thread() {
	while (true) {
		PipelineBuildTask *t = nullptr;
		{
			MutexLock lock(compile_lane.mutex);
			if (compile_lane.current) {
				break;
			}
			if (compile_lane.queue.is_empty()) {
				break;
			}
			t = compile_lane.queue[0];
			compile_lane.queue.remove_at(0);
		}
		_build_pipeline_worker(t);
		_finalize_pipeline_build(t);
		memdelete(t);
	}
}

void SceneShaderRaytracing::_join_lane_for_shutdown() {
	PipelineBuildTask *current = nullptr;
	LocalVector<PipelineBuildTask *> queued;
	{
		MutexLock lock(compile_lane.mutex);
		current = compile_lane.current;
		compile_lane.current = nullptr;
		queued = compile_lane.queue;
		compile_lane.queue.clear();
	}

	if (current) {
		current->abort_requested.set();
		if (current->worker_id != WorkerThreadPool::INVALID_TASK_ID && WorkerThreadPool::get_singleton()) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(current->worker_id);
		}
		// A build that finished before the abort was seen, never drained: its outputs are in no bundle.
		_free_unused_build_outputs(current);
		memdelete(current);
	}
	for (PipelineBuildTask *t : queued) {
		memdelete(t);
	}
}

void SceneShaderRaytracing::init(const String p_defines) {
	async_compilation_enabled = (bool)GLOBAL_GET("rendering/pathtracing/async_shader_compilation");

	// Next to ShaderRD's cache, when it has one (rendering/shader_compiler/shader_cache/enabled, always in the editor).
	binary_cache_api = String(RD::get_singleton()->get_device_api_name()).validate_filename().to_lower();
	const String &shader_cache_dir = ShaderRD::get_shader_cache_user_dir();
	if (!shader_cache_dir.is_empty()) {
		const String dir = shader_cache_dir.path_join("SceneShaderRaytracing");
		Ref<DirAccess> d = DirAccess::open(shader_cache_dir);
		if (d.is_valid() && (d->dir_exists("SceneShaderRaytracing") || d->make_dir("SceneShaderRaytracing") == OK)) {
			binary_cache_dir = dir;
		} else {
			ERR_PRINT("Can't create the ray tracing shader cache folder, its shaders will be compiled at every launch: " + dir);
		}
	}
	// The shader baker's, in an exported project (ShaderBakerExportPlugin). In the editor it is the user's.
	const String &shader_cache_res_dir = ShaderRD::get_shader_cache_res_dir();
	if (!shader_cache_res_dir.is_empty() && shader_cache_res_dir != shader_cache_dir && DirAccess::dir_exists_absolute(shader_cache_res_dir.path_join("SceneShaderRaytracing"))) {
		binary_cache_res_dir = shader_cache_res_dir.path_join("SceneShaderRaytracing");
	}
	print_verbose(vformat("RT: shader binary cache \"%s\", baked binaries \"%s\".", binary_cache_dir, binary_cache_res_dir));

	// Raygen: one mode per bitmask of RAYGEN_SHADER_OPTIONS.
	const uint32_t variant_count = 1u << RAYGEN_SHADER_OPTION_COUNT;
	Vector<String> modes;
	modes.resize((int)variant_count);
	String *modes_ptr = modes.ptrw();
	for (uint32_t mask = 0; mask < variant_count; mask++) {
		modes_ptr[mask] = _raygen_variant_preamble(mask);
	}
	raygen_shader.initialize(modes, p_defines);

	// Source template of the displacement pass; each slot's material code is injected into it.
	displace_shader.initialize({ "" }, p_defines);
	displace_shader_version = displace_shader.version_create(false);
	if (displace_shader_version.is_valid()) {
		Vector<String> displace_sources = displace_shader.version_build_variant_stage_sources(displace_shader_version, 0);
		if (displace_sources.size() > RD::SHADER_STAGE_COMPUTE) {
			displace_template = displace_sources[RD::SHADER_STAGE_COMPUTE];
		}
	}

	// Source template of the opacity micromap bakes, likewise.
	omm_bake_shader.initialize({ "" }, p_defines);
	omm_bake_shader_version = omm_bake_shader.version_create(false);
	if (omm_bake_shader_version.is_valid()) {
		Vector<String> omm_bake_sources = omm_bake_shader.version_build_variant_stage_sources(omm_bake_shader_version, 0);
		if (omm_bake_sources.size() > RD::SHADER_STAGE_COMPUTE) {
			omm_bake_template = omm_bake_sources[RD::SHADER_STAGE_COMPUTE];
		}
	}

	// Slot 0: default HG (source_hash stays zero).
	{
		HitGroupSlot default_slot;
		default_slot.state = HGState::Ready;
		hit_group_slots.push_back(default_slot);
	}

	// Now create a version to access the embedded raytracing shader
	raygen_shader_version = raygen_shader.version_create();
	if (raygen_shader_version.is_valid()) {
		// A probe: no view traces with it unless its flags come up, so its hit groups are not compiled for it.
		const PipelineBundle &b = ensure_pipeline_bundle(RT_FLAG_NONE, false);
		usable = b.pipeline.is_valid();
		if (!usable) {
			WARN_PRINT("Failed to create default raytracing pipeline bundle");
		}
	} else {
		WARN_PRINT("Failed to create raytracing shader version");
	}

	{
		// Shader compiler (not used for RT, but needed for compatibility).
		ShaderCompiler::DefaultIdentifierActions actions;

		actions.renames["MODEL_MATRIX"] = "read_model_matrix";
		actions.renames["MODEL_NORMAL_MATRIX"] = "model_normal_matrix";
		actions.renames["VIEW_MATRIX"] = "read_view_matrix";
		actions.renames["INV_VIEW_MATRIX"] = "inv_view_matrix";
		actions.renames["PROJECTION_MATRIX"] = "projection_matrix";
		actions.renames["INV_PROJECTION_MATRIX"] = "inv_projection_matrix";
		actions.renames["MODELVIEW_MATRIX"] = "rt_modelview_matrix";
		actions.renames["MODELVIEW_NORMAL_MATRIX"] = "rt_modelview_normal_matrix";
		actions.renames["MAIN_CAM_INV_VIEW_MATRIX"] = "inv_view_matrix";
		// No shadow pass: shadow rays meet the geometry the camera's rays do, that of the BLAS.
		actions.renames["IN_SHADOW_PASS"] = "false";

		actions.renames["VERTEX"] = "vertex";
		actions.renames["NORMAL"] = "normal";
		actions.renames["TANGENT"] = "tangent";
		actions.renames["BINORMAL"] = "binormal";
		actions.renames["POSITION"] = "position";
		actions.renames["UV"] = "uv_interp";
		actions.renames["UV2"] = "uv2_interp";
		actions.renames["COLOR"] = "color_interp";
		actions.renames["POINT_SIZE"] = "rt_point_size";
		actions.renames["INSTANCE_ID"] = "rt_instance_id";
		actions.renames["VERTEX_ID"] = "rt_vertex_id";

		actions.renames["ALPHA_SCISSOR_THRESHOLD"] = "alpha_scissor_threshold";
		actions.renames["ALPHA_HASH_SCALE"] = "alpha_hash_scale";
		actions.renames["ALPHA_ANTIALIASING_EDGE"] = "alpha_antialiasing_edge";
		actions.renames["ALPHA_TEXTURE_COORDINATE"] = "alpha_texture_coordinate";

		// Builtins.

		actions.renames["TIME"] = "global_time";
		actions.renames["PREV_TIME"] = "global_prev_time";
		actions.renames["EXPOSURE"] = "(1.0 / scene_data_block.data.emissive_exposure_normalization)";
		actions.renames["PI"] = String::num(Math::PI);
		actions.renames["TAU"] = String::num(Math::TAU);
		actions.renames["E"] = String::num(Math::E);
		actions.renames["OUTPUT_IS_SRGB"] = "SHADER_IS_SRGB";
		actions.renames["CLIP_SPACE_FAR"] = "SHADER_SPACE_FAR";
		actions.renames["VIEWPORT_SIZE"] = "read_viewport_size";

		actions.renames["FRAGCOORD"] = "rt_frag_coord";
		actions.renames["FRONT_FACING"] = "rt_front_facing";
		actions.renames["NORMAL_MAP"] = "normal_map";
		actions.renames["NORMAL_MAP_DEPTH"] = "normal_map_depth";
		actions.renames["ALBEDO"] = "albedo";
		actions.renames["ALPHA"] = "alpha";
		actions.renames["PREMUL_ALPHA_FACTOR"] = "premul_alpha";
		actions.renames["METALLIC"] = "metallic";
		actions.renames["SPECULAR"] = "specular";
		actions.renames["ROUGHNESS"] = "roughness";
		actions.renames["RIM"] = "rim";
		actions.renames["RIM_TINT"] = "rim_tint";
		actions.renames["CLEARCOAT"] = "clearcoat";
		actions.renames["CLEARCOAT_ROUGHNESS"] = "clearcoat_roughness";
		actions.renames["ANISOTROPY"] = "anisotropy";
		actions.renames["ANISOTROPY_FLOW"] = "anisotropy_flow";
		actions.renames["SSS_STRENGTH"] = "sss_strength";
		actions.renames["SSS_TRANSMITTANCE_COLOR"] = "transmittance_color";
		actions.renames["SSS_TRANSMITTANCE_DEPTH"] = "transmittance_depth";
		actions.renames["SSS_TRANSMITTANCE_BOOST"] = "transmittance_boost";
		actions.renames["BACKLIGHT"] = "backlight";
		actions.renames["AO"] = "ao";
		actions.renames["AO_LIGHT_AFFECT"] = "ao_light_affect";
		actions.renames["EMISSION"] = "emission";
		actions.renames["POINT_COORD"] = "rt_point_coord";
		actions.renames["INSTANCE_CUSTOM"] = "instance_custom";
		actions.renames["SCREEN_UV"] = "rt_screen_uv";
		actions.renames["DEPTH"] = "rt_depth";
		actions.renames["FOG"] = "fog";
		actions.renames["RADIANCE"] = "custom_radiance";
		actions.renames["IRRADIANCE"] = "custom_irradiance";
		actions.renames["BONE_INDICES"] = "bone_attrib";
		actions.renames["BONE_WEIGHTS"] = "weight_attrib";
		actions.renames["CUSTOM0"] = "custom0_attrib";
		actions.renames["CUSTOM1"] = "custom1_attrib";
		actions.renames["CUSTOM2"] = "custom2_attrib";
		actions.renames["CUSTOM3"] = "custom3_attrib";
		actions.renames["LIGHT_VERTEX"] = "light_vertex";

		actions.renames["NODE_POSITION_WORLD"] = "read_model_matrix[3].xyz";
		actions.renames["CAMERA_POSITION_WORLD"] = "inv_view_matrix[3].xyz";
		actions.renames["CAMERA_DIRECTION_WORLD"] = "inv_view_matrix[2].xyz";
		actions.renames["CAMERA_VISIBLE_LAYERS"] = "scene_data_block.data.camera_visible_layers";
		actions.renames["NODE_POSITION_VIEW"] = "(read_view_matrix * read_model_matrix)[3].xyz";

		actions.renames["VIEW_INDEX"] = "ViewIndex";
		actions.renames["VIEW_MONO_LEFT"] = "0";
		actions.renames["VIEW_RIGHT"] = "1";
		actions.renames["EYE_OFFSET"] = "eye_offset";

		// For light.
		actions.renames["VIEW"] = "view";
		actions.renames["SPECULAR_AMOUNT"] = "specular_amount";
		actions.renames["LIGHT_COLOR"] = "light_color";
		actions.renames["LIGHT_IS_DIRECTIONAL"] = "is_directional";
		actions.renames["LIGHT"] = "light";
		actions.renames["ATTENUATION"] = "attenuation";
		actions.renames["DIFFUSE_LIGHT"] = "diffuse_light";
		actions.renames["SPECULAR_LIGHT"] = "specular_light";

		actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
		actions.usage_defines["TANGENT"] = "#define TANGENT_USED\n";
		actions.usage_defines["BINORMAL"] = "@TANGENT";
		actions.usage_defines["RIM"] = "#define LIGHT_RIM_USED\n";
		actions.usage_defines["RIM_TINT"] = "@RIM";
		actions.usage_defines["CLEARCOAT"] = "#define LIGHT_CLEARCOAT_USED\n";
		actions.usage_defines["CLEARCOAT_ROUGHNESS"] = "@CLEARCOAT";
		actions.usage_defines["ANISOTROPY"] = "#define LIGHT_ANISOTROPY_USED\n";
		actions.usage_defines["ANISOTROPY_FLOW"] = "@ANISOTROPY";
		actions.usage_defines["AO"] = "#define AO_USED\n";
		actions.usage_defines["AO_LIGHT_AFFECT"] = "#define AO_USED\n";
		actions.usage_defines["UV"] = "#define UV_USED\n";
		actions.usage_defines["UV2"] = "#define UV2_USED\n";
		actions.usage_defines["BONE_INDICES"] = "#define BONES_USED\n";
		actions.usage_defines["BONE_WEIGHTS"] = "#define WEIGHTS_USED\n";
		actions.usage_defines["CUSTOM0"] = "#define CUSTOM0_USED\n";
		actions.usage_defines["CUSTOM1"] = "#define CUSTOM1_USED\n";
		actions.usage_defines["CUSTOM2"] = "#define CUSTOM2_USED\n";
		actions.usage_defines["CUSTOM3"] = "#define CUSTOM3_USED\n";
		actions.usage_defines["NORMAL_MAP"] = "#define NORMAL_MAP_USED\n";
		actions.usage_defines["NORMAL_MAP_DEPTH"] = "@NORMAL_MAP";
		actions.usage_defines["COLOR"] = "#define COLOR_USED\n";
		actions.usage_defines["INSTANCE_CUSTOM"] = "#define ENABLE_INSTANCE_CUSTOM\n";
		actions.usage_defines["POSITION"] = "#define OVERRIDE_POSITION\n";
		actions.usage_defines["LIGHT_VERTEX"] = "#define LIGHT_VERTEX_USED\n";
		actions.usage_defines["PREMUL_ALPHA_FACTOR"] = "#define PREMUL_ALPHA_USED\n";

		actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
		actions.usage_defines["ALPHA_HASH_SCALE"] = "#define ALPHA_HASH_USED\n";
		actions.usage_defines["ALPHA_ANTIALIASING_EDGE"] = "#define ALPHA_ANTIALIASING_EDGE_USED\n";
		actions.usage_defines["ALPHA_TEXTURE_COORDINATE"] = "@ALPHA_ANTIALIASING_EDGE";

		actions.usage_defines["SSS_STRENGTH"] = "#define ENABLE_SSS\n";
		actions.usage_defines["SSS_TRANSMITTANCE_DEPTH"] = "#define ENABLE_TRANSMITTANCE\n";
		actions.usage_defines["BACKLIGHT"] = "#define LIGHT_BACKLIGHT_USED\n";
		actions.usage_defines["SCREEN_UV"] = "#define SCREEN_UV_USED\n";

		actions.usage_defines["FOG"] = "#define CUSTOM_FOG_USED\n";
		actions.usage_defines["RADIANCE"] = "#define CUSTOM_RADIANCE_USED\n";
		actions.usage_defines["IRRADIANCE"] = "#define CUSTOM_IRRADIANCE_USED\n";

		actions.usage_defines["MODEL_MATRIX"] = "#define MODEL_MATRIX_USED\n";

		actions.render_mode_defines["skip_vertex_transform"] = "#define SKIP_TRANSFORM_USED\n";
		actions.render_mode_defines["world_vertex_coords"] = "#define VERTEX_WORLD_COORDS_USED\n";
		actions.render_mode_defines["ensure_correct_normals"] = "#define ENSURE_CORRECT_NORMALS\n";
		actions.render_mode_defines["cull_front"] = "#define DO_SIDE_CHECK\n";
		actions.render_mode_defines["cull_disabled"] = "#define DO_SIDE_CHECK\n";
		actions.render_mode_defines["particle_trails"] = "#define USE_PARTICLE_TRAILS\n";
		actions.render_mode_defines["depth_prepass_alpha"] = "#define USE_OPAQUE_PREPASS\n";

		bool force_lambert = GLOBAL_GET("rendering/shading/overrides/force_lambert_over_burley");

		if (!force_lambert) {
			actions.render_mode_defines["diffuse_burley"] = "#define DIFFUSE_BURLEY\n";
		}

		actions.render_mode_defines["diffuse_lambert_wrap"] = "#define DIFFUSE_LAMBERT_WRAP\n";
		actions.render_mode_defines["diffuse_toon"] = "#define DIFFUSE_TOON\n";

		actions.render_mode_defines["sss_mode_skin"] = "#define SSS_MODE_SKIN\n";

		actions.render_mode_defines["specular_schlick_ggx"] = "#define SPECULAR_SCHLICK_GGX\n";

		actions.render_mode_defines["specular_toon"] = "#define SPECULAR_TOON\n";
		actions.render_mode_defines["specular_disabled"] = "#define SPECULAR_DISABLED\n";
		actions.render_mode_defines["shadows_disabled"] = "#define SHADOWS_DISABLED\n";
		actions.render_mode_defines["ambient_light_disabled"] = "#define AMBIENT_LIGHT_DISABLED\n";
		actions.render_mode_defines["shadow_to_opacity"] = "#define USE_SHADOW_TO_OPACITY\n";
		actions.render_mode_defines["unshaded"] = "#define MODE_UNSHADED\n";

		bool force_vertex_shading = GLOBAL_GET("rendering/shading/overrides/force_vertex_shading");
		if (!force_vertex_shading) {
			// If forcing vertex shading, this will be defined already.
			actions.render_mode_defines["vertex_lighting"] = "#define USE_VERTEX_LIGHTING\n";
		}

		actions.render_mode_defines["debug_shadow_splits"] = "#define DEBUG_DRAW_PSSM_SPLITS\n";
		actions.render_mode_defines["fog_disabled"] = "#define FOG_DISABLED\n";

		actions.base_texture_binding_index = 1;
		actions.texture_layout_set = RenderForwardClustered::MATERIAL_UNIFORM_SET;
		actions.base_uniform_string = "material.";
		actions.base_varying_index = 14;
		actions.suppress_varying_io = true;

		actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
		actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
		actions.global_buffer_array_variable = "global_shader_uniforms.data";
		actions.instance_uniform_index_variable = "rt_instance_uniforms_ofs";

		actions.check_multiview_samplers = RendererCompositorRD::get_singleton()->is_xr_enabled(); // Make sure we check sampling multiview textures.

		// Intersection stage built-in renames (must match locals in the GLSL template).
		actions.renames["ORIGIN"] = "m_ORIGIN";
		actions.renames["DIRECTION"] = "m_DIRECTION";
		actions.renames["WORLD_ORIGIN"] = "m_WORLD_ORIGIN";
		actions.renames["WORLD_DIRECTION"] = "m_WORLD_DIRECTION";
		actions.renames["T_MIN"] = "m_T_MIN";
		actions.renames["T_MAX"] = "m_T_MAX";
		actions.renames["HIT_UV"] = "m_HIT_UV";
		actions.renames["HIT_NORMAL"] = "m_HIT_NORMAL";
		actions.renames["HIT_TANGENT"] = "m_HIT_TANGENT";
		actions.renames["PREV_POSITION"] = "m_PREV_POSITION";
		actions.renames["AABB_MIN"] = "m_AABB_MIN";
		actions.renames["AABB_MAX"] = "m_AABB_MAX";

		// Stage function rename: bypass _mkid prefix to match the GLSL macro.
		actions.renames["report_intersection"] = "report_intersection";

		compiler.initialize(actions);
	}

	// Raster shader/material/sampler defaults unused on RT path.
	default_shader = RID();
	default_material = RID();
	default_shader_rd = RID();
	default_shader_sdfgi_rd = RID();
	default_material_shader_ptr = nullptr;
	default_material_uniform_set = RID();

	overdraw_material_shader = RID();
	overdraw_material = RID();
	overdraw_material_shader_ptr = nullptr;
	overdraw_material_uniform_set = RID();

	debug_shadow_splits_material_shader = RID();
	debug_shadow_splits_material = RID();
	debug_shadow_splits_material_shader_ptr = nullptr;
	debug_shadow_splits_material_uniform_set = RID();

	default_vec4_xform_buffer = RID();
	default_vec4_xform_uniform_set = RID();
	shadow_sampler = RID();
}

void SceneShaderRaytracing::set_default_specialization(const ShaderSpecialization &p_specialization) {
	default_specialization = p_specialization;

	for (SelfList<ShaderData> *E = shader_list.first(); E; E = E->next()) {
		E->self()->pipeline_hash_map.clear_pipelines();
	}
}

void SceneShaderRaytracing::enable_advanced_shader_group(bool p_needs_multiview) {
}

bool SceneShaderRaytracing::is_multiview_shader_group_enabled() const {
	return false;
}

bool SceneShaderRaytracing::is_advanced_shader_group_enabled(bool p_multiview) const {
	return false;
}

uint32_t SceneShaderRaytracing::get_pipeline_compilations(RSE::PipelineSource p_source) {
	MutexLock lock(SceneShaderRaytracing::singleton_mutex);
	return pipeline_compilations[p_source];
}
