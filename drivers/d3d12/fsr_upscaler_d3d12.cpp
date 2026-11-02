/**************************************************************************/
/*  fsr_upscaler_d3d12.cpp                                                */
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

#include "fsr_upscaler_d3d12.h"

#include "core/os/mutex.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "thirdparty/amd-ffx-api/api/include/dx12/ffx_api_dx12.h"
#include "thirdparty/amd-ffx-api/api/include/ffx_api.h"
#include "thirdparty/amd-ffx-api/upscalers/include/ffx_upscale.h"

#include <windows.h>

#include <d3d12.h>

// Follows AMD's FSR upscaler documentation (FidelityFX SDK, docs/techniques/super-resolution-upscaler.md) and its
// defaults: no constant overrides (FFX_API_CONFIGURE_UPSCALE_KEY_*), no provider override.

namespace {

struct State {
	BinaryMutex mutex;
	HMODULE lib = nullptr;
	bool load_attempted = false;
	PfnFfxCreateContext create_context = nullptr;
	PfnFfxDestroyContext destroy_context = nullptr;
	PfnFfxQuery query = nullptr;
	PfnFfxDispatch dispatch = nullptr;

	void *probed_device = nullptr;
	bool available = false;
};

State &state() {
	static State s;
	return s;
}

bool load_library(State &s) {
	if (s.load_attempted) {
		return s.lib != nullptr;
	}
	s.load_attempted = true;

	// The same module as FSR frame generation's: LoadLibrary counts the references.
	s.lib = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
	if (!s.lib) {
		return false;
	}
	s.create_context = (PfnFfxCreateContext)(void *)GetProcAddress(s.lib, "ffxCreateContext");
	s.destroy_context = (PfnFfxDestroyContext)(void *)GetProcAddress(s.lib, "ffxDestroyContext");
	s.query = (PfnFfxQuery)(void *)GetProcAddress(s.lib, "ffxQuery");
	s.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(s.lib, "ffxDispatch");
	if (!s.create_context || !s.destroy_context || !s.query || !s.dispatch) {
		ERR_PRINT("FSR upscaler: amd_fidelityfx_loader_dx12.dll is missing ffx-api entry points.");
		FreeLibrary(s.lib);
		s.lib = nullptr;
		return false;
	}
	return true;
}

void message_callback(uint32_t p_type, const wchar_t *p_message) {
	const String message = String::utf16((const char16_t *)p_message);
	if (p_type == FFX_API_MESSAGE_TYPE_ERROR) {
		ERR_PRINT("FSR upscaler: " + message);
	} else {
		WARN_PRINT("FSR upscaler: " + message);
	}
}

// RenderingDevice creates its D3D12 textures in the typeless format of their family (views pick the type). AMD reads
// them through views of the format announced here: the type RenderingDevice's own views use.
uint32_t typed_surface_format(DXGI_FORMAT p_format) {
	switch (p_format) {
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
			return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_R16G16_TYPELESS:
			return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
		case DXGI_FORMAT_R32G32_TYPELESS:
			return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
		case DXGI_FORMAT_R32_TYPELESS:
			return FFX_API_SURFACE_FORMAT_R32_FLOAT;
		case DXGI_FORMAT_R8_TYPELESS:
			return FFX_API_SURFACE_FORMAT_R8_UNORM;
		default:
			return ffxApiGetSurfaceFormatDX12(p_format);
	}
}

FfxApiResource get_resource(uint64_t p_resource) {
	ID3D12Resource *resource = (ID3D12Resource *)p_resource;
	// COMMON: the render graph holds them in D3D12_BARRIER_LAYOUT_COMMON (GENERAL usage), the only layout where
	// FidelityFX's legacy barriers may meet Godot's enhanced ones (RenderingDeviceDriverD3D12::_check_capabilities()).
	FfxApiResource res = ffxApiGetResourceDX12(resource, FFX_API_RESOURCE_STATE_COMMON);
	if (resource) {
		res.description.format = typed_surface_format(resource->GetDesc().Format);
	}
	return res;
}

} // namespace

bool FSRUpscalerD3D12::is_available(void *p_device) {
	State &s = state();
	MutexLock lock(s.mutex);
	if (!p_device) {
		return false;
	}
	if (s.probed_device == p_device) {
		return s.available;
	}
	s.probed_device = p_device;
	s.available = false;
	if (!load_library(s)) {
		return false;
	}

	uint64_t count = 0;
	ffxQueryDescGetVersions q = {};
	q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
	q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	q.device = p_device;
	q.outputCount = &count;
	if (s.query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) {
		print_line("FSR upscaler: no upscaling provider for this device (amd_fidelityfx_upscaler_dx12.dll missing, or an unsupported GPU).");
		return false;
	}
	uint64_t ids[16] = {};
	const char *names[16] = {};
	count = MIN(count, (uint64_t)16);
	q.versionIds = ids;
	q.versionNames = names;
	if (s.query(nullptr, &q.header) == FFX_API_RETURN_OK) {
		for (uint64_t i = 0; i < count; i++) {
			print_verbose(vformat("FSR upscaler: available provider: %s", names[i] ? names[i] : "?"));
		}
	}
	s.available = true;
	return true;
}

void *FSRUpscalerD3D12::create_context(const ContextParams &p_params) {
	State &s = state();
	MutexLock lock(s.mutex);
	if (!load_library(s)) {
		return nullptr;
	}

	ffxCreateContextDescUpscale create = {};
	create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	// AMD's documented defaults ("Exposure"): its auto exposure, unless there is a particular reason not to.
	create.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
	if (p_params.high_dynamic_range) {
		create.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
	}
	if (p_params.depth_inverted) {
		create.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
	}
	if (p_params.debug_checking) {
		create.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
		create.fpMessage = message_callback;
	}
	create.maxRenderSize = { p_params.max_render_width, p_params.max_render_height };
	create.maxUpscaleSize = { p_params.upscale_width, p_params.upscale_height };

	ffxCreateContextDescUpscaleVersion version = {};
	version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
	version.version = FFX_UPSCALER_VERSION;

	ffxCreateBackendDX12Desc backend = {};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend.device = (ID3D12Device *)p_params.device;

	create.header.pNext = &version.header;
	version.header.pNext = &backend.header;

	ffxContext context = nullptr;
	const ffxReturnCode_t rc = s.create_context(&context, &create.header, nullptr);
	if (rc != FFX_API_RETURN_OK) {
		ERR_PRINT(vformat("FSR upscaler: creating the upscaling context failed (ffx return code %d).", (int)rc));
		return nullptr;
	}

	ffxQueryGetProviderVersion provider = {};
	provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
	if (s.query(&context, &provider.header) == FFX_API_RETURN_OK && provider.versionName) {
		print_line(vformat("FSR upscaler: context created, provider %s, render %dx%d, output %dx%d.", provider.versionName, p_params.max_render_width, p_params.max_render_height, p_params.upscale_width, p_params.upscale_height));
	}
	return context;
}

void FSRUpscalerD3D12::destroy_context(void *p_context) {
	if (!p_context) {
		return;
	}
	State &s = state();
	MutexLock lock(s.mutex);
	ffxContext context = (ffxContext)p_context;
	s.destroy_context(&context, nullptr);
}

bool FSRUpscalerD3D12::dispatch(void *p_context, const DispatchParams &p_params) {
	ERR_FAIL_NULL_V(p_context, false);
	State &s = state();
	MutexLock lock(s.mutex);

	ffxDispatchDescUpscale desc = {};
	desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	desc.commandList = p_params.native_command_list;
	desc.color = get_resource(p_params.color_resource);
	desc.depth = get_resource(p_params.depth_resource);
	desc.motionVectors = get_resource(p_params.motion_vectors_resource);
	desc.exposure = FfxApiResource({}); // FFX_UPSCALE_ENABLE_AUTO_EXPOSURE.
	desc.reactive = get_resource(p_params.reactive_resource);
	desc.transparencyAndComposition = FfxApiResource({});
	desc.output = get_resource(p_params.output_resource);
	desc.jitterOffset = { p_params.jitter_x, p_params.jitter_y };
	desc.motionVectorScale = { p_params.motion_vector_scale_x, p_params.motion_vector_scale_y };
	desc.renderSize = { p_params.render_width, p_params.render_height };
	desc.upscaleSize = { p_params.upscale_width, p_params.upscale_height };
	desc.enableSharpening = p_params.sharpening;
	desc.sharpness = p_params.sharpness;
	desc.frameTimeDelta = p_params.frame_time_delta_ms;
	desc.preExposure = 1.0f; // As the FSR2 path: Godot does not pre-expose its color buffer.
	desc.reset = p_params.reset;
	desc.cameraNear = p_params.camera_near;
	desc.cameraFar = p_params.camera_far;
	desc.cameraFovAngleVertical = p_params.fov_vertical_radians;
	desc.viewSpaceToMetersFactor = 1.0f; // Godot's unit is the meter.
	desc.flags = 0;

	ffxContext context = (ffxContext)p_context;
	const ffxReturnCode_t rc = s.dispatch(&context, &desc.header);
	if (rc != FFX_API_RETURN_OK) {
		ERR_PRINT_ONCE(vformat("FSR upscaler: dispatch failed (ffx return code %d).", (int)rc));
		return false;
	}
	return true;
}
