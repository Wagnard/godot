/**************************************************************************/
/*  scene_shader_raytracing.h                                             */
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

#include "core/object/worker_thread_pool.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/pipeline_hash_map_rd.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h" // IWYU pragma: keep
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_displace.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/raytracing_omm_bake.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/scene_raytracing_raygen.glsl.gen.h"

namespace RendererSceneRenderImplementation {

class SceneShaderRaytracing {
private:
	static SceneShaderRaytracing *singleton;
	static Mutex singleton_mutex;

public:
	enum ShaderGroup {
		SHADER_GROUP_BASE, // Always compiled at the beginning.
		SHADER_GROUP_ADVANCED,
		SHADER_GROUP_MULTIVIEW,
		SHADER_GROUP_ADVANCED_MULTIVIEW,
	};

	enum ShaderVersion {
		SHADER_VERSION_OPAQUE, // Single simplified opaque pass
		SHADER_VERSION_MAX
	};

	enum ShaderColorPassFlags {
		SHADER_COLOR_PASS_FLAG_NONE = 0 // Simplified: no flags needed
	};

	enum PipelineVersion {
		PIPELINE_VERSION_OPAQUE, // Single simplified opaque pass
		PIPELINE_VERSION_MAX
	};

	enum PipelineColorPassFlags {
		PIPELINE_COLOR_PASS_FLAG_NONE = 0 // Simplified: no flags needed
	};

	// Raytracing specialization constant flags (must match shader RT_FLAG_* defines): each combination is a pipeline of its
	// own, its hit groups compiled again. Only what changes the code goes here; the sample count and the bounces are read
	// from the parameters buffer (RT_PARAM_*) and the fog from the scene data, so changing them keeps the pipeline.
	enum RaytracingFlags {
		RT_FLAG_NONE = 0,
		RT_FLAG_DEBUG_VIS_ENABLED = (1 << 0),
		RT_FLAG_DLSS_RR_ENABLED = (1 << 1),
		// The raygen splits the light of the surfaces the camera sees into diffuse and specular for NRD (PT_DENOISER_NRD_REBLUR).
		// (1 << 2) was the fog until it was read from the scene data (SCENE_DATA_FLAGS_USE_FOG).
		RT_FLAG_NRD_ENABLED = (1 << 2),
		RT_FLAG_SER_ENABLED = (1 << 3),
		RT_FLAG_RAY_QUERY_SHADOWS_ENABLED = (1 << 4),
	};

	// RT pipeline limits (must match GLSL payload/hit attribute struct sizes).
	// 1: primary/bounce from raygen
	// 2: shadow ray from closest_hit (NEE)
	constexpr static uint32_t RT_MAX_RECURSION_DEPTH = 2;

	// Pathtracing parameter indices for the float[16] params buffer.
	// Must match RT_PARAM_* defines in raytracing_inc.glsl.
	static constexpr int RT_PARAM_VIS_MODE = 0;
	static constexpr int RT_PARAM_SAMPLE_COUNT = 1;
	static constexpr int RT_PARAM_MAX_BOUNCES = 2;
	static constexpr int RT_PARAM_DENOISER = 3;
	static constexpr int RT_PARAM_FRONT_LAYER_COUNT = 4;
	static constexpr int RT_PARAM_DECAL_COUNT = 5;
	static constexpr int RT_PARAM_PROCEDURAL_MASKS = 6; // RT_INSTANCE_MASK_PROCEDURAL_* bits in the TLAS this frame.
	static constexpr int RT_PARAM_ENV_SAMPLING = 7; // 1: the sky sampling distribution (binding 40) was built this frame.
	static constexpr int RT_PARAM_BACKGROUND_COLOR = 8; // 8-10: linear RGB, with the background energy.
	static constexpr int RT_PARAM_BACKGROUND_COLOR_USED = 11;
	static constexpr int RT_PARAM_REFLECTION_BACKGROUND = 12; // 1: REFLECTION_SOURCE_BG with a color background.
	// RT_PARAM_FLAG_* bits: switches changed at runtime, read by the shaders (not in the pipeline variant key, so toggling
	// one rebuilds nothing). The 16 slots are all taken: a later live switch takes the next bit here.
	static constexpr int RT_PARAM_FLAGS = 13;
	static constexpr uint32_t RT_PARAM_FLAG_LIGHT_SAMPLING_RIS = 1u << 0; // rendering/pathtracing/light_sampling_ris.
	static constexpr uint32_t RT_PARAM_FLAG_RUSSIAN_ROULETTE = 1u << 1; // rendering/pathtracing/russian_roulette.
	static constexpr int RT_PARAM_LIGHT_COUNT = 14;
	static constexpr int RT_PARAM_FRAME_INDEX = 15;

	// The bounces a path may take (RT_PARAM_MAX_BOUNCES).
	static constexpr int RT_MAX_BOUNCES = 8;

	// The rt_flags from pathtracing environment settings.
	// `p_environment` may be invalid (RT active with no pathtracing environment).
	static uint32_t compute_rt_flags(RID p_environment);

	struct ShaderSpecialization {
		union {
			struct {
				uint32_t use_forward_gi : 1;
				uint32_t use_light_projector : 1;
				uint32_t use_light_soft_shadows : 1;
				uint32_t use_directional_soft_shadows : 1;
				uint32_t decal_use_mipmaps : 1;
				uint32_t projector_use_mipmaps : 1;
				uint32_t use_depth_fog : 1;
				uint32_t use_lightmap_bicubic_filter : 1;
				uint32_t soft_shadow_samples : 6;
				uint32_t penumbra_shadow_samples : 6;
				uint32_t directional_soft_shadow_samples : 6;
				uint32_t directional_penumbra_shadow_samples : 6;
			};

			uint32_t packed_0;
		};

		union {
			struct {
				uint32_t multimesh : 1;
				uint32_t multimesh_format_2d : 1;
				uint32_t multimesh_has_color : 1;
				uint32_t multimesh_has_custom_data : 1;
			};

			uint32_t packed_1;
		};

		uint32_t packed_2;
	};

	struct UbershaderConstants {
		union {
			struct {
				uint32_t cull_mode : 2;
			};

			uint32_t packed_0;
		};
	};

	struct ShaderData : public RendererRD::MaterialStorage::ShaderData {
		enum DepthDraw {
			DEPTH_DRAW_DISABLED,
			DEPTH_DRAW_OPAQUE,
			DEPTH_DRAW_ALWAYS
		};

		enum DepthTest {
			DEPTH_TEST_DISABLED,
			DEPTH_TEST_ENABLED
		};

		enum Cull {
			CULL_DISABLED,
			CULL_FRONT,
			CULL_BACK
		};

		enum CullVariant {
			CULL_VARIANT_NORMAL,
			CULL_VARIANT_REVERSED,
			CULL_VARIANT_DOUBLE_SIDED,
			CULL_VARIANT_MAX

		};

		enum AlphaAntiAliasing {
			ALPHA_ANTIALIASING_OFF,
			ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE,
			ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE
		};

		struct PipelineKey {
			RD::VertexFormatID vertex_format_id = 0;
			RD::FramebufferFormatID framebuffer_format_id = 0;
			RD::PolygonCullMode cull_mode = RD::POLYGON_CULL_MAX;
			RSE::PrimitiveType primitive_type = RSE::PRIMITIVE_MAX;
			PipelineVersion version = PipelineVersion::PIPELINE_VERSION_MAX;
			uint32_t color_pass_flags = 0;
			ShaderSpecialization shader_specialization = {};
			uint32_t wireframe = false;
			uint32_t ubershader = false;

			uint32_t hash() const {
				uint32_t h = hash_murmur3_one_64(vertex_format_id);
				h = hash_murmur3_one_32(framebuffer_format_id, h);
				h = hash_murmur3_one_32(cull_mode, h);
				h = hash_murmur3_one_32(primitive_type, h);
				h = hash_murmur3_one_32(version, h);
				h = hash_murmur3_one_32(color_pass_flags, h);
				h = hash_murmur3_one_32(shader_specialization.packed_0, h);
				h = hash_murmur3_one_32(shader_specialization.packed_1, h);
				h = hash_murmur3_one_32(shader_specialization.packed_2, h);
				h = hash_murmur3_one_32(wireframe, h);
				h = hash_murmur3_one_32(ubershader, h);
				return hash_fmix32(h);
			}
		};

		void _create_pipeline(PipelineKey p_pipeline_key);
		PipelineHashMapRD<PipelineKey, ShaderData, void (ShaderData::*)(PipelineKey)> pipeline_hash_map;

		RID version;
		RID raygen_version;

		static const uint32_t VERTEX_INPUT_MASKS_SIZE = SHADER_VERSION_MAX; // Simplified to just one shader version
		std::atomic<uint64_t> vertex_input_masks[VERTEX_INPUT_MASKS_SIZE] = {};

		Vector<ShaderCompiler::GeneratedCode::Texture> texture_uniforms;

		Vector<uint32_t> ubo_offsets;
		uint32_t ubo_size = 0;

		String code;

		DepthDraw depth_draw = DEPTH_DRAW_OPAQUE;
		DepthTest depth_test = DEPTH_TEST_ENABLED;

		int blend_mode = BLEND_MODE_MIX;
		int alpha_antialiasing_mode = ALPHA_ANTIALIASING_OFF;

		bool uses_point_size = false;
		bool uses_alpha = false;
		bool uses_blend_alpha = false;
		bool uses_alpha_clip = false;
		bool uses_alpha_antialiasing = false;
		bool uses_depth_prepass_alpha = false;
		bool uses_discard = false;
		bool uses_roughness = false;
		bool uses_normal = false;
		bool uses_tangent = false;
		bool uses_particle_trails = false;
		bool uses_normal_map = false;
		bool wireframe = false;

		bool unshaded = false;
		bool uses_vertex = false;
		bool uses_position = false;
		bool uses_sss = false;
		bool uses_transmittance = false;
		bool uses_screen_texture = false;
		bool uses_depth_texture = false;
		bool uses_normal_texture = false;
		bool uses_time = false;
		bool uses_vertex_time = false;
		bool uses_fragment_time = false;
		bool writes_modelview_or_projection = false;
		bool uses_world_coordinates = false;
		bool uses_screen_texture_mipmaps = false;
		Cull cull_mode = CULL_DISABLED;

		uint64_t last_pass = 0;
		uint32_t index = 0;

		_FORCE_INLINE_ bool uses_alpha_pass() const {
			bool has_read_screen_alpha = uses_screen_texture || uses_depth_texture || uses_normal_texture;
			bool has_base_alpha = (uses_alpha && (!uses_alpha_clip || uses_alpha_antialiasing)) || has_read_screen_alpha;
			bool has_blend_alpha = uses_blend_alpha;
			bool has_alpha = has_base_alpha || has_blend_alpha;
			bool no_depth_draw = depth_draw == DEPTH_DRAW_DISABLED;
			bool no_depth_test = depth_test == DEPTH_TEST_DISABLED;
			return has_alpha || has_read_screen_alpha || no_depth_draw || no_depth_test;
		}

		_FORCE_INLINE_ bool uses_depth_in_alpha_pass() const {
			bool no_depth_draw = depth_draw == DEPTH_DRAW_DISABLED;
			bool no_depth_test = depth_test == DEPTH_TEST_DISABLED;
			return (uses_depth_prepass_alpha || uses_alpha_antialiasing) && !(no_depth_draw || no_depth_test);
		}

		_FORCE_INLINE_ bool uses_shared_shadow_material() const {
			bool backface_culling = cull_mode == CULL_BACK;
			return !uses_particle_trails && !writes_modelview_or_projection && !uses_vertex && !uses_position && !uses_discard && !uses_depth_prepass_alpha && !uses_alpha_clip && !uses_alpha_antialiasing && backface_culling && !uses_point_size && !uses_world_coordinates && !wireframe;
		}

		virtual void set_code(const String &p_Code);

		virtual bool is_animated() const;
		virtual bool casts_shadows() const;
		virtual RenderingServerTypes::ShaderNativeSourceCode get_native_source_code() const;
		virtual Pair<ShaderRD *, RID> get_native_shader_and_version() const;
		ShaderVersion _get_shader_version(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const;
		RID _get_shader_variant(ShaderVersion p_shader_version) const;
		void _clear_vertex_input_mask_cache();
		RID get_shader_variant(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const;
		RID get_raygen_shader_variant() const;
		uint64_t get_vertex_input_mask(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader);
		RD::PolygonCullMode get_cull_mode_from_cull_variant(CullVariant p_cull_variant);
		bool is_valid() const;

		SelfList<ShaderData> shader_list_element;
		ShaderData();
		virtual ~ShaderData();
	};

	SelfList<ShaderData>::List shader_list;

	RendererRD::MaterialStorage::ShaderData *_create_shader_func();
	static RendererRD::MaterialStorage::ShaderData *_create_shader_funcs() {
		return static_cast<SceneShaderRaytracing *>(singleton)->_create_shader_func();
	}

	static SceneShaderRaytracing *get_singleton();

	struct MaterialData : public RendererRD::MaterialStorage::MaterialData {
		ShaderData *shader_data = nullptr;
		RID uniform_set;
		uint64_t last_pass = 0;
		uint32_t index = 0;
		RID next_pass;
		uint8_t priority;
		virtual void set_render_priority(int p_priority);
		virtual void set_next_pass(RID p_pass);
		virtual bool update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty);
		virtual ~MaterialData();
	};

	RendererRD::MaterialStorage::MaterialData *_create_material_func(ShaderData *p_shader);
	static RendererRD::MaterialStorage::MaterialData *_create_material_funcs(RendererRD::MaterialStorage::ShaderData *p_shader) {
		return static_cast<SceneShaderRaytracing *>(singleton)->_create_material_func(static_cast<ShaderData *>(p_shader));
	}

	SceneRaytracingRaygenShaderRD raygen_shader;

	struct TextureUniformInfo {
		StringName name;
		ShaderLanguage::ShaderNode::Uniform::Hint hint = ShaderLanguage::ShaderNode::Uniform::HINT_NONE;
		bool use_color = false;
		bool is_global = false;
		bool is_array = false; // sampler2DArray: index into the texture array table.
		uint32_t buffer_offset = 0; // Stored as bindless index in UBO
	};

	// Preprocessed custom HG GLSL (templates filled per compile). One entry per slot, all variants.
	struct CustomShaderEntry {
		String name; // The shader's path, for messages.
		String defines; // The compiler's defines the RT templates honor (vertex space).
		String vertex_code;
		String fragment_code;
		String fragment_globals;
		String intersection_code;
		String intersection_globals;
		String uniform_members; // GLSL struct members for uniform buffer
		uint32_t uniform_total_size = 0;
		Vector<uint32_t> uniform_offsets;
		HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
		Vector<TextureUniformInfo> texture_uniforms; // Sampler2D packed as bindless indices after UBO
		bool uses_alpha_clip = false; // Writes ALPHA_SCISSOR_THRESHOLD; needs per-HG any-hit
		bool writes_vertex = false; // vertex() moves VERTEX or writes MODELVIEW_MATRIX: the BLAS needs the displaced positions.
		bool writes_modelview = false; // vertex() writes MODELVIEW_MATRIX (billboards): raster draws VERTEX through it.
		bool uses_vertex_time = false;
		bool vertex_reads_camera = false; // vertex() reads the view or projection.
		Vector<ShaderCompiler::GeneratedCode::Varying> varyings; // Interpolated by the hit shaders on displaced geometry.
		bool is_procedural = false; // Uses intersection shader instead of triangle geometry
		uint32_t alpha_texture_buffer_offset = UINT32_MAX; // Byte offset of hint_alpha texture index in CustomMaterialUniforms UBO; UINT32_MAX if absent
		// Its cut-out can be baked into an opacity micromap per surface (rt_alpha_dependencies()); COLOR reaches it.
		bool alpha_bakeable = false;
		bool alpha_reads_color = false;
	};

	// 128-bit identity (dual hash64 with distinct salt). Treated as source equality.
	struct SourceHash128 {
		uint64_t a = 0;
		uint64_t b = 0;
		bool operator==(const SourceHash128 &p_o) const { return a == p_o.a && b == p_o.b; }
		bool is_zero() const { return a == 0 && b == 0; }
	};
	struct SourceHash128Hasher {
		static _FORCE_INLINE_ uint32_t hash(const SourceHash128 &p_h) {
			// Mix both halves so a-only collisions don't create bucket pile-ups.
			// Constant: integral part of the Golden Ratio's fractional part 0.61803398875… multiplied by 2^64
			return hash_one_uint64(p_h.a ^ (p_h.b * 0x9E3779B97F4A7C15ULL));
		}
	};

	enum class HGState : uint8_t {
		Empty,
		Pending, // Preprocessed; SPIR-V compile not done for this bundle yet.
		Ready, // Per-HG RID valid; next rebuild will publish.
		Failed, // Permanent compile failure for this slot in this bundle.
	};

	// Append-only by source hash. Index 0 = default HG.
	struct HitGroupSlot {
		SourceHash128 source_hash;
		HGState state = HGState::Empty;
		CustomShaderEntry entry;
		// The TLAS met an instance of it (in or near the view) before its hit group was in the pipeline: built before the
		// slots only precached, the nearest first (request_hit_group()).
		bool requested = false;
		float requested_distance = 0.0f;
		uint64_t requested_frame = 0;
	};

	LocalVector<HitGroupSlot> hit_group_slots;
	uint32_t pipeline_swap_count = 0; // Pipelines rebuilt and swapped in (GODOT_RT_LOG_CACHES).
	bool usable = false;
	HashMap<SourceHash128, uint32_t, SourceHash128Hasher> source_hash_to_slot;

	// One RD RT pipeline variant (rt_flags key). Owns base + per-slot HG shaders (RG+miss baked in per HG).
	struct PipelineBundle {
		RID pipeline; // RD::free_rid on swap (deferred internally).
		RID hit_sbt; // RD::free_rid on swap.
		RID base_shader; // Immutable for variant lifetime; uniform_set is bound to this.

		// Parallel to hit_group_slots.
		LocalVector<RID> per_hg_shaders;
		LocalVector<HGState> per_hg_states;

		// Baked into current pipeline (TLAS skip / is_hg_ready_in_bundle).
		LocalVector<bool> live_ready_mask;
		uint32_t live_hg_count = 0;

		// The pipeline's hit group behind each slot's SBT record: the slot's own index after a full build; for hit groups
		// added since (RD::raytracing_pipeline_add_hit_groups()), the index they were appended at. Slots not in the
		// pipeline use empty_group, a hit group with no shader.
		LocalVector<uint32_t> slot_groups;
		uint32_t pipeline_group_count = 0;
		uint32_t empty_group = 0;
		RD::HitShaderBindingTableRange hit_sbt_range = 0;
		uint32_t hit_sbt_capacity = 0; // Records in hit_sbt_range.

		bool dirty = false;
		bool initial_pipeline_built = false;
		// The frame a view last traced with it (-1: never, as init()'s probe); only bundles in use are rebuilt.
		int64_t last_used_frame = -1;
	};

	HashMap<uint32_t, PipelineBundle> pipeline_bundles;

	// Single-lane async bundle rebuild (worker: SPIR-V + raytracing_pipeline_create; main: SBT + swap).
	struct PipelineBuildTask;
	struct CompileLane {
		Mutex mutex;
		PipelineBuildTask *current = nullptr;
		LocalVector<PipelineBuildTask *> queue;
	};

	// 0 for the default hit group; r_failed: the material's shader cannot run in a hit group (an unsupported texture type,
	// a compile error), so the default one stands in for it.
	uint32_t register_custom_shader(uint32_t p_shader_id, RID p_material, bool *r_failed = nullptr);
	uint32_t register_procedural_shader(uint32_t p_shader_id, RID p_material);

	const CustomShaderEntry *get_custom_shader_entry(uint32_t p_slot_index) const;
	uint32_t get_hit_group_slot_count() const { return hit_group_slots.size(); }
	uint32_t get_pipeline_swap_count() const { return pipeline_swap_count; }

	bool is_hg_ready_in_bundle(uint32_t p_slot_index, uint32_t p_rt_flags) const;
	// An instance in the TLAS, p_distance from the camera, waits for this slot's hit group: the next builds take it first.
	void request_hit_group(uint32_t p_slot_index, float p_distance);
	// Hit groups registered but not yet in the pipeline of the variant a view traced with last (0 before any trace):
	// their surfaces trace with the compiling fallback, or not at all for cut-outs.
	uint32_t get_pending_hit_group_count() const;

	// Compute pipeline running the slot's vertex() on every vertex (raytracing_displace.glsl); invalid if the slot
	// does not displace or failed to compile. Compiled on first use, unless precompiled.
	RID get_displace_pipeline(uint32_t p_slot_index, RID &r_shader);
	// The program baking a slot's cut-out into opacity micromaps (raytracing_omm_bake.glsl); invalid when the slot's
	// cut-out cannot be baked (CustomShaderEntry::alpha_bakeable) or failed to compile.
	RID get_omm_bake_pipeline(uint32_t p_slot_index, RID &r_shader);
	// Starts compiling, on a worker, the slot's displacement program and (p_omm_bake) its opacity micromap bake program,
	// those it needs: compiled on the render thread at first use, they stalled the frame its surfaces first appeared in.
	void precompile_slot_programs(uint32_t p_slot_index, bool p_omm_bake);
	void drain_completed_compiles();
	void finalize_custom_shaders();

	// p_mark_used: a view traces with it this frame (its hit groups are then compiled first).
	const PipelineBundle &ensure_pipeline_bundle(uint32_t p_rt_flags, bool p_mark_used = true);

	RID get_raytracing_pipeline(uint32_t p_rt_flags) { return ensure_pipeline_bundle(p_rt_flags).pipeline; }
	RID get_hit_sbt(uint32_t p_rt_flags) { return ensure_pipeline_bundle(p_rt_flags).hit_sbt; }
	RID get_pipeline_base_shader(uint32_t p_rt_flags) { return ensure_pipeline_bundle(p_rt_flags).base_shader; }

	// Uniform set layout shader (same as base_shader).
	RID get_pipeline_shader_rd(uint32_t p_rt_flags) { return ensure_pipeline_bundle(p_rt_flags).base_shader; }

private:
	// Source-hash slot management.
	uint32_t _register_slot(uint32_t p_shader_id, RID p_material, bool p_is_procedural, bool *r_failed = nullptr);
	bool _preprocess_shader(RID p_material, bool p_is_procedural, CustomShaderEntry &r_entry);
	void _strip_texture_globals(String &r_globals, const String &p_tex_name);
	bool _finalize_uniforms_with_textures(CustomShaderEntry &r_entry, const ShaderCompiler::GeneratedCode &p_gen_code, const HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> &p_uniforms, bool p_strip_intersection_globals);

	// Bundle build / rebuild.
	void _bundle_resize_for_slots(PipelineBundle &r_bundle);
	bool _build_initial_bundle(uint32_t p_rt_flags, PipelineBundle &r_bundle);
	void _kick_rebuild_if_idle();
	const PipelineBundle *_get_last_traced_bundle() const;

	// Compile lane / worker.
	void _enqueue_build(PipelineBuildTask *p_task);
	void _dispatch_next_locked(); // CompileLane::mutex MUST be held.
	void _build_pipeline_worker(PipelineBuildTask *p_task);
	static void _build_pipeline_worker_static(void *p_userdata);
	void _load_hit_group_from_cache(uint32_t p_index, PipelineBuildTask *p_task);
	void _compile_hit_group(uint32_t p_index, PipelineBuildTask *p_task);
	void _finalize_pipeline_build(PipelineBuildTask *p_task);
	void _free_unused_build_outputs(PipelineBuildTask *p_task);
	void _drain_lane_inline_main_thread();
	void _join_lane_for_shutdown();
	PipelineBuildTask *_make_pipeline_build_task(uint32_t p_rt_flags, PipelineBundle &p_bundle);

	// Eager/late stage assembly bits cached per variant for compile workers.
	struct VariantCompileContext {
		// The variant's stages (CH and AH: hit group 0's); their SPIR-V (RAYGEN + MISS, and the AH of hit groups without
		// their own) is compiled by the first worker that misses the binary cache.
		Vector<String> base_sources;
		String base_sources_key; // Their hash, part of every hit group's cache key.
		String ch_template; // Template GLSL with /* RT_CUSTOM_* */ markers.
		String ah_template;
		String is_template; // With ENABLE_INTERSECTION_SHADERS.
		// The base any-hit for procedural hit groups without their own: their hit attributes are the intersection
		// shader's (ENABLE_INTERSECTION_SHADERS), hit group 0's are a triangle's.
		String ah_procedural_src;
	};
	HashMap<uint32_t, VariantCompileContext> variant_compile_contexts;
	HashMap<uint32_t, VariantCompileContext> bake_compile_contexts; // The shader baker's, which create no bundle.

	bool _build_variant_compile_context(uint32_t p_rt_flags, VariantCompileContext &r_ctx);
	bool _ensure_variant_compile_context(uint32_t p_rt_flags);

	// A custom hit group's own stages, from the variant's templates (ah: empty without the slot's own any-hit, is: empty
	// for a triangle hit group).
	struct HitGroupSources {
		String ch;
		String ah;
		String is;
	};
	static HitGroupSources _hit_group_sources(const CustomShaderEntry &p_entry, const VariantCompileContext &p_ctx);
	String _displace_source(const CustomShaderEntry &p_entry) const;
	String _omm_bake_source(const CustomShaderEntry *p_entry) const; // Null: hit group 0's (BaseMaterial3D's alpha).

	RaytracingDisplaceShaderRD displace_shader;
	RID displace_shader_version;
	String displace_template;
	// A compute program compiled and created by a worker (precompile_slot_programs()), adopted by the render thread.
	struct ProgramBuild {
		String source;
		String name;
		RID shader;
		RID pipeline;
		bool succeeded = false;
		bool omm_bake = false; // Its list: omm_bake_programs, else displace_programs.
		uint32_t slot = 0;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	};
	struct DisplaceProgram {
		RID shader;
		RID pipeline;
		bool failed = false;
		ProgramBuild *build = nullptr; // In flight.
	};
	LocalVector<DisplaceProgram> displace_programs; // Parallel to hit_group_slots.

	// Opacity micromap bakes: the template, and its program per hit group slot (slot 0: BaseMaterial3D's alpha).
	RaytracingOmmBakeShaderRD omm_bake_shader;
	RID omm_bake_shader_version;
	String omm_bake_template;
	LocalVector<DisplaceProgram> omm_bake_programs; // Parallel to hit_group_slots.

	LocalVector<ProgramBuild *> program_builds; // In flight.
	// The binary (through the disk cache), its shader and pipeline: false if any fails.
	bool _create_compute_program(const String &p_source, const String &p_name, RID &r_shader, RID &r_pipeline) const;
	static void _program_build_worker(void *p_userdata);
	void _start_program_build(LocalVector<DisplaceProgram> &r_programs, bool p_omm_bake, uint32_t p_slot_index, const String &p_source, const String &p_name);
	// Waits for its worker if needed, then installs its result into its program.
	void _finish_program_build(ProgramBuild *p_build);

	bool async_compilation_enabled = true;
	CompileLane compile_lane;

	// The binaries compiled from material code (hit groups, displacement and opacity micromap bake programs) and the
	// variants' base, which ShaderRD does not cache: in <shader cache>/SceneShaderRaytracing, keyed on their full
	// sources, the engine version and the API, as ShaderRD keys its own. Translating them (Mesa's SPIR-V to DXIL on
	// D3D12) took seconds at every launch, the materials traced with a neutral fallback meanwhile.
	String binary_cache_dir; // Empty: no cache (shader cache disabled).
	// Read only, after binary_cache_dir: the binaries the shader baker put in an exported project (ShaderRD's res dir).
	String binary_cache_res_dir;
	String binary_cache_api; // In the file names, as ShaderRD's.
	static String _binary_cache_key(const String &p_name, const Vector<String> &p_sources, const String &p_extra = String());
	Vector<uint8_t> _binary_cache_load(const String &p_key) const;
	void _binary_cache_save(const String &p_key, const Vector<uint8_t> &p_binary) const;
	// One compute program from p_source, through the cache.
	Vector<uint8_t> _compile_compute_binary(const String &p_source, const String &p_name) const;

public:
	// The shader baker (editor/export/shader_baker_export_plugin.cpp) compiles ahead of time the binaries this class
	// builds from GLSL it assembles itself, outside ShaderRD: each variant's base, the hit group of every material, its
	// displacement and opacity micromap bake programs. An exported project reads them from ShaderRD's res dir
	// (res://.godot/shader_cache) under the names the binary cache gives them, after the user's cache.
	struct BakeProgram {
		String cache_key; // _binary_cache_key().
		String name;
		LocalVector<RD::ShaderStage> stages; // In the order the run time compiles them.
		Vector<String> sources; // Parallel to stages.
	};
	// The variants' base programs and hit group 0's opacity micromap bake. p_simple_shadows and p_ser: the project's
	// rendering/pathtracing/use_simple_shadows and use_shader_execution_reordering.
	void get_bake_programs(bool p_simple_shadows, bool p_ser, LocalVector<BakeProgram> &r_programs);
	// p_material's hit group in every such variant, and its displacement and opacity micromap bake programs; nothing for a
	// material path traced by hit group 0.
	void get_material_bake_programs(RID p_material, bool p_simple_shadows, bool p_ser, LocalVector<BakeProgram> &r_programs);
	static String get_binary_cache_file_name(const String &p_key, const String &p_api);
	static Vector<uint8_t> make_binary_cache_file(const Vector<uint8_t> &p_binary);
	// Null when the path tracer's shaders were never initialized (no Forward+ renderer).
	static SceneShaderRaytracing *get_singleton_if_initialized();

	void invalidate_pipeline_bundles();

	ShaderCompiler compiler;

	RID default_shader;
	RID default_material;
	RID overdraw_material_shader;
	RID overdraw_material;
	RID debug_shadow_splits_material_shader;
	RID debug_shadow_splits_material;
	RID default_shader_rd;
	RID raygen_shader_version;
	RID default_shader_sdfgi_rd;

	RID default_vec4_xform_buffer;
	RID default_vec4_xform_uniform_set;

	RID shadow_sampler;

	RID default_material_uniform_set;
	ShaderData *default_material_shader_ptr = nullptr;

	RID overdraw_material_uniform_set;
	ShaderData *overdraw_material_shader_ptr = nullptr;

	RID debug_shadow_splits_material_uniform_set;
	ShaderData *debug_shadow_splits_material_shader_ptr = nullptr;

	ShaderSpecialization default_specialization = {};

	uint32_t pipeline_compilations[RSE::PIPELINE_SOURCE_MAX] = {};

	~SceneShaderRaytracing();

	void init(const String p_defines);
	// False when the base ray tracing pipeline could not be built (shader translation or pipeline creation failed):
	// the path tracer then stays off and the views are rasterized.
	bool is_usable() const { return usable; }
	void set_default_specialization(const ShaderSpecialization &p_specialization);
	void enable_advanced_shader_group(bool p_needs_multiview = false);
	bool is_multiview_shader_group_enabled() const;
	bool is_advanced_shader_group_enabled(bool p_multiview) const;
	uint32_t get_pipeline_compilations(RSE::PipelineSource p_source);

protected:
	SceneShaderRaytracing();
};

} // namespace RendererSceneRenderImplementation
