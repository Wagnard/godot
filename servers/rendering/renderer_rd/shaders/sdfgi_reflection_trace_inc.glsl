// The reflection of the SDFGI cascades along a ray: the cascades' distance fields are sphere traced and the light of the
// cells the ray comes close to is accumulated. Shared by the deferred GI pass (environment/gi.glsl, opaque surfaces) and
// by the forward pass (forward_clustered/scene_forward_clustered.glsl, transparent surfaces).
//
// The including shader declares the SDFGI uniform block as "sdfgi", SDFGI_MAX_CASCADES, and two macros:
//   SDFGI_TRACE_SDF(m_cascade): the sampler3D of that cascade's distance field (linear filter, clamped);
//   SDFGI_TRACE_LIGHT(m_cascade): the same for that cascade's light.
// It may define a third one:
//   SDFGI_TRACE_MAX_PASSES: the most passes over the cascades a ray may take (it advances once per pass at most); a ray
//   that would need more ends with what it met.
// A fragment shader must define it: it runs with whatever a material computed, in invocations nobody sees too (helper
// invocations), and no value may keep the GPU in the march below (a device lost otherwise). The deferred pass does not:
// its roughness comes out of an 8-bit UNORM buffer, so the softness below is at least 0.2 and each of its steps either
// advances the ray or adds coverage; the march ends by construction and opaque surfaces do not pay for the counter
// (0.01 to 0.045 ms of the GI pass at 2560x1440 when it was there).

// cam_pos, cam_normal and ray_dir are world oriented and relative to the camera, their y already multiplied by
// sdfgi.y_mult (directions normalized afterwards). Returns the light met, premultiplied by its coverage, and the
// coverage in alpha (it may pass 1.0): 0.0 where the ray left the cascades without coming close to anything.
vec4 sdfgi_trace_reflection(vec3 cam_pos, vec3 cam_normal, vec3 ray_dir, float roughness) {
	// A roughness under -0.04 would make the softness below negative: nothing the ray meets then counts as met, and it
	// steps back and forth around the first surface for ever. The deferred pass cannot have one (it reads an 8-bit
	// UNORM buffer); the transparent pass has no roughness limiter and a material may write any value.
	roughness = max(roughness, 0.0);

	vec3 pos_to_uvw = 1.0 / sdfgi.grid_size;
	vec4 light_accum = vec4(0.0);

	float blend_size = (sdfgi.grid_size.x / float(sdfgi.probe_axis_size - 1)) * 0.5;

	float radius_sizes[SDFGI_MAX_CASCADES];
	uint cascade = 0xFFFF;

	float base_distance = length(cam_pos);
	for (uint i = 0; i < sdfgi.max_cascades; i++) {
		radius_sizes[i] = (1.0 / sdfgi.cascades[i].to_cell) * (sdfgi.grid_size.x * 0.5 - blend_size);
		if (cascade == 0xFFFF && base_distance < radius_sizes[i]) {
			cascade = i;
		}
	}

	cascade = min(cascade, sdfgi.max_cascades - 1);

	float max_distance = radius_sizes[sdfgi.max_cascades - 1];
	vec3 ray_pos = cam_pos;

	{
		float prev_radius = cascade > 0 ? radius_sizes[cascade - 1] : 0.0;
		float base_blend = (base_distance - prev_radius) / (radius_sizes[cascade] - prev_radius);
		float bias = (1.0 + base_blend) * 1.1;
		vec3 abs_ray_dir = abs(ray_dir);
		//ray_pos += ray_dir * (bias / sdfgi.cascades[cascade].to_cell); //bias to avoid self occlusion
		ray_pos += (ray_dir * 1.0 / max(abs_ray_dir.x, max(abs_ray_dir.y, abs_ray_dir.z)) + cam_normal * 1.4) * bias / sdfgi.cascades[cascade].to_cell;
	}
	float softness = 0.2 + min(1.0, roughness * 5.0) * 4.0; //approximation to roughness so it does not seem like a hard fade
	uint i = 0;
	bool found = false;
#ifdef SDFGI_TRACE_MAX_PASSES
	uint passes = 0;
	while (passes < SDFGI_TRACE_MAX_PASSES) {
#else
	while (true) {
#endif
		if (length(ray_pos) >= max_distance || light_accum.a > 0.99) {
			break;
		}
		if (!found && i >= cascade && length(ray_pos) < radius_sizes[i]) {
			uint next_i = min(i + 1, sdfgi.max_cascades - 1);
			cascade = max(i, cascade); //never go down

			vec3 pos = ray_pos - sdfgi.cascades[i].position;
			pos *= sdfgi.cascades[i].to_cell * pos_to_uvw;

			float fdistance = textureLod(SDFGI_TRACE_SDF(i), pos, 0.0).r * 255.0 - 1.1;

			vec4 hit_light = vec4(0.0);
			if (fdistance < softness) {
				hit_light.rgb = textureLod(SDFGI_TRACE_LIGHT(i), pos, 0.0).rgb;
				hit_light.rgb *= 0.5; //approximation given value read is actually meant for anisotropy
				hit_light.a = clamp(1.0 - (fdistance / softness), 0.0, 1.0);
				hit_light.rgb *= hit_light.a;
			}

			fdistance /= sdfgi.cascades[i].to_cell;

			if (i < (sdfgi.max_cascades - 1)) {
				pos = ray_pos - sdfgi.cascades[next_i].position;
				pos *= sdfgi.cascades[next_i].to_cell * pos_to_uvw;

				float fdistance2 = textureLod(SDFGI_TRACE_SDF(next_i), pos, 0.0).r * 255.0 - 1.1;

				vec4 hit_light2 = vec4(0.0);
				if (fdistance2 < softness) {
					hit_light2.rgb = textureLod(SDFGI_TRACE_LIGHT(next_i), pos, 0.0).rgb;
					hit_light2.rgb *= 0.5; //approximation given value read is actually meant for anisotropy
					hit_light2.a = clamp(1.0 - (fdistance2 / softness), 0.0, 1.0);
					hit_light2.rgb *= hit_light2.a;
				}

				float prev_radius = i == 0 ? 0.0 : radius_sizes[max(0, i - 1)];
				float blend = clamp((length(ray_pos) - prev_radius) / (radius_sizes[i] - prev_radius), 0.0, 1.0);

				fdistance2 /= sdfgi.cascades[next_i].to_cell;

				hit_light = mix(hit_light, hit_light2, blend);
				fdistance = mix(fdistance, fdistance2, blend);
			}

			light_accum += hit_light;
			ray_pos += ray_dir * fdistance;
			found = true;
		}
		i++;
		if (i == sdfgi.max_cascades) {
			i = 0;
			found = false;
#ifdef SDFGI_TRACE_MAX_PASSES
			passes++;
#endif
		}
	}

	return light_accum;
}

// The reflection of a surface from what its ray met (sdfgi_trace_reflection()) and from the probes' low-order specular
// light: the traced light alone on a mirror, the probes' from a roughness of 0.2 on. Alpha is how much of the
// reflection the result stands for; the rest keeps what the surface reflected otherwise (the sky, reflection probes).
vec4 sdfgi_resolve_reflection(vec4 light_accum, vec3 probe_specular, float roughness) {
	vec3 light = light_accum.rgb / max(light_accum.a, 0.00001);
	float alpha = min(1.0, light_accum.a);

	float b = clamp(roughness * 5.0, 0.0, 1.0);

	float sa = 1.0 - b;

	vec4 reflection;
	reflection.a = alpha * sa + b;
	if (reflection.a == 0) {
		reflection.rgb = vec3(0.0);
	} else {
		reflection.rgb = (light * alpha * sa + probe_specular * b) / reflection.a;
	}
	return reflection;
}
