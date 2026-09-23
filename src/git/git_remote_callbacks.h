#pragma once

// libgit2 remote callbacks for fetch/pull/push: logins through git's credential helper, progress
// reporting, and cancellation. Internal to the GitRepository sources.

#include <git2.h>

#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// State shared by the callbacks of one network operation (their `payload`).
struct RemoteContext {
	String workdir;
	int credential_attempts = 0;
	String push_rejection;
	Callable progress;
	String last_step;
	uint64_t last_report_msec = 0;
	bool received_data = false; // Real transfer progress beats the server's own messages.
	bool login_prompts_allowed = true;
	String sideband_buffer;
};

void set_remote_callbacks(git_remote_callbacks &r_callbacks, RemoteContext &p_ctx);

// Sends progress to the RemoteContext's callable, deferred to the main thread. Throttled so a
// fast transfer doesn't flood the message queue; a new step or a finished one always goes out.
void report_progress(RemoteContext *p_ctx, const String &p_step, const String &p_detail, double p_fraction, bool p_cancellable = true);

// For git_checkout_options::progress_cb, with a RemoteContext as payload. Checkout after a pull
// is local and quick, and stopping halfway would leave a mess, so it's reported as not cancellable.
void checkout_progress_cb(const char *p_path, size_t p_completed, size_t p_total, void *p_payload);

// Call at the start of every network operation: clears an old cancel request.
void begin_network_operation();

// Asks the running network operation (on any thread) to stop as soon as it can, including one
// waiting on a login window.
void cancel_network_operation();

// The Error for a libgit2 network result. An operation stopped by cancel_network_operation()
// is ERR_SKIP with "Canceled. Nothing was changed." (libgit2 only says GIT_EUSER).
Error remote_error(int p_err);

} // namespace godot_git
