#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// The color target's alpha: 0 on opaque surfaces with the motion pass, the blend alpha of transparent ones
// (RenderForwardClustered::_render_scene(), apply_alpha_multiplier).
layout(set = 0, binding = 0) uniform texture2D color_buffer;
layout(r8, set = 1, binding = 0) uniform restrict writeonly image2D reactive_mask;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	vec2 pad;
}
params;

void main() {
	const ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	// AMD's FSR upscaler documentation ("Reactive mask"): write alpha, clamped to around 0.9.
	const float reactive = min(texelFetch(color_buffer, pos, 0).a, 0.9);
	imageStore(reactive_mask, pos, vec4(reactive));
}
