/**************************************************************************/
/*  audio_thread_priority_linuxbsd.cpp                                    */
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

#include "audio_thread_priority_linuxbsd.h"

// Nice levels are per thread only on Linux (setpriority(2), "BUGS"); RealtimeKit is Linux-only too.
// Elsewhere (FreeBSD...) the audio thread keeps its priority, as before.
#ifdef __linux__

#include "core/string/print_string.h"
#include "core/variant/variant.h"

#ifdef DBUS_ENABLED
#ifdef SOWRAP_ENABLED
#include "dbus-so_wrap.h"
#else
#include <dbus/dbus.h>
#endif
#endif

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

// SDL gives its playback device threads SDL_THREAD_PRIORITY_TIME_CRITICAL (SDL_audio.c), which is nice -20
// on Linux under the default SCHED_OTHER policy (SDL_threadprio.c, SDL_SetLinuxThreadPriorityAndPolicy);
// RealtimeKit clamps the request to its MinNiceLevel (-15 by default).
// Not a real-time policy: RealtimeKit only grants SCHED_RR/SCHED_FIFO to processes whose RLIMIT_RTTIME is
// bounded, and the kernel then kills the process when the thread runs that long without blocking. SDL makes
// that opt-in (SDL_HINT_THREAD_FORCE_REALTIME_TIME_CRITICAL) because the thread runs application code; so does
// Godot's (custom AudioStreamPlayback and AudioEffect code), and its loops poll the device without blocking.
static constexpr int AUDIO_THREAD_NICE_LEVEL = -20;

#ifdef DBUS_ENABLED

static bool dbus_initialized = false;
static bool dbus_available = false;

// RealtimeKit asks polkit before answering; a missing service answers at once. Bounded so that a stuck bus
// can only delay the first mix, not hang the audio thread for libdbus' default 25 seconds.
static constexpr int DBUS_REQUEST_TIMEOUT_MSEC = 2000;

struct RealtimeService {
	DBusBusType bus;
	const char *name;
	const char *path;
	const char *interface;
	const char *label;
};

// The xdg-desktop-portal Realtime interface first (it also works inside Flatpak's PID namespace and forwards
// to RealtimeKit), then RealtimeKit itself, as SDL (SDL_threadprio.c) and PipeWire (module-rt) do.
static const RealtimeService realtime_services[] = {
	{ DBUS_BUS_SESSION, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Realtime", "xdg-desktop-portal" },
	{ DBUS_BUS_SYSTEM, "org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", "RealtimeKit" },
};

static bool _get_int32_property(DBusConnection *p_connection, const RealtimeService &p_service, const char *p_property, int32_t &r_value, DBusError *r_error) {
	DBusMessage *message = dbus_message_new_method_call(p_service.name, p_service.path, "org.freedesktop.DBus.Properties", "Get");
	if (!message) {
		return false;
	}
	const char *interface = p_service.interface;
	dbus_message_append_args(message, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &p_property, DBUS_TYPE_INVALID);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(p_connection, message, DBUS_REQUEST_TIMEOUT_MSEC, r_error);
	dbus_message_unref(message);
	if (!reply) {
		return false;
	}

	bool found = false;
	DBusMessageIter iter;
	if (dbus_message_iter_init(reply, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
		DBusMessageIter variant;
		dbus_message_iter_recurse(&iter, &variant);
		if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_INT32) {
			dbus_int32_t value = 0;
			dbus_message_iter_get_basic(&variant, &value);
			r_value = value;
			found = true;
		}
	}
	dbus_message_unref(reply);
	return found;
}

// MakeThreadHighPriorityWithPID(pid, tid, nice), clamped to the service's MinNiceLevel as SDL does
// (rtkit_setpriority_nice); RealtimeKit refuses a level below it.
static bool _make_thread_high_priority(const RealtimeService &p_service, pid_t p_tid, int p_current_nice, int &r_nice, String &r_error) {
	DBusError error;
	dbus_error_init(&error);

	// A private connection: this thread owns it, and closes it when done.
	DBusConnection *connection = dbus_bus_get_private(p_service.bus, &error);
	if (!connection) {
		r_error = String(error.name ? error.name : "no connection");
		dbus_error_free(&error);
		return false;
	}
	// libdbus calls _exit() when a bus connection drops unless told not to.
	dbus_connection_set_exit_on_disconnect(connection, FALSE);

	bool done = false;
	int32_t min_nice = 0;
	if (_get_int32_property(connection, p_service, "MinNiceLevel", min_nice, &error)) {
		const int nice = MAX(AUDIO_THREAD_NICE_LEVEL, (int)min_nice);
		if (p_current_nice <= nice) {
			// Already as high as this service would make it (e.g. a process started with a negative nice).
			r_nice = p_current_nice;
			done = true;
		} else {
			DBusMessage *message = dbus_message_new_method_call(p_service.name, p_service.path, p_service.interface, "MakeThreadHighPriorityWithPID");
			if (message) {
				dbus_uint64_t pid = (dbus_uint64_t)getpid();
				dbus_uint64_t tid = (dbus_uint64_t)p_tid;
				dbus_int32_t level = nice;
				dbus_message_append_args(message, DBUS_TYPE_UINT64, &pid, DBUS_TYPE_UINT64, &tid, DBUS_TYPE_INT32, &level, DBUS_TYPE_INVALID);
				DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, message, DBUS_REQUEST_TIMEOUT_MSEC, &error);
				dbus_message_unref(message);
				if (reply) {
					dbus_message_unref(reply);
					r_nice = nice;
					done = true;
				}
			}
		}
	} else if (!dbus_error_is_set(&error)) {
		r_error = "no MinNiceLevel";
	}

	if (!done && dbus_error_is_set(&error)) {
		r_error = String(error.name);
	}
	dbus_error_free(&error);
	dbus_connection_close(connection);
	dbus_connection_unref(connection);
	return done;
}

#endif // DBUS_ENABLED

void AudioThreadPriorityLinuxBSD::initialize() {
#ifdef DBUS_ENABLED
	if (dbus_initialized) {
		return;
	}
	dbus_initialized = true;

#ifdef SOWRAP_ENABLED
	// The X11 and Wayland display servers load libdbus for the portal; loading it again would rewrite the
	// function pointers while the portal's monitor thread uses them. Headless runs have not loaded it.
	if (!dbus_get_version_dylibloader_wrapper_dbus) {
#ifdef DEBUG_ENABLED
		int dylibloader_verbose = 1;
#else
		int dylibloader_verbose = 0;
#endif
		if (initialize_dbus(dylibloader_verbose) != 0) {
			return;
		}
	}
#endif

	// Same floor as the display servers' D-Bus features.
	int version_major = 0;
	int version_minor = 0;
	int version_rev = 0;
	dbus_get_version(&version_major, &version_minor, &version_rev);
	dbus_available = (version_major == 1 && version_minor >= 10) || (version_major > 1);
#endif // DBUS_ENABLED
}

void AudioThreadPriorityLinuxBSD::raise_current_thread() {
	const pid_t tid = (pid_t)syscall(SYS_gettid);

	errno = 0;
	const int current_nice = getpriority(PRIO_PROCESS, (id_t)tid);
	if (errno != 0) {
		return;
	}
	if (current_nice <= AUDIO_THREAD_NICE_LEVEL) {
		return;
	}

	// On Linux, PRIO_PROCESS with a thread ID changes that thread only. It succeeds when RLIMIT_NICE allows
	// the level (e.g. "@audio - nice -20" in limits.conf) or with CAP_SYS_NICE; desktops usually grant neither.
	if (setpriority(PRIO_PROCESS, (id_t)tid, AUDIO_THREAD_NICE_LEVEL) == 0) {
		print_verbose(vformat("Audio thread %d: nice level %d.", tid, AUDIO_THREAD_NICE_LEVEL));
		return;
	}
	String errors = vformat("setpriority: %s", strerror(errno));

#ifdef DBUS_ENABLED
	if (dbus_available) {
		for (const RealtimeService &service : realtime_services) {
			int nice = 0;
			String error;
			if (_make_thread_high_priority(service, tid, current_nice, nice, error)) {
				print_verbose(vformat("Audio thread %d: nice level %d (%s).", tid, nice, service.label));
				return;
			}
			errors += vformat(", %s: %s", service.label, error);
		}
	}
#endif

	print_verbose(vformat("Audio thread %d: priority unchanged, nice level %d (%s).", tid, current_nice, errors));
}

#else // !__linux__

void AudioThreadPriorityLinuxBSD::initialize() {}

void AudioThreadPriorityLinuxBSD::raise_current_thread() {}

#endif // __linux__
