// Helper functions for texture streaming feedback.
// Used by the Forward+ and Mobile renderers.

// Reference texture size used for LOD normalization: 4096x4096.
#define STREAMING_LOD_REFERENCE_LOG2 12.0

// Returns the squared pixel footprint in UV space.
float streaming_footprint_sq(vec2 uv_ddx, vec2 uv_ddy) {
	float uv_ddx_sq = dot(uv_ddx, uv_ddx);
	float uv_ddy_sq = dot(uv_ddy, uv_ddy);

	// Limit anisotropic filtering to 16x.
	const float MAX_ANISOTROPY = 16.0;

	return max(
			min(uv_ddx_sq, uv_ddy_sq),
			max(uv_ddx_sq, uv_ddy_sq) / (MAX_ANISOTROPY * MAX_ANISOTROPY));
}

// Undoes the reference offset on a shader-supplied STREAMING_LOD, so both feedback paths
// hand the same squared UV footprint to the encoder.
float streaming_footprint_sq_from_lod(float lod) {
	return exp2(2.0 * (clamp(lod, -32.0, 32.0) - STREAMING_LOD_REFERENCE_LOG2));
}

#ifdef TEXTURE_STREAMING
// Adds the samplers' mipmap bias to a squared footprint. The sampler's bias is added to the level of detail, whether
// derivatives give it or the shader does (D3D11.3 functional spec 7.18.11 "LOD Calculations":
// biasedLOD = LOD + MipLODBias; Vulkan "Level-of-Detail Operation": VkSamplerCreateInfo::mipLodBias), so a view whose
// samplers are biased (temporal upscalers, TAA, FXAA, a 3D scale below 1, the project's or the viewport's bias) reads a
// finer level than its footprint gives, and the streamed texture must hold that level. As Unreal's virtual texture
// feedback, which takes the view's MaterialTextureMipBias. The squared footprint scales by exp2(2 * bias), the level
// being log2(footprint * size). SceneData is scene_data_block.data in the scene shaders and the path tracer alike.
float streaming_footprint_sq_biased(float footprint_sq) {
	return footprint_sq * exp2(2.0 * scene_data_block.data.texture_mipmap_bias);
}
#endif
