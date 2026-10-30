/**************************************************************************/
/*  shadow_watch.cpp                                                      */
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

#include "shadow_watch.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

ShadowWatch *ShadowWatch::singleton = nullptr;

namespace {

// The offsets of LightStorage::LightData, in floats.
constexpr uint32_t ENTRY_POSITION = 0;
constexpr uint32_t ENTRY_DIRECTION = 4;
constexpr uint32_t ENTRY_SHADOW_OPACITY = 21;
constexpr uint32_t ENTRY_ATLAS_RECT = 24;
constexpr uint32_t ENTRY_SHADOW_MATRIX = 28;

constexpr uint64_t REPORT_INTERVAL_USEC = 3000000;
constexpr uint64_t HEARTBEAT_USEC = 120000000;

enum ReportKind : uint64_t {
	REPORT_COPY = 1,
	REPORT_OVERWRITTEN,
	REPORT_ENTRY,
	REPORT_FLICKER,
};

uint64_t history_key(RID p_atlas, RID p_light) {
	return hash_murmur3_one_64(p_light.get_id(), hash_murmur3_one_64(p_atlas.get_id()));
}

// Blocks whose mean depth moved by more than 1% of the depth range.
uint32_t changed_blocks(const uint32_t *p_a, const uint32_t *p_b, uint32_t p_count, uint32_t p_samples_per_block) {
	const uint64_t threshold = uint64_t(p_samples_per_block) * 655;
	uint32_t changed = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		const uint64_t d = p_a[i] > p_b[i] ? p_a[i] - p_b[i] : p_b[i] - p_a[i];
		if (d > threshold) {
			changed++;
		}
	}
	return changed;
}

uint64_t total_difference(const uint32_t *p_a, const uint32_t *p_b, uint32_t p_count) {
	uint64_t total = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		total += p_a[i] > p_b[i] ? p_a[i] - p_b[i] : p_b[i] - p_a[i];
	}
	return total;
}

// Blocks whose exact value goes A, B, A, B over the last four samples: something drawn every other frame. Exact, so
// that the slower changes of animated casters (a sprite's frames) do not count.
uint32_t alternating_blocks(const uint32_t *p_0, const uint32_t *p_1, const uint32_t *p_2, const uint32_t *p_3, uint32_t p_count) {
	uint32_t count = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_0[i] != p_1[i] && p_0[i] == p_2[i] && p_1[i] == p_3[i]) {
			count++;
		}
	}
	return count;
}

float mean_depth(const uint32_t *p_sums, uint32_t p_count, uint32_t p_samples_per_block) {
	uint64_t total = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		total += p_sums[i];
	}
	return float(double(total) / (double(p_count) * p_samples_per_block * 65535.0));
}

bool lum_differs(float p_a, float p_b) {
	const float m = MAX(p_a, p_b);
	return m > 0.02f && Math::abs(p_a - p_b) > 0.25f * m;
}

bool lum_same(float p_a, float p_b) {
	return Math::abs(p_a - p_b) <= 0.1f * MAX(p_a, p_b) + 0.0001f;
}

} // namespace

bool ShadowWatch::is_enabled() {
	return OS::get_singleton()->get_environment("GODOT_SHADOW_WATCH") == "1";
}

ShadowWatch::ShadowWatch() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_LIGHT\n");
	modes.push_back("\n#define MODE_SCREEN\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}

	Vector<uint8_t> zeros;
	zeros.resize_initialized(SLOT_COUNT * SLOT_UINTS * sizeof(uint32_t));
	results = RD::get_singleton()->storage_buffer_create(zeros.size(), zeros);
	zeros.resize_initialized(SLOT_COUNT * COPY_SLOT * sizeof(uint32_t));
	copy_records = RD::get_singleton()->storage_buffer_create(zeros.size(), zeros);

	singleton = this;
	heartbeat_usec = OS::get_singleton()->get_ticks_usec();
	print_line("ShadowWatch: watching the omni lights in cube shadow mode (Wagnard/godot#1), as GODOT_SHADOW_WATCH=1 asks.");
}

ShadowWatch::~ShadowWatch() {
	if (driver_watch_shader.is_valid()) {
		RD::get_singleton()->debug_watch_push_constants(driver_watch_shader, nullptr);
	}
	singleton = nullptr;
	for (KeyValue<uint64_t, LocalVector<LightSample *>> &E : light_history) {
		for (LightSample *sample : E.value) {
			memdelete(sample);
		}
	}
	for (KeyValue<uint64_t, LocalVector<ScreenSample *>> &E : screen_history) {
		for (ScreenSample *sample : E.value) {
			memdelete(sample);
		}
	}
	RD::get_singleton()->free_rid(results);
	RD::get_singleton()->free_rid(copy_records);
	shader.version_free(shader_version);
}

void ShadowWatch::begin_render() {
	if (current) {
		// The previous render had no screen pass.
		_read_back();
	}
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	if (frame != last_frame) {
		last_frame = frame;
		render_in_frame = 0;
	} else {
		render_in_frame++;
	}
	copies.clear();
	current_serial = ++render_serial;
	render_slot = next_slot;
	next_slot = (next_slot + 1) % SLOT_COUNT;
}

void ShadowWatch::_push_constants_bound_static(const uint32_t *p_data, uint32_t p_count, bool p_root_signature_bound) {
	// CopyEffects::CopyToDPPushConstant: z_far, z_near, texel size x and y, record offset, stamp, z_far again, pad.
	if (singleton == nullptr || p_count < 6) {
		return;
	}
	const uint64_t key = (uint64_t(p_data[5]) << 32) | p_data[4];
	MutexLock lock(singleton->driver_push_mutex);
	if (singleton->driver_pushes.size() > 8192) {
		// Copies of renders the watch skipped are never looked up.
		singleton->driver_pushes.clear();
	}
	singleton->driver_pushes[key] = { p_data[0], p_root_signature_bound };
}

void ShadowWatch::cube_copy_record(RID p_light, RID p_cubemap, float p_z_near, float p_z_far, RID &r_record, uint32_t r_offset[2], uint32_t &r_stamp) {
	if (!driver_watch_registered) {
		driver_watch_registered = true;
		driver_watch_shader = CopyEffects::get_singleton()->get_cube_to_dp_watch_shader();
		if (driver_watch_shader.is_valid()) {
			RD::get_singleton()->debug_watch_push_constants(driver_watch_shader, &ShadowWatch::_push_constants_bound_static);
		}
	}
	CubeCopy copy;
	copy.light = p_light;
	copy.world_position = LightStorage::get_singleton()->light_instance_get_base_transform(p_light).origin;
	copy.cubemap = p_cubemap;
	copy.z_near = p_z_near;
	copy.z_far = p_z_far;
	r_record = RID();
	if (copies.size() < MAX_COPIES) {
		copy.record = render_slot * COPY_SLOT + copies.size() * COPY_RECORD;
		r_record = copy_records;
		r_offset[0] = copy.record;
		r_offset[1] = copy.record + COPY_HALF_RECORD;
		r_stamp = uint32_t(current_serial);
	}
	copies.push_back(copy);
}

void ShadowWatch::watch_lights(const RenderDataRD *p_render_data) {
	LightStorage *light_storage = LightStorage::get_singleton();
	const RID atlas = p_render_data->shadow_atlas;
	if (p_render_data->lights == nullptr || p_render_data->reflection_probe.is_valid() || !light_storage->owns_shadow_atlas(atlas)) {
		return;
	}

	const Transform3D cam_transform = p_render_data->scene_data->cam_transform;
	const Transform3D inverse_cam = cam_transform.affine_inverse();

	// The nearest omni lights in cube mode whose shadow is in this atlas.
	struct Nearest {
		RID light;
		float distance;
		bool operator<(const Nearest &p_other) const { return distance < p_other.distance; }
	};
	LocalVector<Nearest> candidates;
	uint32_t omni_count = 0;
	const PagedArray<RID> &lights = *p_render_data->lights;
	for (uint64_t i = 0; i < lights.size(); i++) {
		const RID base = light_storage->light_instance_get_base_light(lights[i]);
		if (light_storage->light_get_type(base) != RSE::LIGHT_OMNI) {
			continue;
		}
		omni_count++;
		if (!light_storage->light_has_shadow(base) || light_storage->light_omni_get_shadow_mode(base) != RSE::LIGHT_OMNI_SHADOW_CUBE || !light_storage->shadow_atlas_owns_light_instance(atlas, lights[i])) {
			continue;
		}
		candidates.push_back({ lights[i], float(cam_transform.origin.distance_to(light_storage->light_instance_get_base_transform(lights[i]).origin)) });
	}
	if (candidates.is_empty()) {
		return;
	}
	candidates.sort();
	if (candidates.size() > MAX_LIGHTS) {
		candidates.resize(MAX_LIGHTS);
	}

	WatchedRender &render = pending.insert(current_serial, WatchedRender())->value;
	current = &render;
	render.frame = RSG::rasterizer->get_frame_number();
	render.render_in_frame = render_in_frame;
	render.atlas = atlas;
	render.cam_transform = cam_transform;
	render.slot = render_slot;
	render.entry_count = MIN(omni_count, MAX_ENTRIES);
	render.entries_complete = omni_count <= MAX_ENTRIES;
	if (p_render_data->render_buffers.is_valid()) {
		render.size = p_render_data->render_buffers->get_internal_size();
	}

	const int atlas_size = light_storage->shadow_atlas_get_size(atlas);
	for (const Nearest &candidate : candidates) {
		WatchedLight wl;
		wl.light = candidate.light;
		const Transform3D light_transform = light_storage->light_instance_get_base_transform(wl.light);
		wl.world_position = light_transform.origin;

		Vector2i omni_offset;
		const Rect2 rect = light_storage->light_instance_get_shadow_atlas_rect(wl.light, atlas, omni_offset);
		const Rect2i pixels(Math::round(rect.position.x * atlas_size), Math::round(rect.position.y * atlas_size), Math::round(rect.size.width * atlas_size), Math::round(rect.size.height * atlas_size));
		wl.half_rect[0] = pixels;
		wl.half_rect[1] = Rect2i(pixels.position + omni_offset * pixels.size, pixels.size);

		for (int i = 0; i < p_render_data->render_shadow_count; i++) {
			const RendererSceneRender::RenderShadowData &shadow = p_render_data->render_shadows[i];
			if (shadow.light == wl.light && shadow.pass >= 0 && shadow.pass < 6) {
				wl.redrawn = true;
				wl.face_casters[shadow.pass] = shadow.instances.size();
			}
		}

		wl.atlas_rect[0] = rect.position.x;
		wl.atlas_rect[1] = rect.position.y;
		wl.atlas_rect[2] = rect.size.width;
		wl.atlas_rect[3] = rect.size.height;
		wl.flip_offset[0] = omni_offset.x * rect.size.width;
		wl.flip_offset[1] = omni_offset.y * rect.size.height;
		const Vector3 position = inverse_cam.xform(light_transform.origin);
		wl.position[0] = position.x;
		wl.position[1] = position.y;
		wl.position[2] = position.z;
		MaterialStorage::store_transform((inverse_cam * light_transform).inverse(), wl.shadow_matrix);
		render.lights.push_back(wl);
	}

	// A light's copy can be checked against its cubemap only while no later copy of the render reused that cubemap.
	// What the copy read is compared with what the other copies of the same cubemap read, in this render and in the
	// view's previous one: a match names the content it got.
	RID copy_cubemap[MAX_LIGHTS];
	float copy_z_near[MAX_LIGHTS] = {};
	float copy_z_far[MAX_LIGHTS] = {};
	uint32_t record_offset[MAX_LIGHTS];
	uint32_t candidate_offsets[MAX_LIGHTS][MAX_CANDIDATES] = {};
	const LocalVector<CubeCopy> *previous = previous_copies.getptr(atlas.get_id());
	for (uint32_t l = 0; l < render.lights.size(); l++) {
		WatchedLight &wl = render.lights[l];
		record_offset[l] = UINT32_MAX;
		for (int64_t i = int64_t(copies.size()) - 1; i >= 0; i--) {
			if (copies[i].light != wl.light) {
				continue;
			}
			bool reused = false;
			for (uint32_t j = i + 1; j < copies.size(); j++) {
				reused = reused || copies[j].cubemap == copies[i].cubemap;
			}
			if (!reused) {
				wl.copy_check = true;
				copy_cubemap[l] = copies[i].cubemap;
				copy_z_near[l] = copies[i].z_near;
				copy_z_far[l] = copies[i].z_far;
			}
			record_offset[l] = copies[i].record;
			wl.recorded = copies[i].record != UINT32_MAX;
			wl.copy_z_near = copies[i].z_near;
			wl.copy_z_far = copies[i].z_far;
			wl.record_offset = copies[i].record;
			if (wl.recorded) {
				for (int pass = 0; pass < 2; pass++) {
					const LocalVector<CubeCopy> *list = pass == 0 ? &copies : previous;
					for (uint32_t j = 0; list && j < list->size() && wl.candidate_count < MAX_CANDIDATES; j++) {
						const CubeCopy &other = (*list)[j];
						if ((pass == 0 && int64_t(j) == i) || other.record == UINT32_MAX || other.cubemap != copies[i].cubemap) {
							continue;
						}
						candidate_offsets[l][wl.candidate_count] = other.record;
						wl.candidates[wl.candidate_count].world_position = other.world_position;
						wl.candidates[wl.candidate_count].previous_render = pass == 1;
						wl.candidate_count++;
					}
				}
			}
			break;
		}
	}
	previous_copies[atlas.get_id()] = copies;
	for (const CubeCopy &copy : copies) {
		render.copy_params.push_back({ copy.world_position, copy.z_near, copy.z_far });
	}

	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	// The sampler cube_to_dp.glsl reads the cubemap with (CopyEffects::copy_cubemap_to_dp()).
	const RID linear = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID black_cube = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_CUBEMAP_BLACK);
	const RID shader_rid = shader.version_get_shader(shader_version, MODE_LIGHT);
	const uint32_t slot_base = render.slot * SLOT_UINTS;

	rd->draw_command_begin_label("ShadowWatch Lights");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_LIGHT]);
	for (uint32_t l = 0; l < render.lights.size(); l++) {
		const WatchedLight &wl = render.lights[l];
		RD::Uniform u_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, light_storage->shadow_atlas_get_texture(atlas) }));
		RD::Uniform u_cube(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ linear, wl.copy_check ? copy_cubemap[l] : black_cube }));
		RD::Uniform u_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, light_storage->get_omni_light_buffer());
		RD::Uniform u_results(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, results);
		RD::Uniform u_records(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, copy_records);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_atlas, u_cube, u_lights, u_results, u_records), 0);

		LightPushConstant push_constant = {};
		push_constant.record_offset = record_offset[l];
		push_constant.stamp = uint32_t(current_serial);
		push_constant.candidate_count = wl.candidate_count;
		for (uint32_t k = 0; k < wl.candidate_count; k++) {
			push_constant.candidates[k] = candidate_offsets[l][k];
		}
		for (int h = 0; h < 2; h++) {
			push_constant.half_rect[h][0] = wl.half_rect[h].position.x;
			push_constant.half_rect[h][1] = wl.half_rect[h].position.y;
			push_constant.half_rect[h][2] = wl.half_rect[h].size.width;
			push_constant.half_rect[h][3] = wl.half_rect[h].size.height;
		}
		push_constant.z_near = copy_z_near[l];
		push_constant.z_far = copy_z_far[l];
		push_constant.flags = (wl.copy_check ? 3u : 0u) | (l == 0 ? 4u : 0u);
		push_constant.out_offset = slot_base + l * LIGHT_RECORD;
		push_constant.entries_offset = slot_base + SLOT_ENTRIES;
		push_constant.entry_floats = render.entry_count * ENTRY_FLOATS;
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(LightPushConstant));
		rd->compute_list_dispatch(compute_list, 1, 9, 1);
	}
	rd->compute_list_end();
	rd->draw_command_end_label();

	if (p_render_data->render_buffers.is_null()) {
		_read_back();
	}
}

void ShadowWatch::watch_screen(const RenderDataRD *p_render_data, RID p_depth, RID p_color) {
	if (current == nullptr) {
		return;
	}
	WatchedRender &render = *current;
	if (p_depth.is_null() || p_color.is_null()) {
		_read_back();
		return;
	}

	LightStorage *light_storage = LightStorage::get_singleton();
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID shader_rid = shader.version_get_shader(shader_version, MODE_SCREEN);
	const uint32_t slot_base = render.slot * SLOT_UINTS;
	const Projection inverse_projection = p_render_data->scene_data->get_cam_projection().inverse();
	const Transform3D inverse_cam = render.cam_transform.affine_inverse();
	const float texel = 1.0f / float(light_storage->shadow_atlas_get_size(render.atlas));

	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
	RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_color }));
	RD::Uniform u_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest, light_storage->shadow_atlas_get_texture(render.atlas) }));
	RD::Uniform u_results(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, results);

	rd->draw_command_begin_label("ShadowWatch Screen");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_SCREEN]);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_depth, u_color, u_atlas, u_results), 0);
	for (uint32_t l = 0; l < render.lights.size(); l++) {
		const WatchedLight &wl = render.lights[l];
		const RID base = light_storage->light_instance_get_base_light(wl.light);
		const Transform3D light_transform = light_storage->light_instance_get_base_transform(wl.light);
		const Projection ndc_to_light = Projection((inverse_cam * light_transform).inverse()) * inverse_projection;

		ScreenPushConstant push_constant = {};
		MaterialStorage::store_camera(ndc_to_light, push_constant.ndc_to_light);
		// As the scene shader shrinks the cell (scene_forward_lights_inc.glsl, light_process_omni()).
		push_constant.uv_rect[0] = wl.atlas_rect[0] + texel;
		push_constant.uv_rect[1] = wl.atlas_rect[1] + texel;
		push_constant.uv_rect[2] = wl.atlas_rect[2] - texel * 2.0f;
		push_constant.uv_rect[3] = wl.atlas_rect[3] - texel * 2.0f;
		push_constant.flip_offset[0] = wl.flip_offset[0];
		push_constant.flip_offset[1] = wl.flip_offset[1];
		push_constant.inv_radius = 1.0f / MAX(0.001f, light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE));
		push_constant.shadow_bias = light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_BIAS);
		push_constant.grid[0] = GRID_X;
		push_constant.grid[1] = GRID_Y;
		push_constant.out_lum = l == 0 ? slot_base + SLOT_LUM : 0xFFFFFFFFu;
		push_constant.out_vis = slot_base + SLOT_VIS + l * GRID_POINTS;
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(ScreenPushConstant));
		rd->compute_list_dispatch_threads(compute_list, GRID_POINTS, 1, 1);
	}
	rd->compute_list_end();
	rd->draw_command_end_label();

	render.screen = true;
	_read_back();
}

void ShadowWatch::_read_back() {
	const WatchedRender &render = *current;
	current = nullptr;
	RD::get_singleton()->buffer_get_data_async(results, callable_mp_static(&ShadowWatch::_results_arrived_static).bind(current_serial), render.slot * SLOT_UINTS * sizeof(uint32_t), SLOT_UINTS * sizeof(uint32_t));
}

void ShadowWatch::_results_arrived_static(const Vector<uint8_t> &p_data, uint64_t p_serial) {
	if (singleton) {
		singleton->_results_arrived(p_data, p_serial);
	}
}

void ShadowWatch::_results_arrived(const Vector<uint8_t> &p_data, uint64_t p_serial) {
	WatchedRender *render = pending.getptr(p_serial);
	if (render == nullptr) {
		return;
	}
	if (p_data.size() >= int64_t(SLOT_UINTS * sizeof(uint32_t))) {
		_analyze(*render, p_serial, reinterpret_cast<const uint32_t *>(p_data.ptr()));
	}
	pending.erase(p_serial);

	checked_renders++;
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (now - heartbeat_usec > HEARTBEAT_USEC) {
		print_line(vformat("ShadowWatch: %d views checked in the last 2 minutes, %d anomalies reported.", checked_renders, anomalies));
		heartbeat_usec = now;
		checked_renders = 0;
		anomalies = 0;
	}
}

bool ShadowWatch::_may_report(uint64_t p_kind, RID p_light) {
	const uint64_t key = hash_murmur3_one_64(p_kind, hash_murmur3_one_64(p_light.get_id()));
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	uint64_t *last = last_report_usec.getptr(key);
	anomalies++;
	if (last && now - *last < REPORT_INTERVAL_USEC) {
		return false;
	}
	last_report_usec[key] = now;
	return true;
}

String ShadowWatch::_light_name(const WatchedLight &p_light, const WatchedRender &p_render) const {
	return vformat("omni light at (%.2f, %.2f, %.2f), atlas cell (%d, %d) %dx%d, view %dx%d, frame %d (view %d of the frame)", p_light.world_position.x, p_light.world_position.y, p_light.world_position.z, p_light.half_rect[0].position.x, p_light.half_rect[0].position.y, p_light.half_rect[0].size.width, p_light.half_rect[0].size.height, p_render.size.width, p_render.size.height, p_render.frame, p_render.render_in_frame);
}

String ShadowWatch::_history(const LocalVector<LightSample *> &p_history) const {
	static const char *entry_names[] = { "unchecked", "ok", "shadow off", "MISSING", "WRONG" };
	String text;
	for (uint32_t i = 0; i < p_history.size(); i++) {
		const LightSample &s = *p_history[i];
		String faces = "not read";
		if (s.faces) {
			faces = "";
			for (int f = 0; f < 6; f++) {
				faces += vformat("%s%.3f", f ? " " : "", mean_depth(s.face_sum[f], 16, 16));
			}
		}
		text += vformat("\n    frame %d: redrawn %s, casters per face [%d %d %d %d %d %d], half depth %.4f / %.4f, faces [%s], copy mismatches %d/%d and %d/%d, light data %s",
				s.frame, s.redrawn ? "yes" : "no", s.face_casters[0], s.face_casters[1], s.face_casters[2], s.face_casters[3], s.face_casters[4], s.face_casters[5],
				mean_depth(s.half_sum[0], 64, 16), mean_depth(s.half_sum[1], 64, 16), faces, s.mismatch[0], s.checked[0], s.mismatch[1], s.checked[1], entry_names[s.entry]);
	}
	return text;
}

void ShadowWatch::_expected_copy_header(const WatchedLight &p_light, int p_half, float r_header[6]) {
	// As CopyEffects::copy_cubemap_to_dp() sets the push constants, and where the first grid point lands.
	const Rect2i &rect = p_light.half_rect[p_half];
	r_header[0] = p_light.copy_z_far;
	r_header[1] = p_light.copy_z_near;
	r_header[2] = (1.0f / float(rect.size.width)) * (p_half == 1 ? -1.0f : 1.0f);
	r_header[3] = 1.0f / float(rect.size.height);
	r_header[4] = float(rect.position.x + rect.size.width / 64) + 0.5f;
	r_header[5] = float(rect.position.y + rect.size.height / 64) + 0.5f;
}

bool ShadowWatch::_copy_params_as_given(const LightSample &p_sample, const WatchedLight &p_light, int p_half) {
	float expected[6];
	_expected_copy_header(p_light, p_half, expected);
	for (int w = 0; w < 6; w++) {
		float got;
		memcpy(&got, &p_sample.header[p_half][w], sizeof(float));
		if (got != expected[w]) {
			return false;
		}
	}
	return true;
}

String ShadowWatch::_copy_verdict(const LightSample &p_sample, const WatchedLight &p_light, const WatchedRender &p_render, int p_half) const {
	if (!p_sample.recorded) {
		return "Its copy left no record (more than 16 copies in the view): no verdict.";
	}
	if (p_sample.stale[p_half]) {
		String stale = vformat("The copy of half %d left no record in this frame: it did not run, or ran with another record offset or stamp than it was given.", p_half);
		if (p_sample.driver_seen[p_half]) {
			float driver_z_far;
			memcpy(&driver_z_far, &p_sample.driver_z_far[p_half], sizeof(float));
			stale += vformat(" Yet the D3D12 driver WAS handed its constants with that record offset and stamp (z_far %.4f, given %.4f), the root signature it last bound %s, and set them: the draw was submitted with them, so after our driver handed them to Direct3D 12 the copy either did not run or ran with other constants.", driver_z_far, p_light.copy_z_far, p_sample.driver_root_signature_bound[p_half] ? "being the copy shader's" : "NOT being the copy shader's");
		} else {
			stale += " The D3D12 driver never reported receiving its constants either: they were lost before our driver.";
		}
		return stale;
	}
	const uint32_t points = 1024;
	String text;
	float expected[6];
	_expected_copy_header(p_light, p_half, expected);
	float got[7];
	memcpy(got, p_sample.header[p_half], sizeof(got));
	if (!_copy_params_as_given(p_sample, p_light, p_half)) {
		text = vformat("The copy of half %d RAN WITH OTHER CONSTANTS than it was given: z_far %.4f (given %.4f), z_near %.4f (given %.4f), texel size (%.6f, %.6f) (given (%.6f, %.6f)), first point at (%.1f, %.1f) (expected (%.1f, %.1f)).",
				p_half, got[0], expected[0], got[1], expected[1], got[2], got[3], expected[2], expected[3], got[4], got[5], expected[4], expected[5]);
		// The second copy of z_far, at the end of the same constants, written by the same CPU code.
		if (got[0] != expected[0]) {
			if (got[6] == expected[0]) {
				text += vformat(" Its second copy of z_far, at the end of the same constants, is right (%.4f): only the FIRST constant changed after the CPU recorded them, on the way to the GPU (driver or GPU side).", got[6]);
			} else if (got[6] == got[0]) {
				text += vformat(" Its second copy of z_far is wrong the same way (%.4f): the constants were RECORDED with that z_far, on the CPU side.", got[6]);
			} else {
				text += vformat(" Its second copy of z_far is %.4f, neither.", got[6]);
			}
			// What our D3D12 driver was handed for this copy, right where it calls SetGraphicsRoot32BitConstants().
			if (!p_sample.driver_seen[p_half]) {
				text += " The D3D12 driver never reported receiving this copy's constants (not D3D12, or never handed over).";
			} else {
				float driver_z_far;
				memcpy(&driver_z_far, &p_sample.driver_z_far[p_half], sizeof(float));
				const char *root = p_sample.driver_root_signature_bound[p_half] ? "the root signature it last bound being the copy shader's" : "the root signature it last bound NOT being the copy shader's";
				if (driver_z_far == expected[0]) {
					text += vformat(" The D3D12 driver was handed the right z_far (%.4f), %s, and set it with SetGraphicsRoot32BitConstants(): the value changed after our driver handed it to Direct3D 12.", driver_z_far, root);
				} else {
					text += vformat(" The D3D12 driver was already handed z_far %.4f, %s: it changed before our driver, in RenderingDevice or its graph.", driver_z_far, root);
				}
			}
		}
		String owners;
		for (const CopyParams &params : p_render.copy_params) {
			if (params.z_far == got[0] && params.z_near == got[1] && !params.world_position.is_equal_approx(p_light.world_position)) {
				owners += vformat(" (%.2f, %.2f, %.2f)", params.world_position.x, params.world_position.y, params.world_position.z);
			}
		}
		text += owners.is_empty() ? " No other copy of this view was given that z_far and z_near. " : " That z_far and z_near were given to the copies of the omni lights at" + owners + ". ";
	}
	if (got[0] == expected[0] && got[1] == expected[1] && got[2] == expected[2] && got[3] == expected[3] && (got[4] != expected[4] || got[5] != expected[5])) {
		text += "Its constants are right but it drew elsewhere than its region. ";
	}
	if (p_sample.write_diff[p_half] * 50 > points) {
		text += vformat("The copy of half %d wrote other depth than the atlas now holds at %d of %d points: the half was written over after the copy. ", p_half, p_sample.write_diff[p_half], points);
	} else {
		text += vformat("The atlas half %d holds what its copy wrote. ", p_half);
	}
	if (p_sample.checked[p_half] == 0) {
		return text + "Its cubemap was reused by a later copy, so what the copy read cannot be checked.";
	}
	if (p_sample.read_diff[p_half] * 50 <= points) {
		return text + "The copy read the cubemap as it is after the shadows.";
	}
	text += vformat("The copy read other cube content than the cubemap holds after the shadows, at %d of %d points: it ran when the cubemap held other content", p_sample.read_diff[p_half], points);
	uint32_t best = 0;
	int best_index = -1;
	for (uint32_t k = 0; k < p_light.candidate_count; k++) {
		if (p_sample.matches[p_half][k] > best) {
			best = p_sample.matches[p_half][k];
			best_index = k;
		}
	}
	if (best_index >= 0 && best * 10 >= points * 9) {
		const Candidate &c = p_light.candidates[best_index];
		return text + vformat(": exactly what the copy for the omni light at (%.2f, %.2f, %.2f) read in %s (%d of %d points the same).", c.world_position.x, c.world_position.y, c.world_position.z, c.previous_render ? "the view's previous render" : "this render", best, points);
	}
	return text + vformat(", which no other copy of this view read in this or its previous render (best match %d of %d points): the cube before this light's own pass, or another view's.", best, points);
}

void ShadowWatch::_analyze(const WatchedRender &p_render, uint64_t p_serial, const uint32_t *p_data) {
	const float *entries = reinterpret_cast<const float *>(p_data + SLOT_ENTRIES);
	const uint64_t *previous_serial = last_serial.getptr(p_render.atlas.get_id());
	const uint64_t previous_render = previous_serial ? *previous_serial : 0;
	last_serial[p_render.atlas.get_id()] = p_serial;

	for (uint32_t l = 0; l < p_render.lights.size(); l++) {
		const WatchedLight &wl = p_render.lights[l];
		const uint32_t *record = p_data + l * LIGHT_RECORD;

		LightSample *s = memnew(LightSample);
		s->serial = p_serial;
		s->frame = p_render.frame;
		s->redrawn = wl.redrawn;
		s->faces = wl.copy_check;
		s->half_rect = wl.half_rect[0];
		for (int h = 0; h < 2; h++) {
			memcpy(s->half_sum[h], record + h * HALF_STRIDE, sizeof(s->half_sum[h]));
			s->mismatch[h] = record[h * HALF_STRIDE + 66];
			s->checked[h] = record[h * HALF_STRIDE + 67];
		}
		for (int f = 0; f < 6; f++) {
			memcpy(s->face_sum[f], record + FACES_BASE + f * FACE_STRIDE, sizeof(s->face_sum[f]));
		}
		memcpy(s->face_casters, wl.face_casters, sizeof(s->face_casters));
		s->recorded = wl.recorded;
		for (int h = 0; h < 2; h++) {
			const uint32_t *half = record + h * HALF_STRIDE;
			s->read_diff[h] = half[68];
			s->write_diff[h] = half[69];
			s->stale[h] = half[70] != 0;
			for (uint32_t k = 0; k < MAX_CANDIDATES; k++) {
				s->matches[h][k] = half[72 + k];
			}
			for (uint32_t w = 0; w < 7; w++) {
				s->header[h][w] = half[80 + w];
			}
			if (wl.record_offset != UINT32_MAX) {
				const uint64_t key = (uint64_t(uint32_t(p_serial)) << 32) | (wl.record_offset + h * COPY_HALF_RECORD);
				MutexLock lock(driver_push_mutex);
				const DriverPush *push = driver_pushes.getptr(key);
				if (push) {
					s->driver_seen[h] = true;
					s->driver_z_far[h] = push->z_far;
					s->driver_root_signature_bound[h] = push->root_signature_bound;
					driver_pushes.erase(key);
				}
			}
		}

		// What the main pass reads for this light: the entries at its view-space position. Several lights can share
		// a position (lights parked out of sight), so the light is fine when one of them is its own.
		const Vector3 expected(wl.position[0], wl.position[1], wl.position[2]);
		const float *first_entry = nullptr;
		bool shadow_off = false;
		bool entry_ok = false;
		float matrix_error = 0.0f;
		for (uint32_t e = 0; e < p_render.entry_count && !entry_ok; e++) {
			const float *entry = entries + e * ENTRY_FLOATS;
			const Vector3 position(entry[ENTRY_POSITION], entry[ENTRY_POSITION + 1], entry[ENTRY_POSITION + 2]);
			if (position.distance_to(expected) > 0.001f * MAX(1.0f, expected.length())) {
				continue;
			}
			if (entry[ENTRY_SHADOW_OPACITY] <= 0.001f) {
				shadow_off = true;
				continue;
			}
			float r = 0.0f;
			for (int i = 0; i < 4; i++) {
				r = MAX(r, Math::abs(entry[ENTRY_ATLAS_RECT + i] - wl.atlas_rect[i]));
			}
			const float f = MAX(Math::abs(entry[ENTRY_DIRECTION] - wl.flip_offset[0]), Math::abs(entry[ENTRY_DIRECTION + 1] - wl.flip_offset[1]));
			float m = 0.0f;
			for (int i = 0; i < 16; i++) {
				m = MAX(m, Math::abs(entry[ENTRY_SHADOW_MATRIX + i] - wl.shadow_matrix[i]) / MAX(1.0f, Math::abs(wl.shadow_matrix[i])));
			}
			entry_ok = r <= 1e-6f && f <= 1e-6f && m <= 1e-3f;
			if (first_entry == nullptr) {
				first_entry = entry;
				matrix_error = m;
			}
		}
		String entry_problem;
		if (entry_ok) {
			s->entry = ENTRY_OK;
		} else if (first_entry) {
			s->entry = ENTRY_WRONG;
			entry_problem = vformat("atlas rect read (%.5f, %.5f, %.5f, %.5f) expected (%.5f, %.5f, %.5f, %.5f), half offset read (%.5f, %.5f) expected (%.5f, %.5f), shadow matrix error %.5f",
					first_entry[ENTRY_ATLAS_RECT], first_entry[ENTRY_ATLAS_RECT + 1], first_entry[ENTRY_ATLAS_RECT + 2], first_entry[ENTRY_ATLAS_RECT + 3], wl.atlas_rect[0], wl.atlas_rect[1], wl.atlas_rect[2], wl.atlas_rect[3],
					first_entry[ENTRY_DIRECTION], first_entry[ENTRY_DIRECTION + 1], wl.flip_offset[0], wl.flip_offset[1], matrix_error);
		} else if (shadow_off) {
			s->entry = ENTRY_SHADOW_OFF;
		} else if (p_render.entries_complete) {
			s->entry = ENTRY_MISSING;
			entry_problem = "no omni light entry has this light's position";
		}

		if (p_render.screen) {
			s->screen = true;
			const uint32_t *vis = p_data + SLOT_VIS + l * GRID_POINTS;
			for (uint32_t i = 0; i < GRID_POINTS; i++) {
				s->vis[i] = uint8_t(vis[i]);
			}
		}

		LocalVector<LightSample *> &history = light_history[history_key(p_render.atlas, wl.light)];
		const LightSample *prev = history.is_empty() ? nullptr : history[history.size() - 1];
		history.push_back(s);
		if (history.size() > HISTORY) {
			memdelete(history[0]);
			history.remove_at(0);
		}

		for (int h = 0; h < 2; h++) {
			const bool cube_mismatch = s->checked[h] > 0 && s->mismatch[h] * 50 > s->checked[h];
			const bool record_problem = s->recorded && (s->stale[h] || s->write_diff[h] * 50 > 1024);
			const bool params_problem = s->recorded && !s->stale[h] && !_copy_params_as_given(*s, wl, h);
			if ((cube_mismatch || record_problem || params_problem) && _may_report(REPORT_COPY + uint64_t(h) * 16, wl.light)) {
				String what = cube_mismatch ? vformat("atlas half %d does not match the cubemap it was copied from at %d of %d samples", h, s->mismatch[h], s->checked[h]) : (record_problem ? vformat("atlas half %d is not what its copy wrote", h) : vformat("the copy of atlas half %d ran with other constants than it was given", h));
				print_line(vformat("ShadowWatch: [COPY] %s: %s.\n  => %s%s", _light_name(wl, p_render), what, _copy_verdict(*s, wl, p_render, h), _history(history)));
			}
			// Against the view's previous render only: in a render this watch skipped, the light may have been redrawn.
			if (prev && prev->serial == previous_render && !s->redrawn && prev->half_rect == s->half_rect) {
				const uint64_t difference = total_difference(s->half_sum[h], prev->half_sum[h], 64);
				if (difference > 0 && _may_report(REPORT_OVERWRITTEN + uint64_t(h) * 16, wl.light)) {
					print_line(vformat("ShadowWatch: [ATLAS OVERWRITTEN] %s: atlas half %d changed (%d blocks of 64 moved by more than 1%%) though the light was not redrawn: something else wrote its atlas cell.%s", _light_name(wl, p_render), h, changed_blocks(s->half_sum[h], prev->half_sum[h], 64, 16), _history(history)));
				}
			}
		}
		if (!entry_problem.is_empty() && _may_report(REPORT_ENTRY, wl.light)) {
			print_line(vformat("ShadowWatch: [LIGHT DATA] %s: the omni light data the main pass reads is not this view's: %s.%s", _light_name(wl, p_render), entry_problem, _history(history)));
		}
	}

	if (!p_render.screen) {
		return;
	}

	// The image: points that change every other frame while the camera stays still.
	ScreenSample *screen = memnew(ScreenSample);
	screen->frame = p_render.frame;
	screen->cam_transform = p_render.cam_transform;
	memcpy(screen->lum, p_data + SLOT_LUM, sizeof(screen->lum));
	LocalVector<ScreenSample *> &screens = screen_history[p_render.atlas.get_id()];
	screens.push_back(screen);
	if (screens.size() > HISTORY) {
		memdelete(screens[0]);
		screens.remove_at(0);
	}
	if (screens.size() < 4) {
		return;
	}
	const uint32_t n = screens.size();
	const ScreenSample *s0 = screens[n - 1];
	const ScreenSample *s1 = screens[n - 2];
	const ScreenSample *s2 = screens[n - 3];
	const ScreenSample *s3 = screens[n - 4];
	if (!s0->cam_transform.is_equal_approx(s1->cam_transform) || !s0->cam_transform.is_equal_approx(s2->cam_transform) || !s0->cam_transform.is_equal_approx(s3->cam_transform)) {
		return;
	}
	LocalVector<uint32_t> flicker_points;
	for (uint32_t i = 0; i < GRID_POINTS; i++) {
		const float a = s0->lum[i];
		const float b = s1->lum[i];
		const float c = s2->lum[i];
		const float d = s3->lum[i];
		if (lum_differs(a, b) && lum_differs(b, c) && lum_same(a, c) && lum_same(b, d)) {
			flicker_points.push_back(i);
		}
	}
	if (flicker_points.size() < MAX(6u, GRID_POINTS / 100) || !_may_report(REPORT_FLICKER, p_render.atlas)) {
		return;
	}

	String text = vformat("ShadowWatch: [FLICKER] view %dx%d, frame %d: %d of %d screen points change every other frame while the camera is still.", p_render.size.width, p_render.size.height, p_render.frame, flicker_points.size(), GRID_POINTS);
	bool explained = false;
	for (uint32_t l = 0; l < p_render.lights.size(); l++) {
		const WatchedLight &wl = p_render.lights[l];
		const LocalVector<LightSample *> &history = light_history[history_key(p_render.atlas, wl.light)];
		if (history.size() < 4) {
			continue;
		}
		const uint32_t m = history.size();
		const LightSample *l0 = history[m - 1];
		const LightSample *l1 = history[m - 2];
		const LightSample *l2 = history[m - 3];
		const LightSample *l3 = history[m - 4];
		if (l0->frame != s0->frame || l1->frame != s1->frame || l2->frame != s2->frame || l3->frame != s3->frame || !l0->screen || !l1->screen || !l2->screen || !l3->screen) {
			continue;
		}
		uint32_t in_reach = 0;
		uint32_t test_alternates = 0;
		for (uint32_t point : flicker_points) {
			if (l0->vis[point] == 2 || l1->vis[point] == 2) {
				continue;
			}
			in_reach++;
			if (l0->vis[point] != l1->vis[point] && l0->vis[point] == l2->vis[point] && l1->vis[point] == l3->vis[point]) {
				test_alternates++;
			}
		}
		if (in_reach == 0) {
			continue;
		}
		explained = true;
		text += vformat("\n  %s: %d of the points are in its reach; its shadow test there changes every other frame at %d of them.", _light_name(wl, p_render), in_reach, test_alternates);
		if (test_alternates * 2 >= in_reach) {
			bool half_alt[2];
			for (int h = 0; h < 2; h++) {
				half_alt[h] = alternating_blocks(l0->half_sum[h], l1->half_sum[h], l2->half_sum[h], l3->half_sum[h], 64) > 0;
			}
			int face_alt = -1;
			if (l0->faces && l1->faces && l2->faces && l3->faces) {
				for (int f = 0; f < 6 && face_alt < 0; f++) {
					if (alternating_blocks(l0->face_sum[f], l1->face_sum[f], l2->face_sum[f], l3->face_sum[f], 16) > 0) {
						face_alt = f;
					}
				}
			}
			const bool casters_alternate = memcmp(l0->face_casters, l1->face_casters, sizeof(l0->face_casters)) != 0 && memcmp(l0->face_casters, l2->face_casters, sizeof(l0->face_casters)) == 0 && memcmp(l1->face_casters, l3->face_casters, sizeof(l1->face_casters)) == 0;
			if ((l0->checked[0] && l0->mismatch[0] * 50 > l0->checked[0]) || (l0->checked[1] && l0->mismatch[1] * 50 > l0->checked[1]) || (l1->checked[0] && l1->mismatch[0] * 50 > l1->checked[0]) || (l1->checked[1] && l1->mismatch[1] * 50 > l1->checked[1])) {
				text += "\n  => The shadow map alternates because the copy of the cube into the atlas goes wrong in one frame of two (the atlas half does not match its cubemap).";
			} else if (face_alt >= 0) {
				text += vformat("\n  => The shadow map alternates because the cube pass itself draws different depth every other frame (cubemap face %d), %s.", face_alt, casters_alternate ? "with a different caster list every other frame: the culling of the light's casters alternates, or a caster is there one frame of two" : "with the same caster lists: the GPU draws the same casters differently (or an animated caster switches between two poses)");
			} else if ((half_alt[0] || half_alt[1]) && (!l0->redrawn || !l1->redrawn)) {
				text += vformat("\n  => The atlas half %d alternates in frames where the light is not redrawn: something else writes its atlas cell.", half_alt[0] ? 0 : 1);
			} else if (half_alt[0] || half_alt[1]) {
				text += vformat("\n  => The atlas half %d alternates while %s.", half_alt[0] ? 0 : 1, l0->faces ? "its cubemap stays the same and the copy matches it: the atlas is changed after the copy" : "the cubemap could not be read this time");
			} else {
				text += "\n  => The shadow test alternates at those points, but the atlas halves look steady at the watch's 32x32 grid: the change is finer than the grid, in the atlas cell.";
			}
			if (casters_alternate && face_alt < 0) {
				text += "\n  The caster counts per face alternate between frames (tight and full updates of the light's shadow).";
			}
		} else {
			text += vformat("\n  => Its shadow test stays the same at those points (light data %s): what flickers there is not this light's shadow map content (something else flickers, or the main pass reads the shadow differently).", l0->entry == ENTRY_OK ? "correct" : (l0->entry == ENTRY_SHADOW_OFF ? "has its shadow off" : "NOT correct"));
		}
		text += _history(history);
	}
	if (!explained) {
		text += "\n  None of the points is in reach of a watched omni light in cube shadow mode: not their shadow.";
	}
	print_line(text);
}
