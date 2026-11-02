/**************************************************************************/
/*  fsr_upscaler_vulkan.h                                                 */
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

// AMD FSR upscaling through the FidelityFX API on Vulkan (SDK 2.4.0+): amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll, loaded at run time from the executable's folder. The Vulkan twin of FSRUpscalerD3D12,
// with the same rules: the DLL picks the provider, no Vulkan type in the interface, a mutex around every call.
class FSRUpscalerVulkan {
public:
	struct ContextParams {
		void *device = nullptr; // VkDevice.
		void *physical_device = nullptr; // VkPhysicalDevice.
		uint32_t max_render_width = 0;
		uint32_t max_render_height = 0;
		uint32_t upscale_width = 0;
		uint32_t upscale_height = 0;
		bool high_dynamic_range = true;
		bool depth_inverted = true;
		bool debug_checking = false; // FFX_UPSCALE_ENABLE_DEBUG_CHECKING, to the log.
	};

	// What AMD needs to describe an image it did not create.
	struct Texture {
		uint64_t image = 0; // VkImage, or 0.
		uint32_t format = 0; // VkFormat of the view RenderingDevice reads it through.
		uint32_t usage = 0; // VkImageUsageFlags of the image.
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t layers = 1;
		uint32_t mipmaps = 1;
	};

	// Every image is in VK_IMAGE_LAYOUT_GENERAL (the render graph's GENERAL usage), announced
	// FFX_API_RESOURCE_STATE_COMMON, which AMD's Vulkan backend maps to the same layout.
	struct DispatchParams {
		void *native_command_buffer = nullptr; // VkCommandBuffer, recording.
		Texture color;
		Texture depth;
		Texture motion_vectors; // Decoded: no (-1,-1) sentinel.
		Texture reactive; // R8_UNORM, or image 0.
		Texture output; // With VK_IMAGE_USAGE_STORAGE_BIT.
		uint32_t render_width = 0;
		uint32_t render_height = 0;
		uint32_t upscale_width = 0;
		uint32_t upscale_height = 0;
		float jitter_x = 0.0f; // In pixels.
		float jitter_y = 0.0f;
		float motion_vector_scale_x = 0.0f;
		float motion_vector_scale_y = 0.0f;
		bool sharpening = false;
		float sharpness = 0.0f;
		float frame_time_delta_ms = 0.0f;
		bool reset = false;
		float camera_near = 0.0f;
		float camera_far = 0.0f;
		float fov_vertical_radians = 0.0f;
	};

	// The loader loads and reports an upscaling provider for this device. Probed once per device, quietly.
	static bool is_available(void *p_device, void *p_physical_device);
	static void *create_context(const ContextParams &p_params);
	// The GPU must be done with every dispatch recorded with p_context.
	static void destroy_context(void *p_context);
	static bool dispatch(void *p_context, const DispatchParams &p_params);
};
