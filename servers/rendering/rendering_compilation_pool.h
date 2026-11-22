/**************************************************************************/
/*  rendering_compilation_pool.h                                          */
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

#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

// The pool where the renderer and the RenderingDevice drivers compile: ShaderRD variants (and their cache loads),
// pipelines, ray tracing hit groups and programs, D3D12 collections, Vulkan pipeline libraries and deferred operations.
//
// It has 75 % of the main pool's threads, as Unreal Engine's PSO precompile thread pool has 75 % of the hardware
// threads by default (r.pso.PrecompileThreadPoolPercentOfHardwareThreads). Compilations used to run in the main pool,
// one thread per logical core, mostly as high priority tasks: a cold load filled every core for tens of seconds, and a
// game's process runs in the ABOVE_NORMAL priority class on Windows (DisplayServerWindows), so the rest of the machine
// (the desktop, other applications) and the game's own audio thread were starved meanwhile. The cores left over are
// never taken by compilation, however many shaders compile at once (a cap per group task would not hold across them).
//
// Every task and group added to this pool is queried and waited for through it: task and group IDs belong to a pool.
// A pool of under two threads would be no gain: the main pool is used then. GODOT_RENDERING_COMPILATION_THREADS=<n>
// overrides the thread count (0 = the main pool, as before), for measurements.
class RenderingCompilationPool {
	static WorkerThreadPool *_create() {
		WorkerThreadPool *main_pool = WorkerThreadPool::get_singleton();
		const int main_threads = main_pool->get_thread_count();
		int threads = main_threads * 3 / 4;
		const String override_threads = OS::get_singleton()->get_environment("GODOT_RENDERING_COMPILATION_THREADS");
		if (!override_threads.is_empty()) {
			threads = override_threads.to_int();
		}
		if (threads < 2 || main_threads < 2) {
			print_verbose(vformat("RenderingCompilationPool: compilations run in the main pool (%d threads).", main_threads));
			return main_pool;
		}
		// Low priority work (the pipelines precompiled in the background) keeps the main pool's share of the threads.
		const float low_priority_ratio = float(main_pool->get_max_low_priority_thread_count()) / float(main_threads);
		return WorkerThreadPool::get_named_pool(SNAME("RenderingCompilation"), threads, low_priority_ratio);
	}

public:
	// Created on first use (the main pool exists by then), once: a function-local static is initialized thread-safely.
	static WorkerThreadPool *get() {
		static WorkerThreadPool *pool = _create();
		return pool;
	}
};
