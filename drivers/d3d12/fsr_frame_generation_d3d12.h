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

// AMD FSR frame generation (FidelityFX ffx-api, D3D12 only), enabled with GODOT_FSR_FG=1 in a game, never in the editor.
// The interface carries no D3D12 types so the renderer can call it. Everything runs on the thread that owns the swap
// chain; the AMD contexts are not thread safe, and a mutex guards every call as AMD's guide asks.
class FSRFrameGenerationD3D12 {
public:
	struct PrepareParams {
		void *native_command_list = nullptr; // ID3D12GraphicsCommandList*.
		uint64_t depth_resource = 0; // ID3D12Resource*, in NON_PIXEL_SHADER_RESOURCE state.
		uint64_t motion_vectors_resource = 0; // ID3D12Resource*, decoded (no (-1,-1) sentinel).
		// ID3D12Resource*, or 0: the frame before its canvas (HUD-less), in the back buffer's color space and sampled
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
		// A camera cut: AMD's ffxDispatchDescFrameGenerationPrepare "reset".
		bool reset = false;
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

	// Creates AMD's proxy swap chain instead of CreateSwapChainForHwnd(), or returns false (r_swap_chain untouched) so
	// the caller creates a plain one. The pointers are a DXGI_SWAP_CHAIN_DESC1, IDXGIFactory, ID3D12CommandQueue and
	// ID3D12Device; r_swap_chain receives an IDXGISwapChain4 with one reference owned by the caller.
	static bool create_swap_chain(void *p_hwnd, void *p_desc1, void *p_factory, void *p_queue, void *p_device, void **r_swap_chain);

	// D3D12 driver: before the caller releases its last reference to p_swap_chain. Tears down the
	// frame generation context, then the swap chain context, in AMD's documented order.
	static void release_swap_chain(void *p_swap_chain);

	// Renderer: once per rendered frame, after depth and motion vectors are final and before the
	// frame is presented. Configures frame generation and records its prepare pass.
	static void prepare(const PrepareParams &p_params);

	// Right before Present: one wait per frame on the proxy's frame latency waitable object, as AMD's documentation
	// asks. No-op for any other swap chain.
	static void wait_before_present(void *p_swap_chain);

	// Right before Present: turns generation off when no frame was prepared since the previous Present, so the proxy
	// never generates from freed resources. No-op for any other swap chain.
	static void before_present(void *p_swap_chain);

	// D3D12 driver, right before ResizeBuffers on p_swap_chain: destroys the frame generation
	// context, sized for the old back buffers. The next prepare() creates it again.
	static void before_resize(void *p_swap_chain);

	// The main window's swap chain is AMD's proxy.
	static bool is_active();

	// Frames are being generated: a context exists and its last configure turned generation on.
	static bool is_generating();

	// Releases the frame generation context when the main window's viewport turns frame generation off. The proxy
	// keeps presenting; the next prepare() creates the context again.
	static void stop_generating();

	// Whether interpolation runs on an async compute queue (GODOT_FSR_FG_ASYNC=1, default off): the
	// HUD-less color must then be double-buffered. Read once.
	static bool uses_async_workloads();
};
