// rendering/visibility_range/dithered_fade: the threshold a pixel compares an instance's visibility range fade with, to
// keep or drop the instance there. One function for the rasterizer's opaque passes (scene_forward_clustered.glsl) and
// the path tracer's camera rays (raytracing/raytracing_shade_inc.glsl), so that both fade an instance on the same
// pixels of a frame.
//
// Interleaved gradient noise (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH
// 2014), moved every frame while a temporal resolve runs (the view is jittered), which then averages the pattern into a
// smooth fade. The frame number, not taa_frame_count: that one wraps with the jitter cycle, so each jitter phase would
// always meet the same pattern and the resolve would settle on a fixed grain.
//
// p_frag_coord: the pixel's center, as gl_FragCoord.xy.
float visibility_range_fade_threshold(vec2 p_frag_coord, vec2 p_taa_jitter, uint p_frame_number) {
	float dither_frame = any(notEqual(p_taa_jitter, vec2(0.0))) ? float(p_frame_number % 64u) : 0.0;
	vec2 dither_position = p_frag_coord + 5.588238 * dither_frame;
	return fract(52.9829189 * fract(dot(dither_position, vec2(0.06711056, 0.00583715))));
}
