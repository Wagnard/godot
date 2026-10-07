/**************************************************************************/
/*  streamline.h                                                          */
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

#include "core/object/class_db.h" // IWYU pragma: keep
#include "core/object/object.h"
#include "core/os/thread_safe.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/binder_common.h" // IWYU pragma: keep
#include "core/variant/variant.h"
#include "drivers/streamline/streamline_data.h"

class Streamline : public Object {
	GDCLASS(Streamline, Object);
	_THREAD_SAFE_CLASS_
protected:
	static void _bind_methods();
	static Streamline *singleton;
	SafeNumeric<uint32_t> swap_chain_serial;

public:
	static Streamline *get_singleton();
	static void register_singleton();

	void emit_marker(StreamlineMarkerType p_marker);
	void set_parameter(StreamlineParameterType p_parameter_type, const Variant &p_value);
	bool get_capability(StreamlineCapabilityType p_capability_type);

	void set_internal_parameter(const char *p_key, void *p_value);
	void *get_internal_parameter(StreamlineInternalParameterType p_internal_parameter_type);

	void update_project_settings();

	// D3D12: the kind of swap chain frame generation needs changed (DLSS-G to be loaded or
	// unloaded, DLSS-G guide section 18). The D3D12 driver recreates a swap chain whose creation
	// serial differs. Any thread.
	uint32_t get_swap_chain_serial() const { return swap_chain_serial.get(); }
	void bump_swap_chain_serial() { swap_chain_serial.increment(); }

	// The frame token of the draw the render thread is executing, and the one a submission thread presents with:
	// present markers must carry the token of the frame being presented, which the render thread has already left.
	// 0 (the default on every thread) uses the draw's own token.
	uint64_t get_render_frame_token() const;
	static void set_present_frame_token(uint64_t p_token);

	Streamline();
	virtual ~Streamline();
};

VARIANT_ENUM_CAST(StreamlineParameterType);
VARIANT_ENUM_CAST(StreamlineCapabilityType);
