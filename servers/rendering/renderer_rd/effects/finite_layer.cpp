/**************************************************************************/
/*  finite_layer.cpp                                                      */
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

#include "finite_layer.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_default.h"

using namespace RendererRD;

Mutex FiniteLayer::report_mutex;
FiniteLayer::Report FiniteLayer::pending_report;
bool FiniteLayer::has_pending_report = false;

FiniteLayer::FiniteLayer() {
	// The warning is a diagnostic: off, the copy keeps clearing the invalid texels but counts nothing and reads nothing back.
	report = OS::get_singleton()->get_environment("GODOT_RR_LAYER_NAN_WARNING") == "1";

	Vector<String> shader_modes;
	shader_modes.push_back("");
	shader_modes.push_back("\n#define REPORT_NON_FINITE\n");
	shader.initialize(shader_modes);
	shader.set_variant_enabled(report ? 0 : 1, false);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, report ? 1 : 0));
}

FiniteLayer::~FiniteLayer() {
	if (stats_buffer.is_valid()) {
		RD::get_singleton()->free_rid(stats_buffer);
	}
	shader.version_free(shader_version);
}

void FiniteLayer::_reset_stats() {
	const uint32_t empty[5] = { 0, UINT32_MAX, UINT32_MAX, 0, 0 };
	RD::get_singleton()->buffer_update(stats_buffer, 0, sizeof(empty), empty);
}

void FiniteLayer::_stats_arrived(const Vector<uint8_t> &p_data, uint64_t p_frame) {
	ERR_FAIL_COND(p_data.size() < int(5 * sizeof(uint32_t)));
	const uint32_t *stats = reinterpret_cast<const uint32_t *>(p_data.ptr());
	if (stats[0] == 0) {
		return;
	}
	MutexLock lock(report_mutex);
	pending_report.count = stats[0];
	pending_report.rect = Rect2i(stats[1], stats[2], stats[3] - stats[1] + 1, stats[4] - stats[2] + 1);
	pending_report.frame = p_frame;
	has_pending_report = true;
}

bool FiniteLayer::take_report(Report &r_report) {
	MutexLock lock(report_mutex);
	if (!has_pending_report) {
		return false;
	}
	r_report = pending_report;
	has_pending_report = false;
	return true;
}

void FiniteLayer::process(RID p_source, RID p_dest, const Size2i &p_size) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	if (report && stats_buffer.is_null()) {
		stats_buffer = rd->storage_buffer_create(5 * sizeof(uint32_t));
		_reset_stats();
		window_frame = 0;
	}

	PushConstant push_constant = {};
	push_constant.size[0] = p_size.x;
	push_constant.size[1] = p_size.y;

	const RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_source }));
	RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 1, p_dest);
	const RID layer_shader = shader.version_get_shader(shader_version, report ? 1 : 0);
	RID uniform_set;
	if (report) {
		RD::Uniform u_stats(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, stats_buffer);
		uniform_set = uniform_set_cache->get_cache(layer_shader, 0, u_source, u_dest, u_stats);
	} else {
		uniform_set = uniform_set_cache->get_cache(layer_shader, 0, u_source, u_dest);
	}

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	rd->compute_list_dispatch_threads(compute_list, p_size.x, p_size.y, 1);
	rd->compute_list_end();

	if (!report) {
		return;
	}

	// The statistics add up over the window, so that no burst between two reads is missed; then 20 bytes come back
	// asynchronously and the counters start again (the render graph orders the reset after the read).
	window_frame++;
	if (window_frame >= WINDOW_FRAMES) {
		window_frame = 0;
		rd->buffer_get_data_async(stats_buffer, callable_mp_static(&FiniteLayer::_stats_arrived).bind(uint64_t(RSG::rasterizer->get_frame_number())), 0, 5 * sizeof(uint32_t));
		_reset_stats();
	}
}
