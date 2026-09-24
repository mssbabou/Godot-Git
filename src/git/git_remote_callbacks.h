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
	// The login the credential helper gave us, exactly as it described it ("username=...\n"
	// lines), so it can be handed back to confirm or reject it. Empty if none was used.
	String credential;
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

// The child process the current network operation waits on (0: none), so Cancel can end it,
// along with any processes it started.
void track_process(int64_t p_pid);

bool is_cancel_requested();

// Call when a libgit2 network call returns: tells the credential helper whether its login
// worked (so it saves a new one), and turns the result into an Error. An operation stopped by
// cancel_network_operation() is ERR_SKIP with "Canceled. Nothing was changed." (libgit2 only
// says GIT_EUSER).
Error finish_network_operation(RemoteContext &p_ctx, int p_err);

} // namespace godot_git
