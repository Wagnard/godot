/**************************************************************************/
/*  streamline_context.cpp                                                */
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

#include "streamline_context.h"

#include "core/os/os.h"

#include "core/io/dir_access.h"

#ifdef D3D12_ENABLED
#include "drivers/d3d12/fsr_frame_generation_d3d12.h"
#endif

#ifdef STREAMLINE_ENABLED
#ifdef _WIN32
#include <windows.h>

#include <dxgi1_2.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#if STREAMLINE_ENABLED_VULKAN
#include <vulkan/vulkan.h>
#endif

#if STREAMLINE_ENABLED_D3D12
#include <d3d12.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "drivers/streamline/streamline.h"

StreamlineContext &StreamlineContext::get() {
	static StreamlineContext _context;
	return _context;
}

void StreamlineContext::load_functions(bool d3d12) {
#ifdef _WIN32
	HMODULE streamline = LoadLibraryA("sl.interposer.dll");
	if (streamline == nullptr) {
		return;
	}
#define SL_GETPROC(type, name) (type *)(void *)GetProcAddress(streamline, name)
	this->slInit = SL_GETPROC(PFun_slInit, "slInit");
	this->slShutdown = SL_GETPROC(PFun_slShutdown, "slShutdown");
	this->slIsFeatureSupported = SL_GETPROC(PFun_slIsFeatureSupported, "slIsFeatureSupported");
	this->slGetFeatureFunction = SL_GETPROC(PFun_slGetFeatureFunction, "slGetFeatureFunction");
	this->slGetNewFrameToken = SL_GETPROC(PFun_slGetNewFrameToken, "slGetNewFrameToken");
	this->slSetFeatureLoaded = SL_GETPROC(PFun_slSetFeatureLoaded, "slSetFeatureLoaded");
	this->slSetD3DDevice = SL_GETPROC(PFun_slSetD3DDevice, "slSetD3DDevice");

	this->slAllocateResources = SL_GETPROC(PFun_slAllocateResources, "slAllocateResources");
	this->slFreeResources = SL_GETPROC(PFun_slFreeResources, "slFreeResources");
	this->slEvaluateFeature = SL_GETPROC(PFun_slEvaluateFeature, "slEvaluateFeature");
	this->slSetTag = SL_GETPROC(PFun_slSetTag, "slSetTag");
	this->slSetConstants = SL_GETPROC(PFun_slSetConstants, "slSetConstants");
#undef SL_GETPROC

#if STREAMLINE_ENABLED_D3D12
	if (d3d12) {
		this->func_CreateDXGIFactory = (void *)GetProcAddress(streamline, "CreateDXGIFactory");
		this->func_CreateDXGIFactory1 = (void *)GetProcAddress(streamline, "CreateDXGIFactory1");
		this->func_CreateDXGIFactory2 = (void *)GetProcAddress(streamline, "CreateDXGIFactory2");
		this->func_D3D12CreateDevice = (void *)GetProcAddress(streamline, "D3D12CreateDevice");
		this->func_D3D12GetInterface = (void *)GetProcAddress(streamline, "D3D12GetInterface");
		this->func_slUpgradeInterface = (void *)GetProcAddress(streamline, "slUpgradeInterface");
		this->func_DXGIGetDebugInterface1 = (void *)GetProcAddress(streamline, "DXGIGetDebugInterface1");
	}
#endif
#endif
}

void StreamlineContext::load_functions_post_init() {
	if (slGetFeatureFunction) {
		slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", (void *&)this->slReflexSetOptions);
		slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", (void *&)this->slReflexSleep);
		slGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", (void *&)this->slReflexGetState);
		slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", (void *&)this->slPCLSetMarker);
		slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetOptions", (void *&)this->slPCLSetOptions);

		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void *&)this->slDLSSGetOptimalSettings);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", (void *&)this->slDLSSGetState);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", (void *&)this->slDLSSSetOptions);

		slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void *&)this->slDLSSGGetState);
		slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void *&)this->slDLSSGSetOptions);

		// DLSS Ray Reconstruction (DLSS-RR)
		slGetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", (void *&)this->slDLSSDGetOptimalSettings);
		slGetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDGetState", (void *&)this->slDLSSDGetState);
		slGetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", (void *&)this->slDLSSDSetOptions);

		slGetFeatureFunction(sl::kFeatureNIS, "slNISSetOptions", (void *&)this->slNISSetOptions);
	}
}

static void _enumerate_support(StreamlineContext *self, const sl::AdapterInfo &adapterInfo, StreamlineCapabilities &outSupport) {
	if (self->slIsFeatureSupported) {
		outSupport.dlss_available = self->slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo) == sl::Result::eOk;
		outSupport.dlss_g_available = self->slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo) == sl::Result::eOk;
		outSupport.dlss_rr_available = self->slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo) == sl::Result::eOk;
		outSupport.reflex_available = self->slIsFeatureSupported(sl::kFeatureReflex, adapterInfo) == sl::Result::eOk;
		outSupport.pcl_available = self->slIsFeatureSupported(sl::kFeaturePCL, adapterInfo) == sl::Result::eOk;
		outSupport.nis_available = self->slIsFeatureSupported(sl::kFeatureNIS, adapterInfo) == sl::Result::eOk;
	}
}

// The features Godot uses Streamline for. PCL only carries Reflex's latency markers, and NIS only sharpens DLSS's
// output. Reflex counts on an NVIDIA GPU only: slIsFeatureSupported(kFeatureReflex) succeeds on every vendor (Intel Arc
// B580: eOk), but Reflex Low Latency and its frame rate limiter are "NVDA only" and other vendors only get PC Latency
// Stats (ProgrammingGuideReflex.md, overview table), which Godot has no use for.
// Returns eOk if one is supported, otherwise DLSS's result, which says why (ProgrammingGuide.md, 2.4).
static sl::Result _used_feature_support(StreamlineContext *self, const sl::AdapterInfo &p_adapter_info, bool p_nvidia) {
	const sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureDLSS_RR, sl::kFeatureReflex };
	sl::Result dlss_result = sl::Result::eErrorFeatureMissing;
	for (const sl::Feature feature : features) {
		if (feature == sl::kFeatureReflex && !p_nvidia) {
			continue;
		}
		const sl::Result result = self->slIsFeatureSupported(feature, p_adapter_info);
		if (result == sl::Result::eOk) {
			return result;
		}
		if (feature == sl::kFeatureDLSS) {
			dlss_result = result;
		}
	}
	return dlss_result;
}

#ifdef _WIN32
// Whether a GPU of this system can use a feature Godot uses Streamline for, checked per DXGI adapter as in
// ProgrammingGuide.md, 2.4 (an adapter-less slIsFeatureSupported() succeeds for Reflex on any system).
static sl::Result _system_feature_support(StreamlineContext *self) {
	typedef HRESULT(WINAPI * PFN_CREATE_DXGI_FACTORY1)(REFIID, void **);
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	PFN_CREATE_DXGI_FACTORY1 create_factory = dxgi ? (PFN_CREATE_DXGI_FACTORY1)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
	IDXGIFactory1 *factory = nullptr;
	if (create_factory == nullptr || FAILED(create_factory(__uuidof(IDXGIFactory1), (void **)&factory))) {
		if (dxgi) {
			FreeLibrary(dxgi);
		}
		return sl::Result::eOk; // Cannot tell: the main device's adapter decides.
	}
	sl::Result result = sl::Result::eErrorNoSupportedAdapterFound;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc = {};
		if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
			sl::AdapterInfo adapter_info;
			adapter_info.deviceLUID = (uint8_t *)&desc.AdapterLuid;
			adapter_info.deviceLUIDSizeInBytes = sizeof(LUID);
			const sl::Result adapter_result = _used_feature_support(self, adapter_info, desc.VendorId == StreamlineContext::NVIDIA_VENDOR_ID);
			if (adapter_result == sl::Result::eOk || result == sl::Result::eErrorNoSupportedAdapterFound) {
				result = adapter_result;
			}
			if (adapter_result == sl::Result::eOk) {
				print_line(vformat("Streamline: supported on %s (vendor 0x%04x).", String::utf16((const char16_t *)desc.Description), (uint64_t)desc.VendorId));
			}
		}
		adapter->Release();
		if (result == sl::Result::eOk) {
			break;
		}
	}
	factory->Release();
	FreeLibrary(dxgi);
	return result;
}
#endif

bool StreamlineContext::select_adapter(const sl::AdapterInfo &p_adapter_info, bool p_nvidia, bool p_d3d12, const String &p_adapter_name) {
	streamline_capabilities = StreamlineCapabilities();
	if (slIsFeatureSupported == nullptr) {
		return false; // Not loaded, or shut down already.
	}
	if (is_d3d12 != p_d3d12) {
		// The rendering driver fell back to the other API after slInit().
		print_line(vformat("Streamline: initialized for %s, but the device is created with %s: Streamline is not used.", is_d3d12 ? "Direct3D 12" : "Vulkan", p_d3d12 ? "Direct3D 12" : "Vulkan"));
		return false;
	}
	const sl::Result result = _used_feature_support(this, p_adapter_info, p_nvidia);
	if (result != sl::Result::eOk) {
		print_line(vformat("Streamline: DLSS, DLSS-G, DLSS-RR and Reflex are not supported on %s (%s): Streamline is not used.", p_adapter_name, result_to_string(result)));
		return false;
	}
	_enumerate_support(this, p_adapter_info, streamline_capabilities);
	return true;
}

void StreamlineContext::shutdown() {
	if (slShutdown) {
		slShutdown();
	}
	is_shut_down = true;

	slInit = nullptr;
	slShutdown = nullptr;
	slIsFeatureSupported = nullptr;
	slGetFeatureFunction = nullptr;
	slGetNewFrameToken = nullptr;
	slAllocateResources = nullptr;
	slFreeResources = nullptr;
	slEvaluateFeature = nullptr;
	slSetTag = nullptr;
	slSetConstants = nullptr;
	slSetFeatureLoaded = nullptr;
	slSetD3DDevice = nullptr;
	slReflexGetState = nullptr;
	slReflexSetOptions = nullptr;
	slReflexSleep = nullptr;
	slPCLSetMarker = nullptr;
	slPCLSetOptions = nullptr;
	slDLSSGetOptimalSettings = nullptr;
	slDLSSGetState = nullptr;
	slDLSSSetOptions = nullptr;
	slDLSSDGetOptimalSettings = nullptr;
	slDLSSDGetState = nullptr;
	slDLSSDSetOptions = nullptr;
	slDLSSGGetState = nullptr;
	slDLSSGSetOptions = nullptr;
	slNISSetOptions = nullptr;
	func_D3D12GetInterface = nullptr;
	func_D3D12CreateDevice = nullptr;
	func_slUpgradeInterface = nullptr;
	func_DXGIGetDebugInterface1 = nullptr;
	func_CreateDXGIFactory = nullptr;
	func_CreateDXGIFactory1 = nullptr;
	func_CreateDXGIFactory2 = nullptr;

	streamline_capabilities = StreamlineCapabilities();
	dlssg_loaded = false;
	reflex_loaded = false;
	reflex_running = false;
	last_token = nullptr;
	render_token = nullptr;
}

#if STREAMLINE_ENABLED_D3D12
void StreamlineContext::init_device_d3d12(void *d3d12_device) {
	if (this->slSetD3DDevice) {
		sl::Result result = this->slSetD3DDevice(d3d12_device);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
	}
}
#endif

#define STREAMLINE_GAME_ONLY \
	if (!is_game) \
		return;

void StreamlineContext::apply_frame_generation_features(bool p_fsr) {
	STREAMLINE_GAME_ONLY;
	if (!slSetFeatureLoaded) {
		return;
	}
	const bool want_dlssg = !p_fsr && dlssg_wanted && streamline_capabilities.dlss_g_available;
	const bool want_reflex = !p_fsr;
	if (want_dlssg == dlssg_loaded && want_reflex == reflex_loaded) {
		return;
	}
	// slSetFeatureLoaded() requires an idle pipeline and no concurrent DXGI/D3D/Vulkan calls
	// (ProgrammingGuide.md, 2.14): it rebuilds the interposer's hook lists. Unloaded features are
	// unhooked, so the function pointers are fetched again below.
	if (want_dlssg != dlssg_loaded) {
		if (!want_dlssg && (uint32_t)dlssg_viewport != UINT_MAX) {
			dlssg_disable();
		}
		if (is_d3d12) {
			slSetFeatureLoaded(sl::kFeatureDLSS_G, want_dlssg);
		} else {
			// Vulkan: the latency sleep, markers and Reflex calls of other threads wait meanwhile.
			MutexLock lock(Streamline::get_swap_chain_mutex());
			slSetFeatureLoaded(sl::kFeatureDLSS_G, want_dlssg);
		}
		dlssg_loaded = want_dlssg;
	}
	if (want_reflex != reflex_loaded) {
		// D3D12 only: p_fsr is never set on Vulkan.
		slSetFeatureLoaded(sl::kFeatureReflex, want_reflex);
		slSetFeatureLoaded(sl::kFeaturePCL, want_reflex);
		reflex_loaded = want_reflex;
		reflex_running = false;
		// Re-applied by the next frame's marker once loaded again.
		reflex_options_dirty = want_reflex;
		pcl_options_dirty = want_reflex;
	}
	slDLSSGGetState = nullptr;
	slDLSSGSetOptions = nullptr;
	slReflexSetOptions = nullptr;
	slReflexSleep = nullptr;
	slReflexGetState = nullptr;
	slPCLSetMarker = nullptr;
	slPCLSetOptions = nullptr;
	load_functions_post_init();
	if (!dlssg_loaded) {
		slDLSSGGetState = nullptr;
		slDLSSGSetOptions = nullptr;
	}
	if (!reflex_loaded) {
		slReflexSetOptions = nullptr;
		slReflexSleep = nullptr;
		slReflexGetState = nullptr;
		slPCLSetMarker = nullptr;
		slPCLSetOptions = nullptr;
	}
	print_line(vformat("Streamline: DLSS-G %s, Reflex and PCL %s for the next swap chain.", dlssg_loaded ? "loaded" : "unloaded", reflex_loaded ? "loaded" : "unloaded"));
}

bool StreamlineContext::dlss_clamp_render_size(int p_target_width, int p_target_height, int &r_render_width, int &r_render_height) {
	if (slDLSSGetOptimalSettings == nullptr || !streamline_capabilities.dlss_available) {
		return false;
	}

	// Ordered from the highest render resolution to the lowest.
	const sl::DLSSMode modes[] = { sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance };
	constexpr int mode_count = sizeof(modes) / sizeof(modes[0]);

	sl::DLSSOptimalSettings settings[mode_count] = {};
	bool valid[mode_count] = {};

	for (int i = 0; i < mode_count; i++) {
		sl::DLSSOptions options = {};
		options.outputWidth = p_target_width;
		options.outputHeight = p_target_height;
		options.mode = modes[i];
		valid[i] = slDLSSGetOptimalSettings(options, settings[i]) == sl::Result::eOk;

		if (valid[i] &&
				r_render_width >= (int)settings[i].renderWidthMin && r_render_width <= (int)settings[i].renderWidthMax &&
				r_render_height >= (int)settings[i].renderHeightMin && r_render_height <= (int)settings[i].renderHeightMax) {
			return false; // A mode already accepts this size; leave it alone.
		}
	}

	// The requested size falls in a gap between two modes, or below the lowest one. Round down to
	// the exact size of the nearest mode that does not render more pixels than were asked for, so
	// lowering the resolution scale never makes the frame more expensive.
	int chosen = -1;
	for (int i = 0; i < mode_count; i++) {
		if (valid[i] && (int)settings[i].optimalRenderWidth <= r_render_width) {
			chosen = i;
			break;
		}
	}

	if (chosen == -1) {
		// Below even Ultra Performance: use the lowest mode that is available.
		for (int i = mode_count - 1; i >= 0; i--) {
			if (valid[i]) {
				chosen = i;
				break;
			}
		}
	}

	if (chosen == -1) {
		return false;
	}

	r_render_width = (int)settings[chosen].optimalRenderWidth;
	r_render_height = (int)settings[chosen].optimalRenderHeight;
	return true;
}

void StreamlineContext::dlssg_disable() {
	STREAMLINE_GAME_ONLY; // Disable DLSS-G for editor or project settings.

	dlssg_delay = 10;
	dlssg_last_status = 0;
	reflex_set_required_by_dlssg(false);

	if (slDLSSGSetOptions && (uint32_t)dlssg_viewport != UINT_MAX) {
		WARN_PRINT("Force disabling DLSS-G on viewport: " + itos((unsigned int)StreamlineContext::get().dlssg_viewport));

		sl::DLSSGOptions dlssGOptions{};
		dlssGOptions.mode = sl::DLSSGMode::eOff;
		slDLSSGSetOptions(dlssg_viewport, dlssGOptions);

		dlssg_viewport = sl::ViewportHandle(-1);
	}
}

void StreamlineContext::dlssg_unload_schedule() {
	STREAMLINE_GAME_ONLY;
	if (dlssg_off_usec.load() != 0) {
		return;
	}
	// Ordered before the time: the present tick reads the time first.
	dlssg_off_presents.store(presents.load());
	dlssg_off_usec.store(MAX<uint64_t>(1, OS::get_singleton()->get_ticks_usec()));
	print_line(vformat("Streamline: DLSS-G off; sl.dlss_g is unloaded after %d ms and %d presented frames.", DLSS_G_UNLOAD_DELAY_USEC / 1000, DLSS_G_UNLOAD_DELAY_PRESENTS));
}

void StreamlineContext::dlssg_unload_cancel() {
	if (dlssg_off_usec.exchange(0) != 0) {
		print_line("Streamline: DLSS-G on again before its unload; sl.dlss_g stays loaded.");
	}
}

void StreamlineContext::dlssg_unload_present_tick() {
	const uint64_t presented = presents.fetch_add(1) + 1;
	uint64_t off_usec = dlssg_off_usec.load();
	if (off_usec == 0) {
		return;
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (now - off_usec < DLSS_G_UNLOAD_DELAY_USEC || presented - dlssg_off_presents.load() < DLSS_G_UNLOAD_DELAY_PRESENTS) {
		return;
	}
	// The renderer may cancel at the same time: whoever clears the time first wins.
	if (!dlssg_off_usec.compare_exchange_strong(off_usec, 0)) {
		return;
	}
	dlssg_wanted = false;
	print_line(vformat("Streamline: unloading sl.dlss_g, %d ms and %d presented frames after DLSS-G's eOff.", (now - off_usec) / 1000, presented - dlssg_off_presents.load()));
#ifdef D3D12_ENABLED
	if (fsr_switch_pending.exchange(false)) {
		// One swap chain recreation unloads sl.dlss_g and creates the FSR proxy.
		FSRFrameGenerationD3D12::set_requested(true);
		return;
	}
#endif
	Streamline::get_singleton()->bump_swap_chain_serial();
}

void StreamlineContext::reflex_set_options(const sl::ReflexOptions &opts) {
	STREAMLINE_GAME_ONLY; // Disable reflex for editor or project settings.

	reflex_options = opts;
	reflex_options_dirty = false;
	sl::Result result = sl::Result::eOk;
	if (this->slReflexSetOptions) {
		if (is_d3d12) {
			result = this->slReflexSetOptions(opts);
		} else {
			MutexLock lock(Streamline::get_swap_chain_mutex()); // Sets the sleep mode on the swap chain.
			result = this->slReflexSetOptions(opts);
		}
	}
	reflex_running = this->slReflexSetOptions != nullptr && result == sl::Result::eOk && opts.mode != sl::ReflexMode::eOff;
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
}

// Recomputes the Reflex mode that is actually applied. DLSS-G only works while Reflex is
// running, so frame generation promotes an "off" request to low latency instead.
static sl::ReflexMode _effective_reflex_mode(sl::ReflexMode p_requested, bool p_required_by_dlssg) {
	if (p_required_by_dlssg && p_requested == sl::ReflexMode::eOff) {
		return sl::ReflexMode::eLowLatency;
	}
	return p_requested;
}

void StreamlineContext::reflex_request_mode(sl::ReflexMode p_mode) {
	reflex_requested_mode = p_mode;
	reflex_apply_required_by_dlssg();
}

void StreamlineContext::reflex_set_required_by_dlssg(bool p_required) {
	// Called by DLSS on the render graph's replay thread: only the flag is written here, the main thread's
	// marker turns it into Reflex options.
	if (reflex_required_by_dlssg.exchange(p_required) != p_required && p_required) {
		print_verbose("Streamline: DLSS Frame Generation requires Reflex; low latency mode is used if Reflex is off.");
	}
}

void StreamlineContext::reflex_apply_required_by_dlssg() {
	sl::ReflexMode effective = _effective_reflex_mode(reflex_requested_mode, reflex_required_by_dlssg.load());
	if (reflex_options.mode != effective) {
		reflex_options.mode = effective;
		reflex_options_dirty = true;
	}
}

void StreamlineContext::dlssg_report_status(const sl::ViewportHandle &p_viewport) {
	if (slDLSSGGetState == nullptr) {
		return;
	}

	sl::DLSSGState state{};
	if (slDLSSGGetState(p_viewport, state, nullptr) != sl::Result::eOk) {
		return;
	}

	uint32_t status = (uint32_t)state.status;
	if (status == dlssg_last_status) {
		return; // Only report changes, this is polled every frame.
	}
	dlssg_last_status = status;

	if (status == (uint32_t)sl::DLSSGStatus::eOk) {
		return;
	}

	// DLSS-G presents magenta frames when any of these are set, so make the reason visible.
	String reasons;
	if (status & (uint32_t)sl::DLSSGStatus::eFailResolutionTooLow) {
		reasons += "\n- Output resolution is too low (minimum " + itos(state.minWidthOrHeight) + " px).";
	}
	if (status & (uint32_t)sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime) {
		reasons += "\n- Reflex is not running. Reflex must be active while DLSS Frame Generation is on.";
	}
	if (status & (uint32_t)sl::DLSSGStatus::eFailHDRFormatNotSupported) {
		reasons += "\n- The swap chain back buffer format is not supported.";
	}
	if (status & (uint32_t)sl::DLSSGStatus::eFailCommonConstantsInvalid) {
		reasons += "\n- Invalid common constants were provided (camera matrices, jitter or motion vectors).";
	}
	if (status & (uint32_t)sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled) {
		reasons += "\n- IDXGISwapChain3::GetCurrentBackBufferIndex was not called before presenting.";
	}
	if (reasons.is_empty()) {
		reasons = "\n- Unknown status: " + itos(status) + ".";
	}

	ERR_PRINT("DLSS Frame Generation is not working and will present corrupted (magenta) frames:" + reasons);
}

void StreamlineContext::pcl_set_options(const sl::PCLOptions &opts) {
	STREAMLINE_GAME_ONLY; // Disable PCL for editor or project settings.

	pcl_options = opts;
	pcl_options_dirty = false;
	sl::Result result = this->slPCLSetOptions ? this->slPCLSetOptions(opts) : sl::Result::eOk;
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
}

void StreamlineContext::pcl_marker(sl::FrameToken *frameToken, sl::PCLMarker marker) {
	STREAMLINE_GAME_ONLY; // Disable PCL for editor or project settings.

	sl::Result result = this->slPCLSetMarker ? this->slPCLSetMarker(marker, *frameToken) : sl::Result::eOk;
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
}

void StreamlineContext::reflex_sleep(sl::FrameToken *frameToken) {
	STREAMLINE_GAME_ONLY; // Disable reflex for editor or project settings.

	if (frameToken == nullptr) {
		return;
	}

	if (latency_sleep_callback) {
		if (!latency_sleep_callback(latency_sleep_userdata)) {
			ERR_PRINT("Reflex: the Vulkan driver's sleep failed, slReflexSleep() is used from now on.");
			latency_sleep_callback = nullptr;
		}
		return;
	}

	sl::Result result = this->slReflexSleep ? this->slReflexSleep(*frameToken) : sl::Result::eOk;
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
}

void StreamlineContext::reflex_get_state(sl::ReflexState &reflexState) {
	STREAMLINE_GAME_ONLY; // Disable reflex for editor or project settings.

	sl::Result result = sl::Result::eOk;
	if (this->slReflexGetState) {
		if (is_d3d12) {
			result = this->slReflexGetState(reflexState);
		} else {
			MutexLock lock(Streamline::get_swap_chain_mutex()); // Reads the swap chain's latency timings.
			result = this->slReflexGetState(reflexState);
		}
	}
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
}

sl::FrameToken *StreamlineContext::get_new_frame_token() {
	// Streamline's own index only advances on present, so a frame drawn without one (e.g. a minimized
	// window) would reuse an index and slSetConstants would refuse it as duplicated.
	++frame_index;
	last_token_drawn = false;
	sl::Result result = this->slGetNewFrameToken ? this->slGetNewFrameToken(last_token, &frame_index) : sl::Result::eOk;
	ERR_FAIL_COND_V_MSG(result != sl::Result::eOk, nullptr, StreamlineContext::result_to_string(result));
	return last_token;
}

sl::FrameToken *StreamlineContext::get_frame_token_for_draw() {
	// One token per draw. draw() can also run outside the main loop (force_draw(), redraws of a game
	// paused in the debugger), which would otherwise reuse a token slSetConstants already received.
	if (last_token_drawn && slGetNewFrameToken) {
		get_new_frame_token();
	}
	last_token_drawn = true;
	return last_token;
}

const char *StreamlineContext::result_to_string(sl::Result result) {
	switch (result) {
		case sl::Result::eOk:
			return "sl::eOk";
		case sl::Result::eErrorIO:
			return "sl::eErrorIO";
		case sl::Result::eErrorDriverOutOfDate:
			return "sl::eErrorDriverOutOfDate";
		case sl::Result::eErrorOSOutOfDate:
			return "sl::eErrorOSOutOfDate";
		case sl::Result::eErrorOSDisabledHWS:
			return "sl::eErrorOSDisabledHWS";
		case sl::Result::eErrorDeviceNotCreated:
			return "sl::eErrorDeviceNotCreated";
		case sl::Result::eErrorNoSupportedAdapterFound:
			return "sl::eErrorNoSupportedAdapterFound";
		case sl::Result::eErrorAdapterNotSupported:
			return "sl::eErrorAdapterNotSupported";
		case sl::Result::eErrorNoPlugins:
			return "sl::eErrorNoPlugins";
		case sl::Result::eErrorVulkanAPI:
			return "sl::eErrorVulkanAPI";
		case sl::Result::eErrorDXGIAPI:
			return "sl::eErrorDXGIAPI";
		case sl::Result::eErrorD3DAPI:
			return "sl::eErrorD3DAPI";
		case sl::Result::eErrorNRDAPI:
			return "sl::eErrorNRDAPI";
		case sl::Result::eErrorNVAPI:
			return "sl::eErrorNVAPI";
		case sl::Result::eErrorReflexAPI:
			return "sl::eErrorReflexAPI";
		case sl::Result::eErrorNGXFailed:
			return "sl::eErrorNGXFailed";
		case sl::Result::eErrorJSONParsing:
			return "sl::eErrorJSONParsing";
		case sl::Result::eErrorMissingProxy:
			return "sl::eErrorMissingProxy";
		case sl::Result::eErrorMissingResourceState:
			return "sl::eErrorMissingResourceState";
		case sl::Result::eErrorInvalidIntegration:
			return "sl::eErrorInvalidIntegration";
		case sl::Result::eErrorMissingInputParameter:
			return "sl::eErrorMissingInputParameter";
		case sl::Result::eErrorNotInitialized:
			return "sl::eErrorNotInitialized";
		case sl::Result::eErrorComputeFailed:
			return "sl::eErrorComputeFailed";
		case sl::Result::eErrorInitNotCalled:
			return "sl::eErrorInitNotCalled";
		case sl::Result::eErrorExceptionHandler:
			return "sl::eErrorExceptionHandler";
		case sl::Result::eErrorInvalidParameter:
			return "sl::eErrorInvalidParameter";
		case sl::Result::eErrorMissingConstants:
			return "sl::eErrorMissingConstants";
		case sl::Result::eErrorDuplicatedConstants:
			return "sl::eErrorDuplicatedConstants";
		case sl::Result::eErrorMissingOrInvalidAPI:
			return "sl::eErrorMissingOrInvalidAPI";
		case sl::Result::eErrorCommonConstantsMissing:
			return "sl::eErrorCommonConstantsMissing";
		case sl::Result::eErrorUnsupportedInterface:
			return "sl::eErrorUnsupportedInterface";
		case sl::Result::eErrorFeatureMissing:
			return "sl::eErrorFeatureMissing";
		case sl::Result::eErrorFeatureNotSupported:
			return "sl::eErrorFeatureNotSupported";
		case sl::Result::eErrorFeatureMissingHooks:
			return "sl::eErrorFeatureMissingHooks";
		case sl::Result::eErrorFeatureFailedToLoad:
			return "sl::eErrorFeatureFailedToLoad";
		case sl::Result::eErrorFeatureWrongPriority:
			return "sl::eErrorFeatureWrongPriority";
		case sl::Result::eErrorFeatureMissingDependency:
			return "sl::eErrorFeatureMissingDependency";
		case sl::Result::eErrorFeatureManagerInvalidState:
			return "sl::eErrorFeatureManagerInvalidState";
		case sl::Result::eErrorInvalidState:
			return "sl::eErrorInvalidState";
		case sl::Result::eWarnOutOfVRAM:
			return "sl::eWarnOutOfVRAM";
		default:
			return "sl::eUnknown";
	}
}

void StreamlineContext::initialize(bool d3d12) {
	StreamlineContext::get().is_game = true;
	if (Engine::get_singleton()->is_editor_hint() || Engine::get_singleton()->is_project_manager_hint()) {
		StreamlineContext::get().is_game = false;
	}

	if (StreamlineContext::get().slInit != nullptr || StreamlineContext::get().is_shut_down) {
		return; // already initialized.
	}

	StreamlineContext::get().load_functions(d3d12);
	if (StreamlineContext::get().slInit == nullptr) {
		return;
	}

	sl::Preferences pref;

	Vector<sl::Feature> featuresToLoad;

	StreamlineContext::get().is_d3d12 = d3d12;
	if (StreamlineContext::get().is_game) {
		// Always requested: a feature not requested here can never be loaded later. On D3D12 they are
		// then loaded or unloaded per swap chain, see apply_frame_generation_features().
		featuresToLoad.push_back(sl::kFeaturePCL);
		featuresToLoad.push_back(sl::kFeatureReflex);
		featuresToLoad.push_back(sl::kFeatureDLSS_G);

		if (bool(GLOBAL_GET("rendering/streamline/streamline_imgui"))) {
			featuresToLoad.push_back(sl::kFeatureImGUI);
		}
	}
	featuresToLoad.push_back(sl::kFeatureDLSS);
	featuresToLoad.push_back(sl::kFeatureDLSS_RR); // DLSS Ray Reconstruction
	featuresToLoad.push_back(sl::kFeatureNIS);

	pref.featuresToLoad = featuresToLoad.ptr();
	pref.numFeaturesToLoad = featuresToLoad.size();

	pref.renderAPI = d3d12 ? sl::RenderAPI::eD3D12 : sl::RenderAPI::eVulkan;
	pref.applicationId = 0x90d07004;
	pref.flags = sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins | sl::PreferenceFlags::eDisableCLStateTracking;

	// Streamline only writes a log file when given a directory. Static so the pointer stays valid.
	static Char16String log_dir_w;
	if (bool(GLOBAL_GET("rendering/streamline/streamline_log"))) {
		pref.logLevel = sl::LogLevel::eVerbose;
		pref.showConsole = true;
		String log_dir = OS::get_singleton()->get_user_data_dir().path_join("streamline");
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (da.is_valid()) {
			da->make_dir_recursive(log_dir);
		}
		log_dir_w = log_dir.replace("/", "\\").utf16();
		pref.pathToLogsAndData = (const wchar_t *)log_dir_w.get_data();
		print_line("Streamline: logging to " + log_dir);
	} else {
		pref.logLevel = sl::LogLevel::eOff;
		pref.showConsole = false;
	}
	sl::Result result = StreamlineContext::get().slInit(pref, sl::kSDKVersion);
	if (result != sl::Result::eOk) {
		ERR_PRINT(vformat("Streamline: slInit() failed (%s): Streamline is not used.", StreamlineContext::result_to_string(result)));
		StreamlineContext::get().shutdown();
		return;
	}

	// If no GPU of this system can use a feature Godot uses, Streamline is shut down before any DXGI, D3D12 or Vulkan call
	// goes through its proxies. Otherwise the main device's adapter decides (select_adapter()).
#ifdef _WIN32
	if (StreamlineContext::get().slIsFeatureSupported != nullptr) {
		const sl::Result support = _system_feature_support(&StreamlineContext::get());
		if (support != sl::Result::eOk) {
			print_line(vformat("Streamline: DLSS, DLSS-G, DLSS-RR and Reflex are not supported on this system (%s): Streamline is not used.", StreamlineContext::result_to_string(support)));
			StreamlineContext::get().shutdown();
		}
	}
#endif
}

#endif
