/**************************************************************************/
/*  shadow_watch.h                                                        */
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
#include "servers/rendering/renderer_rd/shaders/effects/shadow_watch.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"

namespace RendererRD {

// Wagnard/godot#1 diagnostic. For the omni lights in cube shadow mode of each view, it reads back, a few frames later
// and without waiting for the GPU: the atlas halves and the cubemap faces the light's shadow went through, whether the
// halves match the cubemap they were copied from, the omni light data the main pass reads, and the image and the
// light's shadow test at a grid of screen points. From these, it names the stage that changes every other frame when
// the image flickers. It costs a few small dispatches and a 26 KB read per view. Off unless GODOT_SHADOW_WATCH=1.
class ShadowWatch {
public:
	static constexpr uint32_t MAX_LIGHTS = 4;
	static constexpr uint32_t MAX_ENTRIES = 16;
	static constexpr uint32_t ENTRY_FLOATS = 56; // sizeof(LightStorage::LightData) / 4.
	static constexpr uint32_t GRID_X = 40;
	static constexpr uint32_t GRID_Y = 24;
	static constexpr uint32_t GRID_POINTS = GRID_X * GRID_Y;
	static constexpr uint32_t HISTORY = 6;

private:
	// Results buffer slot, in uints.
	static constexpr uint32_t LIGHT_RECORD = 256;
	static constexpr uint32_t HALF_STRIDE = 68;
	static constexpr uint32_t FACES_BASE = 136;
	static constexpr uint32_t FACE_STRIDE = 18;
	static constexpr uint32_t SLOT_ENTRIES = MAX_LIGHTS * LIGHT_RECORD;
	static constexpr uint32_t SLOT_LUM = SLOT_ENTRIES + MAX_ENTRIES * ENTRY_FLOATS;
	static constexpr uint32_t SLOT_VIS = SLOT_LUM + GRID_POINTS;
	static constexpr uint32_t SLOT_UINTS = SLOT_VIS + MAX_LIGHTS * GRID_POINTS;
	static constexpr uint32_t SLOT_COUNT = 16;

	enum {
		MODE_LIGHT,
		MODE_SCREEN,
		MODE_MAX,
	};

	struct LightPushConstant {
		int32_t half_rect[2][4];
		float z_near;
		float z_far;
		uint32_t flags;
		uint32_t out_offset;
		uint32_t entries_offset;
		uint32_t entry_floats;
		uint32_t pad[2];
	};

	struct ScreenPushConstant {
		float ndc_to_light[16];
		float uv_rect[4];
		float flip_offset[2];
		float inv_radius;
		float shadow_bias;
		uint32_t grid[2];
		uint32_t out_lum;
		uint32_t out_vis;
	};

	struct CubeCopy {
		RID light;
		RID cubemap;
		float z_near = 0.0;
		float z_far = 0.0;
	};

	struct WatchedLight {
		RID light;
		Vector3 world_position;
		Rect2i half_rect[2];
		bool redrawn = false;
		bool copy_check = false;
		uint32_t face_casters[6] = {};
		// The omni light data the main pass should read for it.
		float atlas_rect[4] = {};
		float flip_offset[2] = {};
		float position[3] = {};
		float shadow_matrix[16] = {};
	};

	struct WatchedRender {
		uint64_t frame = 0;
		uint32_t render_in_frame = 0;
		RID atlas;
		Size2i size;
		Transform3D cam_transform;
		uint32_t slot = 0;
		uint32_t entry_count = 0;
		bool entries_complete = false; // All the omni lights of the render are among the entries read.
		bool screen = false;
		LocalVector<WatchedLight> lights;
	};

	enum EntryState {
		ENTRY_UNCHECKED,
		ENTRY_OK,
		ENTRY_SHADOW_OFF,
		ENTRY_MISSING,
		ENTRY_WRONG,
	};

	struct LightSample {
		uint64_t serial = 0;
		uint64_t frame = 0;
		bool redrawn = false;
		bool faces = false;
		EntryState entry = ENTRY_UNCHECKED;
		Rect2i half_rect;
		uint32_t half_sum[2][64] = {};
		uint32_t face_sum[6][16] = {};
		uint32_t mismatch[2] = {};
		uint32_t checked[2] = {};
		uint32_t face_casters[6] = {};
		bool screen = false;
		uint8_t vis[GRID_POINTS] = {};
	};

	struct ScreenSample {
		uint64_t frame = 0;
		Transform3D cam_transform;
		float lum[GRID_POINTS] = {};
	};

	ShadowWatchShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	RID results;

	uint64_t render_serial = 0;
	uint64_t last_frame = UINT64_MAX;
	uint32_t render_in_frame = 0;
	uint32_t next_slot = 0;
	LocalVector<CubeCopy> copies; // The cube copies of the render being drawn, in order.
	HashMap<uint64_t, WatchedRender> pending; // By render serial, until the results arrive.
	WatchedRender *current = nullptr;
	uint64_t current_serial = 0;

	HashMap<uint64_t, LocalVector<LightSample *>> light_history; // By atlas and light.
	HashMap<uint64_t, LocalVector<ScreenSample *>> screen_history; // By atlas.
	HashMap<uint64_t, uint64_t> last_serial; // By atlas: the previous watched render of the view.

	uint64_t checked_renders = 0;
	uint64_t anomalies = 0;
	uint64_t heartbeat_usec = 0;
	HashMap<uint64_t, uint64_t> last_report_usec; // By report kind and light.

	static ShadowWatch *singleton;

	static void _results_arrived_static(const Vector<uint8_t> &p_data, uint64_t p_serial);
	void _results_arrived(const Vector<uint8_t> &p_data, uint64_t p_serial);
	void _analyze(const WatchedRender &p_render, uint64_t p_serial, const uint32_t *p_data);
	bool _may_report(uint64_t p_kind, RID p_light);
	String _light_name(const WatchedLight &p_light, const WatchedRender &p_render) const;
	String _history(const LocalVector<LightSample *> &p_history) const;
	void _read_back();

public:
	static bool is_enabled();

	ShadowWatch();
	~ShadowWatch();

	// At the start of each scene render.
	void begin_render();
	// At each copy of a cube shadow into the atlas.
	void cube_copied(RID p_light, RID p_cubemap, float p_z_near, float p_z_far);
	// After the shadows and the light data of the render, right before its opaque pass.
	void watch_lights(const RenderDataRD *p_render_data);
	// After the opaque and transparent passes, before upscaling: the image and the shadow test at the screen points.
	void watch_screen(const RenderDataRD *p_render_data, RID p_depth, RID p_color);
};

} // namespace RendererRD
