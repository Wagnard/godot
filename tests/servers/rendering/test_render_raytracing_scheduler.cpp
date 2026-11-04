/**************************************************************************/
/*  test_render_raytracing_scheduler.cpp                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_render_raytracing_scheduler)

#ifdef RD_ENABLED

#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"

// NVIDIA's test_render_raytracing_scheduler.cpp (@49a825dc52) on our budgets of the updates of deforming geometry
// (rendering/pathtracing/dynamic_blas_*, rt_dynamic_select()). Ours ranks and budgets only the updates that can wait: a
// new or reshaped BLAS is built before (never held, not counted), and the periodic builds are decided before as well (a
// displaced surface's every 60 refits), so their cases on new items, structure changes, rebuild intervals and slot
// generations have no counterpart here.

namespace TestRenderRaytracingScheduler {

using namespace RendererSceneRenderImplementation;

static RTDynamicCandidate make_candidate(uint64_t p_key, uint32_t p_triangles = 100, bool p_rebuild = false, bool p_rebuild_only = false) {
	RTDynamicCandidate candidate;
	candidate.key = p_key;
	candidate.triangles = p_triangles;
	candidate.rebuild = p_rebuild;
	candidate.rebuild_only = p_rebuild_only;
	candidate.in_view = true;
	candidate.score = 1.0f;
	candidate.aabb = AABB(Vector3(p_key * 10.0f, 0, 0), Vector3(1, 1, 1));
	return candidate;
}

static LocalVector<RTDynamicCandidate> make_candidates(uint32_t p_count, uint32_t p_triangles = 100, bool p_rebuild = false, bool p_rebuild_only = false) {
	LocalVector<RTDynamicCandidate> candidates;
	for (uint32_t i = 0; i < p_count; i++) {
		candidates.push_back(make_candidate(i, p_triangles, p_rebuild, p_rebuild_only));
	}
	return candidates;
}

static uint32_t count_selected(const LocalVector<RTDynamicCandidate> &p_candidates, bool p_downgraded = false) {
	uint32_t count = 0;
	for (const RTDynamicCandidate &candidate : p_candidates) {
		count += candidate.selected && candidate.downgraded == p_downgraded ? 1 : 0;
	}
	return count;
}

static const RTDynamicCandidate *find_key(const LocalVector<RTDynamicCandidate> &p_candidates, uint64_t p_key) {
	for (const RTDynamicCandidate &candidate : p_candidates) {
		if (candidate.key == p_key) {
			return &candidate;
		}
	}
	return nullptr;
}

// A new frame's usage, as _schedule_dynamic_updates() starts it: the cursor carries over.
static void next_frame(RTDynamicBudgetUsage &r_usage) {
	const uint32_t cursor = r_usage.cursor;
	r_usage = RTDynamicBudgetUsage();
	r_usage.frame = 0;
	r_usage.cursor = cursor;
}

TEST_CASE("[RTDynamicScheduler] An instance's BLAS update mode overrides its shader's unless it is auto") {
	CHECK(rt_resolve_blas_update(RSE::RT_BLAS_UPDATE_AUTO, RSE::RT_BLAS_UPDATE_REFIT) == RSE::RT_BLAS_UPDATE_REFIT);
	CHECK(rt_resolve_blas_update(RSE::RT_BLAS_UPDATE_AUTO, RSE::RT_BLAS_UPDATE_AUTO) == RSE::RT_BLAS_UPDATE_AUTO);
	CHECK(rt_resolve_blas_update(RSE::RT_BLAS_UPDATE_REBUILD, RSE::RT_BLAS_UPDATE_REFIT) == RSE::RT_BLAS_UPDATE_REBUILD);
	CHECK(rt_resolve_blas_update(RSE::RT_BLAS_UPDATE_REFIT, RSE::RT_BLAS_UPDATE_REBUILD) == RSE::RT_BLAS_UPDATE_REFIT);
}

TEST_CASE("[RTDynamicScheduler] Budgets cap operations and triangles") {
	SUBCASE("max_operations") {
		RTDynamicBudgets budgets;
		budgets.max_operations = 3;
		RTDynamicBudgetUsage usage;
		LocalVector<RTDynamicCandidate> candidates = make_candidates(10);
		CHECK(rt_dynamic_select(candidates, budgets, usage) == 3);
		CHECK(count_selected(candidates) == 3);
		CHECK(usage.operations == 3);
	}
	SUBCASE("build triangles downgrade periodic builds to refits") {
		RTDynamicBudgets budgets;
		budgets.build_triangles = 250;
		RTDynamicBudgetUsage usage;
		LocalVector<RTDynamicCandidate> candidates = make_candidates(4, 100, true);
		CHECK(rt_dynamic_select(candidates, budgets, usage) == 4);
		CHECK(count_selected(candidates) == 2);
		CHECK(count_selected(candidates, true) == 2);
		CHECK(usage.build_triangles <= 250);
		CHECK(usage.update_triangles == 200);
	}
	SUBCASE("update triangles") {
		RTDynamicBudgets budgets;
		budgets.update_triangles = 300;
		RTDynamicBudgetUsage usage;
		LocalVector<RTDynamicCandidate> candidates = make_candidates(5);
		CHECK(rt_dynamic_select(candidates, budgets, usage) == 3);
		CHECK(usage.update_triangles == 300);
	}
	SUBCASE("no budget: everything is updated as decided") {
		RTDynamicBudgetUsage usage;
		LocalVector<RTDynamicCandidate> candidates = make_candidates(6, 100, true);
		CHECK(rt_dynamic_select(candidates, RTDynamicBudgets(), usage) == 6);
		CHECK(count_selected(candidates) == 6);
	}
}

TEST_CASE("[RTDynamicScheduler] Rebuild-only items never refit") {
	SUBCASE("a spent build budget holds them instead of refitting") {
		RTDynamicBudgets budgets;
		budgets.build_triangles = 250;
		RTDynamicBudgetUsage usage;
		LocalVector<RTDynamicCandidate> candidates = make_candidates(4, 100, true, true);
		CHECK(rt_dynamic_select(candidates, budgets, usage) == 2);
		CHECK(count_selected(candidates) == 2);
		CHECK(count_selected(candidates, true) == 0);
		CHECK(usage.update_triangles == 0);
	}
	SUBCASE("one alone over the build budget still rebuilds when it comes first") {
		RTDynamicBudgets budgets;
		budgets.build_triangles = 100;
		RTDynamicBudgetUsage usage;
		LocalVector<RTDynamicCandidate> candidates = make_candidates(1, 1000, true, true);
		CHECK(rt_dynamic_select(candidates, budgets, usage) == 1);
		CHECK_FALSE(candidates[0].downgraded);
	}
}

TEST_CASE("[RTDynamicScheduler] An oversized item still updates when it comes first") {
	RTDynamicBudgets budgets;
	budgets.max_operations = 2000;
	budgets.update_triangles = 500000;
	RTDynamicBudgetUsage usage;
	LocalVector<RTDynamicCandidate> candidates = make_candidates(3000, 10);
	RTDynamicCandidate big = make_candidate(5000, 400000 * 2);
	big.score = 2.0f;
	candidates.push_back(big);
	rt_dynamic_select(candidates, budgets, usage);
	CHECK(find_key(candidates, 5000)->selected);
	CHECK(usage.operations <= budgets.max_operations);
}

TEST_CASE("[RTDynamicScheduler] Higher visibility is updated first") {
	RTDynamicBudgets budgets;
	budgets.max_operations = 1;
	const float visibility[] = { 0.1f, 0.9f, 0.5f };
	uint32_t waited[] = { 0, 0, 0 };
	RTDynamicBudgetUsage usage;
	// Each frame, the update selected waited 0 frames since and the others one more; the second one now outranks the first.
	const uint64_t expected[] = { 1, 2 };
	for (uint64_t key : expected) {
		LocalVector<RTDynamicCandidate> candidates = make_candidates(3);
		for (RTDynamicCandidate &candidate : candidates) {
			candidate.score = rt_dynamic_score(visibility[candidate.key], waited[candidate.key], budgets.max_stale_frames);
		}
		next_frame(usage);
		CHECK(rt_dynamic_select(candidates, budgets, usage) == 1);
		CHECK(find_key(candidates, key)->selected);
		for (const RTDynamicCandidate &candidate : candidates) {
			waited[candidate.key] = candidate.selected ? 0 : waited[candidate.key] + 1;
		}
	}
}

TEST_CASE("[RTDynamicScheduler] Equal scores take turns") {
	RTDynamicBudgets budgets;
	budgets.max_operations = 2;
	budgets.max_stale_frames = 1000;
	RTDynamicBudgetUsage usage;
	HashMap<uint64_t, int> updates;
	uint32_t waited[8] = {};
	for (int f = 0; f < 8; f++) {
		LocalVector<RTDynamicCandidate> candidates = make_candidates(8);
		for (RTDynamicCandidate &candidate : candidates) {
			// Frames since the update's last frame, as _schedule_dynamic_updates() reads it from the cache entry.
			candidate.score = rt_dynamic_score(0.5f, ++waited[candidate.key], budgets.max_stale_frames);
		}
		next_frame(usage);
		rt_dynamic_select(candidates, budgets, usage);
		for (const RTDynamicCandidate &candidate : candidates) {
			if (candidate.selected) {
				updates[candidate.key]++;
				waited[candidate.key] = 0;
			}
		}
	}
	for (uint64_t i = 0; i < 8; i++) {
		CHECK_MESSAGE(updates[i] == 2, vformat("Item %d was updated %d times in 8 frames.", i, updates[i]));
	}
}

TEST_CASE("[RTDynamicScheduler] Frames waited raise the score, past max_stale_frames first") {
	const uint32_t max_stale = 5;
	CHECK(rt_dynamic_score(0.5f, 1, max_stale) > rt_dynamic_score(0.5f, 0, max_stale));
	CHECK(rt_dynamic_score(0.5f, 4, max_stale) == doctest::Approx(0.5f * 5.0f));
	CHECK_MESSAGE(rt_dynamic_score(0.0f, 0, max_stale) > 0.0f, "Invisible updates still rank by the frames they waited.");

	RTDynamicBudgets budgets;
	budgets.max_operations = 1;
	budgets.max_stale_frames = max_stale;
	RTDynamicBudgetUsage usage;
	LocalVector<RTDynamicCandidate> candidates = make_candidates(2);
	candidates[0].score = rt_dynamic_score(1.0f, max_stale - 1, max_stale);
	candidates[1].score = rt_dynamic_score(0.001f, max_stale, max_stale);
	rt_dynamic_select(candidates, budgets, usage);
	CHECK_MESSAGE(find_key(candidates, 1)->selected, "max_stale_frames forces an update.");
}

TEST_CASE("[RTDynamicScheduler] Viewports share the frame's budgets") {
	RTDynamicBudgets budgets;
	budgets.max_operations = 5;
	RTDynamicBudgetUsage usage;
	next_frame(usage);
	LocalVector<RTDynamicCandidate> first_view = make_candidates(3);
	CHECK(rt_dynamic_select(first_view, budgets, usage) == 3);

	LocalVector<RTDynamicCandidate> second_view;
	for (uint64_t i = 0; i < 4; i++) {
		second_view.push_back(make_candidate(100 + i));
	}
	CHECK(rt_dynamic_select(second_view, budgets, usage) == 2);
	CHECK(usage.operations == 5);
}

TEST_CASE("[RTDynamicScheduler] Visibility ranks by apparent size") {
	const Vector3 camera;
	const float offscreen_weight = 0.25f;
	const AABB near_aabb(Vector3(-0.5, -0.5, -5.5), Vector3(1, 1, 1));
	AABB far_aabb = near_aabb;
	far_aabb.position.z = -50.5;
	const float near_visibility = rt_dynamic_visibility(near_aabb, true, camera, offscreen_weight);
	CHECK(near_visibility > rt_dynamic_visibility(far_aabb, true, camera, offscreen_weight));

	AABB behind = near_aabb;
	behind.position.z = 4.5;
	CHECK(rt_dynamic_visibility(behind, false, camera, offscreen_weight) > 0.0f);
	CHECK(rt_dynamic_visibility(behind, false, camera, offscreen_weight) == doctest::Approx(near_visibility * offscreen_weight));

	CHECK_MESSAGE(rt_dynamic_visibility(AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4)), true, camera, offscreen_weight) == doctest::Approx(1.0f), "A camera inside the bounds sees it fully.");
	CHECK(rt_dynamic_visibility(AABB(), true, camera, offscreen_weight) == 0.0f);
}

} // namespace TestRenderRaytracingScheduler

#endif // RD_ENABLED
