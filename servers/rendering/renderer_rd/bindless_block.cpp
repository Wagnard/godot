/**************************************************************************/
/*  bindless_block.cpp                                                    */
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

#include "bindless_block.h"

void BindlessBlock::initialize(RenderingDevice *p_rd, RID p_default_texture) {
	ERR_FAIL_NULL(p_rd);
	ERR_FAIL_COND(!p_default_texture.is_valid());

	rd = p_rd;

	default_texture = p_default_texture;
	texture_type = rd->texture_get_format(default_texture).texture_type;
	textures.push_back(default_texture);
	texture_to_index[default_texture] = 0;
}

void BindlessBlock::begin_frame() {
	if (!is_initialized()) {
		return;
	}

	bool set_valid = uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set);
	if (set_valid) {
		return;
	}

	// Uniform set was externally invalidated (a texture was destroyed/recreated).
	uniform_set = RID();
	_scrub_dead_textures();
	textures_version++;
	needs_refinalize = true;
}

// Replaces the textures destroyed since they were added with the default texture and frees their slots. Returns whether
// any was. begin_frame() only scrubs when the uniform set was invalidated, which misses a texture added after the set was
// created and destroyed before the next one (textures replaced while a map loads): uniform_set_create() then failed on
// it ("is not a valid texture"), and the tracer went a frame without its textures.
bool BindlessBlock::_scrub_dead_textures() {
	bool scrubbed = false;
	for (uint32_t i = 1; i < textures.size(); i++) {
		if (textures[i] == default_texture || rd->texture_is_valid(textures[i])) {
			continue;
		}
		texture_to_index.erase(textures[i]);
		textures[i] = default_texture;
		if (free_indices_count < free_indices.size()) {
			free_indices[free_indices_count] = i;
		} else {
			free_indices.push_back(i);
		}
		++free_indices_count;
		scrubbed = true;
	}
	return scrubbed;
}

uint32_t BindlessBlock::add_texture(RID p_texture) {
	ERR_FAIL_COND_V_MSG(!is_initialized(), 0, "BindlessBlock not initialized. Call initialize() first.");

	// Substitute null/stale RIDs, and textures of another type, with the default texture.
	if (!p_texture.is_valid() || !rd->texture_is_valid(p_texture) || rd->texture_get_format(p_texture).texture_type != texture_type) {
		p_texture = default_texture;
	}

	HashMap<RID, uint32_t>::Iterator it = texture_to_index.find(p_texture);
	if (it != texture_to_index.end()) {
		return it->value;
	}

	if (textures.size() >= MAX_BINDLESS_TEXTURES && free_indices_count == 0) {
		ERR_PRINT_ONCE("BindlessBlock: Maximum texture count exceeded. Using default texture.");
		return 0;
	}

	if (is_finalized()) {
		needs_refinalize = true;
	}
	textures_version++;

	uint32_t index;
	if (free_indices_count > 0) {
		--free_indices_count;
		index = free_indices[free_indices_count];
		textures[index] = p_texture;
	} else {
		index = textures.size();
		textures.push_back(p_texture);
	}

	texture_to_index[p_texture] = index;
	return index;
}

void BindlessBlock::finalize(RID p_shader, uint32_t p_set_index) {
	ERR_FAIL_COND_MSG(!is_initialized(), "BindlessBlock not initialized.");
	ERR_FAIL_COND_MSG(textures.is_empty(), "BindlessBlock has no textures.");

	if (is_finalized() && !needs_refinalize && rd->uniform_set_is_valid(uniform_set)) {
		return;
	}

	if (uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set)) {
		rd->free_rid(uniform_set);
	}
	uniform_set = _create_uniform_set(p_shader, p_set_index);
	ERR_FAIL_COND_MSG(!uniform_set.is_valid(), "Failed to create bindless uniform set.");

	needs_refinalize = false;
}

RID BindlessBlock::get_secondary_uniform_set(RID p_shader, uint32_t p_set_index, SecondaryLayout p_layout) {
	ERR_FAIL_COND_V_MSG(!is_initialized(), RID(), "BindlessBlock not initialized.");
	ERR_FAIL_INDEX_V(p_layout, SECONDARY_LAYOUT_MAX, RID());

	RID &set = secondary_uniform_sets[p_layout];
	if (set.is_valid() && rd->uniform_set_is_valid(set) && secondary_textures_versions[p_layout] == textures_version) {
		return set;
	}
	if (set.is_valid() && rd->uniform_set_is_valid(set)) {
		rd->free_rid(set);
	}
	set = _create_uniform_set(p_shader, p_set_index);
	secondary_textures_versions[p_layout] = textures_version;
	return set;
}

RID BindlessBlock::_create_uniform_set(RID p_shader, uint32_t p_set_index) {
	ERR_FAIL_COND_V_MSG(!is_initialized(), RID(), "BindlessBlock not initialized.");

	if (_scrub_dead_textures()) {
		textures_version++;
	}

	RD::Uniform u;
	u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
	u.binding = 0;
	u.variable_count = true;
	for (uint32_t i = 0; i < textures.size(); i++) {
		u.append_id(textures[i]);
	}

	RID set = rd->uniform_set_create(Vector<RD::Uniform>{ u }, p_shader, p_set_index);
	if (set.is_valid()) {
		rd->set_resource_name(set, "Bindless Texture Set");
	}
	return set;
}

void BindlessBlock::clear() {
	if (uniform_set.is_valid() && rd && rd->uniform_set_is_valid(uniform_set)) {
		rd->free_rid(uniform_set);
	}
	uniform_set = RID();
	for (RID &set : secondary_uniform_sets) {
		if (set.is_valid() && rd && rd->uniform_set_is_valid(set)) {
			rd->free_rid(set);
		}
		set = RID();
	}
	textures.clear();
	texture_to_index.clear();
	free_indices.clear();
	free_indices_count = 0;
	rd = nullptr;
}

BindlessBlock::~BindlessBlock() {
	clear();
}
