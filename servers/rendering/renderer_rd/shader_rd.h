/**************************************************************************/
/*  shader_rd.h                                                           */
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

#pragma once

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_types.h"

class StringBuilder;

class ShaderRD {
public:
	struct VariantDefine {
		int group = 0;
		CharString text;
		bool default_enabled = true;
		VariantDefine() {}
		VariantDefine(int p_group, const String &p_text, bool p_default_enabled) {
			group = p_group;
			default_enabled = p_default_enabled;
			text = p_text.utf8();
		}
	};

	typedef Pair<ShaderRD *, RID> ShaderVersionPair;
	typedef HashSet<ShaderVersionPair> ShaderVersionPairSet;

private:
	//versions
	CharString general_defines;
	Vector<VariantDefine> variant_defines;
	Vector<bool> variants_enabled;
	Vector<uint32_t> variant_to_group;
	HashMap<int, LocalVector<int>> group_to_variant_map;
	Vector<bool> group_enabled;
	// Groups compiled only for the versions that ask for them (version_request_group()), as Unreal compiles a material's
	// permutations only for the passes it can be drawn in (FMaterialShaderType::ShouldCompilePermutation()). Enabling
	// such a group (enable_group()) compiles it for the versions that asked; a version asking later compiles it then.
	uint64_t on_demand_groups = 0;

	Vector<RD::PipelineImmutableSampler> immutable_samplers;
	Vector<uint64_t> dynamic_buffers;

	struct Version {
		Mutex *mutex = nullptr;
		CharString uniforms;
		CharString vertex_globals;
		CharString compute_globals;
		CharString fragment_globals;
		CharString raygen_globals;
		CharString any_hit_globals;
		CharString closest_hit_globals;
		CharString miss_globals;
		CharString intersection_globals;
		HashMap<StringName, CharString> code_sections;
		Vector<CharString> custom_defines;
		Vector<WorkerThreadPool::GroupID> group_compilation_tasks;
		Vector<bool> group_loaded_from_cache;
		uint64_t requested_groups = 0; // The on-demand groups this version asked for; kept across recompilations.
		uint64_t started_groups = 0; // Groups compiled or loaded since the last _initialize_version(): started once.

		Vector<Vector<uint8_t>> variant_data;
		Vector<RID> variants;

		bool valid;
		bool dirty;
		bool initialize_needed;
		bool embedded;
	};

	struct CompileData {
		Version *version;
		int group = 0;
	};

	// Vector will have the size of SHADER_STAGE_MAX and unused stages will have empty strings.
	void _compile_variant(uint32_t p_variant, CompileData p_data);

	void _initialize_version(Version *p_version);
	void _clear_version(Version *p_version);
	void _compile_version_start(Version *p_version, int p_group);
	void _compile_version_end(Version *p_version, int p_group);
	void _compile_ensure_finished(Version *p_version);
	void _allocate_placeholders(Version *p_version, int p_group);
	// Whether the group is compiled for this version: enabled, and asked for when it is on demand.
	_FORCE_INLINE_ bool _is_group_compiled_for(const Version *p_version, int p_group) const {
		return group_enabled[p_group] && (!(on_demand_groups & (uint64_t(1) << p_group)) || (p_version->requested_groups & (uint64_t(1) << p_group)));
	}
	static void _free_variant(RID p_variant);
	static void _free_variant_on_render_thread(RID p_variant);

	RID_Owner<Version, true> version_owner;
	Mutex versions_mutex;
	HashMap<RID, Mutex *> version_mutexes;

	struct StageTemplate {
		struct Chunk {
			enum Type {
				TYPE_VERSION_DEFINES,
				TYPE_MATERIAL_UNIFORMS,
				TYPE_VERTEX_GLOBALS,
				TYPE_FRAGMENT_GLOBALS,
				TYPE_COMPUTE_GLOBALS,
				TYPE_RAYGEN_GLOBALS,
				TYPE_ANY_HIT_GLOBALS,
				TYPE_CLOSEST_HIT_GLOBALS,
				TYPE_MISS_GLOBALS,
				TYPE_INTERSECTION_GLOBALS,
				TYPE_CODE,
				TYPE_TEXT
			};

			Type type;
			StringName code;
			CharString text;
		};
		LocalVector<Chunk> chunks;
	};

	RD::PipelineType pipeline_type = RD::PIPELINE_TYPE_RASTERIZATION;

	String name;

	CharString base_compute_defines;

	String base_sha256;
	LocalVector<String> group_sha256;

	static inline ShaderVersionPairSet shader_versions_embedded_set;
	static inline Mutex shader_versions_embedded_set_mutex;

	static String shader_cache_user_dir;
	static String shader_cache_res_dir;
	static bool shader_cache_cleanup_on_start;
	static bool shader_cache_save_compressed;
	static bool shader_cache_save_compressed_zstd;
	static bool shader_cache_save_debug;
	bool shader_cache_user_dir_valid = false;
	bool shader_cache_res_dir_valid = false;

	enum StageType {
		STAGE_TYPE_VERTEX,
		STAGE_TYPE_FRAGMENT,
		STAGE_TYPE_COMPUTE,
		STAGE_TYPE_RAYGEN,
		STAGE_TYPE_ANY_HIT,
		STAGE_TYPE_CLOSEST_HIT,
		STAGE_TYPE_MISS,
		STAGE_TYPE_INTERSECTION,
		STAGE_TYPE_MAX,
	};

	StageTemplate stage_templates[STAGE_TYPE_MAX];

	void _build_variant_code(StringBuilder &p_builder, uint32_t p_variant, const Version *p_version, const StageTemplate &p_template);
	Vector<String> _build_variant_stage_sources(uint32_t p_variant, CompileData p_data);

	void _add_stage(const char *p_code, StageType p_stage_type);

	String _version_get_sha1(Version *p_version) const;
	String _get_cache_file_relative_path(Version *p_version, int p_group, const String &p_api_name);
	String _get_cache_file_path(Version *p_version, int p_group, const String &p_api_name, bool p_user_dir);
	void _load_variant_from_cache(uint32_t p_variant, CompileData p_data);
	bool _load_from_cache(Version *p_version, int p_group);
	void _save_to_cache(Version *p_version, int p_group);
	void _initialize_cache();
	void _version_set(Version *p_version, const HashMap<String, String> &p_code, const Vector<String> &p_custom_defines);

protected:
	ShaderRD();
	// Decompresses the shader code embedded in the headers generated by `glsl_builders.py`.
	static CharString _decompress_code(const uint8_t *p_compressed, int p_compressed_size, int p_size);
	void setup(const char *p_vertex_code, const char *p_fragment_code, const char *p_compute_code, const char *p_name);
	void setup_raytracing(const char *p_raygen_code, const char *p_any_hit_code, const char *p_closest_hit_code, const char *p_miss_code, const char *p_intersection_code, const char *p_name);

public:
	RID version_create(bool p_embedded = true);

	void version_set_code(RID p_version, const HashMap<String, String> &p_code, const String &p_uniforms, const String &p_vertex_globals, const String &p_fragment_globals, const Vector<String> &p_custom_defines);
	void version_set_compute_code(RID p_version, const HashMap<String, String> &p_code, const String &p_uniforms, const String &p_compute_globals, const Vector<String> &p_custom_defines);
	void version_set_raytracing_code(RID p_version, const HashMap<String, String> &p_code, const String &p_uniforms, const String &p_raygen_globals, const String &p_any_hit_globals, const String &p_closest_hit_globals, const String &p_miss_globals, const String &p_intersection_globals, const Vector<String> &p_custom_defines);

	_FORCE_INLINE_ RID version_get_shader(RID p_version, int p_variant) {
		ERR_FAIL_INDEX_V(p_variant, variant_defines.size(), RID());
		ERR_FAIL_COND_V(!variants_enabled[p_variant], RID());

		Version *version = version_owner.get_or_null(p_version);
		ERR_FAIL_NULL_V(version, RID());

		MutexLock lock(*version->mutex);

		if (version->dirty) {
			_initialize_version(version);
			for (int i = 0; i < group_enabled.size(); i++) {
				if (!_is_group_compiled_for(version, i)) {
					_allocate_placeholders(version, i);
					continue;
				}
				_compile_version_start(version, i);
			}
		}

		uint32_t group = variant_to_group[p_variant];
		if (version->group_compilation_tasks[group] != 0) {
			_compile_version_end(version, group);
		}

		if (!version->valid) {
			return RID();
		}

		return version->variants[p_variant];
	}

	bool version_is_valid(RID p_version);
	bool version_is_group_compiled(RID p_version, int p_group);
	// Whether no compilation (or load from the shader cache) of the version's enabled groups is in flight: never waits,
	// starts the compilation of a dirty version. Once true, version_is_valid() returns at once (false for a failed one).
	bool version_is_compiled(RID p_version);

	bool version_free(RID p_version);

	// Enable/disable variants for things that you know won't be used at engine initialization time .
	void set_variant_enabled(int p_variant, bool p_enabled);
	bool is_variant_enabled(int p_variant) const;
	int64_t get_variant_count() const;
	int get_variant_to_group(int p_variant) const;

	// Enable/disable groups for things that might be enabled at run time.
	void enable_group(int p_group);
	bool is_group_enabled(int p_group) const;
	// An on-demand group is compiled only for the versions that ask for it (version_request_group()), once it is enabled.
	// Set before any version is created.
	void set_group_on_demand(int p_group, bool p_on_demand);
	bool is_group_on_demand(int p_group) const;
	// The version asks for an on-demand group: compiled now (in the background) if the group is enabled, otherwise when it
	// is. Never waits. Its variants are placeholders until then. Returns true the first time.
	bool version_request_group(RID p_version, int p_group);
	// Whether the group is compiled for this version: enabled, and asked for when it is on demand (the shader baker).
	bool version_is_group_enabled(RID p_version, int p_group);
	int64_t get_group_count() const;
	const LocalVector<int> &get_group_to_variants(int p_group) const;

	const String &get_name() const;

	const Vector<uint64_t> &get_dynamic_buffers() const;

	static void shaders_embedded_set_lock();
	static const ShaderVersionPairSet &shaders_embedded_set_get();
	static void shaders_embedded_set_unlock();

	static void set_shader_cache_user_dir(const String &p_dir);
	static const String &get_shader_cache_user_dir();
	static void set_shader_cache_res_dir(const String &p_dir);
	static const String &get_shader_cache_res_dir();
	static void set_shader_cache_save_compressed(bool p_enable);
	static void set_shader_cache_save_compressed_zstd(bool p_enable);
	static void set_shader_cache_save_debug(bool p_enable);

	static Vector<RD::ShaderStageSPIRVData> compile_stages(const Vector<String> &p_stage_sources, const Vector<uint64_t> &p_dynamic_buffers);
	static PackedByteArray save_shader_cache_bytes(const LocalVector<int> &p_variants, const Vector<Vector<uint8_t>> &p_variant_data);

	Vector<String> version_build_variant_stage_sources(RID p_version, int p_variant);
	RenderingServerTypes::ShaderNativeSourceCode version_get_native_source_code(RID p_version);
	String version_get_cache_file_relative_path(RID p_version, int p_group, const String &p_api_name);

	struct DynamicBuffer {
		static uint64_t encode(uint32_t p_set_id, uint32_t p_binding) {
			return uint64_t(p_set_id) << 32ul | uint64_t(p_binding);
		}
	};

	// Dynamic Buffers specifies Which buffers will be persistent/dynamic when used.
	// See DynamicBuffer::encode. We need this argument because SPIR-V does not distinguish between a
	// uniform buffer and a dynamic uniform buffer. At shader level they're the same thing, but the PSO
	// is created slightly differently and they're bound differently.
	// On D3D12 the Root Layout is also different.
	void initialize(const Vector<String> &p_variant_defines, const String &p_general_defines = "", const Vector<RD::PipelineImmutableSampler> &p_immutable_samplers = Vector<RD::PipelineImmutableSampler>(), const Vector<uint64_t> &p_dynamic_buffers = Vector<uint64_t>());
	void initialize(const Vector<VariantDefine> &p_variant_defines, const String &p_general_defines = "", const Vector<RD::PipelineImmutableSampler> &p_immutable_samplers = Vector<RD::PipelineImmutableSampler>(), const Vector<uint64_t> &p_dynamic_buffers = Vector<uint64_t>());

	virtual ~ShaderRD();
};
