/**************************************************************************/
/*  streamline.cpp                                                        */
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

#include "streamline.h"

#ifdef D3D12_ENABLED
#include "drivers/d3d12/fsr_frame_generation_d3d12.h"
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/object.h"
#include "core/os/os.h"
#ifdef STREAMLINE_ENABLED
#include "drivers/streamline/streamline_context.h"
#endif

Streamline *Streamline::singleton = nullptr;

void Streamline::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_parameter", "parameter_type", "value"), &Streamline::set_parameter);
	ClassDB::bind_method(D_METHOD("get_capability", "capability_type"), &Streamline::get_capability);

	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_REFLEX_MODE);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_DLSS_PRESET);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_DLSS_RR_PRESET);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_FRAME_GENERATION_PROVIDER);

	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_DLSS);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_DLSS_G);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_DLSS_RR);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_NIS);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_REFLEX);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_PCL);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_FSR_FRAME_GENERATION);
}

void Streamline::register_singleton() {
	GDREGISTER_CLASS(Streamline);
	Engine::get_singleton()->add_singleton(Engine::Singleton("Streamline", Streamline::get_singleton()));

	GLOBAL_DEF("rendering/streamline/streamline_log", false);
	GLOBAL_DEF("rendering/streamline/streamline_imgui", false);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/streamline/reflex_mode", PROPERTY_HINT_RANGE, "0,2,1"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/streamline/reflex_frame_limit_us", PROPERTY_HINT_RANGE, "0,1000000,1"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "rendering/streamline/dlss_preset", PROPERTY_HINT_ENUM, "?,F,G,H,I,J,K,L,M,N,O"), "?");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "rendering/streamline/dlss_ray_reconstruction_preset", PROPERTY_HINT_ENUM, "?,D,E,F,G,H,I,J,K,L,M,N,O"), "?");
	GLOBAL_DEF("rendering/streamline/dlss_sharpening", false);
}

Streamline *Streamline::get_singleton() {
	return singleton;
}

Streamline::Streamline() {
	singleton = this;
}

Streamline::~Streamline() {
	singleton = nullptr;
}

void Streamline::update_project_settings() {
#ifdef STREAMLINE_ENABLED
	StreamlineContext::get().dlss_sharpening = bool(GLOBAL_GET("rendering/streamline/dlss_sharpening"));
	set_parameter(STREAMLINE_PARAM_REFLEX_MODE, (double)GLOBAL_GET("rendering/streamline/reflex_mode"));
	set_parameter(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US, (double)GLOBAL_GET("rendering/streamline/reflex_frame_limit_us"));
	set_parameter(STREAMLINE_PARAM_DLSS_PRESET, GLOBAL_GET("rendering/streamline/dlss_preset"));
	set_parameter(STREAMLINE_PARAM_DLSS_RR_PRESET, GLOBAL_GET("rendering/streamline/dlss_ray_reconstruction_preset"));
#endif
}

static thread_local uint64_t present_frame_token = 0;

uint64_t Streamline::get_render_frame_token() const {
#ifdef STREAMLINE_ENABLED
	return (uint64_t)StreamlineContext::get().render_token;
#else
	return 0;
#endif
}

void Streamline::set_present_frame_token(uint64_t p_token) {
	present_frame_token = p_token;
}

Mutex &Streamline::get_swap_chain_mutex() {
	static Mutex swap_chain_mutex;
	return swap_chain_mutex;
}

void Streamline::set_latency_sleep_callback(LatencySleepCallback p_callback, void *p_userdata) {
#ifdef STREAMLINE_ENABLED
	// Set when the device is created and cleared before it is destroyed, outside the main loop that sleeps.
	StreamlineContext::get().latency_sleep_callback = p_callback;
	StreamlineContext::get().latency_sleep_userdata = p_userdata;
#endif
}

bool Streamline::defer_switch_to_fsr() {
#ifdef STREAMLINE_ENABLED
	StreamlineContext &sl_context = StreamlineContext::get();
	if (!sl_context.is_game || !sl_context.is_d3d12 || !sl_context.dlssg_wanted) {
		return false;
	}
	sl_context.fsr_switch_pending = true;
	// With DLSS-G on, the renderer sends eOff on its next frame and starts the unload delay then.
	if ((uint32_t)sl_context.dlssg_viewport == UINT_MAX) {
		sl_context.dlssg_unload_schedule();
	}
	return true;
#else
	return false;
#endif
}

bool Streamline::cancel_switch_to_fsr() {
#ifdef STREAMLINE_ENABLED
	return StreamlineContext::get().fsr_switch_pending.exchange(false);
#else
	return false;
#endif
}

void Streamline::emit_marker(StreamlineMarkerType marker) {
#ifdef STREAMLINE_ENABLED
	StreamlineContext &sl_context = StreamlineContext::get();

	switch (marker) {
		case StreamlineMarkerType::STREAMLINE_MARKER_INITIALIZE_VULKAN:
		case StreamlineMarkerType::STREAMLINE_MARKER_INITIALIZE_D3D12:
			sl_context.initialize(marker == StreamlineMarkerType::STREAMLINE_MARKER_INITIALIZE_D3D12);
			return;
		case StreamlineMarkerType::STREAMLINE_MARKER_AFTER_DEVICE_CREATION:
			sl_context.load_functions_post_init();

			if (sl_context.streamline_capabilities.pcl_available) {
				sl::PCLOptions pcl_options = {};
				pcl_options.virtualKey = sl::PCLHotKey::eVK_F13;
				pcl_options.idThread = 0;
				sl_context.pcl_set_options(pcl_options);
			}

			if (sl_context.streamline_capabilities.reflex_available) {
				sl::ReflexOptions reflex_options = {};
				reflex_options.frameLimitUs = 0;
				reflex_options.virtualKey = (uint16_t)sl::PCLHotKey::eVK_F13;
				reflex_options.mode = sl::ReflexMode::eOff;
				reflex_options.useMarkersToOptimize = false;
				reflex_options.idThread = 0;
				sl_context.reflex_set_options(reflex_options);
			}

			// load initial project settings
			update_project_settings();
			return;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEFORE_SWAPCHAIN_CREATION: {
			// FSR frame generation exists on D3D12 only.
			bool fsr = false;
#ifdef D3D12_ENABLED
			fsr = sl_context.is_d3d12 && FSRFrameGenerationD3D12::is_requested();
#endif
			sl_context.apply_frame_generation_features(fsr);
			return;
		}
		case StreamlineMarkerType::STREAMLINE_MARKER_MODIFY_SWAPCHAIN:
			sl_context.dlssg_disable();
			return;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEFORE_DEVICE_DESTROY:
			sl_context.shutdown();
			return;
		default:
			break;
	}

	// sl.dlss_g's deferred unload counts real presents, with or without Reflex.
	if (marker == StreamlineMarkerType::STREAMLINE_MARKER_END_PRESENT && sl_context.is_game) {
		sl_context.dlssg_unload_present_tick();
	}

	if (!sl_context.is_game || !sl_context.reflex_usable()) {
		// Make sure we still get frame tokens, needed for DLSS.
		if (marker == StreamlineMarkerType::STREAMLINE_MARKER_BEFORE_MESSAGE_LOOP) {
			sl_context.get_new_frame_token();
		}
		return;
	}

	sl::PCLMarker sl_marker = sl::PCLMarker::eMaximum;
	switch (marker) {
		case StreamlineMarkerType::STREAMLINE_MARKER_BEFORE_MESSAGE_LOOP:
			if (sl_context.dlssg_delay > 0) {
				--sl_context.dlssg_delay;
			}
			if (sl_context.pcl_options_dirty) {
				sl_context.pcl_set_options(sl_context.pcl_options);
			}
			if (sl_context.reflex_options_dirty) {
				sl_context.reflex_set_options(sl_context.reflex_options);
			}

			sl_context.get_new_frame_token();
			if (sl_context.reflex_options.mode != sl::ReflexMode::eOff || sl_context.reflex_options.frameLimitUs > 0) {
				sl_context.reflex_sleep(sl_context.last_token);
			}
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEGIN_RENDER:
			sl_marker = sl::PCLMarker::eRenderSubmitStart;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_END_RENDER:
			sl_marker = sl::PCLMarker::eRenderSubmitEnd;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEGIN_SIMULATION:
			sl_marker = sl::PCLMarker::eSimulationStart;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_END_SIMULATION:
			sl_marker = sl::PCLMarker::eSimulationEnd;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEGIN_PRESENT:
			sl_marker = sl::PCLMarker::ePresentStart;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_END_PRESENT:
			sl_marker = sl::PCLMarker::ePresentEnd;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_PC_PING:
			sl_marker = sl::PCLMarker::ePCLatencyPing;
			break;
		default:
			return;
	}

	// With a separate render thread, the main thread may already have renewed last_token. DLSS-G requires
	// the render and present markers to carry the frame index of the drawn frame's constants.
	sl::FrameToken *token = sl_context.last_token;
	switch (marker) {
		case StreamlineMarkerType::STREAMLINE_MARKER_BEGIN_RENDER:
		case StreamlineMarkerType::STREAMLINE_MARKER_END_RENDER:
		case StreamlineMarkerType::STREAMLINE_MARKER_BEGIN_PRESENT:
		case StreamlineMarkerType::STREAMLINE_MARKER_END_PRESENT:
			if (present_frame_token != 0) {
				token = (sl::FrameToken *)present_frame_token; // Submission thread: the presented frame's token.
			} else if (sl_context.render_token != nullptr) {
				token = sl_context.render_token;
			}
			break;
		default:
			break;
	}
	if (token && sl_marker != sl::PCLMarker::eMaximum) {
		if (sl_context.is_d3d12) {
			sl_context.pcl_marker(token, sl_marker);
		} else {
			MutexLock lock(get_swap_chain_mutex());
			sl_context.pcl_marker(token, sl_marker);
		}
	}
#endif
}
void Streamline::set_parameter(StreamlineParameterType p_parameter_type, const Variant &p_value) {
	_THREAD_SAFE_METHOD_
	switch (p_parameter_type) {
#ifdef STREAMLINE_ENABLED
		case StreamlineParameterType::STREAMLINE_PARAM_REFLEX_MODE: {
			double val = p_value;
			sl::ReflexMode newMode;
			if (val > 1.0) {
				newMode = sl::ReflexMode::eLowLatencyWithBoost;
			} else if (val > 0.0) {
				newMode = sl::ReflexMode::eLowLatency;
			} else {
				newMode = sl::ReflexMode::eOff;
			}

			if (StreamlineContext::get().reflex_options.mode != newMode) {
				StreamlineContext::get().reflex_options_dirty = true;
			}
			StreamlineContext::get().reflex_options.mode = newMode;
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US: {
			double val = p_value;
			bool updated = StreamlineContext::get().reflex_options.frameLimitUs != (unsigned int)val;
			StreamlineContext::get().reflex_options.frameLimitUs = (unsigned int)val;
			if (updated) {
				StreamlineContext::get().reflex_options_dirty = true;
			}
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_DLSS_PRESET: {
			if (p_value.is_string()) {
				String preset = p_value;
				if (preset.length() == 1) {
					StreamlineContext::get().dlss_default_preset = (char)preset.get(0);
				}
			}
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_FRAME_GENERATION_PROVIDER: {
#ifdef D3D12_ENABLED
			// FSR frame generation exists on D3D12 only (STREAMLINE_CAPABILITY_FSR_FRAME_GENERATION): elsewhere a switch
			// would only recreate the swap chain.
			if (OS::get_singleton()->get_current_rendering_driver_name() == "d3d12") {
				FSRFrameGenerationD3D12::set_requested(int(p_value) == 1);
			}
#endif
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_DLSS_RR_PRESET: {
			if (p_value.is_string()) {
				String preset = p_value;
				if (preset.length() == 1) {
					StreamlineContext::get().dlss_rr_default_preset = (char)preset.get(0);
				}
			}
			break;
		}
#endif
		default:
			break;
	}
}

bool Streamline::get_capability(StreamlineCapabilityType p_capability_type) {
#ifdef STREAMLINE_ENABLED
	_THREAD_SAFE_METHOD_
	switch (p_capability_type) {
		case STREAMLINE_CAPABILITY_DLSS:
			return StreamlineContext::get().streamline_capabilities.dlss_available;
		case STREAMLINE_CAPABILITY_DLSS_G:
			return StreamlineContext::get().streamline_capabilities.dlss_g_available;
		case STREAMLINE_CAPABILITY_DLSS_RR:
			return StreamlineContext::get().streamline_capabilities.dlss_rr_available;
		case STREAMLINE_CAPABILITY_NIS:
			return StreamlineContext::get().streamline_capabilities.nis_available;
		case STREAMLINE_CAPABILITY_FSR_FRAME_GENERATION:
#ifdef D3D12_ENABLED
			return OS::get_singleton()->get_current_rendering_driver_name() == "d3d12" && FSRFrameGenerationD3D12::is_available();
#else
			return false;
#endif
		case STREAMLINE_CAPABILITY_REFLEX:
			return StreamlineContext::get().streamline_capabilities.reflex_available;
		case STREAMLINE_CAPABILITY_PCL:
			return StreamlineContext::get().streamline_capabilities.pcl_available;
	}
#endif
	return false;
}
bool Streamline::is_active(bool p_d3d12) const {
#ifdef STREAMLINE_ENABLED
	return StreamlineContext::get().is_active(p_d3d12);
#else
	return false;
#endif
}

bool Streamline::select_adapter_vulkan(void *p_physical_device, const uint8_t *p_luid, uint32_t p_luid_size, uint32_t p_vendor_id, const String &p_adapter_name) {
#ifdef STREAMLINE_ENABLED
	_THREAD_SAFE_METHOD_
	const bool nvidia = p_vendor_id == StreamlineContext::NVIDIA_VENDOR_ID;
	if (p_luid == nullptr && !nvidia) {
		// Without a LUID Streamline would answer for the whole system, not for this GPU.
		print_line(vformat("Streamline: %s has no LUID and is not an NVIDIA GPU: Streamline is not used.", p_adapter_name));
		StreamlineContext::get().streamline_capabilities = StreamlineCapabilities();
		return false;
	}
	sl::AdapterInfo adapter_info;
	adapter_info.deviceLUID = (uint8_t *)p_luid;
	adapter_info.deviceLUIDSizeInBytes = p_luid ? p_luid_size : 0;
	adapter_info.vkPhysicalDevice = p_physical_device;
	return StreamlineContext::get().select_adapter(adapter_info, nvidia, false, p_adapter_name);
#else
	return false;
#endif
}

bool Streamline::select_adapter_d3d12(const void *p_adapter_luid, uint32_t p_luid_size, uint32_t p_vendor_id, const String &p_adapter_name) {
#ifdef STREAMLINE_ENABLED
	_THREAD_SAFE_METHOD_
	sl::AdapterInfo adapter_info;
	adapter_info.deviceLUID = (uint8_t *)p_adapter_luid;
	adapter_info.deviceLUIDSizeInBytes = p_luid_size;
	return StreamlineContext::get().select_adapter(adapter_info, p_vendor_id == StreamlineContext::NVIDIA_VENDOR_ID, true, p_adapter_name);
#else
	return false;
#endif
}

void Streamline::shutdown() {
#ifdef STREAMLINE_ENABLED
	_THREAD_SAFE_METHOD_
	StreamlineContext::get().shutdown();
#endif
}

void Streamline::set_internal_parameter(const char *key, void *value) {
#ifdef STREAMLINE_ENABLED
	_THREAD_SAFE_METHOD_
#if STREAMLINE_ENABLED_D3D12
	if (strcmp(key, "d3d12_device") == 0) {
		void *d3d_device = (void *)value;
		StreamlineContext::get().init_device_d3d12(d3d_device);
	}
#endif
#endif
}

void *Streamline::get_internal_parameter(StreamlineInternalParameterType p_internal_parameter_type) {
#ifdef STREAMLINE_ENABLED
#if STREAMLINE_ENABLED_D3D12
	switch (p_internal_parameter_type) {
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12GetInterface:
			return StreamlineContext::get().func_D3D12GetInterface;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12CreateDevice:
			return StreamlineContext::get().func_D3D12CreateDevice;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_slUpgradeInterface:
			return StreamlineContext::get().func_slUpgradeInterface;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_DXGIGetDebugInterface1:
			return StreamlineContext::get().func_DXGIGetDebugInterface1;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory:
			return StreamlineContext::get().func_CreateDXGIFactory;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory1:
			return StreamlineContext::get().func_CreateDXGIFactory1;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory2:
			return StreamlineContext::get().func_CreateDXGIFactory2;
	}
#endif
#endif
	return nullptr;
}
