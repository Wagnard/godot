/**************************************************************************/
/*  pt_rasterized_guides.h                                                */
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

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/renderer_rd/shaders/effects/pt_rasterized_guides.glsl.gen.h"

namespace RendererRD {

/// DLSS-RR's guides where the path tracer's raster pass drew a pathtracing_rasterized card (Sprite3D by default). The path
/// tracer writes the guides of what is behind a card, since its camera rays leave the cards out: RR then took the card
/// for that background's lighting, and blurred it. The cards' material pass (the one SDFGI uses: albedo, view normal,
/// roughness, metallic) gives what the raygen would write for a surface, and the depth buffer their depth.
class PTRasterizedGuides {
	struct PushConstant {
		float inv_projection[16];
		float inv_view_rows[12];
		int32_t size[2];
		int32_t pad[2];
	};

	PtRasterizedGuidesShaderRD shader;
	RID shader_version;
	RID pipeline;

public:
	struct Guides {
		RID diffuse_albedo;
		RID specular_albedo;
		RID normal_roughness;
		RID specular_hit_dist;
		RID depth;
	};

	/// Writes p_guides where the material pass drew a card (p_albedo's alpha) whose view depth (p_view_depth) is the scene
	/// depth buffer's (p_depth): the depth pass cut the cards out and kept what hides them.
	void process(RID p_albedo, RID p_normal, RID p_orm, RID p_view_depth, RID p_depth, const Guides &p_guides, const Size2i &p_size, const Projection &p_projection, const Transform3D &p_cam_transform);

	PTRasterizedGuides();
	~PTRasterizedGuides();
};

} // namespace RendererRD
