/**************************************************************************/
/*  rendering_device_driver.cpp                                           */
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

#include "rendering_device_driver.h"

#include "core/os/os.h"

/**************/
/**** MISC ****/
/**************/

uint64_t RenderingDeviceDriver::dedicated_texture_min_size() {
	// A texture freed from a shared block leaves a gap the allocator gives back to the device only once the whole block
	// is empty: replacements created before the old textures go fill those gaps, and the blocks stay reserved (a live
	// texture quality switch, a zone unload). Large textures get memory of their own, returned when they are freed. Both
	// allocators document their flag for "special, big resources, like fullscreen images used as attachments"
	// (VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT, D3D12MA::ALLOCATION_FLAG_COMMITTED); the threshold below that is
	// this engine's measured choice (VRAM-DEDICATED-TEXTURES-FIXES.md). Small textures stay pooled (creation cost, 64 KB
	// alignment).
	static const uint64_t min_size = []() {
		const String value = OS::get_singleton()->get_environment("GODOT_DEDICATED_TEXTURE_MB");
		// 1 MB measured best of 1/2/4 MB (godot3d: same load time and frame rate as off, D3D12 and Vulkan).
		const double megabytes = value.is_empty() ? 1.0 : value.to_float();
		return megabytes > 0.0 ? uint64_t(megabytes * 1024.0 * 1024.0) : uint64_t(0);
	}();
	return min_size;
}

uint64_t RenderingDeviceDriver::api_trait_get(ApiTrait p_trait) {
	// Sensible canonical defaults.
	switch (p_trait) {
		case API_TRAIT_HONORS_PIPELINE_BARRIERS:
			return 1;
		case API_TRAIT_SHADER_CHANGE_INVALIDATION:
			return SHADER_CHANGE_INVALIDATION_ALL_BOUND_UNIFORM_SETS;
		case API_TRAIT_TEXTURE_TRANSFER_ALIGNMENT:
			return 1;
		case API_TRAIT_TEXTURE_DATA_ROW_PITCH_STEP:
			return 1;
		case API_TRAIT_SECONDARY_VIEWPORT_SCISSOR:
			return 1;
		case API_TRAIT_BUFFER_CLEARS_WITH_COPY_ENGINE:
			return true;
		case API_TRAIT_TEXTURE_CLEARS_WITH_COPY_ENGINE:
			return true;
		case API_TRAIT_USE_GENERAL_IN_COPY_QUEUES:
			return false;
		case API_TRAIT_BUFFERS_REQUIRE_TRANSITIONS:
			return false;
		case API_TRAIT_TEXTURES_REQUIRE_LAYOUT_TRANSITIONS:
			return true;
		case API_TRAIT_TEXTURE_OUTPUTS_REQUIRE_CLEARS:
			return false;
		case API_TRAIT_RESUMABLE_RENDER_PASSES:
			return false;
		case API_TRAIT_ACCELERATION_STRUCTURE_INSTANCE_STANDARD_LAYOUT:
			return false;
		default:
			ERR_FAIL_V(0);
	}
}

void RenderingDeviceDriver::command_suspend_render_pass(CommandBufferID p_cmd_buffer) {
	ERR_FAIL_MSG("Resumable render passes are not supported by this driver.");
}

void RenderingDeviceDriver::command_resume_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, const Rect2i &p_rect) {
	ERR_FAIL_MSG("Resumable render passes are not supported by this driver.");
}

/******************/

void RenderingDeviceDriver::command_begin_compute_pass(CommandBufferID p_cmd_buffer) {
}

void RenderingDeviceDriver::command_end_compute_pass(CommandBufferID p_cmd_buffer) {
}

RenderingDeviceDriver::~RenderingDeviceDriver() {}
