/**************************************************************************/
/*  streamline_context.h                                                  */
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

#ifdef STREAMLINE_ENABLED
#include "core/string/ustring.h"
#include "drivers/streamline/streamline_data.h"
#include "drivers/streamline/streamline_headers.h"

#include <atomic>

class StreamlineContext {
public:
	// Interposer
	PFun_slInit *slInit = nullptr;
	PFun_slShutdown *slShutdown = nullptr;
	PFun_slIsFeatureSupported *slIsFeatureSupported = nullptr;
	PFun_slGetFeatureFunction *slGetFeatureFunction = nullptr;

	PFun_slGetNewFrameToken *slGetNewFrameToken = nullptr;
	PFun_slAllocateResources *slAllocateResources = nullptr;
	PFun_slFreeResources *slFreeResources = nullptr;
	PFun_slEvaluateFeature *slEvaluateFeature = nullptr;
	PFun_slSetTag *slSetTag = nullptr;
	PFun_slSetConstants *slSetConstants = nullptr;
	PFun_slSetFeatureLoaded *slSetFeatureLoaded = nullptr;
	PFun_slSetD3DDevice *slSetD3DDevice = nullptr;

	// Reflex
	sl::ReflexOptions reflex_options;
	bool reflex_options_dirty = true;
	PFun_slReflexGetState *slReflexGetState = nullptr;
	PFun_slReflexSetOptions *slReflexSetOptions = nullptr;
	PFun_slReflexSleep *slReflexSleep = nullptr;
	// Vulkan: the driver's sleep, used instead of slReflexSleep(). See Streamline::set_latency_sleep_callback().
	bool (*latency_sleep_callback)(void *) = nullptr;
	void *latency_sleep_userdata = nullptr;

	// PCL
	sl::PCLOptions pcl_options;
	bool pcl_options_dirty = true;
	PFun_slPCLSetMarker *slPCLSetMarker = nullptr;
	PFun_slPCLSetOptions *slPCLSetOptions = nullptr;

	// DLSS Super Resolution
	PFun_slDLSSGetOptimalSettings *slDLSSGetOptimalSettings = nullptr;
	PFun_slDLSSGetState *slDLSSGetState = nullptr;
	PFun_slDLSSSetOptions *slDLSSSetOptions = nullptr;
	char dlss_default_preset = '?';
	char dlss_rr_default_preset = '?';

	// DLSS Ray Reconstruction (DLSS-D / DLSS-RR)
	PFun_slDLSSDGetOptimalSettings *slDLSSDGetOptimalSettings = nullptr;
	PFun_slDLSSDGetState *slDLSSDGetState = nullptr;
	PFun_slDLSSDSetOptions *slDLSSDSetOptions = nullptr;

	// DLSS Frame Generation
	PFun_slDLSSGGetState *slDLSSGGetState = nullptr;
	PFun_slDLSSGSetOptions *slDLSSGSetOptions = nullptr;

	// NIS
	PFun_slNISSetOptions *slNISSetOptions = nullptr;

	// D3D12
	void *func_D3D12GetInterface = nullptr;
	void *func_D3D12CreateDevice = nullptr;
	void *func_slUpgradeInterface = nullptr;
	void *func_DXGIGetDebugInterface1 = nullptr;
	void *func_CreateDXGIFactory = nullptr;
	void *func_CreateDXGIFactory1 = nullptr;
	void *func_CreateDXGIFactory2 = nullptr;

	void load_functions(bool d3d12);
	void load_functions_post_init();
	static const char *result_to_string(sl::Result result);

	void reflex_set_options(const sl::ReflexOptions &opts);
	void reflex_sleep(sl::FrameToken *frameToken);
	void reflex_get_state(sl::ReflexState &reflexState);
	void pcl_set_options(const sl::PCLOptions &opts);
	void pcl_marker(sl::FrameToken *frameToken, sl::PCLMarker marker);
	sl::FrameToken *get_new_frame_token();
	// Returns last_token, or a new one if a previous draw already used it.
	sl::FrameToken *get_frame_token_for_draw();

	void dlssg_disable();

	// Right before a swap chain is created: loads or unloads DLSS-G, Reflex and PCL for it.
	// DLSS-G is loaded while frame generation is on, and unloaded once it has been off for a while
	// (dlssg_unload_schedule()) or for FSR frame generation (D3D12), whose proxy swap chain cannot
	// coexist with it.
	void apply_frame_generation_features(bool p_fsr);

	// sl.dlss_g is unloaded (DLSS-G guide, section 18) only once DLSS_G_UNLOAD_DELAY_USEC and
	// DLSS_G_UNLOAD_DELAY_PRESENTS have both passed since DLSS-G's eOff. Unloading it right after eOff
	// leaves the frame rate capped at the display limit divided by the generation factor until quit
	// (Streamline/driver bug, see DLSSG-DEFERRED-UNLOAD-FIXES.md).
	static constexpr uint64_t DLSS_G_UNLOAD_DELAY_USEC = 2000000;
	static constexpr uint64_t DLSS_G_UNLOAD_DELAY_PRESENTS = 120;
	// Renderer, every frame DLSS-G stays off while sl.dlss_g is wanted. Keeps the first eOff's time.
	void dlssg_unload_schedule();
	// Renderer, every frame DLSS-G is wanted on: a pending unload is dropped, sl.dlss_g stays.
	void dlssg_unload_cancel();
	// After every real present: counts it, and unloads sl.dlss_g once both delays have passed.
	void dlssg_unload_present_tick();
	// Whether the renderer may turn DLSS-G on: frame generation asked for, and no switch to FSR waiting.
	bool dlssg_requested(bool p_frame_generation) const { return p_frame_generation && !fsr_switch_pending.load(); }
	// A switch to FSR frame generation waits for sl.dlss_g's deferred unload, without frame generation meanwhile.
	std::atomic<bool> fsr_switch_pending{ false };
	bool reflex_usable() const { return streamline_capabilities.reflex_available && reflex_loaded; }

	static void initialize(bool d3d12);

	// Whether Streamline is initialized for this API and not shut down.
	bool is_active(bool p_d3d12) const { return slInit != nullptr && is_d3d12 == p_d3d12; }
	// Main device only, before it is created: enumerates the features supported on its adapter and returns whether
	// Streamline is used for it, that is DLSS, DLSS-G, DLSS-RR or Reflex is supported there (ProgrammingGuide.md, 2.4).
	// Reflex only counts on an NVIDIA GPU (p_nvidia). When it returns false the caller drops Streamline's proxies, then
	// calls shutdown().
	bool select_adapter(const sl::AdapterInfo &p_adapter_info, bool p_nvidia, bool p_d3d12, const String &p_adapter_name);
	static constexpr uint32_t NVIDIA_VENDOR_ID = 0x10DE;
	// slShutdown(), and every Streamline function forgotten: nothing reaches Streamline afterwards and no feature is
	// reported available. Also when the main device is destroyed.
	void shutdown();

	static StreamlineContext &get();

	sl::FrameToken *last_token = nullptr;
	// Our own frame index, handed to slGetNewFrameToken. See get_new_frame_token().
	uint32_t frame_index = 0;
	// last_token has been sent with a draw. See get_frame_token_for_draw().
	bool last_token_drawn = false;

	// The token of the frame the render thread is drawing. Set by the draw command itself, so it
	// travels through the command queue with its frame and the main thread cannot renew it meanwhile.
	sl::FrameToken *render_token = nullptr;
	bool is_game = false;
	// Run the NIS sharpening pass after DLSS.
	bool dlss_sharpening = false;

	sl::ViewportHandle dlssg_viewport;
	int dlssg_delay = 0;
	// Generated frames the active DLSS-G context was set up with; a change re-sets the options.
	uint32_t dlssg_frames = 0;
	// Whether its options enable UI recomposition (HUD-less color and UI alpha tagged).
	bool dlssg_ui_recomposition = false;
	// Frames DLSS-G's enabling has waited for the HUD-less color and UI alpha to be tagged.
	int dlssg_hudless_wait = 0;
	// DLSS-G loaded for the current swap chain, and wanted for the next one (set when frame
	// generation is turned on, cleared by the deferred unload).
	bool dlssg_loaded = true;
	std::atomic<bool> dlssg_wanted{ false };
	// Real presents so far, and when (OS ticks, 0 = none pending) and at which present DLSS-G's eOff
	// started the deferred unload.
	std::atomic<uint64_t> presents{ 0 };
	std::atomic<uint64_t> dlssg_off_usec{ 0 };
	std::atomic<uint64_t> dlssg_off_presents{ 0 };
	bool reflex_loaded = true; // Reflex and PCL together.
	bool is_d3d12 = false;

	// shutdown() was called: initialize() does nothing again.
	bool is_shut_down = false;

#if STREAMLINE_ENABLED_D3D12
	void init_device_d3d12(void *d3d_device);
#endif

	StreamlineCapabilities streamline_capabilities;
};
#endif
