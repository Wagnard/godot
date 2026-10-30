/**************************************************************************/
/*  rendering_context_driver_d3d12.cpp                                    */
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

#include "rendering_context_driver_d3d12.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "drivers/aftermath/aftermath.h"
#include "drivers/streamline/streamline.h"

GODOT_GCC_WARNING_PUSH_AND_IGNORE("-Wnon-virtual-dtor")
GODOT_CLANG_WARNING_PUSH_AND_IGNORE("-Wnon-virtual-dtor")
#include <dxcapi.h>
GODOT_GCC_WARNING_POP
GODOT_CLANG_WARNING_POP

#include <dxgi1_6.h>

#if !defined(_MSC_VER)
#include <thirdparty/directx_headers/include/dxguids/dxguids.h>

#include <guiddef.h>
#endif

using Microsoft::WRL::ComPtr;

// Note: symbols are not available in MinGW and old MSVC import libraries.
// GUID values from https://github.com/microsoft/DirectX-Headers/blob/7a9f4d06911d30eecb56a4956dab29dcca2709ed/include/directx/d3d12.idl#L5877-L5881
const GUID CLSID_D3D12DebugGodot = { 0xf2352aeb, 0xdd84, 0x49fe, { 0xb9, 0x7b, 0xa9, 0xdc, 0xfd, 0xcc, 0x1b, 0x4f } };
const GUID CLSID_D3D12SDKConfigurationGodot = { 0x7cda6aca, 0xa03e, 0x49c8, { 0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce } };
const GUID CLSID_D3D12DeviceRemovedExtendedDataGodot = { 0x4a75bbc4, 0x9ff4, 0x4ad8, { 0x9f, 0x18, 0xab, 0xae, 0x84, 0xdc, 0x5f, 0xf2 } };

#ifdef PIX_ENABLED
#if defined(__GNUC__)
#define _MSC_VER 1800
#endif
#define USE_PIX
#include <WinPixEventRuntime/pix3.h>
#if defined(__GNUC__)
#undef _MSC_VER
#endif
#endif

RenderingContextDriverD3D12::RenderingContextDriverD3D12() {}

RenderingContextDriverD3D12::~RenderingContextDriverD3D12() {
	// Let's release manually everything that may still be holding
	// onto the DLLs before freeing them.
	device_factory.Reset();
	dxgi_factory.Reset();

	if (lib_d3d12) {
		FreeLibrary(lib_d3d12);
	}
	if (lib_dxgi) {
		FreeLibrary(lib_dxgi);
	}
#ifdef DCOMP_ENABLED
	if (lib_dcomp) {
		FreeLibrary(lib_dcomp);
	}
#endif
}

Error RenderingContextDriverD3D12::_init_device_factory() {
	uint32_t agility_sdk_version = GLOBAL_GET("rendering/rendering_device/d3d12/agility_sdk_version");
	String agility_sdk_path = String(".\\") + Engine::get_singleton()->get_architecture_name();

	lib_d3d12 = LoadLibraryW(L"D3D12.dll");
	ERR_FAIL_NULL_V(lib_d3d12, ERR_CANT_CREATE);

	lib_dxgi = LoadLibraryW(L"DXGI.dll");
	ERR_FAIL_NULL_V(lib_dxgi, ERR_CANT_CREATE);

#ifdef DCOMP_ENABLED
	lib_dcomp = LoadLibraryW(L"Dcomp.dll");
	ERR_FAIL_NULL_V(lib_dcomp, ERR_CANT_CREATE);
#endif

	// Streamline only sees the devices it creates. A device from the Agility SDK's factory is handed to it with
	// slUpgradeInterface() (RenderingDeviceDriverD3D12::_initialize_device()); without that function, Streamline must
	// create the device, with the system's D3D12 runtime.
	if (Streamline::get_singleton()->get_internal_parameter(STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12GetInterface) &&
			!Streamline::get_singleton()->get_internal_parameter(STREAMLINE_INTERNAL_PARAMETER_FUNC_slUpgradeInterface)) {
		return OK;
	}

	// Note: symbol is not available in MinGW import library.
	PFN_D3D12_GET_INTERFACE d3d_D3D12GetInterface = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(lib_d3d12, "D3D12GetInterface");
	if (!d3d_D3D12GetInterface) {
		return OK; // Fallback to the system loader.
	}

	ComPtr<ID3D12SDKConfiguration1> sdk_config;
	HRESULT hr = d3d_D3D12GetInterface(CLSID_D3D12SDKConfigurationGodot, IID_PPV_ARGS(sdk_config.GetAddressOf()));
	if (SUCCEEDED(hr)) {
		// The SDK path is relative to the executable (Microsoft's D3D12 redistributable spec, "D3D12SDKPath"). Only try the
		// architecture folder (exported projects) when it holds D3D12Core.dll: a failed attempt prints "Unable to load
		// D3D12Core.dll" to the debugger, which reads as the cause of any later failure.
		const bool arch_folder = FileAccess::exists(OS::get_singleton()->get_executable_path().get_base_dir().path_join(Engine::get_singleton()->get_architecture_name()).path_join("D3D12Core.dll"));
		if (arch_folder) {
			hr = sdk_config->CreateDeviceFactory(agility_sdk_version, agility_sdk_path.ascii().get_data(), IID_PPV_ARGS(device_factory.GetAddressOf()));
		}
		if (!arch_folder || FAILED(hr)) {
			hr = sdk_config->CreateDeviceFactory(agility_sdk_version, ".\\", IID_PPV_ARGS(device_factory.GetAddressOf()));
		}
		// If both calls failed, device factory is going to be nullptr, and D3D12CreateDevice is going to be used as fallback.
		// The runtime then is the system's, silently: no shader model it lacks (6.9: shader execution reordering, opacity
		// micromaps), so say why.
		if (FAILED(hr)) {
			WARN_PRINT(vformat("Direct3D 12: the Agility SDK %d (D3D12Core.dll of that version, in %s or next to the executable) could not be loaded (0x%08ux). The system's Direct3D 12 runtime is used instead, and the shader models it lacks are unavailable.", agility_sdk_version, agility_sdk_path, (uint64_t)hr));
		}
	}
	return OK;
}

Error RenderingContextDriverD3D12::_initialize_debug_layers() {
	ComPtr<ID3D12Debug> debug_controller;
	HRESULT res;

	if (device_factory) {
		res = device_factory->GetConfigurationInterface(CLSID_D3D12DebugGodot, IID_PPV_ARGS(&debug_controller));
	} else {
		PFN_D3D12_GET_DEBUG_INTERFACE d3d_D3D12GetDebugInterface = (PFN_D3D12_GET_DEBUG_INTERFACE)(void *)GetProcAddress(lib_d3d12, "D3D12GetDebugInterface");
		ERR_FAIL_NULL_V(d3d_D3D12GetDebugInterface, ERR_CANT_CREATE);

		res = d3d_D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller));
	}

	// Microsoft's D3D12 redistributable spec, "D3D12 Debug Layer": the debug layer requires the Windows optional feature
	// "Graphics Tools" as well as the redistributable's D3D12SDKLayers.dll. Without it, D3D12Core.dll refuses the
	// D3D12SDKLayers.dll next to it ("requires that the latest SDKLayers for Windows 10 is installed").
	ERR_FAIL_COND_V_MSG(!SUCCEEDED(res), ERR_QUERY_FAILED, vformat("Direct3D 12: the debug layer is unavailable (0x%08ux). It requires the Windows optional feature \"Graphics Tools\" (Settings > System > Optional features), in addition to the Agility SDK's D3D12SDKLayers.dll next to D3D12Core.dll.", (uint64_t)res));
	debug_controller->EnableDebugLayer();
	debug_layer_enabled = true;

	// GODOT_D3D12_GBV=1 adds GPU-based validation: resource states and descriptors checked as the GPU executes (slow).
	if (OS::get_singleton()->get_environment("GODOT_D3D12_GBV") == "1") {
		ComPtr<ID3D12Debug1> debug_controller_1;
		if (SUCCEEDED(debug_controller.As(&debug_controller_1))) {
			debug_controller_1->SetEnableGPUBasedValidation(TRUE);
			print_line("D3D12: GPU-based validation enabled.");
		}
	}
	return OK;
}

void RenderingContextDriverD3D12::_initialize_dred() {
	// Microsoft's DRED (Device Removed Extended Data) is part of the runtime, without the debug layer or Graphics Tools. Its
	// settings apply to the devices created afterwards: by the device factory with the Agility SDK ("D3D12 independent
	// devices": GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData)), else globally.
	ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred_settings;
	HRESULT res;
	if (device_factory) {
		res = device_factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedDataGodot, IID_PPV_ARGS(&dred_settings));
	} else {
		PFN_D3D12_GET_DEBUG_INTERFACE d3d_D3D12GetDebugInterface = (PFN_D3D12_GET_DEBUG_INTERFACE)(void *)GetProcAddress(lib_d3d12, "D3D12GetDebugInterface");
		ERR_FAIL_NULL(d3d_D3D12GetDebugInterface);
		res = d3d_D3D12GetDebugInterface(IID_PPV_ARGS(&dred_settings));
	}
	ERR_FAIL_COND_MSG(!SUCCEEDED(res), vformat("D3D12: DRED is unavailable (0x%08ux).", (uint64_t)res));
	dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	dred_settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	dred_enabled = true;
	print_line("D3D12: DRED auto-breadcrumbs, breadcrumb contexts and page fault reporting enabled.");
}

Error RenderingContextDriverD3D12::_create_dxgi_factory() {
	const UINT dxgi_factory_flags = use_validation_layers() ? DXGI_CREATE_FACTORY_DEBUG : 0;

	typedef HRESULT(WINAPI * PFN_DXGI_CREATE_DXGI_FACTORY2)(UINT, REFIID, void **);
	PFN_DXGI_CREATE_DXGI_FACTORY2 dxgi_CreateDXGIFactory2 = (PFN_DXGI_CREATE_DXGI_FACTORY2)(void *)GetProcAddress(lib_dxgi, "CreateDXGIFactory2");
	ERR_FAIL_NULL_V(dxgi_CreateDXGIFactory2, ERR_CANT_CREATE);

	dxgi_factory_streamline = Streamline::get_singleton()->get_internal_parameter(STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory2) != nullptr;
	if (dxgi_factory_streamline) {
		dxgi_CreateDXGIFactory2 = (PFN_DXGI_CREATE_DXGI_FACTORY2)Streamline::get_singleton()->get_internal_parameter(STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory2);
	}

	HRESULT res = dxgi_CreateDXGIFactory2(dxgi_factory_flags, IID_PPV_ARGS(&dxgi_factory));
	ERR_FAIL_COND_V(!SUCCEEDED(res), ERR_CANT_CREATE);

	return OK;
}

Error RenderingContextDriverD3D12::_initialize_devices() {
	// Create the initial DXGI factory.
	Error err = _create_dxgi_factory();
	ERR_FAIL_COND_V(err != OK, err);

	HRESULT res;

	// Enumerate all possible adapters.
	LocalVector<IDXGIAdapter1 *> adapters;
	IDXGIAdapter1 *adapter = nullptr;
	do {
		adapter = create_adapter(adapters.size());
		if (adapter != nullptr) {
			adapters.push_back(adapter);
		}
	} while (adapter != nullptr);

	ERR_FAIL_COND_V_MSG(adapters.is_empty(), ERR_CANT_CREATE, "Adapters enumeration reported zero accessible devices.");

	// Fill the device descriptions with the adapters.
	driver_devices.resize(adapters.size());
	for (uint32_t i = 0; i < adapters.size(); ++i) {
		DXGI_ADAPTER_DESC1 desc = {};
		adapters[i]->GetDesc1(&desc);

		Device &device = driver_devices[i];
		device.name = desc.Description;
		device.vendor = desc.VendorId;

		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
			device.type = DEVICE_TYPE_CPU;
		} else {
			const bool has_dedicated_vram = desc.DedicatedVideoMemory > 0;
			device.type = has_dedicated_vram ? DEVICE_TYPE_DISCRETE_GPU : DEVICE_TYPE_INTEGRATED_GPU;
		}
	}

	// Release all created adapters.
	for (uint32_t i = 0; i < adapters.size(); ++i) {
		adapters[i]->Release();
	}

	ComPtr<IDXGIFactory5> factory_5;
	dxgi_factory.As(&factory_5);
	if (factory_5 != nullptr) {
		// The type is important as in general, sizeof(bool) != sizeof(BOOL).
		BOOL feature_supported = FALSE;
		res = factory_5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &feature_supported, sizeof(feature_supported));
		if (SUCCEEDED(res)) {
			tearing_supported = feature_supported;
		} else {
			ERR_PRINT("CheckFeatureSupport failed with error " + vformat("0x%08ux", (uint64_t)res) + ".");
		}
	}

	return OK;
}

bool RenderingContextDriverD3D12::use_validation_layers() const {
	// What --gpu-validation got, not what it asked for: the DXGI debug factory and the device's info queue need the
	// debug layer.
	return debug_layer_enabled;
}

Error RenderingContextDriverD3D12::initialize() {
	Error err = _init_device_factory();
	ERR_FAIL_COND_V(err != OK, ERR_CANT_CREATE);

	// Without the debug layer, stay on Direct3D 12 (the error above says why): failing here would switch to Vulkan, and
	// the run would validate another driver than the one asked for.
	if (Engine::get_singleton()->is_validation_layers_enabled()) {
		err = _initialize_debug_layers();
		if (err != OK) {
			ERR_PRINT("Direct3D 12: --gpu-validation requested, but running WITHOUT the debug layer.");
		}
	}
	if (OS::get_singleton()->get_environment("GODOT_D3D12_DRED") == "1") {
		_initialize_dred();
	}

	err = _initialize_devices();
	ERR_FAIL_COND_V(err != OK, ERR_CANT_CREATE);

	return OK;
}

const RenderingContextDriver::Device &RenderingContextDriverD3D12::device_get(uint32_t p_device_index) const {
	DEV_ASSERT(p_device_index < driver_devices.size());
	return driver_devices[p_device_index];
}

uint32_t RenderingContextDriverD3D12::device_get_count() const {
	return driver_devices.size();
}

bool RenderingContextDriverD3D12::device_supports_present(uint32_t p_device_index, SurfaceID p_surface) const {
	// All devices should support presenting to any surface.
	return true;
}

RenderingDeviceDriver *RenderingContextDriverD3D12::driver_create() {
	return memnew(RenderingDeviceDriverD3D12(this));
}

void RenderingContextDriverD3D12::driver_free(RenderingDeviceDriver *p_driver) {
	memdelete(p_driver);
}

RenderingContextDriver::SurfaceID RenderingContextDriverD3D12::surface_create(const void *p_platform_data) {
	const WindowPlatformData *wpd = (const WindowPlatformData *)(p_platform_data);
	Surface *surface = memnew(Surface);
	surface->hwnd = wpd->window;
	return SurfaceID(surface);
}

void RenderingContextDriverD3D12::surface_set_size(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) {
	Surface *surface = (Surface *)(p_surface);
	surface->width = p_width;
	surface->height = p_height;
	surface->needs_resize = true;
}

void RenderingContextDriverD3D12::surface_set_vsync_mode(SurfaceID p_surface, DisplayServerEnums::VSyncMode p_vsync_mode) {
	Surface *surface = (Surface *)(p_surface);
	surface->vsync_mode = p_vsync_mode;
	surface->needs_resize = true;
}

DisplayServerEnums::VSyncMode RenderingContextDriverD3D12::surface_get_vsync_mode(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->vsync_mode;
}

void RenderingContextDriverD3D12::surface_set_hdr_output_enabled(SurfaceID p_surface, bool p_enabled) {
	Surface *surface = (Surface *)(p_surface);
	surface->hdr_output = p_enabled;
	surface->needs_resize = true;
}

bool RenderingContextDriverD3D12::surface_get_hdr_output_enabled(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->hdr_output;
}

void RenderingContextDriverD3D12::surface_set_hdr_output_reference_luminance(SurfaceID p_surface, float p_reference_luminance) {
	Surface *surface = (Surface *)(p_surface);
	surface->hdr_reference_luminance = p_reference_luminance;
}

float RenderingContextDriverD3D12::surface_get_hdr_output_reference_luminance(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->hdr_reference_luminance;
}

void RenderingContextDriverD3D12::surface_set_hdr_output_max_luminance(SurfaceID p_surface, float p_max_luminance) {
	Surface *surface = (Surface *)(p_surface);
	surface->hdr_max_luminance = p_max_luminance;
}

float RenderingContextDriverD3D12::surface_get_hdr_output_max_luminance(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->hdr_max_luminance;
}

void RenderingContextDriverD3D12::surface_set_hdr_output_linear_luminance_scale(SurfaceID p_surface, float p_linear_luminance_scale) {
	Surface *surface = (Surface *)(p_surface);
	surface->hdr_linear_luminance_scale = p_linear_luminance_scale;
}

float RenderingContextDriverD3D12::surface_get_hdr_output_linear_luminance_scale(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->hdr_linear_luminance_scale;
}

float RenderingContextDriverD3D12::surface_get_hdr_output_max_value(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return MAX(surface->hdr_max_luminance / MAX(surface->hdr_reference_luminance, 1.0f), 1.0f);
}

uint32_t RenderingContextDriverD3D12::surface_get_width(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->width;
}

uint32_t RenderingContextDriverD3D12::surface_get_height(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->height;
}

void RenderingContextDriverD3D12::surface_set_needs_resize(SurfaceID p_surface, bool p_needs_resize) {
	Surface *surface = (Surface *)(p_surface);
	surface->needs_resize = p_needs_resize;
}

bool RenderingContextDriverD3D12::surface_get_needs_resize(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->needs_resize;
}

void RenderingContextDriverD3D12::surface_destroy(SurfaceID p_surface) {
	Surface *surface = (Surface *)(p_surface);
	memdelete(surface);
}

bool RenderingContextDriverD3D12::is_debug_utils_enabled() const {
#ifdef DEV_ENABLED
	return true;
#else
	if (Engine::get_singleton() && Engine::get_singleton()->is_gpu_markers_enabled()) {
		return true;
	}
	if (OS::get_singleton() && OS::get_singleton()->is_stdout_verbose()) {
		return true;
	}
	return false;
#endif
}

IDXGIAdapter1 *RenderingContextDriverD3D12::create_adapter(uint32_t p_adapter_index) const {
	ComPtr<IDXGIFactory6> factory_6;
	dxgi_factory.As(&factory_6);

	// TODO: Use IDXCoreAdapterList, which gives more comprehensive information.
	IDXGIAdapter1 *adapter = nullptr;
	if (factory_6) {
		if (factory_6->EnumAdapterByGpuPreference(p_adapter_index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
			return nullptr;
		}
	} else {
		if (dxgi_factory->EnumAdapters1(p_adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
			return nullptr;
		}
	}

	return adapter;
}

ID3D12DeviceFactory *RenderingContextDriverD3D12::device_factory_get() const {
	return device_factory.Get();
}

IDXGIFactory2 *RenderingContextDriverD3D12::dxgi_factory_get() {
	// Check if the factory is still current. It can become invalid if displays change
	if (dxgi_factory && !dxgi_factory->IsCurrent()) {
		dxgi_factory.Reset();
		_create_dxgi_factory();
	}
	return dxgi_factory.Get();
}

Error RenderingContextDriverD3D12::shutdown_streamline() {
	// Streamline's proxies are released before Streamline itself.
	const bool recreate_factory = dxgi_factory_streamline;
	if (recreate_factory) {
		dxgi_factory.Reset();
	}
	Streamline::get_singleton()->shutdown();
	return recreate_factory ? _create_dxgi_factory() : OK;
}

bool RenderingContextDriverD3D12::get_tearing_supported() const {
	return tearing_supported;
}
