/**************************************************************************/
/*  pt_probes.h                                                           */
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

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/effects/pt_probe_reduce.glsl.gen.h"

namespace RendererRD {

/// Irradiance probes for the instances the path tracer leaves to its raster pass (pathtracing_rasterized: Sprite3D by
/// default). Their ambient light came from the Environment, which nothing blocks, while the traced surfaces around them
/// get the traced light: in the shade or indoors they stood out brighter than the scene. Each such instance gets a probe
/// at its center. The path tracer traces RAYS paths from it every frame, as from a surface after a diffuse bounce (a
/// dispatch of depth 2, into binding 31 of its set); reduce() projects them into the 9 SH coefficients of a LightmapGI
/// capture, blended over frames, and they come back asynchronously to light the instance as a capture does.
class PTProbes {
public:
	static constexpr uint32_t MAX_PROBES = 128; // Mirrored as RT_PROBE_MAX in scene_raytracing_raygen.glsl.
	static constexpr uint32_t RAYS = 64; // RT_PROBE_RAYS.
	static constexpr uint32_t SH_STRIDE = 10; // In vec4: 9 coefficients and a header (pt_probe_reduce.glsl).

	struct Request {
		const void *owner = nullptr; // The instance; *slot is its probe, -1 without.
		int32_t *slot = nullptr;
		Vector3 position;
	};

private:
	static constexpr float BLEND = 0.1; // The share of a frame's rays in the blend: about 10 frames, 640 rays.

	struct Slot {
		const void *owner = nullptr;
		int32_t *owner_slot = nullptr;
		uint32_t generation = 0; // Below 2^24: carried as a float.
		uint64_t last_used = 0;
		Vector3 position;
		bool sh_valid = false;
		Color sh[9];
	};

	struct PushConstant {
		uint32_t max_probes;
		uint32_t ray_count;
		float blend;
		uint32_t pad;
	};

	Slot slots[MAX_PROBES];
	uint32_t next_generation = 0;
	uint32_t dispatch_count = 0; // Slots traced this frame: the highest one in use + 1.

	RID rays_buffer;
	RID sh_buffer;
	PtProbeReduceShaderRD shader;
	RID shader_version;
	RID pipeline;

	static PTProbes *singleton;
	static void _sh_arrived(const Vector<uint8_t> &p_data);

public:
	/// Gives this frame's instances a probe each, the first ones first (sort them by distance), and uploads the probes'
	/// positions. Returns how many slots the path tracer traces (the height of its probe dispatch), 0 for none.
	uint32_t update(const LocalVector<Request> &p_requests, uint64_t p_frame);
	/// The probes' buffer, binding 31 of the path tracer's set.
	RID get_rays_buffer();
	/// After the probe dispatch: the coefficients of the p_count first slots, then their asynchronous read back.
	void reduce(uint32_t p_count);
	/// The coefficients of p_owner's probe, as LightmapGI capture SH (Color, alpha unused), when it has some.
	bool get_sh(int32_t p_slot, const void *p_owner, Color *r_sh) const;
	bool has_sh(int32_t p_slot, const void *p_owner) const {
		return p_slot >= 0 && p_slot < int32_t(MAX_PROBES) && slots[p_slot].owner == p_owner && slots[p_slot].sh_valid;
	}
	/// p_owner is freed.
	void release(int32_t p_slot, const void *p_owner);

	PTProbes();
	~PTProbes();
};

} // namespace RendererRD
