/**************************************************************************/
/*  finite_layer.h                                                        */
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

#include "core/math/rect2i.h"
#include "core/os/mutex.h"
#include "servers/rendering/renderer_rd/shaders/effects/finite_layer.glsl.gen.h"

namespace RendererRD {

/// Copies a layer of premultiplied contributions (the path tracer's DLSS-RR transparency layer) with its NaN and Inf
/// texels cleared to 0: RR composites that layer and keeps the result in its history, where a single invalid texel
/// spread into a black area. With GODOT_RR_LAYER_NAN_WARNING=1 it also counts them: the count and the rectangle they
/// cover accumulate on the GPU over a window of frames and are read back asynchronously (20 bytes), so that the renderer
/// can warn and name the effects drawn there.
class FiniteLayer {
public:
	struct Report {
		uint32_t count = 0;
		Rect2i rect; // In the layer's pixels.
		uint64_t frame = 0;
	};

private:
	static constexpr uint32_t WINDOW_FRAMES = 8;

	struct PushConstant {
		int32_t size[2];
		int32_t pad[2];
	};

	FiniteLayerShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID stats_buffer;
	uint32_t window_frame = 0;
	bool report = false; // GODOT_RR_LAYER_NAN_WARNING=1.

	static Mutex report_mutex;
	static Report pending_report;
	static bool has_pending_report;

	static void _stats_arrived(const Vector<uint8_t> &p_data, uint64_t p_frame);
	void _reset_stats();

public:
	/// Copies p_source into p_dest (RGBA16F, storage) over p_size, clearing non-finite texels.
	void process(RID p_source, RID p_dest, const Size2i &p_size);
	/// The last window's report when non-finite texels were found, once (only with GODOT_RR_LAYER_NAN_WARNING=1).
	static bool take_report(Report &r_report);

	FiniteLayer();
	~FiniteLayer();
};

} // namespace RendererRD
