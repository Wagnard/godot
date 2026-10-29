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

	singleton = this;
	heartbeat_usec = OS::get_singleton()->get_ticks_usec();
	print_line("ShadowWatch: watching the omni lights in cube shadow mode (Wagnard/godot#1), as GODOT_SHADOW_WATCH=1 asks.");
}

ShadowWatch::~ShadowWatch() {
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
}

void ShadowWatch::cube_copied(RID p_light, RID p_cubemap, float p_z_near, float p_z_far) {
	CubeCopy copy;
	copy.light = p_light;
	copy.cubemap = p_cubemap;
	copy.z_near = p_z_near;
	copy.z_far = p_z_far;
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
	struct Candidate {
		RID light;
		float distance;
		bool operator<(const Candidate &p_other) const { return distance < p_other.distance; }
	};
	LocalVector<Candidate> candidates;
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

	current_serial = ++render_serial;
	WatchedRender &render = pending.insert(current_serial, WatchedRender())->value;
	current = &render;
	render.frame = RSG::rasterizer->get_frame_number();
	render.render_in_frame = render_in_frame;
	render.atlas = atlas;
	render.cam_transform = cam_transform;
	render.slot = next_slot;
	next_slot = (next_slot + 1) % SLOT_COUNT;
	render.entry_count = MIN(omni_count, MAX_ENTRIES);
	render.entries_complete = omni_count <= MAX_ENTRIES;
	if (p_render_data->render_buffers.is_valid()) {
		render.size = p_render_data->render_buffers->get_internal_size();
	}

	const int atlas_size = light_storage->shadow_atlas_get_size(atlas);
	for (const Candidate &candidate : candidates) {
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
	RID copy_cubemap[MAX_LIGHTS];
	float copy_z_near[MAX_LIGHTS] = {};
	float copy_z_far[MAX_LIGHTS] = {};
	for (uint32_t l = 0; l < render.lights.size(); l++) {
		for (int64_t i = int64_t(copies.size()) - 1; i >= 0; i--) {
			if (copies[i].light != render.lights[l].light) {
				continue;
			}
			bool reused = false;
			for (uint32_t j = i + 1; j < copies.size(); j++) {
				reused = reused || copies[j].cubemap == copies[i].cubemap;
			}
			if (!reused) {
				render.lights[l].copy_check = true;
				copy_cubemap[l] = copies[i].cubemap;
				copy_z_near[l] = copies[i].z_near;
				copy_z_far[l] = copies[i].z_far;
			}
			break;
		}
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
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_atlas, u_cube, u_lights, u_results), 0);

		LightPushConstant push_constant = {};
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

		// What the main pass reads for this light: found by its view-space position.
		const float *entry = nullptr;
		for (uint32_t e = 0; e < p_render.entry_count && entry == nullptr; e++) {
			const float *candidate = entries + e * ENTRY_FLOATS;
			const Vector3 position(candidate[ENTRY_POSITION], candidate[ENTRY_POSITION + 1], candidate[ENTRY_POSITION + 2]);
			const Vector3 expected(wl.position[0], wl.position[1], wl.position[2]);
			if (position.distance_to(expected) <= 0.001f * MAX(1.0f, expected.length())) {
				entry = candidate;
			}
		}
		String entry_problem;
		if (entry == nullptr) {
			if (p_render.entries_complete) {
				s->entry = ENTRY_MISSING;
				entry_problem = "no omni light entry has this light's position";
			}
		} else if (entry[ENTRY_SHADOW_OPACITY] <= 0.001f) {
			s->entry = ENTRY_SHADOW_OFF;
		} else {
			float rect_error = 0.0f;
			for (int i = 0; i < 4; i++) {
				rect_error = MAX(rect_error, Math::abs(entry[ENTRY_ATLAS_RECT + i] - wl.atlas_rect[i]));
			}
			const float flip_error = MAX(Math::abs(entry[ENTRY_DIRECTION] - wl.flip_offset[0]), Math::abs(entry[ENTRY_DIRECTION + 1] - wl.flip_offset[1]));
			float matrix_error = 0.0f;
			for (int i = 0; i < 16; i++) {
				matrix_error = MAX(matrix_error, Math::abs(entry[ENTRY_SHADOW_MATRIX + i] - wl.shadow_matrix[i]) / MAX(1.0f, Math::abs(wl.shadow_matrix[i])));
			}
			if (rect_error > 1e-6f || flip_error > 1e-6f || matrix_error > 1e-3f) {
				s->entry = ENTRY_WRONG;
				entry_problem = vformat("atlas rect read (%.5f, %.5f, %.5f, %.5f) expected (%.5f, %.5f, %.5f, %.5f), half offset read (%.5f, %.5f) expected (%.5f, %.5f), shadow matrix error %.5f",
						entry[ENTRY_ATLAS_RECT], entry[ENTRY_ATLAS_RECT + 1], entry[ENTRY_ATLAS_RECT + 2], entry[ENTRY_ATLAS_RECT + 3], wl.atlas_rect[0], wl.atlas_rect[1], wl.atlas_rect[2], wl.atlas_rect[3],
						entry[ENTRY_DIRECTION], entry[ENTRY_DIRECTION + 1], wl.flip_offset[0], wl.flip_offset[1], matrix_error);
			} else {
				s->entry = ENTRY_OK;
			}
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
			if (s->checked[h] > 0 && s->mismatch[h] * 50 > s->checked[h] && _may_report(REPORT_COPY + uint64_t(h) * 16, wl.light)) {
				print_line(vformat("ShadowWatch: [COPY] %s: atlas half %d does not match the cubemap it was copied from at %d of %d samples: the copy of the cube into the atlas went wrong.%s", _light_name(wl, p_render), h, s->mismatch[h], s->checked[h], _history(history)));
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
				text += vformat("\n  => The shadow map alternates because the cube pass itself draws different depth every other frame (cubemap face %d), %s.", face_alt, casters_alternate ? "with a different caster list every other frame: the culling of the light's casters alternates, or a caster is there one frame of two" : "with the same caster lists: the GPU draws the same casters differently");
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
			text += vformat("\n  => Its shadow map and its shadow test stay the same there (light data %s): the main pass reads them differently from what is in memory, or the flicker is not this light's shadow.", l0->entry == ENTRY_OK ? "correct" : (l0->entry == ENTRY_SHADOW_OFF ? "has its shadow off" : "NOT correct"));
		}
		text += _history(history);
	}
	if (!explained) {
		text += "\n  None of the points is in reach of a watched omni light in cube shadow mode: not their shadow.";
	}
	print_line(text);
}
