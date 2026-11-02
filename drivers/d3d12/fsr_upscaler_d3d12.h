/**************************************************************************/
/*  fsr_upscaler_d3d12.h                                                  */
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
/* "Software"), to deal in the Software without restriction, including   */
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

#include <cstdint>

// AMD FSR upscaling through the FidelityFX API (D3D12): amd_fidelityfx_loader_dx12.dll and
// amd_fidelityfx_upscaler_dx12.dll, loaded at run time from the executable's folder. The upscaler DLL picks the newest
// provider the GPU supports (FSR 3.1 on any GPU with typed UAV loads, FSR 4 on RDNA 4), as AMD's documentation asks.
// The interface carries no D3D12 types so the renderer can call it. AMD's contexts are not thread safe: a mutex guards
// every call.
class FSRUpscalerD3D12 {
public:
	struct ContextParams {
		void *device = nullptr; // ID3D12Device*.
		uint32_t max_render_width = 0;
		uint32_t max_render_height = 0;
		uint32_t upscale_width = 0;
		uint32_t upscale_height = 0;
		bool high_dynamic_range = true;
		bool depth_inverted = true;
		// AMD's validation of the dispatch inputs, reported to the log (FFX_UPSCALE_ENABLE_DEBUG_CHECKING).
		bool debug_checking = false;
	};

	// Every resource is an ID3D12Resource* in D3D12_RESOURCE_STATE_COMMON (the render graph's GENERAL usage); AMD
	// transitions them and puts them back.
	struct DispatchParams {
		void *native_command_list = nullptr; // ID3D12GraphicsCommandList*.
		uint64_t color_resource = 0;
		uint64_t depth_resource = 0;
		uint64_t motion_vectors_resource = 0; // Decoded: no (-1,-1) sentinel.
		uint64_t reactive_resource = 0; // R8_UNORM, or 0.
		uint64_t output_resource = 0; // Writable as a UAV.
		uint32_t render_width = 0;
		uint32_t render_height = 0;
		uint32_t upscale_width = 0;
		uint32_t upscale_height = 0;
		float jitter_x = 0.0f; // In pixels.
		float jitter_y = 0.0f;
		float motion_vector_scale_x = 0.0f;
		float motion_vector_scale_y = 0.0f;
		bool sharpening = false;
		float sharpness = 0.0f; // 0 (none) to 1 (most).
		float frame_time_delta_ms = 0.0f;
		bool reset = false;
		float camera_near = 0.0f;
		float camera_far = 0.0f;
		float fov_vertical_radians = 0.0f;
	};

	// The loader loads and reports an upscaling provider for p_device (ID3D12Device*). Probed once, quietly.
	static bool is_available(void *p_device);

	// Returns an opaque context, or nullptr (the error is printed).
	static void *create_context(const ContextParams &p_params);

	// The GPU must be done with every dispatch recorded with p_context.
	static void destroy_context(void *p_context);

	// Records the upscale into p_params.native_command_list.
	static bool dispatch(void *p_context, const DispatchParams &p_params);
};
