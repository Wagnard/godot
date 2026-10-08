/**************************************************************************/
/*  pipeline_hash_map_rd.h                                                */
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
#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/rw_lock.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rb_map.h"
#include "core/templates/rb_set.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"

#define PRINT_PIPELINE_COMPILATION_KEYS 0

template <typename Key, typename CreationClass, typename CreationFunction>
class PipelineHashMapRD {
private:
	CreationClass *creation_object = nullptr;
	CreationFunction creation_function = nullptr;
	Mutex *compilations_mutex = nullptr;
	uint32_t *compilations = nullptr;
	RBMap<uint32_t, RID> hash_map;
	RWLock hash_map_lock; // Lets several threads look pipelines up while one of them adds the compiled ones.
	LocalVector<Pair<uint32_t, RID>> compiled_queue;
	Mutex compiled_queue_mutex;
	RBSet<uint32_t> compilation_set;
	HashMap<uint32_t, WorkerThreadPool::TaskID> compilation_tasks;
	// A compilation is started once, by its background task or by a thread that needs the pipeline before the task ran
	// (it compiles it itself); the task then does nothing. Threads needing a pipeline being compiled wait for the
	// compilation itself, never for a queued task: with parallel draw lists several pool threads can need the same
	// pipeline, and blocking them all on a task still queued would leave no thread to run it.
	RBSet<uint32_t> started_compilations;
	RBSet<uint32_t> running_compilations;
	// Compiled by the thread that needed them: their task is still to run (and do nothing), so it stays in
	// compilation_tasks for clear_pipelines() to wait for before the map can go.
	RBSet<uint32_t> inline_compilations;
	ConditionVariable running_compilations_condition; // With local_mutex.
	BinaryMutex local_mutex;

	void _compile_task(Key p_key) {
		const uint32_t key_hash = p_key.hash();
		{
			MutexLock local_lock(local_mutex);
			if (started_compilations.has(key_hash)) {
				return; // Compiled by the thread that needed it.
			}
			started_compilations.insert(key_hash);
			running_compilations.insert(key_hash);
		}
		_compile_and_finish(p_key, key_hash);
	}

	void _compile_and_finish(const Key &p_key, uint32_t p_key_hash) {
		(creation_object->*creation_function)(p_key);
		{
			MutexLock local_lock(local_mutex);
			running_compilations.erase(p_key_hash);
		}
		running_compilations_condition.notify_all();
	}

	// Waits for a submitted pipeline: compiles it here if its task hasn't started yet, else waits for the compilation
	// running on another thread. Without a key, a task not started yet is waited for (the original behavior, for
	// callers on the render thread).
	void _wait_for_compilation(uint32_t p_key_hash, const Key *p_key) {
		WorkerThreadPool::TaskID task_id_to_wait = WorkerThreadPool::INVALID_TASK_ID;
		{
			MutexLock local_lock(local_mutex);
			if (!compilation_set.has(p_key_hash)) {
				// The pipeline was never submitted, we can't wait for it.
				return;
			}

			if (!started_compilations.has(p_key_hash)) {
				if (p_key != nullptr) {
					started_compilations.insert(p_key_hash);
					running_compilations.insert(p_key_hash);
					inline_compilations.insert(p_key_hash);
				} else {
					HashMap<uint32_t, WorkerThreadPool::TaskID>::Iterator task_it = compilation_tasks.find(p_key_hash);
					if (task_it != compilation_tasks.end()) {
						task_id_to_wait = task_it->value;
						compilation_tasks.remove(task_it);
					}
				}
			} else {
				while (running_compilations.has(p_key_hash)) {
					running_compilations_condition.wait(local_lock);
				}
				return;
			}
		}

		if (p_key != nullptr) {
			_compile_and_finish(*p_key, p_key_hash);
		} else if (task_id_to_wait != WorkerThreadPool::INVALID_TASK_ID) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id_to_wait);
		}
	}

	bool _add_new_pipelines_to_map() {
		thread_local Vector<uint32_t> hashes_added;
		hashes_added.clear();

		{
			MutexLock lock(compiled_queue_mutex);
			if (compiled_queue.is_empty()) {
				return false;
			}

			RWLockWrite write_lock(hash_map_lock);
			for (const Pair<uint32_t, RID> &pair : compiled_queue) {
				hash_map[pair.first] = pair.second;
				hashes_added.push_back(pair.first);
			}

			compiled_queue.clear();
		}

		{
			MutexLock local_lock(local_mutex);
			for (uint32_t hash : hashes_added) {
				if (inline_compilations.has(hash)) {
					continue;
				}
				HashMap<uint32_t, WorkerThreadPool::TaskID>::Iterator task_it = compilation_tasks.find(hash);
				if (task_it != compilation_tasks.end()) {
					compilation_tasks.remove(task_it);
				}
			}
		}

		return !hashes_added.is_empty();
	}

	bool _find_pipeline(uint32_t p_key_hash, RID &r_pipeline) const {
		RWLockRead read_lock(hash_map_lock);
		const RBMap<uint32_t, RID>::Element *e = hash_map.find(p_key_hash);
		if (e == nullptr) {
			return false;
		}

		r_pipeline = e->value();
		return true;
	}

	void _wait_for_all_pipelines() {
		thread_local LocalVector<WorkerThreadPool::TaskID> tasks_to_wait;
		tasks_to_wait.clear();
		{
			MutexLock local_lock(local_mutex);
			for (KeyValue<uint32_t, WorkerThreadPool::TaskID> key_value : compilation_tasks) {
				tasks_to_wait.push_back(key_value.value);
			}
		}

		for (WorkerThreadPool::TaskID task_id : tasks_to_wait) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
		}
	}

public:
	void add_compiled_pipeline(uint32_t p_hash, RID p_pipeline) {
		compiled_queue_mutex.lock();
		compiled_queue.push_back({ p_hash, p_pipeline });
		compiled_queue_mutex.unlock();
	}

	// Start compilation of a pipeline ahead of time in the background. Returns true if the compilation was started, false if it wasn't required. Source is only used for collecting statistics.
	void compile_pipeline(const Key &p_key, uint32_t p_key_hash, RSE::PipelineSource p_source, bool p_high_priority) {
		DEV_ASSERT((creation_object != nullptr) && (creation_function != nullptr) && "Creation object and function was not set before attempting to compile a pipeline.");

		MutexLock local_lock(local_mutex);
		if (compilation_set.has(p_key_hash)) {
			// Check if the pipeline was already submitted.
			return;
		}

		// Record the pipeline as submitted, a task can't be started for it again.
		compilation_set.insert(p_key_hash);

		if (compilations_mutex != nullptr) {
			MutexLock compilations_lock(*compilations_mutex);
			compilations[p_source]++;
		}

#if PRINT_PIPELINE_COMPILATION_KEYS
		String source_name = "UNKNOWN";
		switch (p_source) {
			case RSE::PIPELINE_SOURCE_CANVAS:
				source_name = "CANVAS";
				break;
			case RSE::PIPELINE_SOURCE_MESH:
				source_name = "MESH";
				break;
			case RSE::PIPELINE_SOURCE_SURFACE:
				source_name = "SURFACE";
				break;
			case RSE::PIPELINE_SOURCE_DRAW:
				source_name = "DRAW";
				break;
			case RSE::PIPELINE_SOURCE_SPECIALIZATION:
				source_name = "SPECIALIZATION";
				break;
		}

		print_line("HASH:", p_key_hash, "SOURCE:", source_name);
#endif

		// Queue a background compilation task.
		WorkerThreadPool::TaskID task_id = WorkerThreadPool::get_singleton()->add_template_task(this, &PipelineHashMapRD::_compile_task, p_key, p_high_priority, "PipelineCompilation");
		compilation_tasks.insert(p_key_hash, task_id);
	}

	void wait_for_pipeline(uint32_t p_key_hash) {
		_wait_for_compilation(p_key_hash, nullptr);
	}

	// Retrieve a pipeline. It'll return an empty pipeline if it's not available yet, but it'll be guaranteed to succeed if 'wait for compilation' is true and stall as necessary. Source is just an optional number to aid debugging.
	RID get_pipeline(const Key &p_key, uint32_t p_key_hash, bool p_wait_for_compilation, RSE::PipelineSource p_source) {
		RID pipeline;
		if (_find_pipeline(p_key_hash, pipeline)) {
			return pipeline;
		}

		// Check if there's any new pipelines that need to be added and try again. This method triggers a mutex lock.
		if (_add_new_pipelines_to_map() && _find_pipeline(p_key_hash, pipeline)) {
			return pipeline;
		}

		// Request compilation. The method will ignore the request if it's already being compiled.
		compile_pipeline(p_key, p_key_hash, p_source, p_wait_for_compilation);

		if (!p_wait_for_compilation) {
			return RID();
		}

		_wait_for_compilation(p_key_hash, &p_key);
		_add_new_pipelines_to_map();

		if (!_find_pipeline(p_key_hash, pipeline)) {
			// Pipeline could not be compiled due to an internal error. Store an empty RID so compilation is not attempted again.
			RWLockWrite write_lock(hash_map_lock);
			if (!hash_map.has(p_key_hash)) {
				hash_map.insert(p_key_hash, RID());
			}
		}

		return pipeline;
	}

	// Delete all cached pipelines. Can stall if background compilation is in progress.
	void clear_pipelines() {
		_wait_for_all_pipelines();
		_add_new_pipelines_to_map();

		RWLockWrite write_lock(hash_map_lock);
		for (KeyValue<uint32_t, RID> entry : hash_map) {
			RD::get_singleton()->free_rid(entry.value);
		}

		hash_map.clear();
		compilation_set.clear();
		{
			MutexLock local_lock(local_mutex);
			started_compilations.clear();
			inline_compilations.clear();
			compilation_tasks.clear();
		}
	}

	// Set the external pipeline compilations array to increase the counters on every time a pipeline is compiled.
	void set_compilations(uint32_t *p_compilations, Mutex *p_compilations_mutex) {
		compilations = p_compilations;
		compilations_mutex = p_compilations_mutex;
	}

	void set_creation_object_and_function(CreationClass *p_creation_object, CreationFunction p_creation_function) {
		creation_object = p_creation_object;
		creation_function = p_creation_function;
	}

	PipelineHashMapRD() {}

	~PipelineHashMapRD() {
		clear_pipelines();
	}
};
