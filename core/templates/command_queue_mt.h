/**************************************************************************/
/*  command_queue_mt.h                                                    */
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

#include <cstdlib>
#include "core/templates/local_vector.h"
#include "core/templates/simple_type.h"
#include "core/templates/tuple.h"
#include "core/typedefs.h"

class CommandQueueMT {
	static const size_t MAX_COMMAND_SIZE = 1024;

	struct CommandBase {
		bool sync = false;
		virtual void call() = 0;
		virtual ~CommandBase() = default;

		CommandBase(bool p_sync) :
				sync(p_sync) {}
	};

	template <typename T, typename M, bool NeedsSync, typename... Args>
	struct Command : public CommandBase {
		T *instance;
		M method;
		Tuple<GetSimpleTypeT<Args>...> args;

		template <typename... FwdArgs>
		_FORCE_INLINE_ Command(T *p_instance, M p_method, FwdArgs &&...p_args) :
				CommandBase(NeedsSync), instance(p_instance), method(p_method), args(std::forward<FwdArgs>(p_args)...) {}

		void call() override {
			call_impl(BuildIndexSequence<sizeof...(Args)>{});
		}

	private:
		template <size_t... I>
		_FORCE_INLINE_ void call_impl(IndexSequence<I...>) {
			// Move out of the Tuple, this will be destroyed as soon as the call is complete.
			(instance->*method)(std::move(get<I>())...);
		}

		// This method exists so we can call it in the parameter pack expansion in call_impl.
		template <size_t I>
		_FORCE_INLINE_ auto &get() { return ::tuple_get<I>(args); }
	};

	// Separate class from Command so we can save the space of the ret pointer for commands that don't return.
	template <typename T, typename M, typename R, typename... Args>
	struct CommandRet : public CommandBase {
		T *instance;
		M method;
		R *ret;
		Tuple<GetSimpleTypeT<Args>...> args;

		_FORCE_INLINE_ CommandRet(T *p_instance, M p_method, R *p_ret, GetSimpleTypeT<Args>... p_args) :
				CommandBase(true), instance(p_instance), method(p_method), ret(p_ret), args{ p_args... } {}

		void call() override {
			*ret = call_impl(BuildIndexSequence<sizeof...(Args)>{});
		}

	private:
		template <size_t... I>
		_FORCE_INLINE_ R call_impl(IndexSequence<I...>) {
			// Move out of the Tuple, this will be destroyed as soon as the call is complete.
			return (instance->*method)(std::move(get<I>())...);
		}

		// This method exists so we can call it in the parameter pack expansion in call_impl.
		template <size_t I>
		_FORCE_INLINE_ auto &get() { return ::tuple_get<I>(args); }
	};

	/***** BASE *******/

	static const uint32_t DEFAULT_COMMAND_MEM_SIZE_KB = 64;

	inline static thread_local bool flushing = false;

	BinaryMutex mutex;
	LocalVector<uint8_t> command_mem;
	ConditionVariable sync_cond_var;
	uint32_t sync_head = 0;
	uint32_t sync_tail = 0;
	uint32_t sync_awaiters = 0;
	WorkerThreadPool::TaskID pump_task_id = WorkerThreadPool::INVALID_TASK_ID;
	uint64_t flush_read_ptr = 0;
	std::atomic<bool> pending{ false };

	template <typename T, typename... Args>
	_FORCE_INLINE_ void create_command(Args &&...p_args) {
		// alloc size is size+T+safeguard
		constexpr uint64_t alloc_size = ((sizeof(T) + 8U - 1U) & ~(8U - 1U));
		static_assert(alloc_size < UINT32_MAX, "Type too large to fit in the command queue.");

		uint64_t size = command_mem.size();
		command_mem.resize(size + alloc_size + sizeof(uint64_t));
		*(uint64_t *)&command_mem[size] = alloc_size;
		void *cmd = &command_mem[size + sizeof(uint64_t)];
		memnew_placement(cmd, T(std::forward<Args>(p_args)...));
		pending.store(true);
	}

	// Negative control: setting GODOT_QUEUE_NOTIFY_ALWAYS=1 restores the previous behaviour of
	// signalling the pump for every single command. Both paths live in the same binary so they
	// can be compared back to back, on the same scene, without rebuilding.
	static inline const bool notify_every_command = []() {
		const char *v = std::getenv("GODOT_QUEUE_NOTIFY_ALWAYS");
		return v != nullptr && v[0] == '1';
	}();

	template <typename T, bool NeedsSync, typename... Args>
	_FORCE_INLINE_ void _push_internal(Args &&...args) {
		MutexLock mlock(mutex);

		// Only wake the pump when the queue goes from empty to non-empty.
		//
		// notify_yield_over() locks the WorkerThreadPool's global task mutex, looks the task up
		// in a hash map and signals a condition variable -- i.e. a kernel wake-up. Doing that for
		// *every* command is what an ETW profile of this project showed: on a 400-entity scene,
		// SwapContext accounted for 39% of the process' CPU samples and KeAlertThreadByThreadIdEx
		// for 26%, with almost nothing left for actual work.
		//
		// While `pending` is true the pump has already been signalled and has not finished
		// draining: _flush() re-reads command_mem.size() on every iteration under this same
		// mutex, so commands appended in the meantime are picked up without a new signal.
		// `pending` is only cleared by _flush(), also under this mutex, so the test cannot race.
		const bool was_idle = !pending.load(std::memory_order_relaxed);

		create_command<T>(std::forward<Args>(args)...);

		if (pump_task_id != WorkerThreadPool::INVALID_TASK_ID && (was_idle || notify_every_command)) {
			WorkerThreadPool::get_singleton()->notify_yield_over(pump_task_id);
		}

		if constexpr (NeedsSync) {
			sync_tail++;
			_wait_for_sync(mlock);
		}
	}

	_FORCE_INLINE_ void _prevent_sync_wraparound() {
		bool safe_to_reset = !sync_awaiters;
		bool already_sync_to_latest = sync_head == sync_tail;
		if (safe_to_reset && already_sync_to_latest) {
			sync_head = 0;
			sync_tail = 0;
		}
	}

	void _flush() {
		// Safeguard against trying to re-lock the binary mutex.
		if (flushing) {
			return;
		}

		flushing = true;

		MutexLock lock(mutex);

		if (unlikely(flush_read_ptr)) {
			// Another thread is flushing.
			lock.temp_unlock(); // Not really temp.
			sync();
			flushing = false;
			return;
		}

		alignas(uint64_t) char cmd_local_mem[MAX_COMMAND_SIZE];

		while (flush_read_ptr < command_mem.size()) {
			uint64_t size = *(uint64_t *)&command_mem[flush_read_ptr];
			flush_read_ptr += sizeof(uint64_t);

			// Protect against race condition between this thread
			// during the call to the command and other threads potentially
			// invalidating the pointer due to reallocs by relocating the object.
			CommandBase *cmd_original = reinterpret_cast<CommandBase *>(&command_mem[flush_read_ptr]);
			CommandBase *cmd_local = reinterpret_cast<CommandBase *>(cmd_local_mem);
			memcpy(cmd_local_mem, (char *)cmd_original, size);

			lock.temp_unlock();
			cmd_local->call();
			lock.temp_relock();

			if (unlikely(cmd_local->sync)) {
				sync_head++;
				lock.temp_unlock(); // Give an opportunity to awaiters right away.
				sync_cond_var.notify_all();
				lock.temp_relock();
			}

			cmd_local->~CommandBase();

			flush_read_ptr += size;
		}

		command_mem.clear();
		pending.store(false);
		flush_read_ptr = 0;

		_prevent_sync_wraparound();

		flushing = false;
	}

	_FORCE_INLINE_ void _wait_for_sync(MutexLock<BinaryMutex> &p_lock) {
		sync_awaiters++;
		uint32_t sync_head_goal = sync_tail;
		do {
			sync_cond_var.wait(p_lock);
		} while (sync_head < sync_head_goal);
		sync_awaiters--;
		_prevent_sync_wraparound();
	}

	void _no_op() {}

public:
	template <typename T, typename M, typename... Args>
	void push(T *p_instance, M p_method, Args &&...p_args) {
		// Standard command, no sync.
		using CommandType = Command<T, M, false, Args...>;
		static_assert(sizeof(CommandType) <= MAX_COMMAND_SIZE);
		_push_internal<CommandType, false>(p_instance, p_method, std::forward<Args>(p_args)...);
	}

	template <typename T, typename M, typename... Args>
	void push_and_sync(T *p_instance, M p_method, Args... p_args) {
		// Standard command, sync.
		using CommandType = Command<T, M, true, Args...>;
		static_assert(sizeof(CommandType) <= MAX_COMMAND_SIZE);
		_push_internal<CommandType, true>(p_instance, p_method, std::forward<Args>(p_args)...);
	}

	template <typename T, typename M, typename R, typename... Args>
	void push_and_ret(T *p_instance, M p_method, R *r_ret, Args... p_args) {
		// Command with return value, sync.
		using CommandType = CommandRet<T, M, R, Args...>;
		static_assert(sizeof(CommandType) <= MAX_COMMAND_SIZE);
		_push_internal<CommandType, true>(p_instance, p_method, r_ret, std::forward<Args>(p_args)...);
	}

	_FORCE_INLINE_ void flush_if_pending() {
		if (unlikely(pending.load())) {
			_flush();
		}
	}

	void flush_all() {
		_flush();
	}

	void sync() {
		push_and_sync(this, &CommandQueueMT::_no_op);
	}

	void wait_and_flush() {
		ERR_FAIL_COND(pump_task_id == WorkerThreadPool::INVALID_TASK_ID);
		WorkerThreadPool::get_singleton()->wait_for_task_completion(pump_task_id);
		_flush();
	}

	void set_pump_task_id(WorkerThreadPool::TaskID p_task_id) {
		MutexLock lock(mutex);
		pump_task_id = p_task_id;
	}

	CommandQueueMT() {
		command_mem.reserve(DEFAULT_COMMAND_MEM_SIZE_KB * 1024);
	}
};
