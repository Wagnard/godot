/**************************************************************************/
/*  render_forward_clustered.h                                            */
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
#include "core/templates/paged_allocator.h"
#include "servers/rendering/multi_uma_buffer.h"
#include "servers/rendering/renderer_rd/cluster_builder_rd.h"
#include "servers/rendering/renderer_rd/effects/dlss.h"
#include "servers/rendering/renderer_rd/effects/fsr2.h"
#include "servers/rendering/renderer_rd/effects/fsr_frame_generation.h"
#include "servers/rendering/renderer_rd/effects/motion_vectors_store.h"
#include "servers/rendering/renderer_rd/effects/ss_effects.h"
#include "servers/rendering/renderer_rd/effects/taa.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/best_fit_normal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/integrate_dfg.glsl.gen.h"

#ifdef METAL_ENABLED
#include "servers/rendering/renderer_rd/effects/metal_fx.h"
#endif

#define RB_SCOPE_FORWARD_CLUSTERED SNAME("forward_clustered")

#define RB_TEX_SPECULAR SNAME("specular")
#define RB_TEX_SPECULAR_MSAA SNAME("specular_msaa")
#define RB_TEX_NORMAL_ROUGHNESS SNAME("normal_roughness")
#define RB_TEX_NORMAL_ROUGHNESS_MSAA SNAME("normal_roughness_msaa")
#define RB_TEX_VOXEL_GI SNAME("voxel_gi")
#define RB_TEX_VOXEL_GI_MSAA SNAME("voxel_gi_msaa")
namespace RendererSceneRenderImplementation {

class RenderForwardClustered : public RendererSceneRenderRD {
	friend SceneShaderForwardClustered;
	friend SceneShaderRaytracing;
	friend class RenderRaytracing;

protected:
	enum {
		SCENE_UNIFORM_SET = 0,
		RENDER_PASS_UNIFORM_SET = 1,
		TRANSFORMS_UNIFORM_SET = 2,
		MATERIAL_UNIFORM_SET = 3,
	};

	enum {
		SDFGI_MAX_CASCADES = 8,
		MAX_VOXEL_GI_INSTANCESS = 8,
		MAX_LIGHTMAPS = 8,
		MAX_VOXEL_GI_INSTANCESS_PER_INSTANCE = 2,
		INSTANCE_DATA_BUFFER_MIN_SIZE = 4096
	};

	enum RenderListType {
		RENDER_LIST_OPAQUE, //used for opaque objects
		RENDER_LIST_MOTION, //used for opaque objects with motion
		RENDER_LIST_ALPHA, //used for transparent objects
		RENDER_LIST_SECONDARY, //used for shadows and other objects
		RENDER_LIST_MAX
	};

	/* Scene Shader */

	SceneShaderForwardClustered scene_shader;

public:
	/* Framebuffer */

	class RenderBufferDataForwardClustered : public RenderBufferCustomDataRD {
		GDCLASS(RenderBufferDataForwardClustered, RenderBufferCustomDataRD)

	private:
		RenderSceneBuffersRD *render_buffers = nullptr;
		RendererRD::FSR2Context *fsr2_context = nullptr;
		RendererRD::DLSSContext *dlss_context = nullptr;
#ifdef METAL_MFXTEMPORAL_ENABLED
		RendererRD::MFXTemporalContext *mfx_temporal_context = nullptr;
#endif

	public:
		ClusterBuilderRD *cluster_builder = nullptr;

		struct SSEffectsData {
			Projection ssil_last_frame_projections[RendererSceneRender::MAX_RENDER_VIEWS];
			Transform3D ssil_last_frame_transform;

			Projection ssr_last_frame_projections[RendererSceneRender::MAX_RENDER_VIEWS];
			Transform3D ssr_last_frame_transform;

			RendererRD::SSEffects::SSILRenderBuffers ssil;
			RendererRD::SSEffects::SSAORenderBuffers ssao;
			RendererRD::SSEffects::SSRRenderBuffers ssr;
		} ss_effects_data;

		enum DepthFrameBufferType {
			DEPTH_FB,
			DEPTH_FB_ROUGHNESS,
			DEPTH_FB_ROUGHNESS_VOXELGI
		};

		RID render_sdfgi_uniform_set;

		void ensure_specular();
		bool has_specular() const { return render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR); }
		RID get_specular() const { return render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR); }
		RID get_specular(uint32_t p_layer) { return render_buffers->get_texture_slice(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR, p_layer, 0); }
		RID get_specular_msaa(uint32_t p_layer) { return render_buffers->get_texture_slice(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR_MSAA, p_layer, 0); }

		void ensure_normal_roughness_texture();
		bool has_normal_roughness() const { return render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS); }
		RID get_normal_roughness() const { return render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS); }
		RID get_normal_roughness(uint32_t p_layer) { return render_buffers->get_texture_slice(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS, p_layer, 0); }
		RID get_normal_roughness_msaa() const { return render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS_MSAA); }
		RID get_normal_roughness_msaa(uint32_t p_layer) { return render_buffers->get_texture_slice(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS_MSAA, p_layer, 0); }

		void ensure_voxelgi();
		bool has_voxelgi() const { return render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI); }
		RID get_voxelgi() const { return render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI); }
		RID get_voxelgi(uint32_t p_layer) { return render_buffers->get_texture_slice(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI, p_layer, 0); }
		RID get_voxelgi_msaa(uint32_t p_layer) { return render_buffers->get_texture_slice(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI_MSAA, p_layer, 0); }

		void ensure_fsr2(RendererRD::FSR2Effect *effect);
		RendererRD::FSR2Context *get_fsr2_context() const { return fsr2_context; }

		void ensure_dlss(RendererRD::DLSSEffect *effect);
		RendererRD::DLSSContext *get_dlss_context() const { return dlss_context; }

#ifdef METAL_MFXTEMPORAL_ENABLED
		bool ensure_mfx_temporal(RendererRD::MFXTemporalEffect *p_effect);
		RendererRD::MFXTemporalContext *get_mfx_temporal_context() const { return mfx_temporal_context; }
#endif

		RID get_color_only_fb();
		RID get_color_pass_fb(uint32_t p_color_pass_flags);
		RID get_depth_fb(DepthFrameBufferType p_type = DEPTH_FB);
		RID get_specular_only_fb();
		RID get_velocity_only_fb();

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
		virtual void free_data() override;

		static RD::DataFormat get_specular_format();
		static uint32_t get_specular_usage_bits(bool p_resolve, bool p_msaa, bool p_storage);
		static RD::DataFormat get_normal_roughness_format();
		static uint32_t get_normal_roughness_usage_bits(bool p_resolve, bool p_msaa, bool p_storage);
		static RD::DataFormat get_voxelgi_format();
		static uint32_t get_voxelgi_usage_bits(bool p_resolve, bool p_msaa, bool p_storage);
	};

protected:
	virtual void setup_render_buffer_data(Ref<RenderSceneBuffersRD> p_render_buffers) override;

	RID render_base_uniform_set;

	uint64_t lightmap_texture_array_version = 0xFFFFFFFF;

	void _update_render_base_uniform_set();
	RID _setup_sdfgi_render_pass_uniform_set(RID p_albedo_texture, RID p_emission_texture, RID p_emission_aniso_texture, RID p_geom_facing_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index);
	RID _setup_render_pass_uniform_set(RenderListType p_render_list, const RenderDataRD *p_render_data, RID p_radiance_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index, bool p_use_directional_shadow_atlas = false);

	struct RenderListParameters;
	struct GeometryInstanceSurfaceDataCache;

	struct BestFitNormal {
		BestFitNormalShaderRD shader;
		RID shader_version;
		RID pipeline;
		RID texture;
	} best_fit_normal;

	struct IntegrateDFG {
		IntegrateDfgShaderRD shader;
		RID shader_version;
		RID pipeline;
		RID texture;
	} dfg_lut;

	struct LTC {
		RID lut1_texture;
		RID lut2_texture;
	} ltc;

	enum PassMode {
		PASS_MODE_COLOR,
		PASS_MODE_SHADOW,
		PASS_MODE_SHADOW_DP,
		PASS_MODE_DEPTH,
		PASS_MODE_DEPTH_NORMAL_ROUGHNESS,
		PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI,
		PASS_MODE_DEPTH_MATERIAL,
		PASS_MODE_SDF,
		PASS_MODE_SHADOW_CUBE, // The 6 faces of a cube shadow in one pass, into a layered framebuffer.
		PASS_MODE_MAX
	};

	enum ColorPassFlags {
		COLOR_PASS_FLAG_TRANSPARENT = 1 << 0,
		COLOR_PASS_FLAG_SEPARATE_SPECULAR = 1 << 1,
		COLOR_PASS_FLAG_MULTIVIEW = 1 << 2,
		COLOR_PASS_FLAG_MOTION_VECTORS = 1 << 3,
	};

	struct RenderElementInfo;

	struct RenderListParameters {
		GeometryInstanceSurfaceDataCache **elements = nullptr;
		RenderElementInfo *element_info = nullptr;
		int element_count = 0;
		bool reverse_cull = false;
		PassMode pass_mode = PASS_MODE_COLOR;
		uint32_t color_pass_flags = 0;
		bool no_gi = false;
		uint32_t view_count = 1;
		RID render_pass_uniform_set;
		bool force_wireframe = false;
		Vector2 uv_offset;
		float lod_distance_multiplier = 0.0;
		float screen_mesh_lod_threshold = 0.0;
		RD::FramebufferFormatID framebuffer_format = 0;
		uint32_t element_offset = 0;
		bool use_directional_soft_shadow = false;
		SceneShaderForwardClustered::ShaderSpecialization base_specialization = {};

		RenderListParameters() = default;
		RenderListParameters(GeometryInstanceSurfaceDataCache **p_elements, RenderElementInfo *p_element_info, int p_element_count, bool p_reverse_cull, PassMode p_pass_mode, uint32_t p_color_pass_flags, bool p_no_gi, bool p_use_directional_soft_shadows, RID p_render_pass_uniform_set, bool p_force_wireframe = false, const Vector2 &p_uv_offset = Vector2(), float p_lod_distance_multiplier = 0.0, float p_screen_mesh_lod_threshold = 0.0, uint32_t p_view_count = 1, uint32_t p_element_offset = 0, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization = {}) {
			elements = p_elements;
			element_info = p_element_info;
			element_count = p_element_count;
			reverse_cull = p_reverse_cull;
			pass_mode = p_pass_mode;
			color_pass_flags = p_color_pass_flags;
			no_gi = p_no_gi;
			view_count = p_view_count;
			render_pass_uniform_set = p_render_pass_uniform_set;
			force_wireframe = p_force_wireframe;
			uv_offset = p_uv_offset;
			lod_distance_multiplier = p_lod_distance_multiplier;
			screen_mesh_lod_threshold = p_screen_mesh_lod_threshold;
			element_offset = p_element_offset;
			use_directional_soft_shadow = p_use_directional_soft_shadows;
			base_specialization = p_base_specialization;
		}
	};

	// A range of a render list recorded on its own thread into a split of the draw list (RD::draw_list_split_begin()).
	// What the recording would change outside the draw list waits in here for the render thread.
	struct RenderListSplit {
		uint32_t from_element = 0;
		uint32_t to_element = 0;
		RD::DrawListID draw_list = 0;
		bool request_redraw = false;
		LocalVector<RendererRD::MaterialStorage::MaterialData *> used_materials;
		uint64_t begin_usec = 0; // Stats only.
		uint64_t end_usec = 0;
	};

	LocalVector<RenderListSplit> render_list_splits;
	LocalVector<RD::DrawListID> render_list_split_ids;
	RenderListParameters *render_list_split_params = nullptr;

	// Every shadow pass of a _render_shadow_end() (often many: one per cube light, 6 per light without one-pass cubes),
	// cut in parts as a draw list split would be, recorded in one _parallel_run into splits reserved ahead of their
	// draw lists (RD::draw_list_split_detached_begin()); the draw lists then open in order and take their parts.
	// GODOT_PARALLEL_SHADOW_PASSES=0 records them pass after pass, each split on its own if large.
	bool shadow_passes_parallel = true;
	LocalVector<uint32_t> shadow_pass_first_part; // Per shadow pass: its parts [first, first + count); count 0: none.
	LocalVector<uint32_t> shadow_pass_part_count;
	LocalVector<RenderListParameters> shadow_pass_params;
	LocalVector<uint32_t> shadow_part_pass; // Per part: its pass.
	LocalVector<Rect2i> shadow_part_viewports;
	LocalVector<RD::FramebufferFormatID> shadow_part_formats;
	LocalVector<RenderListSplit> shadow_part_splits;
	LocalVector<RD::DrawListID> shadow_part_split_ids;

	void _render_shadow_pass_part(uint32_t p_part);

	// Work cut in parts that helper threads and the render thread take in turn (_parallel_run()). The claim holds the
	// run's generation (high 32 bits) and the next part (low 32 bits); a run is closed (low bits all set) while the
	// next one is published, and a claim made on a stale value fails its compare-exchange.
	// Helpers are WorkerThreadPool tasks that stay a while (parallel_linger_usec) looking for the next run once theirs
	// is done: waking a pool thread costs the render thread ~20 us (the pool notifies under its mutex), and a frame has
	// a dozen runs close together. A run only wakes the helpers it lacks; each run says how many may join it.
	// _render_scene() dismisses them when it returns (parallel_epoch): the graph replay that follows needs the cores.
	typedef void (RenderForwardClustered::*ParallelPart)(uint32_t p_part);
	std::atomic<uint64_t> parallel_claim = { 0xFFFFFFFF };
	SafeNumeric<uint32_t> parallel_part_count;
	SafeNumeric<uint32_t> parallel_parts_done;
	uint32_t parallel_generation = 0;
	ParallelPart parallel_part = nullptr;
	std::atomic<uint32_t> parallel_run_workers = { 0 }; // Helpers allowed in the current run.
	SafeNumeric<uint32_t> parallel_run_joined; // Helpers that joined it.
	SafeNumeric<uint32_t> parallel_helpers; // Helper tasks running or about to.
	SafeFlag parallel_helpers_stop;
	SafeNumeric<uint32_t> parallel_epoch; // Helpers woken in an older epoch leave once idle.
	uint64_t parallel_linger_usec = 0;
	LocalVector<WorkerThreadPool::GroupID> parallel_groups;
	uint32_t render_list_max_splits = 0;
	uint32_t render_list_split_min_elements = 0;

	struct RenderListSplitStats {
		bool enabled = false;
		uint64_t frame = 0;
		uint32_t frames = 0;
		uint32_t split_lists = 0;
		uint32_t splits = 0;
		uint64_t split_elements = 0;
		uint64_t split_usec = 0;
		uint64_t split_wait_usec = 0;
		uint64_t split_join_usec = 0;
		uint64_t split_parts_usec = 0;
		uint64_t split_start_usec = 0;
		uint32_t serial_lists = 0;
		uint64_t serial_elements = 0;
		uint64_t serial_usec = 0;
		uint64_t fill_usec = 0; // _fill_render_list
		uint64_t instance_data_usec = 0; // _fill_instance_data
		uint32_t parallel_builds = 0; // Calls of either that used several threads.
		uint32_t shadow_builds = 0; // _render_shadow_build(): calls, shadow passes, elements.
		uint32_t shadow_passes = 0;
		uint64_t shadow_elements = 0;
		uint64_t shadow_usec = 0; // Sorting the shadow passes and writing their instance data, either path.
		uint64_t shadow_draw_calls = 0; // Deferred builds only.
		uint64_t sort_usec = 0; // Sorting the main view's opaque, motion and alpha lists, either path.
		uint32_t shadow_parallel_passes = 0; // Shadow passes recorded together ahead of their draw lists, and the time it took.
		uint64_t shadow_parallel_usec = 0;
		// _parallel_run(), all uses: parts, those run by the calling thread, sum of the parts' durations, wall time.
		SafeNumeric<uint64_t> run_parts;
		SafeNumeric<uint64_t> run_parts_on_caller;
		SafeNumeric<uint64_t> run_parts_usec;
		uint64_t run_usec = 0;
		uint32_t runs = 0;
		uint32_t helpers_woken = 0;
	} render_list_split_stats;

	// GODOT_LIST_STABILITY_STATS=1 prints every 240 frames how much of each sorted list matches the same list in the
	// previous frame (phase 0 of INCREMENTAL-CACHE-STUDY.md, statistics only): elements at the same position with the
	// same sort keys, shifted or reordered in the list, with other keys, entered and left, and those whose instance moved. A list is
	// the same from one frame to the next by view (main lists) or by light and pass (shadow passes).
	enum ListStabilityCategory {
		LIST_STABILITY_OPAQUE,
		LIST_STABILITY_MOTION,
		LIST_STABILITY_ALPHA,
		LIST_STABILITY_SHADOW_DIRECTIONAL,
		LIST_STABILITY_SHADOW_OMNI,
		LIST_STABILITY_SHADOW_SPOT,
		LIST_STABILITY_MAX,
	};

	struct ListStability {
		struct Element {
			const void *surface = nullptr;
			uint64_t sort_key1 = 0;
			uint64_t sort_key2 = 0;
			uint32_t cube_face_mask = 0;
			uint32_t transform_hash = 0;
		};
		struct History {
			LocalVector<Element> elements;
			uint64_t frame = 0;
		};
		struct Counters {
			uint64_t lists = 0;
			uint64_t identical_lists = 0; // Same elements, keys and order: what a cached list would reuse whole.
			uint64_t elements = 0;
			uint64_t same_position = 0;
			uint64_t shifted = 0; // Same keys and order, another position: something entered, left or moved above it.
			uint64_t reordered = 0; // Same keys, out of order with the elements kept before it.
			uint64_t rekeyed = 0; // In the previous list with other keys.
			uint64_t entered = 0;
			uint64_t left = 0;
			uint64_t moved = 0; // Kept, but the instance has another transform.
		};
		bool enabled = false;
		uint64_t frame = 0;
		uint32_t frames = 0;
		HashMap<uint64_t, History> histories;
		Counters counters[LIST_STABILITY_MAX];
		LocalVector<Element> current; // Filled by the caller of _list_stability_compare().
		HashMap<const void *, uint32_t> previous_index;
	} list_stability;

	// Which light and pass the next _render_shadow_append() draws, set by _render_shadow_pass() for the statistics.
	uint64_t shadow_append_stability_key = 0;
	ListStabilityCategory shadow_append_stability_category = LIST_STABILITY_SHADOW_DIRECTIONAL;

	void _list_stability_compare(uint64_t p_key, ListStabilityCategory p_category);
	void _list_stability_add_main_list(uint64_t p_view_key, RenderListType p_render_list, ListStabilityCategory p_category);

	struct LightmapData {
		float normal_xform[12];
		float texture_size[2];
		float exposure_normalization;
		uint32_t flags;
	};

	struct LightmapCaptureData {
		float sh[9 * 4];
	};

	// When changing any of these enums, remember to change the corresponding enums in the shader files as well.
	enum {
		INSTANCE_DATA_FLAG_MULTIMESH_INDIRECT = 1 << 2,
		INSTANCE_DATA_FLAGS_DYNAMIC = 1 << 3,
		INSTANCE_DATA_FLAGS_NON_UNIFORM_SCALE = 1 << 4,
		INSTANCE_DATA_FLAG_USE_GI_BUFFERS = 1 << 5,
		INSTANCE_DATA_FLAG_USE_SDFGI = 1 << 6,
		INSTANCE_DATA_FLAG_USE_LIGHTMAP_CAPTURE = 1 << 7,
		INSTANCE_DATA_FLAG_USE_LIGHTMAP = 1 << 8,
		INSTANCE_DATA_FLAG_USE_SH_LIGHTMAP = 1 << 9,
		INSTANCE_DATA_FLAG_USE_VOXEL_GI = 1 << 10,
		INSTANCE_DATA_FLAG_PARTICLES = 1 << 11,
		INSTANCE_DATA_FLAG_MULTIMESH = 1 << 12,
		INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D = 1 << 13,
		INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR = 1 << 14,
		INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA = 1 << 15,
		INSTANCE_DATA_FLAGS_PARTICLE_TRAIL_SHIFT = 16,
		INSTANCE_DATA_FLAGS_PARTICLE_TRAIL_MASK = 0xFF,
		INSTANCE_DATA_FLAGS_FADE_SHIFT = 24,
		INSTANCE_DATA_FLAGS_FADE_MASK = 0xFFUL << INSTANCE_DATA_FLAGS_FADE_SHIFT
	};

	struct SceneState {
		// This struct is loaded into Set 1 - Binding 1, populated at start of rendering a frame, must match with shader code
		struct UBO {
			uint32_t cluster_shift;
			uint32_t cluster_width;
			uint32_t cluster_type_size;
			uint32_t max_cluster_element_count_div_32;

			uint32_t ss_effects_flags;
			float ssao_light_affect;
			float ssao_ao_affect;
			uint32_t pad1;

			float sdf_to_bounds[16];

			int32_t sdf_offset[3];
			uint32_t pad2;

			int32_t sdf_size[3];
			uint32_t gi_upscale_for_msaa;

			uint32_t volumetric_fog_enabled;
			float volumetric_fog_inv_length;
			float volumetric_fog_detail_spread;
			uint32_t volumetric_fog_pad;
		};

		struct PushConstantUbershader {
			SceneShaderForwardClustered::ShaderSpecialization specialization;
			SceneShaderForwardClustered::UbershaderConstants constants;
		};

		struct PushConstant {
			uint32_t base_index; //
			uint32_t uv_offset; //packed
			uint32_t multimesh_motion_vectors_current_offset;
			uint32_t multimesh_motion_vectors_previous_offset;
			PushConstantUbershader ubershader;
		};

		struct InstanceData {
			float transform[12];
			float compressed_aabb_position[4];
			float compressed_aabb_size[4];
			float uv_scale[4];
			uint32_t flags;
			uint32_t instance_uniforms_ofs; //base offset in global buffer for instance variables
			uint32_t gi_offset; //GI information when using lightmapping (VCT or lightmap index)
			uint32_t layer_mask;
			float prev_transform[12];
			float lightmap_uv_scale[4];
#ifdef REAL_T_IS_DOUBLE
			float model_precision[4];
			float prev_model_precision[4];
#endif

			// These setters allow us to copy the data over with operation when using floats.
			inline void set_lightmap_uv_scale(const Rect2 &p_rect) {
#ifdef REAL_T_IS_DOUBLE
				lightmap_uv_scale[0] = p_rect.position.x;
				lightmap_uv_scale[1] = p_rect.position.y;
				lightmap_uv_scale[2] = p_rect.size.x;
				lightmap_uv_scale[3] = p_rect.size.y;
#else
				Rect2 *rect = reinterpret_cast<Rect2 *>(lightmap_uv_scale);
				*rect = p_rect;
#endif
			}

			inline void set_compressed_aabb(const AABB &p_aabb) {
#ifdef REAL_T_IS_DOUBLE
				compressed_aabb_position[0] = p_aabb.position.x;
				compressed_aabb_position[1] = p_aabb.position.y;
				compressed_aabb_position[2] = p_aabb.position.z;

				compressed_aabb_size[0] = p_aabb.size.x;
				compressed_aabb_size[1] = p_aabb.size.y;
				compressed_aabb_size[2] = p_aabb.size.z;
#else
				Vector3 *compressed_aabb_position_vec3 = reinterpret_cast<Vector3 *>(compressed_aabb_position);
				Vector3 *compressed_aabb_size_vec3 = reinterpret_cast<Vector3 *>(compressed_aabb_size);
				*compressed_aabb_position_vec3 = p_aabb.position;
				*compressed_aabb_size_vec3 = p_aabb.size;
#endif
			}

			inline void set_uv_scale(const Vector4 &p_uv_scale) {
#ifdef REAL_T_IS_DOUBLE
				uv_scale[0] = p_uv_scale.x;
				uv_scale[1] = p_uv_scale.y;
				uv_scale[2] = p_uv_scale.z;
				uv_scale[3] = p_uv_scale.w;
#else
				Vector4 *uv_scale_vec4 = reinterpret_cast<Vector4 *>(uv_scale);
				*uv_scale_vec4 = p_uv_scale;
#endif
			}
		};

		static_assert(std::is_trivially_destructible_v<InstanceData>);
		static_assert(std::is_trivially_constructible_v<InstanceData>);

		UBO ubo;

		LocalVector<RID> uniform_buffers;
		LocalVector<RID> implementation_uniform_buffers;
		uint32_t used_uniform_buffer_count = 0;

		LightmapData lightmaps[MAX_LIGHTMAPS];
		RID lightmap_ids[MAX_LIGHTMAPS];
		bool lightmap_has_sh[MAX_LIGHTMAPS];
		uint32_t lightmaps_used = 0;
		uint32_t max_lightmaps;
		RID lightmap_buffer;

		MultiUmaBuffer<1u> instance_buffer[RENDER_LIST_MAX] = { MultiUmaBuffer<1u>("RENDER_LIST_OPAQUE"), MultiUmaBuffer<1u>("RENDER_LIST_MOTION"), MultiUmaBuffer<1u>("RENDER_LIST_ALPHA"), MultiUmaBuffer<1u>("RENDER_LIST_SECONDARY") };
		InstanceData *curr_gpu_ptr[RENDER_LIST_MAX] = {};

		LightmapCaptureData *lightmap_captures = nullptr;
		uint32_t max_lightmap_captures;
		RID lightmap_capture_buffer;

		RID voxelgi_ids[MAX_VOXEL_GI_INSTANCESS];
		uint32_t voxelgis_used = 0;

		bool used_screen_texture = false;
		bool used_normal_texture = false;
		bool used_depth_texture = false;
		bool used_sss = false;
		bool used_lightmap = false;
		bool used_opaque_stencil = false;

		struct ShadowPass {
			uint32_t element_from;
			uint32_t element_count;
			PassMode pass_mode;

			RID rp_uniform_set;
			float lod_distance_multiplier;
			float screen_mesh_lod_threshold;

			RID framebuffer;
			Rect2i rect;
			bool clear_depth;
			bool flip_cull;

			uint32_t uniform_buffer_index;

			int *render_info = nullptr; // Shadow render info of the pass, for its draw calls.
			uint32_t draw_calls = 0;
			int32_t cube_copy = -1; // The cube shadow to copy into the atlas once this pass is drawn (cube_shadow_copies).

			uint64_t stability_key = 0; // list_stability.
			ListStabilityCategory stability_category = LIST_STABILITY_SHADOW_DIRECTIONAL;
		};

		LocalVector<ShadowPass> shadow_passes;

		void grow_instance_buffer(RenderListType p_render_list, uint32_t p_req_element_count, bool p_append);
	} scene_state;

	static RenderForwardClustered *singleton;

	uint32_t _setup_environment(const RenderDataRD *p_render_data, bool p_no_fog, const Size2i &p_screen_size, const Size2 &p_viewport_size, const Color &p_default_bg_color, bool p_opaque_render_buffers = false, bool p_apply_alpha_multiplier = false, bool p_pancake_shadows = false);
	void _setup_voxelgis(const PagedArray<RID> &p_voxelgis);
	void _setup_lightmaps(const RenderDataRD *p_render_data, const PagedArray<RID> &p_lightmaps, const Transform3D &p_cam_transform);
	static uint32_t _count_directional_lights(const RenderDataRD *p_render_data);

	struct RenderElementInfo {
		enum { MAX_REPEATS = (1 << 20) - 1 };
		union {
			struct {
				uint32_t lod_index : 8;
				uint32_t uses_softshadow : 1;
				uint32_t uses_projector : 1;
				uint32_t uses_forward_gi : 1;
				uint32_t uses_lightmap : 1;
				uint32_t cube_face_mask : 6; // PASS_MODE_SHADOW_CUBE: the faces that see the element.
			};
			uint32_t value;
		};
		uint32_t repeat;
	};

	static_assert(std::is_trivially_destructible_v<RenderElementInfo>);
	static_assert(std::is_trivially_constructible_v<RenderElementInfo>);

	template <PassMode p_pass_mode, uint32_t p_color_pass_flags = 0>
	_FORCE_INLINE_ void _render_list_template(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element, RenderListSplit *p_split);
	void _render_list(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element, RenderListSplit *p_split = nullptr);
	uint32_t _render_list_get_split_count(const RenderListParameters *p_params);
	void _render_list_split(RenderListParameters *p_params, uint32_t p_split_count);
	void _render_list_split_part(uint32_t p_part);

	uint64_t _parallel_run(uint32_t p_part_count, uint32_t p_thread_count, ParallelPart p_part);
	void _parallel_helper_task(uint32_t p_index, uint32_t p_epoch);
	bool _parallel_run_claimed(bool p_caller);
	void _parallel_release_groups(bool p_wait);
	void _render_list_with_draw_list(RenderListParameters *p_params, RID p_framebuffer, BitField<RD::DrawFlags> p_draw_flags = RD::DRAW_DEFAULT_ALL, const Vector<Color> &p_clear_color_values = Vector<Color>(), float p_clear_depth_value = 0.0, uint32_t p_clear_stencil_value = 0, const Rect2 &p_region = Rect2());

	void _fill_instance_data(RenderListType p_render_list, int *p_render_info = nullptr, uint32_t p_offset = 0, int32_t p_max_elements = -1, bool p_update_buffer = true);
	void _fill_render_list(RenderListType p_render_list, const RenderDataRD *p_render_data, PassMode p_pass_mode, bool p_using_sdfgi = false, bool p_using_opaque_gi = false, bool p_using_motion_pass = false, bool p_append = false, bool p_alpha_only = false);

	class GeometryInstanceForwardClustered;

	// _fill_render_list() over a range of the culled instances. Every instance and surface is written by its own
	// chunk only; what the list, the scene state and the render info get is kept in the chunk and merged in order.
	struct FillRenderListChunk {
		enum {
			USED_SSS = 1 << 0,
			USED_SCREEN_TEXTURE = 1 << 1,
			USED_NORMAL_TEXTURE = 1 << 2,
			USED_DEPTH_TEXTURE = 1 << 3,
			USED_LIGHTMAP = 1 << 4,
			USED_OPAQUE_STENCIL = 1 << 5,
		};

		uint32_t from_instance = 0;
		uint32_t to_instance = 0;
		LocalVector<GeometryInstanceSurfaceDataCache *> elements; // For the list being filled.
		LocalVector<GeometryInstanceSurfaceDataCache *> alpha_elements;
		LocalVector<GeometryInstanceSurfaceDataCache *> motion_elements;
		LocalVector<GeometryInstanceForwardClustered *> lightmap_captures; // Their index in the frame is set by the merge.
		uint32_t used = 0;
		int64_t visible_primitives = 0;
		int64_t shadow_primitives = 0;
		char padding[64]; // Each thread writes its own chunk: keep the next chunk's fields off this one's cache lines.
	};

	struct FillRenderListParameters {
		RenderListType render_list = RENDER_LIST_OPAQUE;
		const RenderDataRD *render_data = nullptr;
		PassMode pass_mode = PASS_MODE_COLOR;
		bool using_sdfgi = false;
		bool using_opaque_gi = false;
		bool using_motion_pass = false;
		bool alpha_only = false;
		Plane near_plane;
		float z_max = 0.0;
		uint32_t max_lightmap_captures = 0; // Per chunk: the merge checks the total.
	} fill_render_list_params;

	LocalVector<FillRenderListChunk> fill_render_list_chunks;
	uint32_t list_build_min_instances = 0; // Smallest chunk of instances worth a thread; 0 builds serially.
	uint32_t list_build_max_threads = 0;

	void _fill_render_list_chunk(FillRenderListChunk &p_chunk);
	void _fill_render_list_part(uint32_t p_part);

	// _fill_instance_data() over ranges of elements; whether each element can be drawn with the one before it goes in
	// fill_instance_data_repeats, the runs are counted afterwards in order.
	struct FillInstanceDataParameters {
		RenderListType render_list = RENDER_LIST_OPAQUE;
		uint32_t offset = 0;
		uint32_t count = 0;
		uint32_t part_count = 0;
	} fill_instance_data_params;
	LocalVector<uint8_t> fill_instance_data_repeats;

	void _fill_instance_data_range(RenderListType p_render_list, uint32_t p_offset, uint32_t p_from, uint32_t p_to);
	void _fill_instance_data_part(uint32_t p_part);
	_FORCE_INLINE_ void _store_instance_data(const GeometryInstanceForwardClustered *p_inst, const GeometryInstanceSurfaceDataCache *p_surface, uint32_t p_flags, uint32_t p_gi_offset, SceneState::InstanceData *r_instance_data);

	HashMap<Size2i, RID> sdfgi_framebuffer_size_cache;

	struct GeometryInstanceData;
	class GeometryInstanceForwardClustered;

	struct GeometryInstanceLightmapSH {
		Color sh[9];
	};

	// Cached data for drawing surfaces
	struct GeometryInstanceSurfaceDataCache {
		enum {
			FLAG_PASS_DEPTH = 1,
			FLAG_PASS_OPAQUE = 2,
			FLAG_PASS_ALPHA = 4,
			FLAG_PASS_SHADOW = 8,
			FLAG_USES_SHARED_SHADOW_MATERIAL = 128,
			FLAG_USES_SUBSURFACE_SCATTERING = 2048,
			FLAG_USES_SCREEN_TEXTURE = 4096,
			FLAG_USES_DEPTH_TEXTURE = 8192,
			FLAG_USES_NORMAL_TEXTURE = 16384,
			FLAG_USES_DOUBLE_SIDED_SHADOWS = 32768,
			FLAG_USES_PARTICLE_TRAILS = 65536,
			FLAG_USES_MOTION_VECTOR = 131072,
			FLAG_USES_STENCIL = 262144,
		};

		union {
			struct {
				uint64_t sort_key1;
				uint64_t sort_key2;
			};
			struct {
				// Needs to be grouped together to be used in RenderElementInfo, as the value is masked directly.
				uint64_t lod_index : 8;
				uint64_t uses_softshadow : 1;
				uint64_t uses_projector : 1;
				uint64_t uses_forward_gi : 1;
				uint64_t uses_lightmap : 1;

				// Sorted based on optimal order for respecting priority and reducing the amount of rebinding of shaders, materials,
				// and geometry. This current order was found to be the most optimal in large projects. If you wish to measure
				// differences, refer to RenderingDeviceGraph and the methods available to print statistics for draw lists.
				uint64_t depth_layer : 4;
				uint64_t surface_index : 8;
				uint64_t geometry_id : 32;
				uint64_t material_id_hi : 8;

				uint64_t material_id_lo : 24;
				uint64_t shader_id : 32;
				uint64_t priority : 8;
			};
		} sort;

		RSE::PrimitiveType primitive = RSE::PRIMITIVE_MAX;
		uint32_t flags = 0;
		uint32_t rt_pass_flags = 0;
		uint32_t surface_index = 0;
		uint32_t color_pass_inclusion_mask = 0;

		void *surface = nullptr;
		RID material_uniform_set;
		SceneShaderForwardClustered::ShaderData *shader = nullptr;
		SceneShaderForwardClustered::MaterialData *material = nullptr;

		void *surface_shadow = nullptr;
		RID material_uniform_set_shadow;
		SceneShaderForwardClustered::ShaderData *shader_shadow = nullptr;

		mutable Transform3D cached_final_transform;
		mutable bool cached_final_transform_valid = false;

		mutable RID rt_deformed_handle;

		GeometryInstanceSurfaceDataCache *next = nullptr;
		GeometryInstanceForwardClustered *owner = nullptr;
		SelfList<GeometryInstanceSurfaceDataCache> compilation_dirty_element;
		SelfList<GeometryInstanceSurfaceDataCache> compilation_all_element;

		GeometryInstanceSurfaceDataCache() :
				compilation_dirty_element(this), compilation_all_element(this) {}
	};

	class GeometryInstanceForwardClustered : public RenderGeometryInstanceBase {
	public:
		/// Heap-allocated procedural RT state. Only created when the instance is procedural.
		RTProceduralState *rt_procedural = nullptr;

		// lightmap
		RID lightmap_instance;
		Rect2 lightmap_uv_scale;
		uint32_t lightmap_slice_index;
		GeometryInstanceLightmapSH *lightmap_sh = nullptr;

		//used during rendering

		uint32_t gi_offset_cache = 0;
		uint8_t cube_face_mask = 0; // Faces of the cube shadow being gathered that see it (_render_shadow_cube_gather()).
		bool store_transform_cache = true;
		RID transforms_uniform_set;
		uint32_t instance_count = 0;
		uint32_t trail_steps = 1;
		bool can_sdfgi = false;
		bool using_projectors = false;
		bool using_softshadows = false;

		//used during setup
		uint64_t prev_transform_change_frame = 0xFFFFFFFF;
		uint64_t last_aged_frame = 0;
		uint64_t created_frame = 0; // Pending frame of the first set_transform(): no motion history before it.
		enum TransformStatus {
			NONE,
			MOVED,
			TELEPORTED,
		} transform_status = TransformStatus::MOVED;
		Transform3D prev_transform;
		RID voxel_gi_instances[MAX_VOXEL_GI_INSTANCESS_PER_INSTANCE];
		GeometryInstanceSurfaceDataCache *surface_caches = nullptr;
		SelfList<GeometryInstanceForwardClustered> dirty_list_element;

		GeometryInstanceForwardClustered() :
				dirty_list_element(this) {}

		virtual void _mark_dirty() override;

		virtual void set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) override;
		virtual void reset_motion_vectors() override;
		virtual bool get_motion_history(MotionHistory &r_history) const override;
		virtual void set_motion_history(const MotionHistory &p_history) override;
		virtual void set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) override;
		virtual void set_lightmap_capture(const Color *p_sh9) override;

		RTProceduralState *_ensure_procedural_state();
		void _free_procedural_state();

		virtual void set_rt_procedural(bool p_procedural, const AABB &p_aabb) override;
		virtual void set_rt_procedural_bounds(const Vector<float> &p_aabb_data, bool p_expose_bounds) override;

		virtual void clear_light_instances() override {}
		virtual void pair_light_instance(const RID p_light_instance, RSE::LightType light_type, uint32_t placement_idx) override {}
		virtual void pair_reflection_probe_instances(const RID *p_reflection_probe_instances, uint32_t p_reflection_probe_instance_count) override {}
		virtual void pair_decal_instances(const RID *p_decal_instances, uint32_t p_decal_instance_count) override {}
		virtual void pair_voxel_gi_instances(const RID *p_voxel_gi_instances, uint32_t p_voxel_gi_instance_count) override;

		virtual void set_softshadow_projector_pairing(bool p_softshadow, bool p_projector) override;

		void age_out_motion(uint64_t p_frame);
	};

	// These are not used in the Forward+ path, it has different light clustering tech.
	virtual uint32_t get_max_lights_total() override { return 0; }
	virtual uint32_t get_max_lights_per_mesh() override { return 0; }

	static void _geometry_instance_dependency_changed(Dependency::DependencyChangedNotification p_notification, DependencyTracker *p_tracker);
	static void _geometry_instance_dependency_deleted(const RID &p_dependency, DependencyTracker *p_tracker);

	SelfList<GeometryInstanceForwardClustered>::List geometry_instance_dirty_list;
	SelfList<GeometryInstanceSurfaceDataCache>::List geometry_surface_compilation_dirty_list;
	SelfList<GeometryInstanceSurfaceDataCache>::List geometry_surface_compilation_all_list;

	PagedAllocator<GeometryInstanceForwardClustered> geometry_instance_alloc;
	PagedAllocator<GeometryInstanceSurfaceDataCache> geometry_instance_surface_alloc;
	PagedAllocator<GeometryInstanceLightmapSH> geometry_instance_lightmap_sh;

	struct SurfacePipelineData {
		void *mesh_surface = nullptr;
		void *mesh_surface_shadow = nullptr;
		SceneShaderForwardClustered::ShaderData *shader = nullptr;
		SceneShaderForwardClustered::ShaderData *shader_shadow = nullptr;
		bool instanced = false;
		bool uses_opaque = false;
		bool uses_transparent = false;
		bool uses_depth = false;
		bool can_use_lightmap = false;
	};

	struct GlobalPipelineData {
		union {
			uint32_t key;

			struct {
				uint32_t texture_samples : 3;
				uint32_t use_reflection_probes : 1;
				uint32_t use_separate_specular : 1;
				uint32_t use_motion_vectors : 1;
				uint32_t use_normal_and_roughness : 1;
				uint32_t use_lightmaps : 1;
				uint32_t use_voxelgi : 1;
				uint32_t use_sdfgi : 1;
				uint32_t use_multiview : 1;
				uint32_t use_16_bit_shadows : 1;
				uint32_t use_32_bit_shadows : 1;
				uint32_t use_shadow_cubemaps : 1;
				uint32_t use_shadow_dual_paraboloid : 1;
			};
		};
	};

	GlobalPipelineData global_pipeline_data_compiled = {};
	GlobalPipelineData global_pipeline_data_required = {};

	typedef Pair<SceneShaderForwardClustered::ShaderData *, SceneShaderForwardClustered::ShaderData::PipelineKey> ShaderPipelinePair;

	void _update_global_pipeline_data_requirements_from_project();
	void _update_global_pipeline_data_requirements_from_light_storage();
	void _geometry_instance_add_surface_with_material(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, SceneShaderForwardClustered::MaterialData *p_material, uint32_t p_material_id, uint32_t p_shader_id, RID p_mesh);
	void _geometry_instance_add_surface_with_material_chain(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, SceneShaderForwardClustered::MaterialData *p_material, RID p_mat_src, RID p_mesh);
	void _geometry_instance_add_surface(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, RID p_material, RID p_mesh);
	void _geometry_instance_update(RenderGeometryInstance *p_geometry_instance);
	void _mesh_compile_pipeline_for_surface(SceneShaderForwardClustered::ShaderData *p_shader, void *p_mesh_surface, bool p_ubershader, bool p_instanced_surface, RSE::PipelineSource p_source, SceneShaderForwardClustered::ShaderData::PipelineKey &r_pipeline_key, Vector<ShaderPipelinePair> *r_pipeline_pairs = nullptr);
	void _mesh_compile_pipelines_for_surface(const SurfacePipelineData &p_surface, const GlobalPipelineData &p_global, RSE::PipelineSource p_source, Vector<ShaderPipelinePair> *r_pipeline_pairs = nullptr);
	void _mesh_generate_all_pipelines_for_surface_cache(GeometryInstanceSurfaceDataCache *p_surface_cache, const GlobalPipelineData &p_global);
	void _update_dirty_geometry_instances();
	void _update_dirty_geometry_pipelines();

	// Global data about the scene that can be used to pre-allocate resources without relying on culling.
	struct GlobalSurfaceData {
		bool screen_texture_used = false;
		bool normal_texture_used = false;
		bool depth_texture_used = false;
		bool sss_used = false;
	} global_surface_data;

	/* Render List */

	// Surfaces with equal sort keys (the same mesh and material: identical units), or equal priority and depth in the
	// alpha list, are ordered by address, so a list that holds the same surfaces sorts the same way every frame.
	// SortArray is not stable: it reordered 17 % of Solfarer's opaque list every frame with nothing moving (list
	// stability counter). Equal keys stay contiguous, so instancing is unchanged. GODOT_STABLE_SORT=0 leaves ties
	// unordered as before.
	static inline bool sort_ties_by_surface = true;

	struct RenderList {
		LocalVector<GeometryInstanceSurfaceDataCache *> elements;
		LocalVector<RenderElementInfo> element_info;

		void clear() {
			elements.clear();
			element_info.clear();
		}

		//should eventually be replaced by radix

		struct SortByKey {
			_FORCE_INLINE_ bool operator()(const GeometryInstanceSurfaceDataCache *A, const GeometryInstanceSurfaceDataCache *B) const {
				if (A->sort.sort_key2 == B->sort.sort_key2 && A->sort.sort_key1 == B->sort.sort_key1) {
					return sort_ties_by_surface && A < B;
				}
				return (A->sort.sort_key2 == B->sort.sort_key2) ? (A->sort.sort_key1 < B->sort.sort_key1) : (A->sort.sort_key2 < B->sort.sort_key2);
			}
		};

		void sort_by_key() {
			SortArray<GeometryInstanceSurfaceDataCache *, SortByKey> sorter;
			sorter.sort(elements.ptr(), elements.size());
		}

		void sort_by_key_range(uint32_t p_from, uint32_t p_size) {
			SortArray<GeometryInstanceSurfaceDataCache *, SortByKey> sorter;
			sorter.sort(elements.ptr() + p_from, p_size);
		}

		struct SortByDepth {
			_FORCE_INLINE_ bool operator()(const GeometryInstanceSurfaceDataCache *A, const GeometryInstanceSurfaceDataCache *B) const {
				return (A->owner->depth < B->owner->depth);
			}
		};

		void sort_by_depth() { //used for shadows

			SortArray<GeometryInstanceSurfaceDataCache *, SortByDepth> sorter;
			sorter.sort(elements.ptr(), elements.size());
		}

		struct SortByReverseDepthAndPriority {
			_FORCE_INLINE_ bool operator()(const GeometryInstanceSurfaceDataCache *A, const GeometryInstanceSurfaceDataCache *B) const {
				if (A->sort.priority == B->sort.priority && A->owner->depth == B->owner->depth) {
					return sort_ties_by_surface && A < B;
				}
				return (A->sort.priority == B->sort.priority) ? (A->owner->depth > B->owner->depth) : (A->sort.priority < B->sort.priority);
			}
		};

		void sort_by_reverse_depth_and_priority() { //used for alpha

			SortArray<GeometryInstanceSurfaceDataCache *, SortByReverseDepthAndPriority> sorter;
			sorter.sort(elements.ptr(), elements.size());
		}

		_FORCE_INLINE_ void add_element(GeometryInstanceSurfaceDataCache *p_element) {
			elements.push_back(p_element);
		}
	};

	RenderList render_list[RENDER_LIST_MAX];

	// Shadow passes are filled one after the other: filling writes per-pass values on instances and surfaces that
	// several passes share. Sorting them and writing their instance data only reads a few of those values, copied per
	// element as each pass is filled; that part runs later for all the passes at once (_render_shadow_build()):
	// sorts on several threads (a large pass in sorted runs merged afterwards), then the instance data in ranges.
	struct ShadowElement {
		uint64_t sort_key1;
		uint64_t sort_key2;
		GeometryInstanceSurfaceDataCache *surface;
		uint32_t flags;
		uint32_t gi_offset;
		uint32_t cube_face_mask; // PASS_MODE_SHADOW_CUBE.
	};

	struct ShadowElementByKey {
		_FORCE_INLINE_ bool operator()(const ShadowElement &A, const ShadowElement &B) const {
			// Equal keys then by cube face mask (only elements seen by the same faces draw instanced together), then by surface.
			if (A.sort_key2 == B.sort_key2 && A.sort_key1 == B.sort_key1) {
				if (A.cube_face_mask != B.cube_face_mask) {
					return A.cube_face_mask < B.cube_face_mask;
				}
				return sort_ties_by_surface && A.surface < B.surface;
			}
			return (A.sort_key2 == B.sort_key2) ? (A.sort_key1 < B.sort_key1) : (A.sort_key2 < B.sort_key2);
		}
	};

	struct ShadowSortRun {
		uint32_t from = 0;
		uint32_t count = 0;
	};

	struct ShadowSortRunLarger {
		_FORCE_INLINE_ bool operator()(const ShadowSortRun &A, const ShadowSortRun &B) const {
			return A.count != B.count ? A.count > B.count : A.from < B.from;
		}
	};

	LocalVector<ShadowElement> shadow_elements; // Parallel to render_list[RENDER_LIST_SECONDARY].elements.
	LocalVector<ShadowElement> shadow_merge_buffer;
	LocalVector<ShadowSortRun> shadow_sort_runs; // Largest first, so the long sorts start first.
	LocalVector<uint8_t> shadow_repeats; // Per element: drawn with the previous one.
	LocalVector<uint8_t> shadow_pass_begins; // Per element: first of its pass.
	uint32_t shadow_instance_part_count = 0;
	bool shadow_build_parallel = true;
	bool shadow_build_merge = true; // Off: a pass cut in several sort runs is drawn run after run, unmerged.

	// The main view's lists sorted together (_sort_render_lists()): a large opaque list in runs merged afterwards, the
	// motion and alpha lists whole, each a _parallel_run part. GODOT_PARALLEL_SORT=0 sorts them one after the other.
	struct OpaqueSortRun {
		uint32_t from = 0;
		uint32_t count = 0;
	};
	bool list_sort_parallel = true;
	LocalVector<OpaqueSortRun> opaque_sort_runs;
	LocalVector<GeometryInstanceSurfaceDataCache *> opaque_merge_buffer;

	void _sort_render_lists();
	void _sort_render_lists_part(uint32_t p_part);
	bool shadow_build_deferred = false;

	void _render_shadow_build();
	void _render_shadow_sort_part(uint32_t p_part);
	void _render_shadow_instance_part(uint32_t p_part);
	void _render_shadow_instance_range(uint32_t p_from, uint32_t p_to);

	virtual void _update_shader_quality_settings() override;

	/* Effects */

	RendererRD::TAA *taa = nullptr;
	RendererRD::FSR2Effect *fsr2_effect = nullptr;
	RendererRD::DLSSEffect *dlss_effect = nullptr;
	RendererRD::FSRFrameGenerationEffect *fsr_frame_generation_effect = nullptr;
	RendererRD::SSEffects *ss_effects = nullptr;

#ifdef METAL_MFXTEMPORAL_ENABLED
	RendererRD::MFXTemporalEffect *mfx_temporal_effect = nullptr;
#endif
	RendererRD::MotionVectorsStore *motion_vectors_store = nullptr;

	/* Cluster builder */

	ClusterBuilderSharedDataRD cluster_builder_shared;
	ClusterBuilderRD *current_cluster_builder = nullptr;

	/* SDFGI */
	void _update_sdfgi(RenderDataRD *p_render_data);

	/* Volumetric fog */
	RID shadow_sampler;

	void _update_volumetric_fog(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const Projection &p_cam_projection, const Transform3D &p_cam_transform, const Transform3D &p_prev_cam_inv_transform, RID p_shadow_atlas, int p_directional_light_count, bool p_use_directional_shadows, int p_positional_light_count, int p_voxel_gi_count, const PagedArray<RID> &p_fog_volumes);

	/* Render shadows */

	void _render_shadow_pass(RID p_light, RID p_shadow_atlas, int p_pass, const PagedArray<RenderGeometryInstance *> &p_instances, float p_lod_distance_multiplier = 0, float p_screen_mesh_lod_threshold = 0.0, bool p_open_pass = true, bool p_close_pass = true, bool p_clear_region = true, RenderingServerTypes::RenderInfo *p_render_info = nullptr, const Size2i &p_viewport_size = Size2i(1, 1), const Transform3D &p_main_cam_transform = Transform3D());
	void _render_shadow_begin();
	void _render_shadow_append(RID p_framebuffer, const PagedArray<RenderGeometryInstance *> &p_instances, const Projection &p_projection, const Transform3D &p_transform, float p_zfar, float p_bias, float p_normal_bias, bool p_reverse_cull_face, bool p_use_dp, bool p_use_dp_flip, bool p_use_pancake, float p_lod_distance_multiplier = 0.0, float p_screen_mesh_lod_threshold = 0.0, const Rect2i &p_rect = Rect2i(), bool p_flip_y = false, bool p_clear_region = true, bool p_begin = true, bool p_end = true, RenderingServerTypes::RenderInfo *p_render_info = nullptr, const Size2i &p_viewport_size = Size2i(1, 1), const Transform3D &p_main_cam_transform = Transform3D(), bool p_cube_layered = false);

	// Cube omni shadows with their 6 faces in one pass (SHADER_GROUP_CUBE_LAYERED): the culler's 6 caster lists of a
	// light merged into cube_shadow_instances, each instance once with the faces that see it (cube_face_mask), drawn as
	// _render_shadow_pass(CUBE_SHADOW_ALL_FACES). GODOT_SHADOW_CUBE_ONE_PASS=0 renders the faces one by one.
	enum {
		CUBE_SHADOW_ALL_FACES = 6,
	};
	bool shadow_cube_one_pass = false;
	// The gathered cube lights share one shadow session (one _render_shadow_end(), one parallel recording); each
	// light's cubemap is copied into the atlas right after its draw list, before the next light reuses the cubemap.
	bool cube_shadow_batching = false;
	struct CubeShadowCopy {
		RID light;
		RID cubemap;
		RID atlas_fb;
		Rect2i atlas_rect;
		uint32_t atlas_size = 1;
		Vector2i dual_paraboloid_offset;
		float z_near = 0.0;
		float z_far = 0.0;
	};
	LocalVector<CubeShadowCopy> cube_shadow_copies;
	void _render_shadow_cube_copy(const CubeShadowCopy &p_copy);
	PagedArrayPool<RenderGeometryInstance *> cube_shadow_instance_pool;
	PagedArray<RenderGeometryInstance *> cube_shadow_instances;
	LocalVector<bool> cube_shadow_rendered; // Per entry of RenderDataRD::cube_shadows, this frame.
	bool _render_shadow_cube_gather(const RenderDataRD *p_render_data, uint32_t p_first);
	void _render_shadow_cube_release();
	void _render_shadow_process();
	void _render_shadow_end();

	/* Render Scene */
	void _process_ssao(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_buffers, const Projection *p_projections);
	void _process_ssil(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_buffers, const Projection *p_projections, const Transform3D &p_transform);
	void _process_ssr(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_slices, const Projection *p_projections, const Vector3 *p_eye_offsets, const Transform3D &p_transform);
	void _copy_framebuffer_to_ss_effects(Ref<RenderSceneBuffersRD> p_render_buffers, bool p_use_ssil, bool p_use_ssr);
	void _setup_lights_cluster_decals(RenderDataRD *p_render_data, uint32_t &r_directional_light_count, uint32_t &r_positional_light_count);
	void _pre_opaque_render(RenderDataRD *p_render_data, bool p_use_ssao, bool p_use_ssil, bool p_use_ssr, bool p_use_gi, const RID *p_normal_roughness_slices, RID p_voxel_gi_buffer);
	void _process_sss(Ref<RenderSceneBuffersRD> p_render_buffers, const Projection &p_camera);

	/* Debug */
	void _debug_draw_cluster(Ref<RenderSceneBuffersRD> p_render_buffers);

protected:
	/* setup */

	virtual RID _render_buffers_get_normal_texture(Ref<RenderSceneBuffersRD> p_render_buffers) override;
	virtual RID _render_buffers_get_velocity_texture(Ref<RenderSceneBuffersRD> p_render_buffers) override;

	virtual void environment_set_ssao_quality(RSE::EnvironmentSSAOQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) override;
	virtual void environment_set_ssil_quality(RSE::EnvironmentSSILQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) override;
	virtual void environment_set_ssr_half_size(bool p_half_size) override;
	virtual void environment_set_ssr_roughness_quality(RSE::EnvironmentSSRRoughnessQuality p_quality) override;

	virtual void sub_surface_scattering_set_quality(RSE::SubSurfaceScatteringQuality p_quality) override;
	virtual void sub_surface_scattering_set_scale(float p_scale, float p_depth_scale) override;

	/* Rendering */

	virtual void _render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) override;
	virtual void _render_buffers_debug_draw(const RenderDataRD *p_render_data) override;

	// 3D scaling/upscaling shared between the raster and raytraced render paths.
	enum Scale3DMode {
		SCALE_3D_NONE,
		SCALE_3D_FSR2,
		SCALE_3D_MFX,
		SCALE_3D_DLSS,
	};

	// Optional DLSS Ray Reconstruction guide buffers. Supplied by the caller so
	// the shared upscaler does not depend on the raytracing subsystem (the
	// raytraced path fills these in; the raster path leaves them inactive).
	struct DLSSRRGuideBuffers {
		bool active = false;
		RID diffuse_albedo;
		RID specular_albedo;
		RID normal_roughness;
		RID specular_hit_dist;
	};

	Scale3DMode _resolve_scale_3d_mode(Ref<RenderSceneBuffersRD> p_render_buffers) const;
	void _render_3d_upscaling(const RenderDataRD *p_render_data, Scale3DMode p_scale_type, bool p_using_taa, double p_time_step, const DLSSRRGuideBuffers &p_dlss_rr);
	// PROTOTYPE (GODOT_FSR_FG): whether this render feeds FSR frame generation (its motion vectors
	// are then required), and its inputs once depth and motion vectors are final, after the
	// temporal pass. With DLSS upscaling the DLSS pass hands them over instead.
	bool _feeds_fsr_frame_generation(const RenderDataRD *p_render_data) const;
	void _prepare_fsr_frame_generation(const RenderDataRD *p_render_data, double p_time_step);
	virtual void _free_rt_viewport_state(RenderSceneBuffersRD *p_render_buffers);

	virtual void _render_material(const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region, float p_exposure_normalization) override;
	virtual void _render_uv2(const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region) override;
	virtual void _render_sdfgi(Ref<RenderSceneBuffersRD> p_render_buffers, const Vector3i &p_from, const Vector3i &p_size, const AABB &p_bounds, const PagedArray<RenderGeometryInstance *> &p_instances, const RID &p_albedo_texture, const RID &p_emission_texture, const RID &p_emission_aniso_texture, const RID &p_geom_facing_texture, float p_exposure_normalization) override;
	virtual void _render_particle_collider_heightfield(RID p_fb, const Transform3D &p_cam_transform, const Projection &p_cam_projection, const PagedArray<RenderGeometryInstance *> &p_instances) override;

public:
	static RenderForwardClustered *get_singleton() { return singleton; }

	ClusterBuilderSharedDataRD *get_cluster_builder_shared() { return &cluster_builder_shared; }
	RendererRD::SSEffects *get_ss_effects() { return ss_effects; }

	/* callback from updating our lighting UBOs, used to populate cluster builder */
	virtual void setup_added_reflection_probe(const Transform3D &p_transform, const Vector3 &p_half_size) override;
	virtual void setup_added_light(const RSE::LightType p_type, const Transform3D &p_transform, float p_radius, float p_spot_aperture, const Vector2 &p_area_size) override;
	virtual void setup_added_decal(const Transform3D &p_transform, const Vector3 &p_half_size) override;

	virtual void base_uniforms_changed() override;

	/* SDFGI UPDATE */

	virtual void sdfgi_update(const Ref<RenderSceneBuffers> &p_render_buffers, RID p_environment, const Vector3 &p_world_position) override;
	virtual int sdfgi_get_pending_region_count(const Ref<RenderSceneBuffers> &p_render_buffers) const override;
	virtual AABB sdfgi_get_pending_region_bounds(const Ref<RenderSceneBuffers> &p_render_buffers, int p_region) const override;
	virtual uint32_t sdfgi_get_pending_region_cascade(const Ref<RenderSceneBuffers> &p_render_buffers, int p_region) const override;
	RID sdfgi_get_ubo() const { return gi.sdfgi_ubo; }

	/* GEOMETRY INSTANCE */

	virtual RenderGeometryInstance *geometry_instance_create(RID p_base) override;
	virtual void geometry_instance_free(RenderGeometryInstance *p_geometry_instance) override;

	virtual uint32_t geometry_instance_get_pair_mask() override;

	/* PIPELINES */

	virtual void mesh_generate_pipelines(RID p_mesh, bool p_background_compilation) override;
	virtual uint32_t get_pipeline_compilations(RSE::PipelineSource p_source) override;

	/* SHADER LIBRARY */

	virtual void enable_features(BitField<FeatureBits> p_feature_bits) override;
	virtual String get_name() const override;

	virtual bool free(RID p_rid) override;

	virtual void update() override;

	RenderForwardClustered();
	~RenderForwardClustered();
};
} // namespace RendererSceneRenderImplementation
