/**************************************************************************/
/*  bindless_block.h                                                      */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_device.h"

class BindlessBlock {
public:
	constexpr static uint32_t MAX_BINDLESS_TEXTURES = 128000;

	uint32_t add_texture(RID p_texture);

	// p_default_texture fills unused slots and sets the type of the table: a texture of another type is replaced by it,
	// as one wrong descriptor would invalidate the whole set.
	void initialize(RenderingDevice *p_rd, RID p_default_texture);

	void begin_frame();

	void finalize(RID p_shader, uint32_t p_set_index);

	// For consumers whose set layout differs from the finalized one (e.g. other shader stages); the caller owns the set.
	// Recreate when get_version() changes. Not const: the textures destroyed since they were added are scrubbed first.
	RID create_uniform_set(RID p_shader, uint32_t p_set_index) { return _create_uniform_set(p_shader, p_set_index); }
	uint64_t get_version() const { return textures_version; }

	RID get_uniform_set() const { return uniform_set; }

	// The other set layouts of the table, for shaders whose stages differ from the finalized one's: every caller of a
	// layout passes a shader of that layout.
	enum SecondaryLayout {
		SECONDARY_LAYOUT_COMPUTE, // The displacement passes.
		SECONDARY_LAYOUT_FRAGMENT, // The path tracer's transparent pass (USE_RT_SHADOWS).
		SECONDARY_LAYOUT_MAX,
	};

	// The same table in a second set, for shaders of another layout.
	RID get_secondary_uniform_set(RID p_shader, uint32_t p_set_index, SecondaryLayout p_layout = SECONDARY_LAYOUT_COMPUTE);
	uint32_t get_texture_count() const { return textures.size(); }
	bool is_initialized() const { return rd != nullptr; }
	bool is_finalized() const { return uniform_set.is_valid(); }

	void clear();
	~BindlessBlock();

private:
	RenderingDevice *rd = nullptr;
	LocalVector<RID> textures;
	HashMap<RID, uint32_t> texture_to_index;
	LocalVector<uint32_t> free_indices;
	uint32_t free_indices_count = 0; // logical top to avoid resize of free_indices array
	RID default_texture;
	RD::TextureType texture_type = RD::TEXTURE_TYPE_2D;
	RID uniform_set;
	bool needs_refinalize = false;

	uint64_t textures_version = 0; // Bumped whenever the table changes.
	RID secondary_uniform_sets[SECONDARY_LAYOUT_MAX];
	uint64_t secondary_textures_versions[SECONDARY_LAYOUT_MAX] = {};

	RID _create_uniform_set(RID p_shader, uint32_t p_set_index);
	bool _scrub_dead_textures();
};
