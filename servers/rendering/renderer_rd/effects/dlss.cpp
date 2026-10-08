/**************************************************************************/
/*  dlss.cpp                                                              */
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

#include "dlss.h"

#ifdef STREAMLINE_ENABLED
#define ENABLE_DLSS 1
#endif

#ifdef ENABLE_DLSS
#include "core/os/os.h"
#include "drivers/streamline/streamline.h"
#include "drivers/streamline/streamline_context.h"
#include "servers/rendering/renderer_rd/effects/camera_reprojection.h"

#ifdef D3D12_ENABLED
#include "drivers/d3d12/fsr_frame_generation_d3d12.h"
#endif
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

// kBufferTypeUIAlpha and DLSSGOptions::enableUserInterfaceRecomposition exist since Streamline 2.12;
// the headers in thirdparty/ are 2.10, where the option is the same field of the same struct
// version (kStructVersion4) under the name bReserved16. The runtimes that load (2.12 in bin/, the
// OTA DLSS-G plugin, 2.14.1 in exports) know both.
static constexpr sl::BufferType DLSS_BUFFER_TYPE_UI_ALPHA = 69; // sl::kBufferTypeUIAlpha

// The token of the frame being drawn: the one the draw command carried onto the render
// thread, or the main-thread token when no draw command set it — the very first frame, or a
// direct draw. See RenderingServerDefault::draw for why the main-thread token alone was not
// enough.
// GODOT_PARALLEL_RECORDING_STATS=1: where the DLSS callback's time goes, printed every 240 calls.
enum DLSSCallbackSection {
	DLSS_SECTION_OPTIONS,
	DLSS_SECTION_CONSTANTS,
	DLSS_SECTION_TAGS,
	DLSS_SECTION_FRAME_GENERATION,
	DLSS_SECTION_EVALUATE,
	DLSS_SECTION_REST,
	DLSS_SECTION_MAX,
};
static struct {
	int enabled = -1;
	uint32_t calls = 0;
	uint64_t usec[DLSS_SECTION_MAX] = {};
	uint64_t lap_usec = 0;
} dlss_callback_stats;

static void dlss_callback_lap(DLSSCallbackSection p_section) {
	if (dlss_callback_stats.enabled <= 0) {
		return;
	}
	const uint64_t now_usec = OS::get_singleton()->get_ticks_usec();
	dlss_callback_stats.usec[p_section] += now_usec - dlss_callback_stats.lap_usec;
	dlss_callback_stats.lap_usec = now_usec;
}

static sl::FrameToken *sl_frame_token() {
	StreamlineContext &sl = StreamlineContext::get();
	return sl.render_token != nullptr ? sl.render_token : sl.last_token;
}
#endif

// The compositor's back-buffer-sized HUD-less copy and UI alpha, see
// DLSSEffect::set_frame_generation_hudless().
static RID frame_generation_hudless;
static RID frame_generation_ui_alpha;

void RendererRD::DLSSEffect::set_frame_generation_hudless(RID p_hudless, RID p_ui_alpha) {
	frame_generation_hudless = p_hudless;
	frame_generation_ui_alpha = p_ui_alpha;
}

RID RendererRD::DLSSEffect::get_frame_generation_hudless() {
	return frame_generation_hudless;
}

bool RendererRD::DLSSEffect::frame_generation_hudless_double_buffered() {
#if defined(ENABLE_DLSS) && defined(D3D12_ENABLED)
	return FSRFrameGenerationD3D12::is_generating() && FSRFrameGenerationD3D12::uses_async_workloads();
#else
	return false;
#endif
}

using namespace RendererRD;

#ifdef ENABLE_DLSS
// DLSS-G asked for by the DLSS pass just recorded (Viewport.frame_generation), enabled or not yet: set when the pass
// is recorded, consumed by the compositor's HUD-less capture that follows it in the same frame.
static bool frame_generation_dlssg_requested = false;

// DLSS-G can be asked for: a game on hardware that supports it, with DLSS-G (not AMD FSR) as the
// frame generation provider.
static bool dlssg_is_provider() {
	const StreamlineContext &sl = StreamlineContext::get();
	bool provider = sl.is_game && sl.streamline_capabilities.dlss_g_available;
#ifdef D3D12_ENABLED
	provider = provider && !FSRFrameGenerationD3D12::is_requested();
#endif
	return provider;
}

// Texture layout/state constants (avoid including Vulkan/D3D12 headers here).
static constexpr uint64_t DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY = 5;
// Vulkan: the DLSS output is written by NGX, so the render graph holds it as a storage image around
// the callback: TEXTURE_LAYOUT_STORAGE_OPTIMAL, i.e. VK_IMAGE_LAYOUT_GENERAL.
static constexpr uint64_t DLSS_VK_IMAGE_LAYOUT_GENERAL = 1;
// D3D12: every texture handed to Streamline is held by the render graph in the GENERAL layout
// (D3D12_BARRIER_LAYOUT_COMMON) and announced as D3D12_RESOURCE_STATE_COMMON. Streamline transitions
// with legacy ResourceBarrier; mixed with Godot's enhanced barriers that is only valid through
// COMMON (debug layer error #1350 otherwise, from NVIDIA's original integration on).
static constexpr uint64_t DLSS_D3D12_RESOURCE_STATE_COMMON = 0x0;

// Single source for "is the DLSS output declared as written": the graph usage in upscale() and
// the state announced to Streamline in _upscale_internal() must agree, or Streamline issues its
// barriers from a state the resource is not in. NGX writes the output as a UAV, so in practice
// it always carries the storage bit; without it, keep the old read declaration.
static bool dlss_output_is_storage(RID p_output) {
	return p_output.is_valid() && (RD::get_singleton()->texture_get_format(p_output).usage_bits & RD::TEXTURE_USAGE_STORAGE_BIT);
}

namespace RendererRD {
class DLSSContextInner;

// A texture as Streamline is told about it: format and native handles, resolved on the render thread. The DLSS
// callback runs when the graph is replayed, which may be on another thread once the render thread has moved on to
// the next frame (RHI-THREAD-STUDY.md, step 1a): it must not look anything up through RenderingDevice.
struct DLSSResolvedTexture {
	bool valid = false;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t array_layers = 0;
	uint32_t mipmaps = 0;
	uint64_t image = 0;
	uint64_t view = 0;
	uint64_t memory = 0;
	uint64_t format = 0;
	uint64_t usage = 0;
};

// Everything one recorded DLSS callback uses that the render thread may change before it runs: the frame's
// parameters, its Streamline token, the textures. Owned by the callback, which frees it.
struct DLSSCallbackPayload {
	DLSSContextInner *context = nullptr;
	DLSSEffect *effect = nullptr;
	DLSSContext::Parameters params;
	sl::FrameToken *frame_token = nullptr;
	bool output_storage = false;
	DLSSResolvedTexture color;
	DLSSResolvedTexture output;
	DLSSResolvedTexture depth;
	DLSSResolvedTexture velocity;
	DLSSResolvedTexture rr_diffuse_albedo;
	DLSSResolvedTexture rr_specular_albedo;
	DLSSResolvedTexture rr_normal_roughness;
	DLSSResolvedTexture rr_specular_hit_dist;
	// The compositor sets the HUD-less color and UI alpha after the 3D pass that recorded this callback: they are
	// captured when the frame's recording ends (DLSSEffect::finalize_frame_callbacks()), or by the callback itself
	// when the graph is replayed earlier in the frame (a flush), on the render thread.
	bool hudless_final = false;
	DLSSResolvedTexture hudless;
	DLSSResolvedTexture ui_alpha;
};
} // namespace RendererRD

static RendererRD::DLSSResolvedTexture dlss_resolve_texture(RID p_texture) {
	RendererRD::DLSSResolvedTexture resolved;
	if (!p_texture.is_valid()) {
		return resolved;
	}
	RenderingDevice *rd = RD::get_singleton();
	const RD::TextureFormat texture_format = rd->texture_get_format(p_texture);
	resolved.valid = true;
	resolved.width = texture_format.width;
	resolved.height = texture_format.height;
	resolved.array_layers = texture_format.array_layers;
	resolved.mipmaps = texture_format.mipmaps;
	resolved.image = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_texture);
	resolved.view = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture);
	resolved.memory = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY, p_texture);
	resolved.format = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture);
	resolved.usage = rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS, p_texture);
	return resolved;
}

// The DLSS callbacks recorded in the frame being recorded, waiting for its HUD-less capture. Render thread only.
static LocalVector<RendererRD::DLSSCallbackPayload *> dlss_pending_payloads;

static void dlss_capture_hudless(RendererRD::DLSSCallbackPayload *p_payload) {
	p_payload->hudless = dlss_resolve_texture(frame_generation_hudless);
	p_payload->ui_alpha = dlss_resolve_texture(frame_generation_ui_alpha);
	p_payload->hudless_final = true;
}

void RendererRD::DLSSEffect::finalize_frame_callbacks() {
	for (DLSSCallbackPayload *payload : dlss_pending_payloads) {
		dlss_capture_hudless(payload);
	}
	dlss_pending_payloads.clear();
}
static constexpr float DLSS_OPTIMAL_MODE_MAX_DISTANCE = 1000000.0f;
namespace RendererRD {
class DLSSContextInner : public DLSSContext {
public:
	sl::ViewportHandle viewport;
	sl::Constants constants;
	sl::DLSSOptions currentDlssOptions;
	sl::DLSSOptimalSettings currentOptimalSettings;
	sl::DLSSDOptions currentDlssDOptions; // DLSS Ray Reconstruction options
	bool hudless_tagged = false; // kBufferTypeHUDLessColor currently tagged on this viewport.

	DLSSContextInner();
	virtual ~DLSSContextInner();

	sl::DLSSMode find_optimal_mode(uint32_t outputWidth, uint32_t outputHeight, uint32_t desiredWidth, uint32_t desiredHeight, sl::DLSSOptimalSettings &out_optimalSettings, bool use_dlss_rr = false) {
		// For DLSS-RR, use the DLSS-D API; for regular DLSS, use the standard DLSS API
		if (use_dlss_rr) {
			if (StreamlineContext::get().slDLSSDGetOptimalSettings == nullptr) {
				return sl::DLSSMode::eOff;
			}
		} else {
			if (StreamlineContext::get().slDLSSGetOptimalSettings == nullptr) {
				return sl::DLSSMode::eOff;
			}
		}

		sl::DLSSMode modes[] = { sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance };
		sl::DLSSOptimalSettings settings[sizeof(modes) / sizeof(modes[0])];
		bool validSettings[sizeof(modes) / sizeof(modes[0])];
		Vector2 distance[sizeof(modes) / sizeof(modes[0])];
		memset(validSettings, 0, sizeof(validSettings));

		for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			sl::Result result;

			if (use_dlss_rr) {
				sl::DLSSDOptions dlssDOptions = {};
				dlssDOptions.outputWidth = outputWidth;
				dlssDOptions.outputHeight = outputHeight;
				dlssDOptions.mode = modes[i];
				sl::DLSSDOptimalSettings dlssDSettings;
				result = StreamlineContext::get().slDLSSDGetOptimalSettings(dlssDOptions, dlssDSettings);
				if (result == sl::Result::eOk) {
					// Copy to common settings struct
					settings[i].optimalRenderWidth = dlssDSettings.optimalRenderWidth;
					settings[i].optimalRenderHeight = dlssDSettings.optimalRenderHeight;
					settings[i].optimalSharpness = dlssDSettings.optimalSharpness;
					settings[i].renderWidthMin = dlssDSettings.renderWidthMin;
					settings[i].renderHeightMin = dlssDSettings.renderHeightMin;
					settings[i].renderWidthMax = dlssDSettings.renderWidthMax;
					settings[i].renderHeightMax = dlssDSettings.renderHeightMax;
				}
			} else {
				sl::DLSSOptions dlssOptions = {};
				dlssOptions.outputWidth = outputWidth;
				dlssOptions.outputHeight = outputHeight;
				dlssOptions.mode = modes[i];
				result = StreamlineContext::get().slDLSSGetOptimalSettings(dlssOptions, settings[i]);
			}

			if (result != sl::Result::eOk) {
				continue;
			}

			sl::DLSSOptimalSettings &optimalSettings = settings[i];
			if (desiredWidth >= optimalSettings.renderWidthMin &&
					desiredWidth <= optimalSettings.renderWidthMax &&
					desiredHeight >= optimalSettings.renderHeightMin &&
					desiredHeight <= optimalSettings.renderHeightMax) {
				validSettings[i] = true;
				distance[i] = Vector2(fabsf((float)optimalSettings.optimalRenderWidth - (float)desiredWidth), fabsf((float)optimalSettings.optimalRenderHeight - (float)desiredHeight));
			}
		}

		// now select the closest match
		Vector2 closestDistance(DLSS_OPTIMAL_MODE_MAX_DISTANCE, DLSS_OPTIMAL_MODE_MAX_DISTANCE);
		int closestDistanceMatch = -1;
		for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			if (validSettings[i]) {
				if (distance[i].length_squared() < closestDistance.length_squared()) {
					closestDistanceMatch = i;
					closestDistance = distance[i];
				}
			}
		}

		if (closestDistanceMatch != -1) {
			out_optimalSettings = settings[closestDistanceMatch];
			return modes[closestDistanceMatch];
		}

		ERR_FAIL_V_MSG(sl::DLSSMode::eOff, "Couldn't find an appropriate DLSS mode.");
	}
}; // end class
}; // end namespace RendererRD

static Vector<unsigned int> g_dlss_freeViewportIndices;
static unsigned int g_dlss_viewportIndex = 1;

DLSSContextInner::~DLSSContextInner() {
	// Release the per-viewport resources Streamline allocated for this context.
	//
	// Without this, destroying a context only returned its viewport index to the free list
	// below -- which hands the very same index to the NEXT context created. Streamline then
	// reconfigured the feature on a viewport whose previous allocation was still live and
	// still referenced by command lists that had not retired, so every scaling-mode or
	// quality change both leaked and aliased GPU resources. Under D3D12 that eventually
	// removed the device (0x887A0005, faulting module nvwgf2umx.dll).
	//
	// Compare FSR2Context::~FSR2Context, which calls ffxFsr2ContextDestroy -- FSR2 never
	// exhibited the crash.
	StreamlineContext &sl_ctx = StreamlineContext::get();

	// DLSS-G first. The only code that turns it off lives in _upscale_internal, which stops
	// running the moment this context is gone -- switching the viewport to bilinear or FSR2
	// left frame generation on, consuming depth and motion vectors that configure() was about
	// to destroy and recreate at the same size. The DLSS-G guide requires eOff before any
	// resolution change; dlssg_disable() does that and arms the usual re-enable delay.
	// Sixteen bilinear/DLSS toggles hung the GPU without this; the game only survived them
	// because its settings code happened to resize the swap chain every time, which reaches
	// dlssg_disable() through the swap-chain marker.
	if (sl_ctx.dlssg_viewport == viewport) {
		sl_ctx.dlssg_disable();
	}

	if (sl_ctx.slFreeResources != nullptr || sl_ctx.slSetTag != nullptr) {
		// slFreeResources is documented as requiring any command list with a pending
		// slEvaluateFeature to be flushed first, "to prevent invalid resource access on the
		// GPU". Contexts are only destroyed when the render buffers are reconfigured -- a
		// settings change, not a per-frame event -- so the stall is not on any hot path.
		RD::get_singleton()->_flush_and_stall_for_all_frames();
	}

	if (sl_ctx.slSetTag != nullptr) {
		// Every buffer this context tags is tagged eValidUntilPresent, and the Streamline
		// programming guide requires such tags to be set to null before the resource is
		// destroyed, "in order to release the reference held by SL". The render buffers are
		// about to be rebuilt -- the scaling input, depth, motion vectors and the RR buffers
		// change size with the quality mode -- so drop every reference now, before any of them
		// is freed and its address recycled for something else. A null tag needs no command
		// list. Untagging a type that was never tagged is a no-op.
		const sl::BufferType tagged_types[] = {
			sl::kBufferTypeScalingInputColor,
			sl::kBufferTypeScalingOutputColor,
			sl::kBufferTypeDepth,
			sl::kBufferTypeMotionVectors,
			sl::kBufferTypeAlbedo,
			sl::kBufferTypeSpecularAlbedo,
			sl::kBufferTypeNormalRoughness,
			sl::kBufferTypeSpecularHitDistance,
			sl::kBufferTypeHUDLessColor,
			DLSS_BUFFER_TYPE_UI_ALPHA,
		};
		constexpr uint32_t tagged_count = sizeof(tagged_types) / sizeof(tagged_types[0]);
		sl::ResourceTag null_tags[tagged_count];
		for (uint32_t i = 0; i < tagged_count; i++) {
			null_tags[i] = sl::ResourceTag(nullptr, tagged_types[i], sl::ResourceLifecycle::eValidUntilPresent);
		}
		sl::Result result = sl_ctx.slSetTag(viewport, null_tags, tagged_count, nullptr);
		if (result != sl::Result::eOk) {
			WARN_PRINT("Streamline: failed to release resource tags on context destruction: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	if (sl_ctx.slFreeResources != nullptr) {
		// DLSS-RR allocates under its own feature id, so releasing only kFeatureDLSS would
		// leave it behind. last_parameters records what this context actually evaluated.
		//
		// DLSS-G is deliberately NOT freed here. Its resources belong to the mode, not to the
		// context: DLSSGMode::eOff releases them itself, and slFreeResources(kFeatureDLSS_G)
		// is only meant for integrations that set eRetainResourcesWhenOff (this one does
		// not). Worse, at this point DLSS-G is still eOn on this viewport with its Present
		// hook live -- the on/off transitions are driven per frame from _upscale_internal
		// and the new context reuses this very viewport index -- so freeing under it crashed
		// the process on D3D12 the moment the frame-generation multiplier changed (2x -> 4x).
		if (last_parameters.dlss_rr) {
			sl_ctx.slFreeResources(sl::kFeatureDLSS_RR, viewport);
		}

		// The NIS guide: "If previously tagged resources are destroyed (whether they are
		// volatile or not), slFreeResources must be called unless NIS is no longer used."
		if (sl_ctx.dlss_sharpening && last_parameters.sharpness > 0.0f) {
			sl_ctx.slFreeResources(sl::kFeatureNIS, viewport);
		}

		sl_ctx.slFreeResources(sl::kFeatureDLSS, viewport);
	}

	g_dlss_freeViewportIndices.push_back((unsigned int)viewport);
}

DLSSContextInner::DLSSContextInner() {
	if (g_dlss_freeViewportIndices.size() == 0) {
		g_dlss_freeViewportIndices.push_back(g_dlss_viewportIndex++);
	}
	viewport = g_dlss_freeViewportIndices[g_dlss_freeViewportIndices.size() - 1];
	g_dlss_freeViewportIndices.remove_at(g_dlss_freeViewportIndices.size() - 1);
}

DLSSEffect::DLSSEffect() {
	// Initialize motion vector decode shader
	Vector<String> modes;
	modes.push_back("\n");
	shaders.mvec_decode_shader.initialize(modes, "");
	shaders.mvec_decode_version = shaders.mvec_decode_shader.version_create();
	shaders.mvec_decode_pipeline = RD::get_singleton()->compute_pipeline_create(shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0));
}

DLSSEffect::~DLSSEffect() {
	// Deinitialize motion vector decode
	shaders.mvec_decode_shader.version_free(shaders.mvec_decode_version);
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	DLSSContextInner *context = memnew(RendererRD::DLSSContextInner);

	context->currentDlssOptions.mode = context->find_optimal_mode(p_target_size.width, p_target_size.height, p_internal_size.width, p_internal_size.height, context->currentOptimalSettings);
	context->currentDlssOptions.outputWidth = p_target_size.width;
	context->currentDlssOptions.outputHeight = p_target_size.height;

	context->is_d3d12 = (RD::get_singleton()->get_device_api_name().to_lower() == "d3d12");

	return context;
}

static sl::float4x4 sl_make_identity_matrix() {
	sl::float4x4 ret;
	ret.setRow(0, sl::float4(1.0f, 0.0f, 0.0f, 0.0f));
	ret.setRow(1, sl::float4(0.0f, 1.0f, 0.0f, 0.0f));
	ret.setRow(2, sl::float4(0.0f, 0.0f, 1.0f, 0.0f));
	ret.setRow(3, sl::float4(0.0f, 0.0f, 0.0f, 1.0f));
	return ret;
}

// Streamline applies its matrices to row vectors, p' = p * M: sl_matrix_helpers.h composes
// clipToPrevClip = clipToCameraView * viewToPrevView * viewToClipPrev (first transform on the
// left) and keeps translations in row 3, and SL's own mvec.hlsl does mul(M, v) on a constant
// buffer filled row by row, which under HLSL's default column_major packing is v * M.
// Godot's Projection is the column-vector form (p' = M * p), so Streamline needs its transpose:
// row i of the sl::float4x4 is column i of the Projection. The rows used to be copied as rows,
// which handed Streamline M^T -- a camera translation then reprojected as almost no camera
// motion, and a rotation with the wrong sign. Inverse pairs (clipToCameraView, prevClipToClip)
// and a still camera (clipToPrevClip = identity) cannot tell the two layouts apart.
static sl::float4x4 sl_convert_matrix(const Projection &mtx) {
	sl::float4x4 ret;
	for (int i = 0; i < 4; i++) {
		ret.setRow(i, sl::float4(mtx.columns[i].x, mtx.columns[i].y, mtx.columns[i].z, mtx.columns[i].w));
	}
	return ret;
}

static sl::float3 sl_convert_vector(const Vector3 &vec) {
	return sl::float3(vec.x, vec.y, vec.z);
}

void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {
	DLSSContextInner *context = (DLSSContextInner *)p_params.context;

	// Delay enablement
	if (context->delay > 0) {
		--context->delay;
		return;
	}

	// If DLSS is not loaded, escape early
	if (StreamlineContext::get().slDLSSSetOptions == nullptr) {
		return;
	}

	// Begin frame if needed.
	if (sl_frame_token() == nullptr) {
		StreamlineContext::get().get_new_frame_token();
	}

	context->last_parameters = p_params;
	context->last_effect = this;

	// Decode mvecs
	{
		RD::get_singleton()->draw_command_begin_label("Decode Invalid Motion Vectors");
		UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
		ERR_FAIL_NULL(uniform_set_cache);
		MaterialStorage *material_storage = MaterialStorage::get_singleton();
		ERR_FAIL_NULL(material_storage);

		// setup our uniforms
		RD::Uniform u_velocity_image(RD::UNIFORM_TYPE_IMAGE, 0, p_params.velocity);
		RD::Uniform u_depth_texture(RD::UNIFORM_TYPE_TEXTURE, 0, p_params.depth);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

		RID shader = shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0);
		ERR_FAIL_COND(shader.is_null());

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, shaders.mvec_decode_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_velocity_image), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_depth_texture), 1);

		auto texture_format = RD::get_singleton()->texture_get_format(p_params.velocity);

		float push_constants[20];
		push_constants[0] = texture_format.width;
		push_constants[1] = texture_format.height;
		push_constants[2] = 0.0f;
		push_constants[3] = 0.0f;
		memcpy(push_constants + 4, &p_params.reprojection.columns[0].x, sizeof(float) * 16);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, push_constants, sizeof(push_constants));

		RD::get_singleton()->compute_list_dispatch_threads(compute_list, texture_format.width, texture_format.height, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	// The compositor's HUD-less capture, later in this frame, follows this pass's request.
	frame_generation_dlssg_requested = p_params.dlss_g && dlssg_is_provider();

	DLSSCallbackPayload *payload = memnew(DLSSCallbackPayload);
	payload->context = context;
	payload->effect = this;
	payload->params = p_params;
	payload->frame_token = sl_frame_token();
	payload->output_storage = dlss_output_is_storage(p_params.output);
	payload->color = dlss_resolve_texture(p_params.color);
	payload->output = dlss_resolve_texture(p_params.output);
	payload->depth = dlss_resolve_texture(p_params.depth);
	payload->velocity = dlss_resolve_texture(p_params.velocity);
	if (p_params.dlss_rr) {
		payload->rr_diffuse_albedo = dlss_resolve_texture(p_params.dlss_rr_diffuse_albedo);
		payload->rr_specular_albedo = dlss_resolve_texture(p_params.dlss_rr_specular_albedo);
		payload->rr_normal_roughness = dlss_resolve_texture(p_params.dlss_rr_normal_roughness);
		payload->rr_specular_hit_dist = dlss_resolve_texture(p_params.dlss_rr_specular_hit_dist);
	}
	dlss_pending_payloads.push_back(payload);

	// Inject DLSS into the render graph
	RD::CallbackResource res[8]; // Increased for DLSS-RR buffers
	int num_resources = 0;
	res[num_resources++].rid = p_params.color;
	res[num_resources++].rid = p_params.output;
	res[num_resources++].rid = p_params.depth;
	res[num_resources++].rid = p_params.velocity;

	// Add DLSS-RR buffers if provided
	if (p_params.dlss_rr) {
		if (p_params.dlss_rr_diffuse_albedo.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_diffuse_albedo;
		}
		if (p_params.dlss_rr_specular_albedo.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_specular_albedo;
		}
		if (p_params.dlss_rr_normal_roughness.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_normal_roughness;
		}
		if (p_params.dlss_rr_specular_hit_dist.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_specular_hit_dist;
		}
	}

	for (int i = 0; i < num_resources; i++) {
		res[i].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	}
	// The output is WRITTEN by the callback (NGX, and NIS after it). Declared as a sample, the
	// graph saw the callback as one more reader of that texture: nothing ordered the glow and
	// tonemap passes, which also sample it, after the callback, and the graph's level reordering
	// ran them first -- every frame displayed the previous frame's DLSS output. Measured in game:
	// the 3D lagged the UI by exactly one frame of camera motion under DLSS only, and the lag
	// vanished with --gpu-profile, whose timestamps serialize the graph. MetalFX declares its
	// destination the same way (metal_fx.cpp).
	if (payload->output_storage) {
		res[1].usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE; // res[1] is p_params.output.
	}
	if (p_params.context->is_d3d12) {
		// GENERAL is a write usage for the graph: the output stays ordered before its readers.
		for (int i = 0; i < num_resources; i++) {
			res[i].usage = RD::CALLBACK_RESOURCE_USAGE_GENERAL;
		}
	}
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)DLSSEffect::_upscale_internal_graph_callback, payload, VectorView<RD::CallbackResource>(res, num_resources));
}

void DLSSEffect::_upscale_internal(RDD::CommandBufferID cmdid, DLSSCallbackPayload &p_payload) {
	const DLSSContext::Parameters &p_params = p_payload.params;
	DLSSContextInner *context = p_payload.context;

	void *nativeCmdlist = RD::get_singleton()->get_device_driver()->command_buffer_get_native_handle(cmdid);

	// Helper function for tagging resources, resolved when the callback was recorded.
	auto assignResource = [context](sl::Resource *resources, sl::ResourceTag *resourceTags, int &numResources, const DLSSResolvedTexture &p_texture, sl::BufferType bufferType, sl::ResourceLifecycle lifecycle, bool p_storage = false) {
		if (!p_texture.valid) {
			return;
		}

		// The state the render graph actually put the resource in; Streamline tracks nothing
		// itself (eDisableCLStateTracking) and issues its barriers from this.
		uint64_t texture_state = p_storage ? DLSS_VK_IMAGE_LAYOUT_GENERAL : DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY;
		if (context->is_d3d12) {
			texture_state = DLSS_D3D12_RESOURCE_STATE_COMMON;
		}
		auto &destinationResource = resources[numResources];
		if (context->is_d3d12) {
			destinationResource = sl::Resource(sl::ResourceType::eTex2d,
					(void *)p_texture.view, texture_state);
		} else {
			destinationResource = sl::Resource(sl::ResourceType::eTex2d,
					(void *)p_texture.image, (void *)p_texture.memory, (void *)p_texture.view, texture_state);
		}
		destinationResource.width = p_texture.width;
		destinationResource.height = p_texture.height;
		destinationResource.nativeFormat = p_texture.format;
		destinationResource.arrayLayers = p_texture.array_layers;
		destinationResource.flags = 0;
		destinationResource.mipLevels = p_texture.mipmaps;
		destinationResource.usage = p_texture.usage;

		resourceTags[numResources] = sl::ResourceTag(resources + numResources, bufferType, lifecycle, nullptr);
		++numResources;
	};

	// Set DLSS or DLSS-RR options depending on mode
	bool use_dlss_rr = p_params.dlss_rr && StreamlineContext::get().slDLSSDSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.dlss_rr_available;

	if (use_dlss_rr) {
		// Set DLSS-RR (Ray Reconstruction) options
		context->currentDlssDOptions.mode = context->currentDlssOptions.mode;
		context->currentDlssDOptions.outputWidth = context->currentDlssOptions.outputWidth;
		context->currentDlssDOptions.outputHeight = context->currentDlssOptions.outputHeight;
		context->currentDlssDOptions.colorBuffersHDR = sl::Boolean::eTrue;
		context->currentDlssDOptions.alphaUpscalingEnabled = p_params.dlss_rr_alpha_upscaling ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		context->currentDlssDOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked; // Normal XYZ + Roughness W

		// Set world-to-camera and camera-to-world matrices for DLSS-RR
		// worldToCameraView = view matrix (world-to-camera transformation)
		// cameraViewToWorld = inverse view matrix (camera-to-world transformation)
		Transform3D view_matrix = p_params.cam_transform.affine_inverse();
		context->currentDlssDOptions.worldToCameraView = sl_convert_matrix(Projection(view_matrix)); //
		context->currentDlssDOptions.cameraViewToWorld = sl_convert_matrix(Projection(view_matrix).inverse());
		char dlssPreset = p_params.preset;
		if (dlssPreset == '?') {
			dlssPreset = StreamlineContext::get().dlss_rr_default_preset;
		}

		if (dlssPreset == '?') {
			context->currentDlssDOptions.dlaaPreset = sl::DLSSDPreset::eDefault;
			context->currentDlssDOptions.qualityPreset = sl::DLSSDPreset::eDefault;
			context->currentDlssDOptions.balancedPreset = sl::DLSSDPreset::eDefault;
			context->currentDlssDOptions.performancePreset = sl::DLSSDPreset::eDefault;
			context->currentDlssDOptions.ultraPerformancePreset = sl::DLSSDPreset::eDefault;
		} else {
			int presetNo = ((int)dlssPreset - (int)'D');
			sl::DLSSDPreset preset = (sl::DLSSDPreset)((int)sl::DLSSDPreset::ePresetD + presetNo);
			context->currentDlssDOptions.dlaaPreset = preset;
			context->currentDlssDOptions.qualityPreset = preset;
			context->currentDlssDOptions.balancedPreset = preset;
			context->currentDlssDOptions.performancePreset = preset;
			context->currentDlssDOptions.ultraPerformancePreset = preset;
		}

		sl::Result result = StreamlineContext::get().slDLSSDSetOptions(context->viewport, context->currentDlssDOptions);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slDLSSDSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	} else if (StreamlineContext::get().slDLSSSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.dlss_available) {
		// Set regular DLSS options
		if (p_params.exposure.is_null() || !p_params.exposure.is_valid()) {
			context->currentDlssOptions.useAutoExposure = sl::Boolean::eTrue;
		} else {
			context->currentDlssOptions.useAutoExposure = sl::Boolean::eFalse;
		}

		context->currentDlssOptions.colorBuffersHDR = sl::Boolean::eTrue;
		char dlssPreset = p_params.preset;
		if (dlssPreset == '?') {
			dlssPreset = StreamlineContext::get().dlss_default_preset;
		}

		if (dlssPreset == '?') {
			context->currentDlssOptions.dlaaPreset = sl::DLSSPreset::eDefault;
			context->currentDlssOptions.qualityPreset = sl::DLSSPreset::eDefault;
			context->currentDlssOptions.balancedPreset = sl::DLSSPreset::eDefault;
			context->currentDlssOptions.performancePreset = sl::DLSSPreset::eDefault;
			context->currentDlssOptions.ultraPerformancePreset = sl::DLSSPreset::eDefault;
		} else {
			int presetNo = ((int)dlssPreset - (int)'F');
			sl::DLSSPreset preset = (sl::DLSSPreset)((int)sl::DLSSPreset::ePresetF + presetNo);
			context->currentDlssOptions.dlaaPreset = preset;
			context->currentDlssOptions.qualityPreset = preset;
			context->currentDlssOptions.balancedPreset = preset;
			context->currentDlssOptions.performancePreset = preset;
			context->currentDlssOptions.ultraPerformancePreset = preset;
		}

		sl::Result result = StreamlineContext::get().slDLSSSetOptions(context->viewport, context->currentDlssOptions);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slDLSSSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	dlss_callback_lap(DLSS_SECTION_OPTIONS);

	// Set SL Options
	if (StreamlineContext::get().slSetConstants != nullptr) {
		sl::float4x4 mtxIdentity = sl_make_identity_matrix();

		// Streamline forms its clip-space point from the pixel and the value it reads in the
		// tagged depth buffer, then runs it through these matrices. That depth is reverse-Z in
		// [0,1] (near = 1, depthInverted below). The matrices used to be the raw GL projection
		// (z in [-1,1], near = -1, not reversed) and p_params.reprojection (z in [-1,1] reversed,
		// y flipped) — three conventions in one struct, each disagreeing with the depth. Fed
		// raw, the whole [0,1] range lands in the few centimetres in front of the near plane, so
		// a camera translation reprojects the sky and a building 30 m away as if both were
		// 10 cm from the eye; a rotation is a depth-independent homography and comes out right
		// regardless. Seen as static geometry jumping for a frame under DLSS-G whenever the
		// camera displacement changed (start of a walk; every physics tick with interpolation
		// off), never on mouse look.
		//
		// So Streamline gets its own set, built in the depth buffer's convention: reverse-Z
		// remapped to [0,1], y as the GL/D3D projection has it (up), un-jittered as required.
		// p_params.reprojection is left untouched: the motion vector decode pass above depends
		// on its [-1,1] convention (motion_vector_inc.glsl remaps depth*2-1 before using it).
		Projection sl_corr;
		sl_corr.set_depth_correction(false, true, true);
		Projection sl_proj = sl_corr * p_params.cam_projection;
		Projection sl_prev_proj = sl_corr * p_params.prev_cam_projection;
		// Relative camera motion first (camera_reprojection.h): from the absolute transforms this
		// was not the identity for a still camera a few km from the origin.
		Projection sl_reproj = sl_prev_proj * Projection(camera_view_delta(p_params.prev_cam_transform, p_params.cam_transform)) * sl_proj.inverse();

		context->constants.cameraViewToClip = sl_convert_matrix(sl_proj); // projection mtx (unjittered)
		context->constants.clipToCameraView = sl_convert_matrix(sl_proj.inverse()); // projection mtx (unjittered, inverted)
		context->constants.clipToLensClip = mtxIdentity; // keep identity unless some lens distortion is applied
		context->constants.clipToPrevClip = sl_convert_matrix(sl_reproj); // reprojection matrix
		context->constants.prevClipToClip = sl_convert_matrix(sl_reproj.inverse()); // inverted reprojection matrix

		// Basis stores its axes transposed, as rows: rows[i] is not axis i. For any camera not
		// facing world -Z the rows describe a mirrored camera.
		const Basis &cam_basis = p_params.cam_transform.get_basis();
		context->constants.cameraPos = sl_convert_vector(p_params.cam_transform.get_origin());
		context->constants.cameraFwd = sl_convert_vector(-cam_basis.get_column(2));
		context->constants.cameraUp = sl_convert_vector(cam_basis.get_column(1));
		context->constants.cameraRight = sl_convert_vector(cam_basis.get_column(0));

		context->constants.cameraNear = p_params.z_near;
		context->constants.cameraFar = p_params.z_far;
		context->constants.cameraFOV = Math::deg_to_rad(p_params.fovy);
		context->constants.cameraMotionIncluded = sl::Boolean::eTrue;
		context->constants.cameraAspectRatio = static_cast<float>(context->currentDlssOptions.outputWidth) / static_cast<float>(context->currentDlssOptions.outputHeight);
		context->constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
		context->constants.depthInverted = p_params.reverse_depth ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		context->constants.motionVectors3D = sl::Boolean::eFalse;
		context->constants.motionVectorsDilated = sl::Boolean::eFalse;
		context->constants.motionVectorsJittered = sl::Boolean::eFalse;
		context->constants.jitterOffset = sl::float2(p_params.jitter.x, p_params.jitter.y);
		context->constants.mvecScale = sl::float2(1.0f, 1.0f);
		context->constants.orthographicProjection = sl::Boolean::eFalse;
		context->constants.reset = p_params.reset_accumulation ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		sl::Result result = StreamlineContext::get().slSetConstants(context->constants, *p_payload.frame_token, context->viewport);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slSetConstants. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	dlss_callback_lap(DLSS_SECTION_CONSTANTS);

	// Tag resources
	if (StreamlineContext::get().slSetTag != nullptr) {
		sl::Resource resources[10];
		sl::ResourceTag resourceTags[10];
		int numResources = 0;

		assignResource(resources, resourceTags, numResources, p_payload.color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent);
		assignResource(resources, resourceTags, numResources, p_payload.output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, p_payload.output_storage);
		assignResource(resources, resourceTags, numResources, p_payload.depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent);
		assignResource(resources, resourceTags, numResources, p_payload.velocity, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent);

		// Tag DLSS-RR specific buffers if enabled
		if (use_dlss_rr) {
			// kBufferTypeAlbedo is used for diffuse albedo
			assignResource(resources, resourceTags, numResources, p_payload.rr_diffuse_albedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
			assignResource(resources, resourceTags, numResources, p_payload.rr_specular_albedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
			// kBufferTypeNormalRoughness for packed normal+roughness (XYZ=normal, W=roughness)
			assignResource(resources, resourceTags, numResources, p_payload.rr_normal_roughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent);
			assignResource(resources, resourceTags, numResources, p_payload.rr_specular_hit_dist, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent);
		}

		// DLSS-G's HUD-less color: the scene before any canvas, back-buffer-sized, in the back
		// buffer's color space (RendererCompositorRD::capture_hudless()). Its content is written
		// later in this frame, after the canvas; that is allowed for eValidUntilPresent, whose
		// content and state only have to be right at Present. The compositor ends the frame with
		// the texture sampled, so the read state assignResource() declares is the one it is in.
		// Without one -- DLSS-G off, a frame where nothing was captured -- the tag is nulled, as
		// the guide requires before the resource can go away.
		// With it, the UI alpha (1 where the canvas changed the frame), which lets DLSS-G interpolate
		// the scene and the UI separately and recompose them (enableUserInterfaceRecomposition, set
		// below): the HUD-less color alone only attenuates HUD distortion.
		const bool dlssg_requested = p_params.dlss_g && dlssg_is_provider();
		if ((dlssg_requested || StreamlineContext::get().dlssg_viewport == context->viewport) && p_payload.hudless.valid && p_payload.ui_alpha.valid) {
			assignResource(resources, resourceTags, numResources, p_payload.hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent);
			assignResource(resources, resourceTags, numResources, p_payload.ui_alpha, DLSS_BUFFER_TYPE_UI_ALPHA, sl::ResourceLifecycle::eValidUntilPresent);
			context->hudless_tagged = true;
		} else if (context->hudless_tagged) {
			resourceTags[numResources++] = sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent);
			resourceTags[numResources++] = sl::ResourceTag(nullptr, DLSS_BUFFER_TYPE_UI_ALPHA, sl::ResourceLifecycle::eValidUntilPresent);
			context->hudless_tagged = false;
		}

		sl::Result result = StreamlineContext::get().slSetTag(context->viewport, resourceTags, numResources, nativeCmdlist);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slSetTag. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	dlss_callback_lap(DLSS_SECTION_TAGS);

	// Toggle DLSS Frame Generation (only enabled in game mode). On D3D12, sl.dlss_g is only loaded
	// while frame generation is wanted (see the end of this block): until the swap chain has been
	// recreated with it, slDLSSGSetOptions is null and this waits.
	if (StreamlineContext::get().slDLSSGSetOptions != nullptr && dlssg_is_provider() && StreamlineContext::get().dlssg_loaded) {
		sl::DLSSGOptions dlssGOptions{};
		bool wantActivateDLSSG = p_params.dlss_g;
		bool canActivateDLSSG = StreamlineContext::get().dlssg_delay == 0;

		// Multi Frame Generation: the count is what the caller asks, clamped to what the device
		// reports (DLSSGState::numFramesToGenerateMax — 1 on 40-series, 3 on 50-series). Asking
		// for more than the maximum is refused by the runtime, so it is clamped rather than passed.
		uint32_t wantFrames = MAX(1, p_params.dlss_g_frames);
		// Read only when a value is about to be applied: the state query also resets the
		// "frames presented since last call" counter, and there is nothing to learn every frame.
		bool applyFrames = wantActivateDLSSG && (StreamlineContext::get().dlssg_viewport != context->viewport || StreamlineContext::get().dlssg_frames != wantFrames);
		if (applyFrames && StreamlineContext::get().slDLSSGGetState != nullptr) {
			sl::DLSSGState dlssGState{};
			if (StreamlineContext::get().slDLSSGGetState(context->viewport, dlssGState, nullptr) == sl::Result::eOk && dlssGState.numFramesToGenerateMax > 0) {
				wantFrames = MIN(wantFrames, dlssGState.numFramesToGenerateMax);
			}
		}

		// UI recomposition needs the HUD-less color and the UI alpha tagged (DLSS-G guide 6.6); it
		// follows their availability, re-setting the options when that changes.
		const bool wantUiRecomposition = context->hudless_tagged;

		// UI recomposition is decided when DLSS-G is enabled. Changing it on a running DLSS-G makes NGX
		// recreate the feature, and the previous one's resources were never released (67 references
		// left on the device at Streamline shutdown, 143 live D3D12 objects at exit). Turn DLSS-G off
		// instead -- DLSSGMode::eOff releases its resources -- and let the usual path re-enable it.
		if (wantActivateDLSSG && StreamlineContext::get().dlssg_viewport == context->viewport && StreamlineContext::get().dlssg_ui_recomposition != wantUiRecomposition) {
			WARN_PRINT(String("DLSS-G: UI recomposition ") + (wantUiRecomposition ? "available" : "unavailable") + ", restarting frame generation.");
			StreamlineContext::get().dlssg_disable();
			canActivateDLSSG = false; // Re-enabled after dlssg_disable()'s delay, not in this frame.
		}

		// A multiplier change while DLSS-G is on re-sets the options on the same viewport.
		if (wantActivateDLSSG && StreamlineContext::get().dlssg_viewport == context->viewport && StreamlineContext::get().dlssg_frames != wantFrames) {
			WARN_PRINT("DLSS-G on viewport " + itos((unsigned int)context->viewport) + ": " + itos(wantFrames + 1) + "x");
			dlssGOptions.mode = sl::DLSSGMode::eOn;
			dlssGOptions.numFramesToGenerate = wantFrames;
			dlssGOptions.bReserved16 = wantUiRecomposition ? sl::Boolean::eTrue : sl::Boolean::eFalse; // enableUserInterfaceRecomposition
			sl::Result result = StreamlineContext::get().slDLSSGSetOptions(context->viewport, dlssGOptions);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
			}
			StreamlineContext::get().dlssg_frames = wantFrames;
			StreamlineContext::get().dlssg_ui_recomposition = wantUiRecomposition;
		}

		// Disable previous DLSS-G context if needed
		if (StreamlineContext::get().dlssg_viewport != sl::ViewportHandle(-1) && ((!wantActivateDLSSG && StreamlineContext::get().dlssg_viewport == context->viewport) || (wantActivateDLSSG && StreamlineContext::get().dlssg_viewport != context->viewport))) {
			WARN_PRINT("Disabling DLSS-G on viewport: " + itos((unsigned int)StreamlineContext::get().dlssg_viewport));
			dlssGOptions.mode = sl::DLSSGMode::eOff;
			sl::Result result = StreamlineContext::get().slDLSSGSetOptions(StreamlineContext::get().dlssg_viewport, dlssGOptions);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
			}

			StreamlineContext::get().dlssg_viewport = sl::ViewportHandle(-1);
		}

		// Enable new DLSS-G context if needed. Wait a few frames for the HUD-less color and UI alpha
		// (captured from the request on), so DLSS-G starts with UI recomposition rather than being
		// restarted for it; without them by then (2D MSAA, several viewports on the window), start
		// without.
		static constexpr int DLSSG_HUDLESS_WAIT_FRAMES = 5;
		bool waitForHudless = false;
		if (canActivateDLSSG && wantActivateDLSSG && StreamlineContext::get().dlssg_viewport != context->viewport && !wantUiRecomposition && StreamlineContext::get().dlssg_hudless_wait < DLSSG_HUDLESS_WAIT_FRAMES) {
			StreamlineContext::get().dlssg_hudless_wait++;
			waitForHudless = true;
		}
		if (canActivateDLSSG && wantActivateDLSSG && !waitForHudless && StreamlineContext::get().dlssg_viewport != context->viewport) {
			StreamlineContext::get().dlssg_hudless_wait = 0;
			WARN_PRINT("Enabling DLSS-G on viewport: " + itos((unsigned int)context->viewport) + " at " + itos(wantFrames + 1) + "x, UI recomposition " + (wantUiRecomposition ? "on" : "off"));

			dlssGOptions.mode = sl::DLSSGMode::eOn;
			dlssGOptions.numFramesToGenerate = wantFrames;
			dlssGOptions.bReserved16 = wantUiRecomposition ? sl::Boolean::eTrue : sl::Boolean::eFalse; // enableUserInterfaceRecomposition
			sl::Result result = StreamlineContext::get().slDLSSGSetOptions(context->viewport, dlssGOptions);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
			}

			StreamlineContext::get().dlssg_viewport = context->viewport;
			StreamlineContext::get().dlssg_frames = wantFrames;
			StreamlineContext::get().dlssg_ui_recomposition = wantUiRecomposition;
		}
	}

#ifdef D3D12_ENABLED
	// DLSS-G guide, section 18: the swap chain is torn down and recreated every time DLSS-G is
	// switched on or off, with sl.dlss_g loaded only while it is on; loaded but off, it renders
	// off-screen and copies every frame. Asked here, done by the D3D12 driver before its next
	// Present (Streamline::get_swap_chain_serial(), STREAMLINE_MARKER_BEFORE_SWAPCHAIN_CREATION).
	if (context->is_d3d12 && dlssg_is_provider()) {
		StreamlineContext &sl = StreamlineContext::get();
		if (p_params.dlss_g && !sl.dlssg_wanted) {
			sl.dlssg_wanted = true;
			Streamline::get_singleton()->bump_swap_chain_serial();
		} else if (!p_params.dlss_g && sl.dlssg_wanted && sl.dlssg_viewport == sl::ViewportHandle(-1)) {
			sl.dlssg_wanted = false;
			Streamline::get_singleton()->bump_swap_chain_serial();
		}
	}
#endif

	dlss_callback_lap(DLSS_SECTION_FRAME_GENERATION);

	// Evaluate DLSS Super Resolution or DLSS Ray Reconstruction
	if (context->currentDlssOptions.mode != sl::DLSSMode::eOff) {
		const sl::BaseStructure *inputs[] = { &context->viewport };
		sl::Result result;

		if (use_dlss_rr && StreamlineContext::get().streamline_capabilities.dlss_rr_available) {
			// Use DLSS Ray Reconstruction
			result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureDLSS_RR, *p_payload.frame_token, inputs, 1, nativeCmdlist);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slEvaluateFeature for DLSS Ray Reconstruction. Result: " + String(StreamlineContext::result_to_string(result)));
			}
		} else if (StreamlineContext::get().streamline_capabilities.dlss_available) {
			// Use regular DLSS
			result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureDLSS, *p_payload.frame_token, inputs, 1, nativeCmdlist);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slEvaluateFeature for DLSS Super Resolution. Result: " + String(StreamlineContext::result_to_string(result)));
			}
		}
	}

	dlss_callback_lap(DLSS_SECTION_EVALUATE);

	// NIS sharpening
	// **************
	// Off unless rendering/streamline/dlss_sharpening is set. `sharpness` is derived from the
	// viewport's fsr_sharpness, whose default of 0.2 maps to a NIS sharpness of 0.9: every DLSS
	// user got a heavy sharpening pass they never asked for. Worse, NIS is evaluated in place on
	// the DLSS output, with the same resource tagged as both its input and its output -- not the
	// layout its programming guide shows -- and on D3D12 that pass crashed the driver (read of
	// null in nvwgf2umx.dll, same offset every time, inside the NIS evaluation) whenever the
	// render buffers had just been reconfigured: a quality or frame-generation change, a
	// bilinear/DLSS toggle. Skipping the pass removed the crash on the reporter's game.
	if (StreamlineContext::get().dlss_sharpening && p_params.sharpness > 0.0f && StreamlineContext::get().slNISSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.nis_available) {
		{ // Set NIS settings
			sl::NISOptions options;
			options.hdrMode = sl::NISHDR::eNone;
			options.mode = sl::NISMode::eSharpen;
			options.sharpness = p_params.sharpness;
			StreamlineContext::get().slNISSetOptions(context->viewport, options);
		}

		{ // Tag NIS buffers
			sl::Resource resources[3];
			sl::ResourceTag resourceTags[3];
			int numResources = 0;

			// Still the DLSS output, in the same storage state the graph set for the whole callback.
			const bool output_storage = p_payload.output_storage;
			assignResource(resources, resourceTags, numResources, p_payload.output, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, output_storage);
			assignResource(resources, resourceTags, numResources, p_payload.output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, output_storage);

			sl::Result result = StreamlineContext::get().slSetTag(context->viewport, resourceTags, numResources, nativeCmdlist);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slSetTag for NIS. Result: " + String(StreamlineContext::result_to_string(result)));
			}
		}

		{ // Evaluate NIS
			const sl::BaseStructure *inputs[] = { &context->viewport };
			sl::Result result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureNIS, *p_payload.frame_token, inputs, 1, nativeCmdlist);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slEvaluateFeature for NIS. Result: " + String(StreamlineContext::result_to_string(result)));
			}
		}
	}

#ifdef D3D12_ENABLED
	// PROTOTYPE (GODOT_FSR_FG): AMD FSR frame generation on top of DLSS upscaling. Depth and the
	// motion vectors (decoded in place above, no (-1,-1) sentinel left) are final here, and the
	// frame is presented right after this callback's command list is executed.
	if (context->is_d3d12 && FSRFrameGenerationD3D12::is_requested() && !p_params.dlss_g) {
		// Viewport.frame_generation off: no generation, context released.
		FSRFrameGenerationD3D12::stop_generating();
	} else if (context->is_d3d12 && FSRFrameGenerationD3D12::is_requested()) {
		FSRFrameGenerationD3D12::PrepareParams fg;
		fg.native_command_list = nativeCmdlist;
		fg.depth_resource = p_payload.depth.image;
		fg.motion_vectors_resource = p_payload.velocity.image;
		fg.render_width = p_params.internal_size.width;
		fg.render_height = p_params.internal_size.height;
		fg.jitter_x = p_params.jitter.x;
		fg.jitter_y = p_params.jitter.y;
		fg.frame_time_delta_ms = p_params.delta_time * 1000.0f;
		fg.camera_near = p_params.z_near;
		fg.camera_far = p_params.z_far;
		fg.fov_vertical_radians = Math::deg_to_rad(p_params.fovy);
		fg.depth_inverted = p_params.reverse_depth;
		if (p_payload.hudless.valid) {
			fg.hudless_resource = p_payload.hudless.image;
		}
		const Basis &fg_basis = p_params.cam_transform.get_basis();
		const Vector3 fg_origin = p_params.cam_transform.get_origin();
		const Vector3 fg_up = fg_basis.get_column(1).normalized();
		const Vector3 fg_right = fg_basis.get_column(0).normalized();
		const Vector3 fg_forward = (-fg_basis.get_column(2)).normalized();
		for (int i = 0; i < 3; i++) {
			fg.camera_position[i] = fg_origin[i];
			fg.camera_up[i] = fg_up[i];
			fg.camera_right[i] = fg_right[i];
			fg.camera_forward[i] = fg_forward[i];
		}
		FSRFrameGenerationD3D12::prepare(fg);
	}
#endif
}

void RendererRD::DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	DLSSCallbackPayload *payload = (DLSSCallbackPayload *)p_userdata;
	if (!payload->hudless_final) {
		// Replayed before its frame's recording ended (a flush, on the render thread): the HUD-less state as it is now.
		dlss_capture_hudless(payload);
		dlss_pending_payloads.erase(payload);
	}
	if (dlss_callback_stats.enabled < 0) {
		dlss_callback_stats.enabled = OS::get_singleton()->get_environment("GODOT_PARALLEL_RECORDING_STATS") == "1" ? 1 : 0;
	}
	dlss_callback_stats.lap_usec = OS::get_singleton()->get_ticks_usec();
	payload->effect->_upscale_internal(p_command_buffer, *payload);
	memdelete(payload);
	dlss_callback_lap(DLSS_SECTION_REST);
	if (dlss_callback_stats.enabled > 0 && ++dlss_callback_stats.calls == 240) {
		const uint64_t *u = dlss_callback_stats.usec;
		print_line(vformat("DLSS callback, per call: options %d us, constants %d us, tags %d us, frame generation %d us, evaluate %d us, rest %d us.",
				u[DLSS_SECTION_OPTIONS] / 240, u[DLSS_SECTION_CONSTANTS] / 240, u[DLSS_SECTION_TAGS] / 240, u[DLSS_SECTION_FRAME_GENERATION] / 240, u[DLSS_SECTION_EVALUATE] / 240, u[DLSS_SECTION_REST] / 240));
		dlss_callback_stats.calls = 0;
		for (uint64_t &usec : dlss_callback_stats.usec) {
			usec = 0;
		}
	}
}

bool DLSSEffect::frame_generation_wants_hudless() {
#ifdef D3D12_ENABLED
	if (FSRFrameGenerationD3D12::is_generating()) {
		return true;
	}
#endif
	// From the request on, not only once DLSS-G runs: it is enabled after a delay of several frames
	// (dlssg_delay), by which time the HUD-less color and UI alpha are tagged and DLSS-G can start
	// with UI recomposition. Turning recomposition on afterwards makes NGX recreate the feature, and
	// the previous one was never released: its resources still held 67 references on the device
	// when Streamline shut down (D3D12 debug layer: 143 live objects at exit).
	// Consumed: the DLSS pass sets it again every frame it runs with DLSS-G asked for, so switching to
	// another scaler (no DLSS pass any more) does not leave the capture running.
	const bool requested = frame_generation_dlssg_requested;
	frame_generation_dlssg_requested = false;
	return requested || StreamlineContext::get().dlssg_viewport != sl::ViewportHandle(-1);
}

bool DLSSEffect::is_ready(DLSSContext *p_context) {
	DLSSContextInner *context = (DLSSContextInner *)p_context;
	if (context->currentDlssOptions.mode == sl::DLSSMode::eOff) {
		return false; // unsupported mode.
	}
	if (context->delay > 0) {
		return false; // still in delay mode
	}
	return true;
}
#else
DLSSEffect::DLSSEffect() {}
DLSSEffect::~DLSSEffect() {}
bool DLSSEffect::frame_generation_wants_hudless() {
	return false;
}
DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	return nullptr;
}
void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {}
bool DLSSEffect::is_ready(DLSSContext *p_context) {
	return false;
}
void DLSSEffect::_upscale_internal(RDD::CommandBufferID cmdid, DLSSCallbackPayload &p_payload) {}
void DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {}
void DLSSEffect::finalize_frame_callbacks() {}
#endif
