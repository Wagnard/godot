/**************************************************************************/
/*  shadow_flicker_diag.h                                                 */
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

#pragma once

// Diagnostic for Wagnard/godot#1 (SHADOW-FLICKER-DIAG.md): finds the positional light shadows whose inputs alternate
// between two states every frame (A, B, A, B...), the shape of the reported OmniLight3D shadow flicker. Each probe gives,
// per light and per frame, a few numbers; when a light's numbers alternate between exactly two sets for 8 consecutive
// frames, both sets are printed (at most once per light and probe every 10 s). Nothing changes the rendering.
// On by default in builds with DEBUG_ENABLED (editor, debug templates); GODOT_SHADOW_FLICKER_DIAG=0 turns it off.

#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"
#include "core/templates/hashfuncs.h"

class ShadowFlickerDiag {
public:
	static constexpr int MAX_FIELDS = 16;

	static bool enabled() {
#ifdef DEBUG_ENABLED
		static const bool on = OS::get_singleton()->get_environment("GODOT_SHADOW_FLICKER_DIAG") != "0";
		return on;
#else
		return false; // Release templates: players never pay for it.
#endif
	}

	static ShadowFlickerDiag &get() {
		static ShadowFlickerDiag diag;
		return diag;
	}

	// p_probe: a static string naming the probe; p_names: its field names (p_count of them).
	void record(const char *p_probe, const char *const *p_names, uint64_t p_key, uint64_t p_frame, const uint64_t *p_fields, int p_count) {
		if (!announced) {
			announced = true;
			print_line("SHADOW FLICKER DIAG: on (GODOT_SHADOW_FLICKER_DIAG=0 turns it off). Prints a line when a light's shadow inputs alternate every frame.");
		}
		const uint64_t key = hash_murmur3_one_64(p_key, hash_murmur3_one_64(uint64_t(p_probe)));
		History &h = histories[key];
		const uint64_t sig = hash_murmur3_buffer(p_fields, sizeof(uint64_t) * p_count);
		if (h.frame + 1 != p_frame || h.records < 2) {
			h.alternations = 0;
		}
		if (h.records >= 2 && sig == h.sig[1] && sig != h.sig[0]) {
			h.alternations++;
		} else if (h.frame + 1 == p_frame) {
			h.alternations = 0;
		}
		h.sig[1] = h.sig[0];
		h.sig[0] = sig;
		for (int i = 0; i < MAX_FIELDS; i++) {
			h.fields[1][i] = h.fields[0][i];
			h.fields[0][i] = i < p_count ? p_fields[i] : 0;
		}
		h.frame = p_frame;
		h.records++;

		if (h.alternations == 8) {
			const uint64_t now = OS::get_singleton()->get_ticks_msec();
			if (h.last_print == 0 || now - h.last_print > 10000) {
				h.last_print = now;
				String a;
				String b;
				for (int i = 0; i < p_count; i++) {
					a += vformat(" %s=%s", p_names[i], _format(h.fields[0][i]));
					b += vformat(" %s=%s", p_names[i], _format(h.fields[1][i]));
				}
				print_line(vformat("SHADOW FLICKER DIAG [%s] key %s frame %d: alternates every frame between\n  A:%s\n  B:%s", p_probe, String::num_uint64(p_key, 16), p_frame, a, b));
			}
		}
	}

private:
	// Small values (counts, flags, positions in cm) in decimal, the rest (hashes, float bits, keys) in hexadecimal.
	static String _format(uint64_t p_value) {
		const int64_t s = int64_t(p_value);
		if (s > -10000000 && s < 10000000) {
			return itos(s);
		}
		return "0x" + String::num_uint64(p_value, 16);
	}

	struct History {
		uint64_t frame = 0;
		uint64_t records = 0;
		uint64_t sig[2] = {};
		uint64_t fields[2][MAX_FIELDS] = {};
		uint32_t alternations = 0;
		uint64_t last_print = 0;
	};
	HashMap<uint64_t, History> histories;
	bool announced = false;
};
