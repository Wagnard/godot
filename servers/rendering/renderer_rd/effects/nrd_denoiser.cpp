/**************************************************************************/
/*  nrd_denoiser.cpp                                                      */
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

#include "nrd_denoiser.h"

#include "core/io/marshalls.h"

// A cache file: header, version, the binary's size, the binary (RenderingDevice::shader_compile_binary_from_spirv()).
static const char *nrd_binary_cache_header = "GDNR";
static const uint32_t nrd_binary_cache_version = 1;

String RendererRD::NRDDenoiser::get_cache_file_name(const String &p_key, const String &p_api) {
	return String(CACHE_FOLDER).path_join(p_key + "." + p_api + ".cache");
}

Vector<uint8_t> RendererRD::NRDDenoiser::make_cache_file(const Vector<uint8_t> &p_binary) {
	Vector<uint8_t> bytes;
	bytes.resize(12 + p_binary.size());
	uint8_t *w = bytes.ptrw();
	memcpy(w, nrd_binary_cache_header, 4);
	encode_uint32(nrd_binary_cache_version, w + 4);
	encode_uint32(p_binary.size(), w + 8);
	memcpy(w + 12, p_binary.ptr(), p_binary.size());
	return bytes;
}

#ifdef NRD_DENOISER_ENABLED

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/thread.h"
#include "core/string/string_builder.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/version.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"

// From misc/scripts/install_nrd.py (nrd_path): NRD's headers and this effect's front and back end shaders.
#include <NRD.h>
#include <nrd_godot_shaders.gen.h>

namespace RendererRD {

// ReBLUR's diffuse and specular denoiser, the one NRD instance's only one.
static constexpr nrd::Identifier NRD_IDENTIFIER = 1;
static constexpr nrd::Denoiser NRD_DENOISER = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;

// What a view's denoising keeps from frame to frame: NRD's instance, its history (the permanent pool), its inputs in its
// encodings and its outputs. Freed when the view's buffers are reconfigured; denoise() makes it again.
class NRDContext : public RenderBufferCustomDataRD {
	GDCLASS(NRDContext, RenderBufferCustomDataRD);

public:
	nrd::Instance *instance = nullptr;
	Size2i size;
	uint32_t frame_index = 0;
	LocalVector<RID> permanent_pool;
	LocalVector<RID> transient_pool;
	// One per constant data a frame's dispatches upload, in their order: none is rewritten while the frame uses it.
	LocalVector<RID> constant_buffers;
	uint32_t constant_buffer_size = 0;
	// NRD's IN_DIFF_RADIANCE_HITDIST, IN_SPEC_RADIANCE_HITDIST, IN_NORMAL_ROUGHNESS (nrd_frontend.hlsl writes them), and
	// OUT_DIFF_RADIANCE_HITDIST, OUT_SPEC_RADIANCE_HITDIST (nrd_backend.hlsl reads them).
	RID in_diffuse;
	RID in_specular;
	RID in_normal_roughness;
	RID out_diffuse;
	RID out_specular;

	virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {
		free_data();
	}

	virtual void free_data() override {
		RD *rd = RD::get_singleton();
		for (LocalVector<RID> *rids : { &permanent_pool, &transient_pool, &constant_buffers }) {
			for (const RID &rid : *rids) {
				rd->free_rid(rid);
			}
			rids->clear();
		}
		for (RID *rid : { &in_diffuse, &in_specular, &in_normal_roughness, &out_diffuse, &out_specular }) {
			if (rid->is_valid()) {
				rd->free_rid(*rid);
				*rid = RID();
			}
		}
		if (instance) {
			nrd::DestroyInstance(*instance);
			instance = nullptr;
		}
		size = Size2i();
		frame_index = 0;
	}

	virtual ~NRDContext() override {
		free_data();
	}
};

struct NRDPipeline {
	RID shader;
	RID pipeline;
};

// The binaries of NRD's programs (NRDDenoiser::CACHE_FOLDER), as SceneShaderRaytracing keeps its own.
struct NRDBinaryCache {
	// ShaderRD's cache folders: the user's (empty when rendering/shader_compiler/shader_cache/enabled is off) and the
	// shader baker's in an exported project.
	String dir;
	String res_dir;
	String api;

	void init();
	Vector<uint8_t> load(const String &p_key) const;
	void save(const String &p_key, const Vector<uint8_t> &p_binary) const;
};

struct NRDDenoiser::State {
	// NRD's pipelines, created at their first dispatch, by shader identifier (the same in every instance).
	HashMap<String, NRDPipeline> pipelines;
	NRDPipeline frontend;
	NRDPipeline backend;
	RID samplers[2]; // nrd::Sampler: NEAREST_CLAMP, LINEAR_CLAMP.
	NRDBinaryCache cache;
};

#define RB_NRD_CONTEXT SNAME("nrd_context")

static RD::DataFormat _rd_format(nrd::Format p_format) {
	switch (p_format) {
		case nrd::Format::R8_UNORM:
			return RD::DATA_FORMAT_R8_UNORM;
		case nrd::Format::R8_SNORM:
			return RD::DATA_FORMAT_R8_SNORM;
		case nrd::Format::R8_UINT:
			return RD::DATA_FORMAT_R8_UINT;
		case nrd::Format::R8_SINT:
			return RD::DATA_FORMAT_R8_SINT;
		case nrd::Format::RG8_UNORM:
			return RD::DATA_FORMAT_R8G8_UNORM;
		case nrd::Format::RG8_SNORM:
			return RD::DATA_FORMAT_R8G8_SNORM;
		case nrd::Format::RG8_UINT:
			return RD::DATA_FORMAT_R8G8_UINT;
		case nrd::Format::RG8_SINT:
			return RD::DATA_FORMAT_R8G8_SINT;
		case nrd::Format::RGBA8_UNORM:
			return RD::DATA_FORMAT_R8G8B8A8_UNORM;
		case nrd::Format::RGBA8_SNORM:
			return RD::DATA_FORMAT_R8G8B8A8_SNORM;
		case nrd::Format::RGBA8_UINT:
			return RD::DATA_FORMAT_R8G8B8A8_UINT;
		case nrd::Format::RGBA8_SINT:
			return RD::DATA_FORMAT_R8G8B8A8_SINT;
		case nrd::Format::RGBA8_SRGB:
			return RD::DATA_FORMAT_R8G8B8A8_SRGB;
		case nrd::Format::R16_UNORM:
			return RD::DATA_FORMAT_R16_UNORM;
		case nrd::Format::R16_SNORM:
			return RD::DATA_FORMAT_R16_SNORM;
		case nrd::Format::R16_UINT:
			return RD::DATA_FORMAT_R16_UINT;
		case nrd::Format::R16_SINT:
			return RD::DATA_FORMAT_R16_SINT;
		case nrd::Format::R16_SFLOAT:
			return RD::DATA_FORMAT_R16_SFLOAT;
		case nrd::Format::RG16_UNORM:
			return RD::DATA_FORMAT_R16G16_UNORM;
		case nrd::Format::RG16_SNORM:
			return RD::DATA_FORMAT_R16G16_SNORM;
		case nrd::Format::RG16_UINT:
			return RD::DATA_FORMAT_R16G16_UINT;
		case nrd::Format::RG16_SINT:
			return RD::DATA_FORMAT_R16G16_SINT;
		case nrd::Format::RG16_SFLOAT:
			return RD::DATA_FORMAT_R16G16_SFLOAT;
		case nrd::Format::RGBA16_UNORM:
			return RD::DATA_FORMAT_R16G16B16A16_UNORM;
		case nrd::Format::RGBA16_SNORM:
			return RD::DATA_FORMAT_R16G16B16A16_SNORM;
		case nrd::Format::RGBA16_UINT:
			return RD::DATA_FORMAT_R16G16B16A16_UINT;
		case nrd::Format::RGBA16_SINT:
			return RD::DATA_FORMAT_R16G16B16A16_SINT;
		case nrd::Format::RGBA16_SFLOAT:
			return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		case nrd::Format::R32_UINT:
			return RD::DATA_FORMAT_R32_UINT;
		case nrd::Format::R32_SINT:
			return RD::DATA_FORMAT_R32_SINT;
		case nrd::Format::R32_SFLOAT:
			return RD::DATA_FORMAT_R32_SFLOAT;
		case nrd::Format::RG32_UINT:
			return RD::DATA_FORMAT_R32G32_UINT;
		case nrd::Format::RG32_SINT:
			return RD::DATA_FORMAT_R32G32_SINT;
		case nrd::Format::RG32_SFLOAT:
			return RD::DATA_FORMAT_R32G32_SFLOAT;
		case nrd::Format::RGB32_UINT:
			return RD::DATA_FORMAT_R32G32B32_UINT;
		case nrd::Format::RGB32_SINT:
			return RD::DATA_FORMAT_R32G32B32_SINT;
		case nrd::Format::RGB32_SFLOAT:
			return RD::DATA_FORMAT_R32G32B32_SFLOAT;
		case nrd::Format::RGBA32_UINT:
			return RD::DATA_FORMAT_R32G32B32A32_UINT;
		case nrd::Format::RGBA32_SINT:
			return RD::DATA_FORMAT_R32G32B32A32_SINT;
		case nrd::Format::RGBA32_SFLOAT:
			return RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		case nrd::Format::R10_G10_B10_A2_UNORM:
			return RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32;
		case nrd::Format::R10_G10_B10_A2_UINT:
			return RD::DATA_FORMAT_A2B10G10R10_UINT_PACK32;
		case nrd::Format::R11_G11_B10_UFLOAT:
			return RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32;
		case nrd::Format::R9_G9_B9_E5_UFLOAT:
			return RD::DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32;
		default:
			ERR_FAIL_V_MSG(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, vformat("Unknown NRD texture format %d.", int(p_format)));
	}
}

// Read as textures and written as storage images, as NRD's passes use them.
static RID _create_texture(RD::DataFormat p_format, const Size2i &p_size, const String &p_name) {
	RD::TextureFormat tf;
	tf.format = p_format;
	tf.width = p_size.width;
	tf.height = p_size.height;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	RID texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(texture, p_name);
	return texture;
}

// Column-major, a vector as a column (nrd::CommonSettings).
static void _store_matrix(float *r_dst, const Projection &p_matrix) {
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			r_dst[c * 4 + r] = float(p_matrix.columns[c][r]);
		}
	}
}

// The programs' names, in their binaries and their cache keys.
static const char *NRD_FRONTEND_NAME = "NRD front end";
static const char *NRD_BACKEND_NAME = "NRD back end";

static String _pass_name(const String &p_shader_identifier) {
	return "NRD " + p_shader_identifier.get_slicec('|', 0);
}

// Keyed by the program's SPIR-V (NRD's version is in it) and the engine's version (RenderingDevice's binary format).
static String _cache_key(const String &p_name, const uint8_t *p_spirv, uint64_t p_size) {
	unsigned char hash[32];
	CryptoCore::sha256(p_spirv, p_size, hash);
	StringBuilder key;
	key.append("[GodotVersionNumber]");
	key.append(GODOT_VERSION_NUMBER);
	key.append("[GodotVersionHash]");
	key.append(GODOT_VERSION_HASH);
	key.append("[Name]");
	key.append(p_name);
	key.append("[SPIRV]");
	key.append(String::hex_encode_buffer(hash, 32));
	return key.as_string().sha256_text();
}

void NRDBinaryCache::init() {
	api = String(RD::get_singleton()->get_device_api_name()).validate_filename().to_lower();
	const String &user_dir = ShaderRD::get_shader_cache_user_dir();
	if (!user_dir.is_empty()) {
		Ref<DirAccess> d = DirAccess::open(user_dir);
		if (d.is_valid() && (d->dir_exists(NRDDenoiser::CACHE_FOLDER) || d->make_dir(NRDDenoiser::CACHE_FOLDER) == OK)) {
			dir = user_dir;
		} else {
			ERR_PRINT("Can't create the NRD shader cache folder, its shaders will be compiled at every launch: " + user_dir.path_join(NRDDenoiser::CACHE_FOLDER));
		}
	}
	const String &shader_res_dir = ShaderRD::get_shader_cache_res_dir();
	if (!shader_res_dir.is_empty() && shader_res_dir != user_dir && DirAccess::dir_exists_absolute(shader_res_dir.path_join(NRDDenoiser::CACHE_FOLDER))) {
		res_dir = shader_res_dir;
	}
}

Vector<uint8_t> NRDBinaryCache::load(const String &p_key) const {
	const String file_name = NRDDenoiser::get_cache_file_name(p_key, api);
	Ref<FileAccess> f;
	if (!dir.is_empty()) {
		f = FileAccess::open(dir.path_join(file_name), FileAccess::READ);
	}
	if (f.is_null() && !res_dir.is_empty()) {
		f = FileAccess::open(res_dir.path_join(file_name), FileAccess::READ);
	}
	if (f.is_null()) {
		return Vector<uint8_t>();
	}
	char header[5] = {};
	f->get_buffer((uint8_t *)header, 4);
	if (String(header) != nrd_binary_cache_header || f->get_32() != nrd_binary_cache_version) {
		return Vector<uint8_t>();
	}
	const uint32_t size = f->get_32();
	if (size == 0 || uint64_t(size) > f->get_length() - f->get_position()) {
		return Vector<uint8_t>();
	}
	Vector<uint8_t> binary;
	binary.resize(size);
	if (f->get_buffer(binary.ptrw(), size) != size) {
		return Vector<uint8_t>();
	}
	return binary;
}

void NRDBinaryCache::save(const String &p_key, const Vector<uint8_t> &p_binary) const {
	if (dir.is_empty() || p_binary.is_empty()) {
		return;
	}
	const String path = dir.path_join(NRDDenoiser::get_cache_file_name(p_key, api));
	// Written aside, then renamed: a crash while writing leaves no truncated entry.
	const String temporary_path = path + "." + itos(Thread::get_caller_id()) + ".tmp";
	{
		Ref<FileAccess> f = FileAccess::open(temporary_path, FileAccess::WRITE);
		ERR_FAIL_COND_MSG(f.is_null(), "Can't write the NRD shader cache: " + temporary_path);
		f->store_buffer(NRDDenoiser::make_cache_file(p_binary));
	}
	Ref<DirAccess> d = DirAccess::open(dir);
	ERR_FAIL_COND(d.is_null());
	if (d->rename(temporary_path, path) != OK) {
		d->remove(temporary_path);
	}
}

static NRDPipeline _create_pipeline(const NRDBinaryCache &p_cache, const uint8_t *p_spirv, uint64_t p_size, const String &p_name) {
	RD *rd = RD::get_singleton();
	const String key = _cache_key(p_name, p_spirv, p_size);
	Vector<uint8_t> binary = p_cache.load(key);
	if (binary.is_empty()) {
		print_verbose(vformat("NRD: shader cache miss for %s, compiling it.", p_name));
		RD::ShaderStageSPIRVData stage;
		stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
		stage.spirv.resize(p_size);
		memcpy(stage.spirv.ptrw(), p_spirv, p_size);
		binary = rd->shader_compile_binary_from_spirv({ stage }, p_name);
		ERR_FAIL_COND_V_MSG(binary.is_empty(), NRDPipeline(), "NRD: could not compile " + p_name + ".");
		p_cache.save(key, binary);
	}
	NRDPipeline pipeline;
	pipeline.shader = rd->shader_create_from_bytecode(binary);
	ERR_FAIL_COND_V_MSG(pipeline.shader.is_null(), NRDPipeline(), "NRD: could not create " + p_name + ".");
	rd->set_resource_name(pipeline.shader, p_name);
	pipeline.pipeline = rd->compute_pipeline_create(pipeline.shader);
	return pipeline;
}

bool NRDDenoiser::is_available() {
	return true;
}

void NRDDenoiser::get_bake_programs(LocalVector<BakeProgram> &r_programs) {
	auto add = [&r_programs](const String &p_name, const uint8_t *p_spirv, uint64_t p_size) {
		BakeProgram program;
		program.name = p_name;
		program.cache_key = _cache_key(p_name, p_spirv, p_size);
		program.spirv.resize(p_size);
		memcpy(program.spirv.ptrw(), p_spirv, p_size);
		r_programs.push_back(program);
	};
	add(NRD_FRONTEND_NAME, nrd_frontend_spirv, sizeof(nrd_frontend_spirv));
	add(NRD_BACKEND_NAME, nrd_backend_spirv, sizeof(nrd_backend_spirv));

	// Every pass denoise() dispatches: an instance's pipelines, as it makes them (no GPU work).
	const nrd::DenoiserDesc denoiser_desc = { NRD_IDENTIFIER, NRD_DENOISER };
	nrd::InstanceCreationDesc creation_desc = {};
	creation_desc.denoisers = &denoiser_desc;
	creation_desc.denoisersNum = 1;
	nrd::Instance *instance = nullptr;
	ERR_FAIL_COND_MSG(nrd::CreateInstance(creation_desc, instance) != nrd::Result::SUCCESS, "NRD: could not create its instance.");
	const nrd::InstanceDesc &instance_desc = *nrd::GetInstanceDesc(*instance);
	for (uint32_t i = 0; i < instance_desc.pipelinesNum; i++) {
		const nrd::PipelineDesc &pipeline_desc = instance_desc.pipelines[i];
		add(_pass_name(String::utf8(pipeline_desc.shaderIdentifier)), (const uint8_t *)pipeline_desc.computeShaderSPIRV.bytecode, pipeline_desc.computeShaderSPIRV.size);
	}
	nrd::DestroyInstance(*instance);
}

NRDDenoiser::NRDDenoiser() {
	state = memnew(State);
	RD *rd = RD::get_singleton();
	RD::SamplerState sampler_state;
	sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	state->samplers[int(nrd::Sampler::NEAREST_CLAMP)] = rd->sampler_create(sampler_state);
	sampler_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	state->samplers[int(nrd::Sampler::LINEAR_CLAMP)] = rd->sampler_create(sampler_state);
	state->cache.init();
	state->frontend = _create_pipeline(state->cache, nrd_frontend_spirv, sizeof(nrd_frontend_spirv), NRD_FRONTEND_NAME);
	state->backend = _create_pipeline(state->cache, nrd_backend_spirv, sizeof(nrd_backend_spirv), NRD_BACKEND_NAME);
}

NRDDenoiser::~NRDDenoiser() {
	RD *rd = RD::get_singleton();
	for (const KeyValue<String, NRDPipeline> &E : state->pipelines) {
		rd->free_rid(E.value.shader); // Frees its pipeline too.
	}
	for (const RID &shader : { state->frontend.shader, state->backend.shader }) {
		if (shader.is_valid()) {
			rd->free_rid(shader);
		}
	}
	for (const RID &sampler : state->samplers) {
		rd->free_rid(sampler);
	}
	memdelete(state);
}

void NRDDenoiser::denoise(RenderSceneBuffersRD *p_render_buffers, const Parameters &p_params) {
	ERR_FAIL_NULL(p_render_buffers);
	ERR_FAIL_COND(state->frontend.pipeline.is_null() || state->backend.pipeline.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	Ref<NRDContext> context;
	if (p_render_buffers->has_custom_data(RB_NRD_CONTEXT)) {
		context = p_render_buffers->get_custom_data(RB_NRD_CONTEXT);
	} else {
		context.instantiate();
		p_render_buffers->set_custom_data(RB_NRD_CONTEXT, context);
	}

	if (!context->instance || context->size != p_params.size) {
		context->free_data();
		const nrd::DenoiserDesc denoiser_desc = { NRD_IDENTIFIER, NRD_DENOISER };
		nrd::InstanceCreationDesc creation_desc = {};
		creation_desc.denoisers = &denoiser_desc;
		creation_desc.denoisersNum = 1;
		ERR_FAIL_COND_MSG(nrd::CreateInstance(creation_desc, context->instance) != nrd::Result::SUCCESS, "NRD: could not create its instance.");
		context->size = p_params.size;

		const nrd::InstanceDesc &instance_desc = *nrd::GetInstanceDesc(*context->instance);
		for (uint32_t i = 0; i < instance_desc.permanentPoolSize; i++) {
			const nrd::TextureDesc &desc = instance_desc.permanentPool[i];
			const Size2i size = (p_params.size + Size2i(desc.downsampleFactor - 1, desc.downsampleFactor - 1)) / int(desc.downsampleFactor);
			context->permanent_pool.push_back(_create_texture(_rd_format(desc.format), size, vformat("NRD permanent %d", i)));
		}
		for (uint32_t i = 0; i < instance_desc.transientPoolSize; i++) {
			const nrd::TextureDesc &desc = instance_desc.transientPool[i];
			const Size2i size = (p_params.size + Size2i(desc.downsampleFactor - 1, desc.downsampleFactor - 1)) / int(desc.downsampleFactor);
			context->transient_pool.push_back(_create_texture(_rd_format(desc.format), size, vformat("NRD transient %d", i)));
		}
		context->constant_buffer_size = (instance_desc.constantBufferMaxDataSize + 15) & ~15u;
		// Formats of NRD's inputs and outputs (NRDDescs.h, ResourceType): radiance RGBA16F, the normal in the library's
		// encoding (R10G10B10A2 unless built otherwise).
		const RD::DataFormat normal_format = nrd::GetLibraryDesc()->normalEncoding == nrd::NormalEncoding::R10_G10_B10_A2_UNORM ? RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32 : RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		context->in_diffuse = _create_texture(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, p_params.size, "NRD IN_DIFF_RADIANCE_HITDIST");
		context->in_specular = _create_texture(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, p_params.size, "NRD IN_SPEC_RADIANCE_HITDIST");
		context->in_normal_roughness = _create_texture(normal_format, p_params.size, "NRD IN_NORMAL_ROUGHNESS");
		context->out_diffuse = _create_texture(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, p_params.size, "NRD OUT_DIFF_RADIANCE_HITDIST");
		context->out_specular = _create_texture(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, p_params.size, "NRD OUT_SPEC_RADIANCE_HITDIST");
	}

	const nrd::InstanceDesc &instance_desc = *nrd::GetInstanceDesc(*context->instance);
	const nrd::LibraryDesc &library_desc = *nrd::GetLibraryDesc();

	// The settings. The world is the camera's, moved to its position: NRD gets the camera's motion from the two view
	// matrices, and their translations a few kilometers from the origin would leave a still camera a fraction of a pixel of
	// phantom motion in floats (CAMERA-REPROJECTION-FIXES.md, camera_view_delta()). NRD's README allows it: "If coordinate
	// system moves with the camera, camera delta must be included to reflect camera motion".
	nrd::CommonSettings common;
	_store_matrix(common.viewToClipMatrix, p_params.projection);
	_store_matrix(common.viewToClipMatrixPrev, p_params.prev_projection);
	Transform3D camera = p_params.cam_transform;
	camera.origin = Vector3();
	Transform3D prev_camera = p_params.prev_cam_transform;
	prev_camera.origin = p_params.prev_cam_transform.origin - p_params.cam_transform.origin;
	_store_matrix(common.worldToViewMatrix, Projection(camera.affine_inverse()));
	_store_matrix(common.worldToViewMatrixPrev, Projection(prev_camera.affine_inverse()));
	// The raygen's velocity: previous minus current, in UV (pixelUvPrev = pixelUv + mv).
	common.motionVectorScale[0] = 1.0f;
	common.motionVectorScale[1] = 1.0f;
	common.motionVectorScale[2] = 0.0f;
	common.cameraJitter[0] = p_params.jitter.x;
	common.cameraJitter[1] = p_params.jitter.y;
	common.cameraJitterPrev[0] = p_params.prev_jitter.x;
	common.cameraJitterPrev[1] = p_params.prev_jitter.y;
	common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] = uint16_t(p_params.size.width);
	common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] = uint16_t(p_params.size.height);
	common.frameIndex = context->frame_index;
	common.accumulationMode = context->frame_index == 0 ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
	ERR_FAIL_COND(nrd::SetCommonSettings(*context->instance, common) != nrd::Result::SUCCESS);

	// The raygen picks the primary hit's lobe with a probability clamped to [1/4, 3/4] against Bayer dithering, as
	// AREA_3X3 asks (raytracing_inc.glsl, nrd_lobe_threshold()); the rest is NRD's defaults.
	nrd::ReblurSettings settings;
	settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
	ERR_FAIL_COND(nrd::SetDenoiserSettings(*context->instance, NRD_IDENTIFIER, &settings) != nrd::Result::SUCCESS);

	const uint32_t groups_x = (p_params.size.width + 7) / 8;
	const uint32_t groups_y = (p_params.size.height + 7) / 8;

	// Front end: the raygen's light of each lobe and its normal and roughness guide into NRD's encodings.
	{
		struct {
			float hit_distance_parameters[4];
			uint32_t size[2];
			uint32_t relax;
			uint32_t pad;
		} push_constant = {};
		push_constant.hit_distance_parameters[0] = settings.hitDistanceParameters.A;
		push_constant.hit_distance_parameters[1] = settings.hitDistanceParameters.B;
		push_constant.hit_distance_parameters[2] = settings.hitDistanceParameters.C;
		push_constant.size[0] = p_params.size.width;
		push_constant.size[1] = p_params.size.height;

		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, state->frontend.pipeline);
		rd->compute_list_bind_uniform_set(compute_list,
				uniform_set_cache->get_cache(state->frontend.shader, 0,
						RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, p_params.diffuse),
						RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_params.specular),
						RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_params.normal_roughness),
						RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, p_params.view_z),
						RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, context->in_diffuse),
						RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, context->in_specular),
						RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, context->in_normal_roughness)),
				0);
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
		rd->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
		rd->compute_list_end();
	}

	// NRD's dispatches. Each in a compute list of its own: RenderingDevice's graph orders them and their barriers by what
	// each reads and writes.
	const nrd::DispatchDesc *dispatches = nullptr;
	uint32_t dispatch_count = 0;
	ERR_FAIL_COND(nrd::GetComputeDispatches(*context->instance, &NRD_IDENTIFIER, 1, dispatches, dispatch_count) != nrd::Result::SUCCESS);

	uint32_t constant_buffer_index = 0;
	RID constant_buffer;
	for (uint32_t i = 0; i < dispatch_count; i++) {
		const nrd::DispatchDesc &dispatch = dispatches[i];
		const nrd::PipelineDesc &pipeline_desc = instance_desc.pipelines[dispatch.pipelineIndex];

		const String identifier = String::utf8(pipeline_desc.shaderIdentifier);
		NRDPipeline *pipeline = state->pipelines.getptr(identifier);
		if (!pipeline) {
			pipeline = &state->pipelines.insert(identifier, _create_pipeline(state->cache, (const uint8_t *)pipeline_desc.computeShaderSPIRV.bytecode, pipeline_desc.computeShaderSPIRV.size, _pass_name(identifier)))->value;
		}
		ERR_FAIL_COND(pipeline->pipeline.is_null());

		// Set resourcesSpaceIndex: the textures, then the storage images, each range from the base register, at the
		// SPIR-V binding offsets of their kind (LibraryDesc::spirvBindingOffsets).
		LocalVector<RD::Uniform> resources;
		uint32_t resource_index = 0;
		for (uint32_t r = 0; r < pipeline_desc.resourceRangesNum; r++) {
			const nrd::ResourceRangeDesc &range = pipeline_desc.resourceRanges[r];
			const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
			const uint32_t binding_offset = storage ? library_desc.spirvBindingOffsets.storageTextureAndBufferOffset : library_desc.spirvBindingOffsets.textureOffset;
			for (uint32_t k = 0; k < range.descriptorsNum; k++) {
				const nrd::ResourceDesc &resource = dispatch.resources[resource_index++];
				RID texture;
				switch (resource.type) {
					case nrd::ResourceType::IN_MV:
						texture = p_params.motion_vectors;
						break;
					case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
						texture = context->in_normal_roughness;
						break;
					case nrd::ResourceType::IN_VIEWZ:
						texture = p_params.view_z;
						break;
					case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
						texture = context->in_diffuse;
						break;
					case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
						texture = context->in_specular;
						break;
					case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
						texture = context->out_diffuse;
						break;
					case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
						texture = context->out_specular;
						break;
					case nrd::ResourceType::TRANSIENT_POOL:
						texture = context->transient_pool[resource.indexInPool];
						break;
					case nrd::ResourceType::PERMANENT_POOL:
						texture = context->permanent_pool[resource.indexInPool];
						break;
					default:
						ERR_FAIL_MSG(vformat("NRD: unexpected resource %s.", nrd::GetResourceTypeString(resource.type)));
				}
				resources.push_back(RD::Uniform(storage ? RD::UNIFORM_TYPE_IMAGE : RD::UNIFORM_TYPE_TEXTURE, binding_offset + instance_desc.resourcesBaseRegisterIndex + k, texture));
			}
		}

		// Set constantBufferAndSamplersSpaceIndex: the samplers and the constant data. A constant buffer per upload, so
		// that none changes under a dispatch of the same frame; the data of a dispatch that matches the previous one's is
		// not uploaded again.
		LocalVector<RD::Uniform> constants;
		for (uint32_t s = 0; s < instance_desc.samplersNum; s++) {
			constants.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, library_desc.spirvBindingOffsets.samplerOffset + instance_desc.samplersBaseRegisterIndex + s, state->samplers[int(instance_desc.samplers[s])]));
		}
		if (pipeline_desc.hasConstantData) {
			if (constant_buffer.is_null() || !dispatch.constantBufferDataMatchesPreviousDispatch) {
				if (constant_buffer_index == context->constant_buffers.size()) {
					context->constant_buffers.push_back(rd->uniform_buffer_create(context->constant_buffer_size));
				}
				constant_buffer = context->constant_buffers[constant_buffer_index++];
				rd->buffer_update(constant_buffer, 0, dispatch.constantBufferDataSize, dispatch.constantBufferData);
			}
			constants.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, library_desc.spirvBindingOffsets.constantBufferOffset + instance_desc.constantBufferRegisterIndex, constant_buffer));
		}

		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, pipeline->pipeline);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(pipeline->shader, instance_desc.resourcesSpaceIndex, resources), instance_desc.resourcesSpaceIndex);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(pipeline->shader, instance_desc.constantBufferAndSamplersSpaceIndex, constants), instance_desc.constantBufferAndSamplersSpaceIndex);
		rd->compute_list_dispatch(compute_list, dispatch.gridWidth, dispatch.gridHeight, 1);
		rd->compute_list_end();
	}

	// Back end: the denoised light out of NRD's encoding, into the raygen's textures.
	{
		struct {
			uint32_t size[2];
			uint32_t relax;
			uint32_t pad;
		} push_constant = {};
		push_constant.size[0] = p_params.size.width;
		push_constant.size[1] = p_params.size.height;

		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, state->backend.pipeline);
		rd->compute_list_bind_uniform_set(compute_list,
				uniform_set_cache->get_cache(state->backend.shader, 0,
						RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, context->out_diffuse),
						RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, context->out_specular),
						RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_params.diffuse),
						RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, p_params.specular)),
				0);
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
		rd->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
		rd->compute_list_end();
	}

	context->frame_index++;
}

} // namespace RendererRD

#else // !NRD_DENOISER_ENABLED

namespace RendererRD {

struct NRDDenoiser::State {};

bool NRDDenoiser::is_available() {
	return false;
}

void NRDDenoiser::get_bake_programs(LocalVector<BakeProgram> &r_programs) {}

void NRDDenoiser::denoise(RenderSceneBuffersRD *p_render_buffers, const Parameters &p_params) {}

NRDDenoiser::NRDDenoiser() {}

NRDDenoiser::~NRDDenoiser() {}

} // namespace RendererRD

#endif // NRD_DENOISER_ENABLED
