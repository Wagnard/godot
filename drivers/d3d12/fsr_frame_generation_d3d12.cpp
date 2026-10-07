/**************************************************************************/
/*  fsr_frame_generation_d3d12.cpp                                        */
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

#include "fsr_frame_generation_d3d12.h"

#include "core/config/engine.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "drivers/streamline/streamline.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include "thirdparty/amd-ffx-api/api/include/dx12/ffx_api_dx12.h"
#include "thirdparty/amd-ffx-api/api/include/ffx_api.h"
#include "thirdparty/amd-ffx-api/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "thirdparty/amd-ffx-api/framegeneration/include/ffx_framegeneration.h"

// Everything below follows AMD's FidelityFX SDK 2.3.0 sample (Samples/Upscalers/FidelityFX_FSR,
// fsrapirendermodule.cpp) and its documentation (frame-interpolation-api.md,
// frame-interpolation-swap-chain.md), with the swap chain's frame pacing left at AMD's defaults.
// Two choices differ from the sample, both measured (FSR-FG-STUDY.md): interpolation on the game's
// queue by default (uses_async_workloads()), and HUD-less UI composition (AMD's third mode, for
// engines that draw their UI into the frame).

namespace {

struct FfxFunctions {
	PfnFfxCreateContext create_context = nullptr;
	PfnFfxDestroyContext destroy_context = nullptr;
	PfnFfxConfigure configure = nullptr;
	PfnFfxQuery query = nullptr;
	PfnFfxDispatch dispatch = nullptr;
};

struct State {
	BinaryMutex mutex;
	HMODULE lib = nullptr;
	bool load_attempted = false;
	FfxFunctions fn;

	ID3D12Device *device = nullptr; // Not owned.
	IDXGISwapChain4 *swap_chain = nullptr; // Not owned: the D3D12 driver holds the reference.
	ffxContext swap_chain_context = nullptr;
	HANDLE frame_latency = nullptr; // The proxy's frame latency waitable object (our duplicate).

	ffxContext fg_context = nullptr;
	uint32_t fg_display_width = 0;
	uint32_t fg_display_height = 0;
	uint32_t fg_render_width = 0;
	uint32_t fg_render_height = 0;
	uint32_t fg_backbuffer_format = 0;
	uint32_t fg_flags = 0;
	bool fg_enabled = false; // What the last configure of fg_context asked for.

	uint64_t frame_id = 0;
	// Frames configured and prepared since the last Present (see before_present()).
	uint32_t prepares_since_present = 0;
	bool logged_first_prepare = false;
	bool logged_hudless = false;
	bool logged_hudless_mismatch = false;
};

State &state() {
	static State s;
	return s;
}

bool env_bool(const char *p_name, bool p_default) {
	const String value = OS::get_singleton()->get_environment(p_name);
	return value.is_empty() ? p_default : value == "1";
}

bool load_library(State &s, bool p_quiet = false) {
	if (s.load_attempted) {
		return s.lib != nullptr;
	}
	s.load_attempted = true;

	s.lib = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
	if (!s.lib) {
		if (!p_quiet) {
			ERR_PRINT("FSR FG: amd_fidelityfx_loader_dx12.dll not found next to the executable; frame generation disabled.");
		}
		return false;
	}
	s.fn.create_context = (PfnFfxCreateContext)(void *)GetProcAddress(s.lib, "ffxCreateContext");
	s.fn.destroy_context = (PfnFfxDestroyContext)(void *)GetProcAddress(s.lib, "ffxDestroyContext");
	s.fn.configure = (PfnFfxConfigure)(void *)GetProcAddress(s.lib, "ffxConfigure");
	s.fn.query = (PfnFfxQuery)(void *)GetProcAddress(s.lib, "ffxQuery");
	s.fn.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(s.lib, "ffxDispatch");
	if (!s.fn.create_context || !s.fn.destroy_context || !s.fn.configure || !s.fn.query || !s.fn.dispatch) {
		ERR_PRINT("FSR FG: amd_fidelityfx_loader_dx12.dll is missing ffx-api entry points; frame generation disabled.");
		FreeLibrary(s.lib);
		s.lib = nullptr;
		return false;
	}
	print_line("FSR FG: loaded amd_fidelityfx_loader_dx12.dll.");
	return true;
}

void log_frame_generation_versions(State &s) {
	uint64_t count = 0;
	ffxQueryDescGetVersions q = {};
	q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
	q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	q.device = s.device;
	q.outputCount = &count;
	if (s.fn.query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) {
		print_line("FSR FG: no frame generation provider reported for this device.");
		return;
	}
	uint64_t ids[16] = {};
	const char *names[16] = {};
	count = MIN(count, (uint64_t)16);
	q.versionIds = ids;
	q.versionNames = names;
	if (s.fn.query(nullptr, &q.header) == FFX_API_RETURN_OK) {
		for (uint64_t i = 0; i < count; i++) {
			print_line(vformat("FSR FG: available frame generation provider: %s", names[i] ? names[i] : "?"));
		}
	}
}

// Frame generation off on the proxy swap chain: it presents the frames it is given, and drops its
// references to the HUD-less color and the last prepare's resources. AMD's sample sets
// frameGenerationEnabled on every configure; this is the configure of a frame that does not generate.
void configure_disabled(State &s) {
	ffxConfigureDescFrameGeneration cfg = {};
	cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	cfg.swapChain = s.swap_chain;
	cfg.frameGenerationEnabled = false;
	s.fn.configure(&s.fg_context, &cfg.header);
	s.fg_enabled = false;
}

void destroy_fg_context(State &s) {
	if (!s.fg_context) {
		return;
	}
	// AMD's shutdown order: disable frame generation on the proxy swap chain first. That configure
	// waits for in-flight interpolation and UI composition that still reference the context's
	// resources (D3D12 error #921 otherwise), then destroy the context.
	if (s.swap_chain) {
		configure_disabled(s);
	}
	s.fn.destroy_context(&s.fg_context, nullptr);
	s.fg_context = nullptr;
}

// RenderingDevice creates its D3D12 textures in the typeless format of their family (views pick
// the type), while the proxy's back buffer is typed. The same family is the same data.
DXGI_FORMAT typeless_family(DXGI_FORMAT p_format) {
	switch (p_format) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8A8_TYPELESS;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			return DXGI_FORMAT_R10G10B10A2_TYPELESS;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return DXGI_FORMAT_R16G16B16A16_TYPELESS;
		default:
			return p_format;
	}
}

ffxReturnCode_t frame_generation_callback(ffxDispatchDescFrameGeneration *p_params, void *p_user_ctx) {
	State &s = state();
	return s.fn.dispatch((ffxContext *)p_user_ctx, &p_params->header);
}

} // namespace

namespace {
// -1: not read yet; then 0 (DLSS-G or none) or 1 (FSR).
std::atomic<int> fsr_provider{ -1 };

bool is_game_process() {
	return !Engine::get_singleton()->is_editor_hint() && !Engine::get_singleton()->is_project_manager_hint();
}
} // namespace

bool FSRFrameGenerationD3D12::is_requested() {
	int provider = fsr_provider.load();
	if (provider < 0) {
		const int initial = OS::get_singleton()->get_environment("GODOT_FSR_FG") == "1" ? 1 : 0;
		fsr_provider.compare_exchange_strong(provider, initial);
		provider = fsr_provider.load();
	}
	return provider == 1 && is_game_process();
}

void FSRFrameGenerationD3D12::set_requested(bool p_fsr) {
	if (!is_game_process() || is_requested() == p_fsr) {
		return;
	}
	fsr_provider.store(p_fsr ? 1 : 0);
	Streamline::get_singleton()->bump_swap_chain_serial();
	print_line(vformat("Frame generation provider: %s (the window's swap chain is recreated).", p_fsr ? "AMD FSR" : "NVIDIA DLSS-G"));
}

bool FSRFrameGenerationD3D12::is_available() {
	State &s = state();
	MutexLock lock(s.mutex);
	return load_library(s, true);
}

bool FSRFrameGenerationD3D12::uses_async_workloads() {
	// Optical flow and interpolation on an async compute queue, overlapping the next frame's
	// rendering, as in AMD's sample (GODOT_FSR_FG_ASYNC=1). Off by default: AMD advises to profile
	// (frame-interpolation-api.md), and measured in MySupCom at 4K, DLSS Quality, RTX 5070, still camera:
	// async, interpolation 4.3-5.8 ms and Godot's viewport 5.4-6.2 ms (they contend), 114 fps rendered;
	// game queue, interpolation 2.5-2.7 ms and viewport 4.7-5.2 ms, 120 fps (the display's cap).
	static const bool enabled = env_bool("GODOT_FSR_FG_ASYNC", false);
	return enabled;
}

bool FSRFrameGenerationD3D12::is_active() {
	State &s = state();
	MutexLock lock(s.mutex);
	return s.swap_chain != nullptr;
}

bool FSRFrameGenerationD3D12::is_generating() {
	State &s = state();
	MutexLock lock(s.mutex);
	return s.swap_chain != nullptr && s.fg_context != nullptr && s.fg_enabled;
}

void FSRFrameGenerationD3D12::stop_generating() {
	if (!is_requested()) {
		return;
	}
	State &s = state();
	MutexLock lock(s.mutex);
	if (s.fg_context) {
		destroy_fg_context(s);
		print_line("FSR FG: frame generation off (Viewport.frame_generation); context released.");
	}
}

bool FSRFrameGenerationD3D12::create_swap_chain(void *p_hwnd, void *p_desc1, void *p_factory, void *p_queue, void *p_device, void **r_swap_chain) {
	if (!is_requested()) {
		return false;
	}
	State &s = state();
	MutexLock lock(s.mutex);
	if (!load_library(s)) {
		return false;
	}
	if (s.swap_chain) {
		ERR_PRINT("FSR FG: a proxy swap chain already exists; only the main window gets frame generation.");
		return false;
	}

	s.device = (ID3D12Device *)p_device;

	ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 desc = {};
	desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
	IDXGISwapChain4 *proxy = nullptr;
	desc.swapchain = &proxy;
	desc.hwnd = (HWND)p_hwnd;
	// The proxy only creates its frame latency waitable object (wait_before_present()) when the desc
	// asks for it at creation, as the sample does. It forces the flag on the real swap chain either
	// way and re-adds it in its ResizeBuffers, so Godot's own creation_flags and ResizeBuffers calls
	// stay unchanged.
	DXGI_SWAP_CHAIN_DESC1 sc_desc = *(DXGI_SWAP_CHAIN_DESC1 *)p_desc1;
	sc_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	desc.desc = &sc_desc;
	desc.fullscreenDesc = nullptr;
	desc.dxgiFactory = (IDXGIFactory *)p_factory;
	desc.gameQueue = (ID3D12CommandQueue *)p_queue;

	ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version = {};
	version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
	version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
	desc.header.pNext = &version.header;

	ffxReturnCode_t rc = s.fn.create_context(&s.swap_chain_context, &desc.header, nullptr);
	if (rc != FFX_API_RETURN_OK || !proxy) {
		ERR_PRINT(vformat("FSR FG: creating the proxy swap chain failed (ffx return code %d); using a plain swap chain.", (int)rc));
		s.swap_chain_context = nullptr;
		return false;
	}

	s.swap_chain = proxy;
	s.frame_id = 0;
	*r_swap_chain = proxy;
	s.frame_latency = proxy->GetFrameLatencyWaitableObject();

	print_line(vformat("FSR FG: proxy swap chain created (%dx%d), frame latency waitable object %s.", desc.desc->Width, desc.desc->Height, s.frame_latency ? "yes" : "NO"));
	log_frame_generation_versions(s);

	return true;
}

void FSRFrameGenerationD3D12::release_swap_chain(void *p_swap_chain) {
	State &s = state();
	MutexLock lock(s.mutex);
	if (!s.swap_chain || s.swap_chain != (IDXGISwapChain4 *)p_swap_chain) {
		return;
	}
	destroy_fg_context(s);
	// Destroying the swap chain context does not destroy the proxy: the caller still holds a
	// reference, and releasing that last reference runs the proxy's destructor.
	if (s.swap_chain_context) {
		s.fn.destroy_context(&s.swap_chain_context, nullptr);
		s.swap_chain_context = nullptr;
	}
	// AMD: the application closes its duplicate after destroying the swap chain context.
	if (s.frame_latency) {
		CloseHandle(s.frame_latency);
		s.frame_latency = nullptr;
	}
	s.swap_chain = nullptr;
	print_line("FSR FG: proxy swap chain released.");
}

void FSRFrameGenerationD3D12::before_present(void *p_swap_chain) {
	if (!is_requested()) {
		return;
	}
	State &s = state();
	MutexLock lock(s.mutex);
	if (s.swap_chain != (IDXGISwapChain4 *)p_swap_chain) {
		return;
	}
	// The proxy's Present generates from the last configuration it was given: the HUD-less color,
	// and the depth and motion vectors of the last prepare. Without a prepare since the previous
	// Present, those may be resources Godot has freed since: after a window resize the 3D is not
	// drawn for a few frames while the render buffers are recreated, and the third Present read a
	// released ID3D12Resource (access violation in amd_fidelityfx_framegeneration_dx12). Such a frame
	// does not generate; the next prepare() configures generation on again.
	if (s.fg_context && s.fg_enabled && s.prepares_since_present == 0) {
		configure_disabled(s);
	}
	s.prepares_since_present = 0;
}

void FSRFrameGenerationD3D12::before_resize(void *p_swap_chain) {
	State &s = state();
	MutexLock lock(s.mutex);
	if (s.swap_chain != (IDXGISwapChain4 *)p_swap_chain) {
		return;
	}
	// As AMD's sample does on resize: the context is sized for the old back buffers.
	destroy_fg_context(s);
}

void FSRFrameGenerationD3D12::wait_before_present(void *p_swap_chain) {
	if (!is_requested()) {
		return;
	}
	State &s = state();
	HANDLE frame_latency = nullptr;
	{
		MutexLock lock(s.mutex);
		if (s.swap_chain != (IDXGISwapChain4 *)p_swap_chain || !s.frame_latency) {
			return;
		}
		frame_latency = s.frame_latency;
	}
	// AMD (frame-interpolation-swap-chain.md, "Waitable Object"): the game uses the proxy's frame
	// latency waitable object to keep the CPU from running ahead of the GPU, one wait per frame.
	// This is the thread that creates and destroys the swap chain, so the handle stays valid. The
	// documentation waits without a timeout; one second only keeps a lost release from hanging.
	WaitForSingleObject(frame_latency, 1000);
}

void FSRFrameGenerationD3D12::prepare(const PrepareParams &p_params) {
	if (!is_requested()) {
		return;
	}
	State &s = state();
	MutexLock lock(s.mutex);
	if (!s.swap_chain || !s.device) {
		return;
	}

	DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
	if (FAILED(s.swap_chain->GetDesc1(&sc_desc))) {
		return;
	}
	const uint32_t backbuffer_format = ffxApiGetSurfaceFormatDX12(sc_desc.Format);
	uint32_t flags = 0;
	if (p_params.depth_inverted) {
		flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
	}
	if (sc_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
		flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
	}
	if (uses_async_workloads()) {
		flags |= FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
	}

	// (Re)create the frame generation context when the display or render size or the format
	// changes. maxRenderSize is the current render size: a render-scale change recreates it.
	if (s.fg_context && (s.fg_display_width != sc_desc.Width || s.fg_display_height != sc_desc.Height || s.fg_render_width != p_params.render_width || s.fg_render_height != p_params.render_height || s.fg_backbuffer_format != backbuffer_format || s.fg_flags != flags)) {
		destroy_fg_context(s);
	}
	if (!s.fg_context) {
		ffxCreateContextDescFrameGeneration create = {};
		create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
		create.flags = flags;
		create.displaySize = { sc_desc.Width, sc_desc.Height };
		create.maxRenderSize = { p_params.render_width, p_params.render_height };
		create.backBufferFormat = backbuffer_format;

		ffxCreateContextDescFrameGenerationVersion version = {};
		version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
		version.version = FFX_FRAMEGENERATION_VERSION;

		ffxCreateBackendDX12Desc backend = {};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backend.device = s.device;

		create.header.pNext = &version.header;
		version.header.pNext = &backend.header;

		ffxReturnCode_t rc = s.fn.create_context(&s.fg_context, &create.header, nullptr);
		if (rc != FFX_API_RETURN_OK) {
			ERR_PRINT(vformat("FSR FG: creating the frame generation context failed (ffx return code %d).", (int)rc));
			s.fg_context = nullptr;
			return;
		}
		s.fg_display_width = sc_desc.Width;
		s.fg_display_height = sc_desc.Height;
		s.fg_render_width = p_params.render_width;
		s.fg_render_height = p_params.render_height;
		s.fg_backbuffer_format = backbuffer_format;
		s.fg_flags = flags;
		s.frame_id = 0;

		ffxQueryGetProviderVersion provider = {};
		provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (s.fn.query(&s.fg_context, &provider.header) == FFX_API_RETURN_OK && provider.versionName) {
			print_line(vformat("FSR FG: frame generation context created, provider %s, display %dx%d, render %dx%d.", provider.versionName, sc_desc.Width, sc_desc.Height, p_params.render_width, p_params.render_height));
		}
	}

	s.frame_id++;

	ffxConfigureDescFrameGeneration cfg = {};
	cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	cfg.swapChain = s.swap_chain;
	cfg.presentCallback = nullptr;
	cfg.frameGenerationCallback = frame_generation_callback;
	cfg.frameGenerationCallbackUserContext = &s.fg_context;
	cfg.frameGenerationEnabled = true;
	cfg.allowAsyncWorkloads = uses_async_workloads();
	// AMD's third UI mode: a HUD-less copy of the back buffer, from which frame generation tells
	// the UI apart. With allowAsyncWorkloads, interpolation reads it on an async compute queue while
	// the next frame renders: the compositor double-buffers it then (frame-interpolation-api.md,
	// "When HUDLess composition mode is used"). Its state at Present is COMPUTE_READ (= NON_PIXEL_SHADER_RESOURCE),
	// as in AMD's sample. Format: the back buffer's own, typed, so no separate hudless format
	// (ffxCreateContextDescFrameGenerationHudless) is needed.
	cfg.HUDLessColor = FfxApiResource({});
	if (p_params.hudless_resource) {
		ID3D12Resource *hudless = (ID3D12Resource *)p_params.hudless_resource;
		const D3D12_RESOURCE_DESC hudless_desc = hudless->GetDesc();
		if (hudless_desc.Width == sc_desc.Width && hudless_desc.Height == sc_desc.Height && typeless_family(hudless_desc.Format) == typeless_family(sc_desc.Format)) {
			cfg.HUDLessColor = ffxApiGetResourceDX12(hudless, FFX_API_RESOURCE_STATE_COMMON);
			cfg.HUDLessColor.description.format = backbuffer_format;
			if (!s.logged_hudless) {
				s.logged_hudless = true;
				print_line(vformat("FSR FG: HUD-less color in use (%dx%d).", (int64_t)hudless_desc.Width, (int64_t)hudless_desc.Height));
			}
		} else if (!s.logged_hudless_mismatch) {
			s.logged_hudless_mismatch = true;
			print_line(vformat("FSR FG: HUD-less color ignored: %dx%d format %d, back buffer %dx%d format %d.", (int64_t)hudless_desc.Width, (int64_t)hudless_desc.Height, (int64_t)hudless_desc.Format, (int64_t)sc_desc.Width, (int64_t)sc_desc.Height, (int64_t)sc_desc.Format));
		}
	}
	cfg.flags = 0;
	cfg.onlyPresentGenerated = false;
	cfg.generationRect = { 0, 0, (int32_t)sc_desc.Width, (int32_t)sc_desc.Height };
	cfg.frameID = s.frame_id;
	ffxReturnCode_t rc = s.fn.configure(&s.fg_context, &cfg.header);
	if (rc != FFX_API_RETURN_OK) {
		ERR_PRINT(vformat("FSR FG: configure failed (ffx return code %d).", (int)rc));
		return;
	}
	s.fg_enabled = true;

	ffxDispatchDescFrameGenerationPrepareV2 prep = {};
	prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
	prep.frameID = s.frame_id;
	prep.flags = 0;
	prep.commandList = p_params.native_command_list;
	prep.renderSize = { p_params.render_width, p_params.render_height };
	prep.jitterOffset = { p_params.jitter_x, p_params.jitter_y };
	// Godot's velocity buffer is in UV space, like the FSR2 path (effects/fsr2.cpp).
	prep.motionVectorScale = { (float)p_params.render_width, (float)p_params.render_height };
	prep.frameTimeDelta = p_params.frame_time_delta_ms;
	prep.reset = false;
	prep.cameraNear = p_params.camera_near;
	prep.cameraFar = p_params.camera_far;
	prep.cameraFovAngleVertical = p_params.fov_vertical_radians;
	prep.viewSpaceToMetersFactor = 0.0f; // As AMD's sample.
	// COMMON: the render graph holds them in D3D12_BARRIER_LAYOUT_COMMON (GENERAL usage), the only
	// layout where FidelityFX's legacy barriers may meet Godot's enhanced ones.
	prep.depth = ffxApiGetResourceDX12((ID3D12Resource *)p_params.depth_resource, FFX_API_RESOURCE_STATE_COMMON);
	prep.motionVectors = ffxApiGetResourceDX12((ID3D12Resource *)p_params.motion_vectors_resource, FFX_API_RESOURCE_STATE_COMMON);
	for (int i = 0; i < 3; i++) {
		prep.cameraPosition[i] = p_params.camera_position[i];
		prep.cameraUp[i] = p_params.camera_up[i];
		prep.cameraRight[i] = p_params.camera_right[i];
		prep.cameraForward[i] = p_params.camera_forward[i];
	}
	rc = s.fn.dispatch(&s.fg_context, &prep.header);
	if (rc != FFX_API_RETURN_OK) {
		ERR_PRINT(vformat("FSR FG: prepare dispatch failed (ffx return code %d).", (int)rc));
		return;
	}
	s.prepares_since_present++;
	if (!s.logged_first_prepare) {
		s.logged_first_prepare = true;
		print_line("FSR FG: first frame prepared; frame generation is running.");
	}
}
