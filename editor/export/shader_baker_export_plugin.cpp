/**************************************************************************/
/*  shader_baker_export_plugin.cpp                                        */
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

#include "shader_baker_export_plugin.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/string/string_builder.h"
#include "core/version.h"
#include "editor/editor_node.h"
#include "scene/3d/label_3d.h"
#include "scene/3d/sprite_3d.h"
#include "servers/rendering/renderer_rd/effects/nrd_denoiser.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered_pt.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/rendering_shader_container.h"

// Ensure that AlphaCut is the same between the two classes so we can share the code to detect transparency.
static_assert(ENUM_MEMBERS_EQUAL(SpriteBase3D::ALPHA_CUT_DISABLED, Label3D::ALPHA_CUT_DISABLED));
static_assert(ENUM_MEMBERS_EQUAL(SpriteBase3D::ALPHA_CUT_DISCARD, Label3D::ALPHA_CUT_DISCARD));
static_assert(ENUM_MEMBERS_EQUAL(SpriteBase3D::ALPHA_CUT_OPAQUE_PREPASS, Label3D::ALPHA_CUT_OPAQUE_PREPASS));
static_assert(ENUM_MEMBERS_EQUAL(SpriteBase3D::ALPHA_CUT_HASH, Label3D::ALPHA_CUT_HASH));
static_assert(ENUM_MEMBERS_EQUAL(SpriteBase3D::ALPHA_CUT_MAX, Label3D::ALPHA_CUT_MAX));

String ShaderBakerExportPlugin::get_name() const {
	return "ShaderBaker";
}

bool ShaderBakerExportPlugin::_is_active(const Vector<String> &p_features) const {
	// Shader baker should only work when a RendererRD driver is active, as the embedded shaders won't be found otherwise.
	return RendererSceneRenderRD::get_singleton() != nullptr && RendererRD::MaterialStorage::get_singleton() != nullptr && p_features.has("shader_baker");
}

bool ShaderBakerExportPlugin::_initialize_container_format(const Ref<EditorExportPlatform> &p_platform, const Ref<EditorExportPreset> &p_preset) {
	shader_container_driver = p_preset->get_project_setting("rendering/rendering_device/driver");
	ERR_FAIL_COND_V_MSG(shader_container_driver.is_empty(), false, "Invalid `rendering/rendering_device/driver` setting, disabling shader baking.");

	for (Ref<ShaderBakerExportPluginPlatform> platform : platforms) {
		if (platform->matches_driver(shader_container_driver)) {
			shader_container_format = platform->create_shader_container_format(p_platform, p_preset);
			ERR_FAIL_NULL_V_MSG(shader_container_format, false, "Unable to create shader container format for the export platform.");
			return true;
		}
	}

	return false;
}

void ShaderBakerExportPlugin::_cleanup_container_format() {
	if (shader_container_format != nullptr) {
		memdelete(shader_container_format);
		shader_container_format = nullptr;
	}
}

bool ShaderBakerExportPlugin::_initialize_cache_directory() {
	shader_cache_export_path = get_export_base_path().path_join("shader_baker").path_join(shader_cache_platform_name).path_join(shader_container_driver);

	if (!DirAccess::dir_exists_absolute(shader_cache_export_path)) {
		Error err = DirAccess::make_dir_recursive_absolute(shader_cache_export_path);
		ERR_FAIL_COND_V_MSG(err != OK, false, "Can't create shader cache folder for exporting.");
	}

	return true;
}

bool ShaderBakerExportPlugin::_begin_customize_resources(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features) {
	if (!_is_active(p_features)) {
		return false;
	}

	if (!_initialize_container_format(p_platform, get_export_preset())) {
		return false;
	}

	if (Engine::get_singleton()->is_generate_spirv_debug_info_enabled()) {
		WARN_PRINT("Shader baker can't generate a compatible shader when run with --debug-shaders (or --generate-spirv-debug-info). Restart the editor without this argument if you want to bake shaders.");
		return false;
	}

	shader_cache_platform_name = p_platform->get_os_name();
	shader_cache_renderer_name = RendererSceneRenderRD::get_singleton()->get_name();
	tasks_processed = 0;
	tasks_total = 0;
	tasks_cancelled = false;

	StringBuilder to_hash;
	to_hash.append("[GodotVersionNumber]");
	to_hash.append(GODOT_VERSION_NUMBER);
	to_hash.append("[GodotVersionHash]");
	to_hash.append(GODOT_VERSION_HASH);
	to_hash.append("[Renderer]");
	to_hash.append(shader_cache_renderer_name);
	// The binaries are the driver's: resources customized for another one would not be visited again.
	to_hash.append("[Driver]");
	to_hash.append(shader_container_driver);
	// The path tracer's variants follow these two (SceneShaderRaytracing::get_bake_programs()).
	raytracing_simple_shadows = get_export_preset()->get_project_setting("rendering/pathtracing/use_simple_shadows");
	raytracing_ser = get_export_preset()->get_project_setting("rendering/pathtracing/use_shader_execution_reordering");
	to_hash.append("[PathTracing]");
	to_hash.append(itos(bool(get_option("shader_baker/path_tracing"))) + itos(raytracing_simple_shadows) + itos(raytracing_ser));
	customization_configuration_hash = to_hash.as_string().hash64();

	BitField<RenderingShaderLibrary::FeatureBits> renderer_features = {};
#ifndef XR_DISABLED
	bool xr_enabled = GLOBAL_GET("xr/shaders/enabled");
	renderer_features.set_flag(RenderingShaderLibrary::FEATURE_ADVANCED_BIT);
	if (xr_enabled) {
		renderer_features.set_flag(RenderingShaderLibrary::FEATURE_MULTIVIEW_BIT);
	}
#endif // XR_DISABLED

	int vrs_mode = GLOBAL_GET("rendering/vrs/mode");
	if (vrs_mode != 0) {
		renderer_features.set_flag(RenderingShaderLibrary::FEATURE_VRS_BIT);
	}

	// Both FP16 and FP32 variants should be included.
	renderer_features.set_flag(RenderingShaderLibrary::FEATURE_FP16_BIT);
	renderer_features.set_flag(RenderingShaderLibrary::FEATURE_FP32_BIT);

	RendererSceneRenderRD::get_singleton()->enable_features(renderer_features);

	// The path tracer's shaders are set up at its first traced frame, which an exporting editor may never have had:
	// set up now, its ShaderRD shaders and the transparent pass' ray traced shadow variants are baked below with the
	// others, and its own programs (_customize_raytracing_programs()) from their templates. Forward+ always runs
	// RenderForwardClusteredPT.
	raytracing_baked = false;
	if (bool(get_option("shader_baker/path_tracing"))) {
		RendererSceneRenderImplementation::RenderForwardClustered *forward_clustered = RendererSceneRenderImplementation::RenderForwardClustered::get_singleton();
		raytracing_baked = forward_clustered != nullptr && static_cast<RendererSceneRenderImplementation::RenderForwardClusteredPT *>(forward_clustered)->setup_raytracing_for_shader_baker();
	}

	// Included all shaders created by renderers and effects.
	ShaderRD::shaders_embedded_set_lock();
	const ShaderRD::ShaderVersionPairSet &pair_set = ShaderRD::shaders_embedded_set_get();
	for (Pair<ShaderRD *, RID> pair : pair_set) {
		_customize_shader_version(pair.first, pair.second);
	}

	ShaderRD::shaders_embedded_set_unlock();

	// Include all shaders created by embedded materials.
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	material_storage->shader_embedded_set_lock();
	const HashSet<RID> &rid_set = material_storage->shader_embedded_set_get();
	for (RID rid : rid_set) {
		RendererRD::MaterialStorage::ShaderData *shader_data = material_storage->shader_get_data(rid);
		if (shader_data != nullptr) {
			Pair<ShaderRD *, RID> shader_version_pair = shader_data->get_native_shader_and_version();
			if (shader_version_pair.first != nullptr) {
				_customize_shader_version(shader_version_pair.first, shader_version_pair.second);
			}
		}
	}

	material_storage->shader_embedded_set_unlock();

	// The path tracer's programs that belong to no material, and its NRD denoiser's.
	_customize_raytracing_programs(RID());
	_customize_nrd_programs();

	return true;
}

bool ShaderBakerExportPlugin::_begin_customize_scenes(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features) {
	if (!_is_active(p_features)) {
		return false;
	}

	if (shader_container_format == nullptr) {
		// Resource customization failed to initialize.
		return false;
	}

	return true;
}

void ShaderBakerExportPlugin::_end_customize_resources() {
	if (!_initialize_cache_directory()) {
		return;
	}

	// Run a progress bar that waits for all shader baking tasks to finish.
	bool progress_active = true;
	EditorProgress editor_progress("baking_shaders", TTR("Baking shaders"), tasks_total);
	editor_progress.step("Baking...", 0);
	while (progress_active) {
		uint32_t tasks_for_progress = 0;
		{
			MutexLock lock(tasks_mutex);
			if (tasks_processed >= tasks_total) {
				progress_active = false;
			} else {
				tasks_condition.wait(lock);
				tasks_for_progress = tasks_processed;
			}
		}

		if (progress_active && editor_progress.step("Baking...", tasks_for_progress)) {
			// User skipped the shader baker, we just don't pack the shaders in the project.
			tasks_cancelled = true;
			progress_active = false;
		}
	}

	String shader_cache_user_dir = ShaderRD::get_shader_cache_user_dir();
	for (const ShaderGroupItem &group_item : shader_group_items) {
		// Wait for all shader compilation tasks of the group to be finished.
		for (WorkerThreadPool::TaskID task_id : group_item.variant_tasks) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
		}

		if (!tasks_cancelled) {
			WorkResult work_result;
			{
				MutexLock lock(shader_work_results_mutex);
				work_result = shader_work_results[group_item.cache_path];
			}

			PackedByteArray cache_file_bytes = ShaderRD::save_shader_cache_bytes(group_item.variants, work_result.variant_data);
			add_file(shader_cache_user_dir.path_join(group_item.cache_path), cache_file_bytes, false);

			String cache_file_path = shader_cache_export_path.path_join(group_item.cache_path);
			if (!DirAccess::exists(cache_file_path)) {
				DirAccess::make_dir_recursive_absolute(cache_file_path.get_base_dir());
			}

			Ref<FileAccess> cache_file_access = FileAccess::open(cache_file_path, FileAccess::WRITE);
			if (cache_file_access.is_valid()) {
				cache_file_access->store_buffer(cache_file_bytes);
			}
		}
	}

	for (const RaytracingItem &item : raytracing_items) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(item.task);
		if (tasks_cancelled) {
			continue;
		}

		PackedByteArray binary;
		{
			MutexLock lock(shader_work_results_mutex);
			binary = raytracing_work_results[item.cache_path];
		}
		if (binary.is_empty()) {
			// Failed to compile: left to the run time, which reports it.
			shader_paths_processed.erase(item.cache_path);
			continue;
		}

		const PackedByteArray cache_file_bytes = item.nrd ? RendererRD::NRDDenoiser::make_cache_file(binary) : RendererSceneRenderImplementation::SceneShaderRaytracing::make_binary_cache_file(binary);
		add_file(shader_cache_user_dir.path_join(item.cache_path), cache_file_bytes, false);

		String cache_file_path = shader_cache_export_path.path_join(item.cache_path);
		if (!DirAccess::exists(cache_file_path.get_base_dir())) {
			DirAccess::make_dir_recursive_absolute(cache_file_path.get_base_dir());
		}

		Ref<FileAccess> cache_file_access = FileAccess::open(cache_file_path, FileAccess::WRITE);
		if (cache_file_access.is_valid()) {
			cache_file_access->store_buffer(cache_file_bytes);
		}
	}

	if (!tasks_cancelled) {
		String file_cache_path = shader_cache_export_path.path_join("file_cache");
		Ref<FileAccess> cache_list_access = FileAccess::open(file_cache_path, FileAccess::READ_WRITE);
		if (cache_list_access.is_null()) {
			cache_list_access = FileAccess::open(file_cache_path, FileAccess::WRITE);
		}

		if (cache_list_access.is_valid()) {
			String cache_list_line;
			while (cache_list_line = cache_list_access->get_line(), !cache_list_line.is_empty()) {
				// Only add if it wasn't already added.
				if (!shader_paths_processed.has(cache_list_line)) {
					PackedByteArray cache_file_bytes = FileAccess::get_file_as_bytes(shader_cache_export_path.path_join(cache_list_line));
					if (!cache_file_bytes.is_empty()) {
						add_file(shader_cache_user_dir.path_join(cache_list_line), cache_file_bytes, false);
					}
				}

				shader_paths_processed.erase(cache_list_line);
			}

			for (const String &shader_path : shader_paths_processed) {
				cache_list_access->store_line(shader_path);
			}

			cache_list_access->close();
		}
	}

	shader_paths_processed.clear();
	shaders_customized.clear();
	shader_work_results.clear();
	shader_group_items.clear();
	raytracing_items.clear();
	raytracing_work_results.clear();

	_cleanup_container_format();
}

Ref<Resource> ShaderBakerExportPlugin::_customize_resource(const Ref<Resource> &p_resource, const String &p_path) {
	RendererRD::MaterialStorage *singleton = RendererRD::MaterialStorage::get_singleton();
	DEV_ASSERT(singleton != nullptr);

	Ref<Shader> shader = p_resource;
	if (shader.is_valid()) {
		// A shader no saved material uses is only met here: the game builds its material at run time
		// (`ShaderMaterial.shader = load(...)`). Its versions and hit groups follow its code and render modes, not a
		// material's parameters, so a material made for the bake compiles what the game's will.
		const RID shader_rid = shader->get_rid();
		if (shader_rid.is_valid() && !shaders_customized.has(shader_rid)) {
			Ref<ShaderMaterial> shader_material;
			shader_material.instantiate();
			shader_material->set_shader(shader);
			_customize_resource(shader_material, String());
		}
		return Ref<Resource>();
	}

	Ref<Material> material = p_resource;
	if (material.is_valid()) {
		RID material_rid = material->get_rid();
		// The export also visits a built-in material's shader: baked here already.
		const Ref<ShaderMaterial> shader_material = material;
		if (shader_material.is_valid() && shader_material->get_shader().is_valid()) {
			shaders_customized.insert(shader_material->get_shader()->get_rid());
		}
		if (material_rid.is_valid()) {
			RendererRD::MaterialStorage::ShaderData *shader_data = singleton->material_get_shader_data(material_rid);
			if (shader_data != nullptr) {
				Pair<ShaderRD *, RID> shader_version_pair = shader_data->get_native_shader_and_version();
				if (shader_version_pair.first != nullptr) {
					_customize_shader_version(shader_version_pair.first, shader_version_pair.second);
				}
			}
			_customize_raytracing_programs(material_rid);
		}
	}

	return Ref<Resource>();
}

Node *ShaderBakerExportPlugin::_customize_scene(Node *p_root, const String &p_path) {
	LocalVector<Node *> nodes_to_visit;
	nodes_to_visit.push_back(p_root);
	while (!nodes_to_visit.is_empty()) {
		// Visit all nodes recursively in the scene to find the Label3Ds and Sprite3Ds.
		Node *node = nodes_to_visit[nodes_to_visit.size() - 1];
		nodes_to_visit.remove_at(nodes_to_visit.size() - 1);

		Label3D *label_3d = Object::cast_to<Label3D>(node);
		Sprite3D *sprite_3d = Object::cast_to<Sprite3D>(node);
		if (label_3d != nullptr || sprite_3d != nullptr) {
			// Create materials for Label3D and Sprite3D, which are normally generated at runtime on demand.
			HashMap<StringName, Variant> properties;

			// These must match the defaults set by Sprite3D/Label3D.
			properties["transparent"] = true; // Label3D doesn't have this property, but it is always true anyway.
			properties["shaded"] = false;
			properties["double_sided"] = true;
			properties["no_depth_test"] = false;
			properties["fixed_size"] = false;
			properties["billboard"] = StandardMaterial3D::BILLBOARD_DISABLED;
			properties["texture_filter"] = StandardMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS;
			properties["alpha_antialiasing_mode"] = StandardMaterial3D::ALPHA_ANTIALIASING_OFF;
			properties["alpha_cut"] = SpriteBase3D::ALPHA_CUT_DISABLED;

			// Read only the properties used below. Reading the whole property list would also read
			// getters that error on a scene outside the tree, such as Node3D's global_transform.
			for (KeyValue<StringName, Variant> &E : properties) {
				bool valid = false;
				Variant property = node->get(E.key, &valid);
				if (valid) {
					E.value = property;
				}
			}

			// This must follow the logic in Sprite3D::draw_texture_rect().
			BaseMaterial3D::Transparency mat_transparency = BaseMaterial3D::Transparency::TRANSPARENCY_DISABLED;
			if (properties["transparent"]) {
				SpriteBase3D::AlphaCutMode acm = SpriteBase3D::AlphaCutMode(int(properties["alpha_cut"]));
				if (acm == SpriteBase3D::ALPHA_CUT_DISCARD) {
					mat_transparency = BaseMaterial3D::Transparency::TRANSPARENCY_ALPHA_SCISSOR;
				} else if (acm == SpriteBase3D::ALPHA_CUT_OPAQUE_PREPASS) {
					mat_transparency = BaseMaterial3D::Transparency::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS;
				} else if (acm == SpriteBase3D::ALPHA_CUT_HASH) {
					mat_transparency = BaseMaterial3D::Transparency::TRANSPARENCY_ALPHA_HASH;
				} else {
					mat_transparency = BaseMaterial3D::Transparency::TRANSPARENCY_ALPHA;
				}
			}

			StandardMaterial3D::BillboardMode billboard_mode = StandardMaterial3D::BillboardMode(int(properties["billboard"]));
			Ref<Material> sprite_3d_material = StandardMaterial3D::get_material_for_2d(bool(properties["shaded"]), mat_transparency, bool(properties["double_sided"]), billboard_mode == StandardMaterial3D::BILLBOARD_ENABLED, billboard_mode == StandardMaterial3D::BILLBOARD_FIXED_Y, false, bool(properties["no_depth_test"]), bool(properties["fixed_size"]), BaseMaterial3D::TextureFilter(int(properties["texture_filter"])), BaseMaterial3D::AlphaAntiAliasing(int(properties["alpha_antialiasing_mode"])));
			_customize_resource(sprite_3d_material, String());

			if (label_3d != nullptr) {
				// Generate variants with and without MSDF support since we don't have access to the font here.
				Ref<Material> label_3d_material = StandardMaterial3D::get_material_for_2d(bool(properties["shaded"]), mat_transparency, bool(properties["double_sided"]), billboard_mode == StandardMaterial3D::BILLBOARD_ENABLED, billboard_mode == StandardMaterial3D::BILLBOARD_FIXED_Y, true, bool(properties["no_depth_test"]), bool(properties["fixed_size"]), BaseMaterial3D::TextureFilter(int(properties["texture_filter"])), BaseMaterial3D::AlphaAntiAliasing(int(properties["alpha_antialiasing_mode"])));
				_customize_resource(label_3d_material, String());
			}
		}

		// Visit children.
		int child_count = node->get_child_count();
		for (int i = 0; i < child_count; i++) {
			nodes_to_visit.push_back(node->get_child(i));
		}
	}

	return nullptr;
}

void ShaderBakerExportPlugin::_get_export_options(const Ref<EditorExportPlatform> &p_export_platform, List<EditorExportPlatform::ExportOption> *r_options) const {
	// Forward+ only: the path tracer's shaders (Environment.pathtracing_enabled). Off for a project that never path
	// traces, which would bake every custom material's hit groups for nothing.
	r_options->push_back(EditorExportPlatform::ExportOption(PropertyInfo(Variant::BOOL, "shader_baker/path_tracing"), true));
}

uint64_t ShaderBakerExportPlugin::_get_customization_configuration_hash() const {
	return customization_configuration_hash;
}

void ShaderBakerExportPlugin::_customize_shader_version(ShaderRD *p_shader, RID p_version) {
	const int64_t variant_count = p_shader->get_variant_count();
	const int64_t group_count = p_shader->get_group_count();
	LocalVector<ShaderGroupItem> group_items;
	group_items.resize(group_count);

	RBSet<uint32_t> groups_to_compile;
	for (int64_t i = 0; i < group_count; i++) {
		if (!p_shader->is_group_enabled(i)) {
			continue;
		}

		String cache_path = p_shader->version_get_cache_file_relative_path(p_version, i, shader_container_driver);
		if (shader_paths_processed.has(cache_path)) {
			continue;
		}

		shader_paths_processed.insert(cache_path);
		groups_to_compile.insert(i);

		group_items[i].cache_path = cache_path;
		group_items[i].variants = p_shader->get_group_to_variants(i);

		{
			MutexLock lock(shader_work_results_mutex);
			shader_work_results[cache_path].variant_data.resize(variant_count);
		}
	}

	for (int64_t i = 0; i < variant_count; i++) {
		int group = p_shader->get_variant_to_group(i);
		if (!p_shader->is_variant_enabled(i) || !groups_to_compile.has(group)) {
			continue;
		}

		WorkItem work_item;
		work_item.cache_path = group_items[group].cache_path;
		work_item.shader_name = p_shader->get_name();
		work_item.stage_sources = p_shader->version_build_variant_stage_sources(p_version, i);
		work_item.dynamic_buffers = p_shader->get_dynamic_buffers();
		work_item.variant = i;

		WorkerThreadPool::TaskID task_id = WorkerThreadPool::get_singleton()->add_template_task(this, &ShaderBakerExportPlugin::_process_work_item, work_item);
		group_items[group].variant_tasks.push_back(task_id);
		tasks_total++;
	}

	for (uint32_t i : groups_to_compile) {
		shader_group_items.push_back(group_items[i]);
	}
}

void ShaderBakerExportPlugin::_process_work_item(WorkItem p_work_item) {
	if (!tasks_cancelled) {
		// Only process the item if the tasks haven't been cancelled by the user yet.
		Vector<RD::ShaderStageSPIRVData> spirv_data = ShaderRD::compile_stages(p_work_item.stage_sources, p_work_item.dynamic_buffers);
		if (unlikely(spirv_data.is_empty())) {
			ERR_PRINT("Unable to retrieve SPIR-V data for shader.");
		} else {
			Ref<RenderingShaderContainer> shader_container = shader_container_format->create_container();

			// Compile shader binary from SPIR-V.
			bool code_compiled = shader_container->set_code_from_spirv(p_work_item.shader_name, spirv_data);
			if (unlikely(!code_compiled)) {
				ERR_PRINT("Failed to compile code to native for SPIR-V.");
			} else {
				PackedByteArray shader_bytes = shader_container->to_bytes();
				{
					MutexLock lock(shader_work_results_mutex);
					shader_work_results[p_work_item.cache_path].variant_data.ptrw()[p_work_item.variant] = shader_bytes;
				}
			}
		}
	}

	{
		MutexLock lock(tasks_mutex);
		tasks_processed++;
	}

	tasks_condition.notify_one();
}

void ShaderBakerExportPlugin::_customize_raytracing_programs(RID p_material) {
	using RendererSceneRenderImplementation::SceneShaderRaytracing;
	if (!raytracing_baked) {
		return;
	}
	SceneShaderRaytracing *raytracing = SceneShaderRaytracing::get_singleton_if_initialized();
	if (raytracing == nullptr) {
		return;
	}

	LocalVector<SceneShaderRaytracing::BakeProgram> programs;
	if (p_material.is_valid()) {
		raytracing->get_material_bake_programs(p_material, raytracing_simple_shadows, raytracing_ser, programs);
	} else {
		raytracing->get_bake_programs(raytracing_simple_shadows, raytracing_ser, programs);
	}

	for (const SceneShaderRaytracing::BakeProgram &program : programs) {
		// Where SceneShaderRaytracing's binary cache keeps it, under the shader cache.
		const String cache_path = String("SceneShaderRaytracing").path_join(SceneShaderRaytracing::get_binary_cache_file_name(program.cache_key, shader_container_driver));
		if (shader_paths_processed.has(cache_path)) {
			continue;
		}
		shader_paths_processed.insert(cache_path);

		RaytracingWorkItem work_item;
		work_item.cache_path = cache_path;
		work_item.shader_name = program.name;
		for (const RD::ShaderStage stage : program.stages) {
			work_item.stages.push_back(stage);
		}
		work_item.stage_sources = program.sources;

		RaytracingItem item;
		item.cache_path = cache_path;
		item.task = WorkerThreadPool::get_singleton()->add_template_task(this, &ShaderBakerExportPlugin::_process_raytracing_work_item, work_item);
		raytracing_items.push_back(item);
		tasks_total++;
	}
}

// NRD's compute programs, from the SPIR-V the engine embeds, as NRDDenoiser makes them at their first use. Only with the path
// tracer's shaders: NRD denoises it alone.
void ShaderBakerExportPlugin::_customize_nrd_programs() {
	if (!bool(get_option("shader_baker/path_tracing"))) {
		return;
	}
	LocalVector<RendererRD::NRDDenoiser::BakeProgram> programs;
	RendererRD::NRDDenoiser::get_bake_programs(programs);
	for (const RendererRD::NRDDenoiser::BakeProgram &program : programs) {
		const String cache_path = RendererRD::NRDDenoiser::get_cache_file_name(program.cache_key, shader_container_driver);
		if (shader_paths_processed.has(cache_path)) {
			continue;
		}
		shader_paths_processed.insert(cache_path);

		RaytracingWorkItem work_item;
		work_item.cache_path = cache_path;
		work_item.shader_name = program.name;
		work_item.spirv = program.spirv;

		RaytracingItem item;
		item.cache_path = cache_path;
		item.nrd = true;
		item.task = WorkerThreadPool::get_singleton()->add_template_task(this, &ShaderBakerExportPlugin::_process_raytracing_work_item, work_item);
		raytracing_items.push_back(item);
		tasks_total++;
	}
}

void ShaderBakerExportPlugin::_process_raytracing_work_item(RaytracingWorkItem p_work_item) {
	if (!tasks_cancelled) {
		// As SceneShaderRaytracing compiles them at run time: each stage on its own, the binary from all of them.
		Vector<RD::ShaderStageSPIRVData> spirv_data;
		if (!p_work_item.spirv.is_empty()) {
			RD::ShaderStageSPIRVData stage;
			stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
			stage.spirv = p_work_item.spirv;
			spirv_data.push_back(stage);
		}
		for (int i = 0; i < p_work_item.stages.size(); i++) {
			RD::ShaderStageSPIRVData stage;
			String error;
			stage.shader_stage = RD::ShaderStage(p_work_item.stages[i]);
			stage.spirv = RD::get_singleton()->shader_compile_spirv_from_source(stage.shader_stage, p_work_item.stage_sources[i], RD::SHADER_LANGUAGE_GLSL, &error);
			if (stage.spirv.is_empty()) {
				ERR_PRINT(vformat("Unable to compile the %s stage of the path tracing shader %s: %s", RD::SHADER_STAGE_NAMES[stage.shader_stage], p_work_item.shader_name, error));
				spirv_data.clear();
				break;
			}
			spirv_data.push_back(stage);
		}

		if (!spirv_data.is_empty()) {
			Ref<RenderingShaderContainer> shader_container = shader_container_format->create_container();
			if (unlikely(!shader_container->set_code_from_spirv(p_work_item.shader_name, spirv_data))) {
				ERR_PRINT(vformat("Failed to compile the path tracing shader %s to native code.", p_work_item.shader_name));
			} else {
				PackedByteArray shader_bytes = shader_container->to_bytes();
				MutexLock lock(shader_work_results_mutex);
				raytracing_work_results[p_work_item.cache_path] = shader_bytes;
			}
		}
	}

	{
		MutexLock lock(tasks_mutex);
		tasks_processed++;
	}

	tasks_condition.notify_one();
}

ShaderBakerExportPlugin::ShaderBakerExportPlugin() {
	// Do nothing.
}

ShaderBakerExportPlugin::~ShaderBakerExportPlugin() {
	// Do nothing.
}

void ShaderBakerExportPlugin::add_platform(Ref<ShaderBakerExportPluginPlatform> p_platform) {
	platforms.push_back(p_platform);
}

void ShaderBakerExportPlugin::remove_platform(Ref<ShaderBakerExportPluginPlatform> p_platform) {
	platforms.erase(p_platform);
}
