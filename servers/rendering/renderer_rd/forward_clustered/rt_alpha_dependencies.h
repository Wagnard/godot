/**************************************************************************/
/*  rt_alpha_dependencies.h                                               */
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

#include "servers/rendering/shader_language.h"

// Whether a spatial shader's cut-out can be baked into an opacity micromap: what decides if a ray passes through a
// point of a triangle (ALPHA against ALPHA_SCISSOR_THRESHOLD, and `discard`) must depend only on what is the same for
// every instance, frame and view of a mesh surface with that material: its vertex attributes (through vertex() and the
// varyings), the material's uniforms and textures, constants. The time, the camera, the view-space vectors, the
// instance (its transform, instance uniforms, custom data), global uniforms and the facing all make it unbakeable.
// Conservative: an assignment depends on everything its expression reads and on every condition that encloses it, a
// function's values on every condition under which it returns, breaks or continues early.
struct RTAlphaDependencies {
	bool bakeable = false;
	// COLOR reaches the decision: a MultiMesh's instance colors multiply it, so its instances may differ.
	bool reads_color = false;
	// The material's uniforms that reach the decision (values and textures): only a change of one of them changes the
	// cut-out, so they and the hit group key its opacity micromaps (RTMaterialData::omm_alpha_key).
	Vector<StringName> uniforms;
	// The first input that prevents baking, for the logs.
	String reason;
};

RTAlphaDependencies rt_alpha_dependencies(const ShaderLanguage::ShaderNode *p_shader);
