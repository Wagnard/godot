/**************************************************************************/
/*  nrd_denoiser.h                                                        */
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
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"

class RenderSceneBuffersRD;

namespace RendererRD {

// The path tracer's denoiser on GPUs without DLSS Ray Reconstruction (PT_DENOISER_NRD_REBLUR): ReBLUR from NVIDIA's
// Real-time Denoisers (NRD), a library that only describes its compute passes. They run here, through RenderingDevice,
// from the SPIR-V NRD embeds: NRD's "native implementation" integration (NRD README, "How to integrate"), as NVIDIA's RTX
// Remix runtime does, on Vulkan and on D3D12 (SPIR-V to DXIL through Mesa, as every other shader).
//
// NRD is under the NVIDIA RTX SDKs licence: it is not in the repository. misc/scripts/install_nrd.py builds the unmodified
// library and this effect's front and back end shaders (nrd_frontend.hlsl, nrd_backend.hlsl) next to the other build
// dependencies; without it the engine builds with NRD_DENOISER_ENABLED undefined and NRDDenoiser::is_available() false.
class NRDDenoiser {
public:
	struct Parameters {
		// In the raygen's layout (scene_raytracing_raygen.glsl, NRD_ENABLED): per lobe, the light over the lobe's albedo and
		// the hit distance of the bounce that took it. Denoised in place.
		RID diffuse;
		RID specular;
		RID normal_roughness; // World space normal, linear roughness.
		RID view_z; // R32F view depth, past the denoising range where the camera ray missed.
		RID motion_vectors; // RG16F, previous minus current UV, the raygen's velocity.
		Size2i size;

		// Unjittered; the jitter in pixels.
		Projection projection;
		Projection prev_projection;
		Transform3D cam_transform;
		Transform3D prev_cam_transform;
		Vector2 jitter;
		Vector2 prev_jitter;
	};

	static bool is_available();

	// The binaries of NRD's programs are kept under the shader cache (CACHE_FOLDER), keyed by their SPIR-V: on D3D12 each one
	// would otherwise be translated to DXIL again at every launch. The shader baker (ShaderBakerExportPlugin) writes the same
	// files for an exported project. They are NRD's object code, inside the application: allowed by its licence.
	static constexpr const char *CACHE_FOLDER = "NRDDenoiser";
	struct BakeProgram {
		String name;
		String cache_key;
		Vector<uint8_t> spirv; // A compute shader.
	};
	// Every program a denoise() may build, none without NRD.
	static void get_bake_programs(LocalVector<BakeProgram> &r_programs);
	static String get_cache_file_name(const String &p_key, const String &p_api);
	static Vector<uint8_t> make_cache_file(const Vector<uint8_t> &p_binary);

	// Denoises p_params.diffuse and specular, keeping its history in p_render_buffers.
	void denoise(RenderSceneBuffersRD *p_render_buffers, const Parameters &p_params);

	NRDDenoiser();
	~NRDDenoiser();

private:
	// Only nrd_denoiser.cpp sees NRD's headers (NRD_DENOISER_ENABLED): the class looks the same to every includer.
	struct State;
	State *state = nullptr;
};

} // namespace RendererRD
