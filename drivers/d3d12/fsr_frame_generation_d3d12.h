/**************************************************************************/
/*  fsr_frame_generation_d3d12.h                                          */
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

// PROTOTYPE: AMD FSR frame generation (FidelityFX SDK 2.3.0, ffx-api, D3D12 only), enabled with the
// environment variable GODOT_FSR_FG=1 in a game (never in the editor). See FSR-FG-STUDY.md.
//
// The interface below carries no D3D12 types so the renderer (effects/dlss.cpp) can call it.
// Everything runs on the thread that owns the swap chain and records the frame: the render thread
// in the Separate model, the main thread in Safe. The AMD contexts are not thread safe; a mutex
// still guards every call, as AMD's guide asks.
class FSRFrameGenerationD3D12 {
public:
	struct PrepareParams {
		void *native_command_list = nullptr; // ID3D12GraphicsCommandList*.
		uint64_t depth_resource = 0; // ID3D12Resource*, in NON_PIXEL_SHADER_RESOURCE state.
		uint64_t motion_vectors_resource = 0; // ID3D12Resource*, decoded (no (-1,-1) sentinel).
		// ID3D12Resource*, or 0: the frame before its canvas, back-buffer-sized, in the back
		// buffer's color space. Written later in the frame; sampled (NON_PIXEL_SHADER_RESOURCE)
		// by the time of Present. Used only if its size and format family match the back buffer.
		uint64_t hudless_resource = 0;
		uint32_t render_width = 0;
		uint32_t render_height = 0;
		float jitter_x = 0.0f; // In pixels.
		float jitter_y = 0.0f;
		float frame_time_delta_ms = 0.0f;
		float camera_near = 0.0f;
		float camera_far = 0.0f;
		float fov_vertical_radians = 0.0f;
		bool depth_inverted = true;
		float camera_position[3] = {};
		float camera_up[3] = {};
		float camera_right[3] = {};
		float camera_forward[3] = {};
	};

	// AMD FSR is the frame generation provider: a game (never the editor) that started with
	// GODOT_FSR_FG=1 or switched with Streamline.set_parameter(STREAMLINE_PARAM_FRAME_GENERATION_PROVIDER).
	static bool is_requested();
	// Any thread. Switching recreates the main window's swap chain (Streamline::bump_swap_chain_serial()).
	static void set_requested(bool p_fsr);
	// amd_fidelityfx_loader_dx12.dll loads (next to the executable). Probed once, quietly.
	static bool is_available();

	// D3D12 driver: creates the proxy swap chain for this window instead of
	// IDXGIFactory::CreateSwapChainForHwnd. Returns false (and leaves r_swap_chain untouched) when
	// FSR frame generation is not requested or failed, so the caller creates a plain swap chain.
	// p_desc1 is a DXGI_SWAP_CHAIN_DESC1*, p_factory an IDXGIFactory*, p_queue an
	// ID3D12CommandQueue*, p_device an ID3D12Device*, r_swap_chain receives an IDXGISwapChain4*
	// with one reference owned by the caller.
	static bool create_swap_chain(void *p_hwnd, void *p_desc1, void *p_factory, void *p_queue, void *p_device, void **r_swap_chain);

	// D3D12 driver: before the caller releases its last reference to p_swap_chain. Tears down the
	// frame generation context, then the swap chain context, in AMD's documented order.
	static void release_swap_chain(void *p_swap_chain);

	// Renderer: once per rendered frame, after depth and motion vectors are final and before the
	// frame is presented. Configures frame generation and records its prepare pass.
	static void prepare(const PrepareParams &p_params);

	// D3D12 driver, right before Present on p_swap_chain (the render thread in the Separate model):
	// one wait per frame on the proxy's frame latency waitable object, as AMD's documentation asks,
	// so the CPU does not run ahead of the GPU. No-op for any other swap chain.
	static void wait_before_present(void *p_swap_chain);

	// D3D12 driver, right before Present on p_swap_chain: turns frame generation off for this Present
	// when no frame was prepared since the previous one, so the proxy never generates from resources
	// freed since. The next prepare() turns it on again. No-op for any other swap chain.
	static void before_present(void *p_swap_chain);

	// D3D12 driver, right before ResizeBuffers on p_swap_chain: destroys the frame generation
	// context, sized for the old back buffers. The next prepare() creates it again.
	static void before_resize(void *p_swap_chain);

	// The proxy swap chain exists (GODOT_FSR_FG=1, main window created through it).
	static bool is_active();

	// Frames are being generated: a context exists and its last configure turned generation on.
	static bool is_generating();

	// Renderer, for the viewport shown on the main window when its Viewport.frame_generation is off:
	// releases the frame generation context (AMD's shutdown order). The proxy keeps presenting the
	// frames it is given; the next prepare() creates the context again.
	static void stop_generating();

	// Whether interpolation runs on an async compute queue (GODOT_FSR_FG_ASYNC=1, default off): the
	// HUD-less color must then be double-buffered. Read once.
	static bool uses_async_workloads();
};
