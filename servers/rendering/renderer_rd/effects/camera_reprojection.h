/**************************************************************************/
/*  camera_reprojection.h                                                 */
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

#include "core/math/transform_3d.h"

namespace RendererRD {

// The camera's motion between the previous frame and this one, as the transform taking a point
// from the current view space to the previous view space — the middle factor of every
// `prev_proj * (...) * cur_proj.inverse()` reprojection.
//
// Built relative first on purpose. The reprojections used to be written
// `(corr * prev_proj) * prev.affine_inverse() * cur * (corr * cur_proj).inverse()`: Projection
// only multiplies Projections, so both transforms were converted and the chain evaluated left to
// right in float, folding the camera's WORLD translation into the projection before prev and cur
// could cancel. A few kilometres from the origin that is no longer the identity for a camera that
// did not move: a constant phantom motion vector of up to 0.24 internal px on the floor in front
// of it at (7000, 0, -7000), 0.5 px at 15 km, depending on the view direction (measured with the
// engine's own operators). Static pixels must read [0, 0] (DLSS guide); DLSS smeared the drift
// into the floor under a moving light, FSR2 hid it (it treats >= 0.1 px as motion).
//
// Grouping the two transforms as a Transform3D product would fix the still case but still loses a
// float ulp of the world position (~1e-3 m at 10 km) for a MOVING camera; subtracting the origins
// first avoids both. The translation is exact for a still camera; what remains is ~2e-4 px of
// rounding in the basis product and the projection inverse.
_FORCE_INLINE_ Transform3D camera_view_delta(const Transform3D &p_previous, const Transform3D &p_current) {
	const Basis previous_inverse = p_previous.basis.inverse();
	return Transform3D(previous_inverse * p_current.basis, previous_inverse.xform(p_current.origin - p_previous.origin));
}

} // namespace RendererRD
