/**************************************************************************/
/*  pt_rasterized_guides.cpp                                              */
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

#include "pt_rasterized_guides.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

PTRasterizedGuides::PTRasterizedGuides() {
	Vector<String> shader_modes;
	shader_modes.push_back("");
	shader.initialize(shader_modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

PTRasterizedGuides::~PTRasterizedGuides() {
	shader.version_free(shader_version);
}

void PTRasterizedGuides::process(RID p_albedo, RID p_normal, RID p_orm, RID p_view_depth, RID p_depth, const Guides &p_guides, const Size2i &p_size, const Projection &p_projection, const Transform3D &p_cam_transform) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	PushConstant push_constant = {};
	MaterialStorage::store_camera(p_projection.inverse(), push_constant.inv_projection);
	for (int i = 0; i < 3; i++) {
		push_constant.inv_view_rows[i * 4 + 0] = p_cam_transform.basis.rows[i][0];
		push_constant.inv_view_rows[i * 4 + 1] = p_cam_transform.basis.rows[i][1];
		push_constant.inv_view_rows[i * 4 + 2] = p_cam_transform.basis.rows[i][2];
		push_constant.inv_view_rows[i * 4 + 3] = 0.0;
	}
	push_constant.size[0] = p_size.x;
	push_constant.size[1] = p_size.y;

	const RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_albedo(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_albedo }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_normal }));
	RD::Uniform u_orm(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_orm }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_depth }));
	RD::Uniform u_view_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ nearest_sampler, p_view_depth }));
	RD::Uniform u_diffuse(RD::UNIFORM_TYPE_IMAGE, 4, p_guides.diffuse_albedo);
	RD::Uniform u_specular(RD::UNIFORM_TYPE_IMAGE, 5, p_guides.specular_albedo);
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_IMAGE, 6, p_guides.normal_roughness);
	RD::Uniform u_hit_dist(RD::UNIFORM_TYPE_IMAGE, 7, p_guides.specular_hit_dist);
	RD::Uniform u_rr_depth(RD::UNIFORM_TYPE_IMAGE, 8, p_guides.depth);
	const RID guides_shader = shader.version_get_shader(shader_version, 0);

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(guides_shader, 0, u_albedo, u_normal, u_orm, u_depth, u_diffuse, u_specular, u_normal_roughness, u_hit_dist, u_rr_depth, u_view_depth), 0);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	rd->compute_list_dispatch_threads(compute_list, p_size.x, p_size.y, 1);
	rd->compute_list_end();
}
