/**************************************************************************/
/*  pt_probes.cpp                                                         */
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

#include "pt_probes.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

PTProbes *PTProbes::singleton = nullptr;

PTProbes::PTProbes() {
	singleton = this;

	Vector<String> shader_modes;
	shader_modes.push_back("");
	shader.initialize(shader_modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

PTProbes::~PTProbes() {
	singleton = nullptr;
	if (rays_buffer.is_valid()) {
		RD::get_singleton()->free_rid(rays_buffer);
	}
	if (sh_buffer.is_valid()) {
		RD::get_singleton()->free_rid(sh_buffer);
	}
	shader.version_free(shader_version);
}

RID PTProbes::get_rays_buffer() {
	if (rays_buffer.is_null()) {
		rays_buffer = RD::get_singleton()->storage_buffer_create((MAX_PROBES + MAX_PROBES * RAYS * 2) * sizeof(float) * 4);
	}
	return rays_buffer;
}

uint32_t PTProbes::update(const LocalVector<Request> &p_requests, uint64_t p_frame) {
	// The instances that have a probe keep it, then the others take a free slot or the one unused the longest, in the
	// order given: without a slot left, the farthest keep the Environment's ambient light.
	for (const Request &r : p_requests) {
		const int32_t s = *r.slot;
		if (s >= 0 && s < int32_t(MAX_PROBES) && slots[s].owner == r.owner) {
			slots[s].last_used = p_frame;
			slots[s].position = r.position;
		} else {
			*r.slot = -1;
		}
	}
	for (const Request &r : p_requests) {
		if (*r.slot >= 0) {
			continue;
		}
		int32_t best = -1;
		uint64_t oldest = UINT64_MAX;
		for (uint32_t i = 0; i < MAX_PROBES; i++) {
			if (!slots[i].owner) {
				best = i;
				break;
			}
			if (slots[i].last_used < p_frame && slots[i].last_used < oldest) {
				oldest = slots[i].last_used;
				best = i;
			}
		}
		if (best < 0) {
			break; // Every slot serves a nearer instance this frame.
		}
		Slot &slot = slots[best];
		if (slot.owner_slot) {
			*slot.owner_slot = -1; // Released by release() when its owner is freed, so the owner is alive.
		}
		slot.owner = r.owner;
		slot.owner_slot = r.slot;
		slot.generation = next_generation;
		next_generation = (next_generation + 1) & 0xFFFFFF;
		slot.last_used = p_frame;
		slot.position = r.position;
		slot.sh_valid = false;
		*r.slot = best;
	}

	dispatch_count = 0;
	for (uint32_t i = 0; i < MAX_PROBES; i++) {
		if (slots[i].owner && slots[i].last_used == p_frame) {
			dispatch_count = i + 1;
		}
	}
	if (dispatch_count == 0) {
		return 0;
	}

	// Position and generation; -1 for the slots below the highest one that are unused this frame.
	LocalVector<float> positions;
	positions.resize(dispatch_count * 4);
	for (uint32_t i = 0; i < dispatch_count; i++) {
		const Slot &slot = slots[i];
		const bool used = slot.owner && slot.last_used == p_frame;
		positions[i * 4 + 0] = slot.position.x;
		positions[i * 4 + 1] = slot.position.y;
		positions[i * 4 + 2] = slot.position.z;
		positions[i * 4 + 3] = used ? float(slot.generation) : -1.0f;
	}
	RD::get_singleton()->buffer_update(get_rays_buffer(), 0, positions.size() * sizeof(float), positions.ptr());
	return dispatch_count;
}

void PTProbes::reduce(uint32_t p_count) {
	if (p_count == 0) {
		return;
	}
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);

	if (sh_buffer.is_null()) {
		// Every header holds a generation no probe has, so that each starts its blend afresh.
		Vector<uint8_t> initial;
		initial.resize(MAX_PROBES * SH_STRIDE * sizeof(float) * 4);
		float *values = reinterpret_cast<float *>(initial.ptrw());
		for (uint32_t i = 0; i < MAX_PROBES * SH_STRIDE * 4; i++) {
			values[i] = 0.0f;
		}
		for (uint32_t i = 0; i < MAX_PROBES; i++) {
			values[(i * SH_STRIDE + 9) * 4] = -2.0f;
		}
		sh_buffer = rd->storage_buffer_create(initial.size(), initial);
	}

	PushConstant push_constant = {};
	push_constant.max_probes = MAX_PROBES;
	push_constant.ray_count = RAYS;
	push_constant.blend = BLEND;

	RD::Uniform u_rays(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, get_rays_buffer());
	RD::Uniform u_sh(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, sh_buffer);
	const RID reduce_shader = shader.version_get_shader(shader_version, 0);

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(reduce_shader, 0, u_rays, u_sh), 0);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	rd->compute_list_dispatch(compute_list, p_count, 1, 1);
	rd->compute_list_end();

	rd->buffer_get_data_async(sh_buffer, callable_mp_static(&PTProbes::_sh_arrived), 0, p_count * SH_STRIDE * sizeof(float) * 4);
}

void PTProbes::_sh_arrived(const Vector<uint8_t> &p_data) {
	if (!singleton) {
		return;
	}
	const float *values = reinterpret_cast<const float *>(p_data.ptr());
	const uint32_t count = MIN(uint32_t(p_data.size()) / (SH_STRIDE * sizeof(float) * 4), MAX_PROBES);
	for (uint32_t i = 0; i < count; i++) {
		Slot &slot = singleton->slots[i];
		const float *probe = values + i * SH_STRIDE * 4;
		// Coefficients of a probe since given to another instance are dropped.
		if (!slot.owner || probe[9 * 4] != float(slot.generation)) {
			continue;
		}
		for (uint32_t j = 0; j < 9; j++) {
			slot.sh[j] = Color(probe[j * 4 + 0], probe[j * 4 + 1], probe[j * 4 + 2]);
		}
		slot.sh_valid = true;
	}
}

bool PTProbes::get_sh(int32_t p_slot, const void *p_owner, Color *r_sh) const {
	if (p_slot < 0 || p_slot >= int32_t(MAX_PROBES)) {
		return false;
	}
	const Slot &slot = slots[p_slot];
	if (slot.owner != p_owner || !slot.sh_valid) {
		return false;
	}
	for (uint32_t j = 0; j < 9; j++) {
		r_sh[j] = slot.sh[j];
	}
	return true;
}

void PTProbes::release(int32_t p_slot, const void *p_owner) {
	if (p_slot < 0 || p_slot >= int32_t(MAX_PROBES) || slots[p_slot].owner != p_owner) {
		return;
	}
	Slot &slot = slots[p_slot];
	slot.owner = nullptr;
	slot.owner_slot = nullptr;
	slot.sh_valid = false;
}
