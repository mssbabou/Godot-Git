#include "git/git_remote_callbacks.h"

#include <git2/sys/errors.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <atomic>

#include "git/git_util.h"

namespace godot_git {

namespace {

// PID of the child process the network operation is waiting on (`git credential fill`, `git lfs
// fetch`, ...; 0 if none), so Cancel can stop it instead of waiting forever on e.g. a login
// window nobody finishes.
std::atomic<int64_t> running_pid{ 0 };

// Set by cancel_network_operation(); the network callbacks see it and make libgit2 stop.
std::atomic<bool> cancel_requested{ false };

} // namespace

void report_progress(RemoteContext *p_ctx, const String &p_step, const String &p_detail, double p_fraction, bool p_cancellable) {
	if (!p_ctx || !p_ctx->progress.is_valid()) {
		return;
	}
	// Percentages can change thousands of times a second: at most ~10 updates a second. Lines of
	// text (a hook's output) always go through, or the one line a slow hook prints gets lost.
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (p_step == p_ctx->last_step && p_detail.contains("%") && p_fraction < 1.0 && now - p_ctx->last_report_msec < 100) {
		return;
	}
	p_ctx->last_step = p_step;
	p_ctx->last_report_msec = now;
	const String text = p_detail.is_empty() ? p_step : vformat("%s %s", p_step, p_detail);
	p_ctx->progress.call_deferred(text, p_fraction, p_cancellable);
}

namespace {

// Return value for libgit2's progress callbacks: anything < 0 aborts the operation.
int keep_going() {
	return cancel_requested ? GIT_EUSER : 0;
}

String percent(uint64_t p_current, uint64_t p_total) {
	return vformat("%d%%", p_total > 0 ? p_current * 100 / p_total : 0);
}

// Runs `git credential <p_action>` (fill, approve or reject) with a credential description as
// input, the way the git CLI talks to its credential helper (e.g. Git Credential Manager,
// osxkeychain), and returns what it printed. p_interactive sets credential.interactive.
String run_git_credential(const String &p_workdir, const String &p_action, const String &p_input, const String &p_interactive = String()) {
	// Never let git fall back to a terminal prompt; there is no terminal. GUI helpers still work.
	OS::get_singleton()->set_environment("GIT_TERMINAL_PROMPT", "0");

	PackedStringArray args;
	args.push_back("-C");
	args.push_back(p_workdir);
	if (!p_interactive.is_empty()) {
		args.push_back("-c");
		args.push_back("credential.interactive=" + p_interactive);
	}
	args.push_back("credential");
	args.push_back(p_action);
	Dictionary process = OS::get_singleton()->execute_with_pipe("git", args, true);
	Ref<FileAccess> io = process.get("stdio", Variant());
	if (io.is_null()) {
		return String();
	}

	const int64_t pid = process.get("pid", -1);
	track_process(pid);
	io->store_string(p_input + String("\n"));
	io->flush();

	// Read until git exits. Godot's pipes never report eof_reached() (it's hard-coded to false
	// on Windows and Unix); a closed pipe shows up as a read error instead.
	String output;
	while (true) {
		const String line = io->get_line();
		if (!line.is_empty()) {
			output += line + String("\n");
		}
		if (io->get_error() != OK) {
			break;
		}
	}
	track_process(0);
	wait_for_exit_code(pid); // Reaps it.
	return output;
}

// Asks the credential helper for a login and keeps its answer in the context. With prompts
// allowed it may open a sign-in window. Git Credential Manager has to be told that explicitly:
// started from the editor it has no console, decides nobody can see a window, and fails (or
// hangs) instead of showing one.
bool fill_credential(RemoteContext *p_ctx, const String &p_url, const String &p_username) {
	String request = vformat("url=%s\n", p_url);
	if (!p_username.is_empty()) {
		request += vformat("username=%s\n", p_username);
	}
	p_ctx->credential = run_git_credential(p_ctx->workdir, "fill", request, p_ctx->login_prompts_allowed ? "always" : "never");
	if (!p_ctx->credential.contains("password=")) {
		p_ctx->credential = String();
	}
	return !p_ctx->credential.is_empty();
}

// Tells the credential helper whether the login in the context worked ("approve") or was
// rejected ("reject"), like the git CLI does after every request. Approving is what makes a
// helper save a login it just asked for; rejecting makes it forget a stale one and ask next time.
void settle_credential(RemoteContext *p_ctx, const String &p_action) {
	if (!p_ctx->credential.is_empty()) {
		run_git_credential(p_ctx->workdir, p_action, p_ctx->credential);
		p_ctx->credential = String();
	}
}

String credential_field(const String &p_credential, const String &p_key) {
	for (const String &line : p_credential.split("\n", false)) {
		if (line.begins_with(vformat("%s=", p_key))) {
			return line.substr(p_key.length() + 1);
		}
	}
	return String();
}

int credentials_cb(git_credential **r_out, const char *p_url, const char *p_username_from_url, unsigned int p_allowed_types, void *p_payload) {
	RemoteContext *ctx = static_cast<RemoteContext *>(p_payload);

	// libgit2 asks again when the server rejected the login we gave it. Make the helper forget
	// it, then (if a sign-in window is allowed) ask once more, which signs in fresh. That's how
	// an expired or revoked login recovers without a terminal.
	const int attempt = ctx->credential_attempts++;
	if (attempt > 0) {
		settle_credential(ctx, "reject");
		if (!ctx->login_prompts_allowed) {
			git_error_set_str(GIT_ERROR_NET, "The saved login was rejected by the server, so it was removed. Press Fetch to sign in again.");
			return GIT_EAUTH;
		}
		if (attempt > 1) {
			git_error_set_str(GIT_ERROR_NET, "The server rejected the login. Check that this account has access to the repository.");
			return GIT_EAUTH;
		}
	}

	if (p_allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) {
		// The helper may open a sign-in window or a browser tab; say so, or it looks like a hang.
		report_progress(ctx, "Signing in...", "(check for a sign-in window or browser tab)", -1);
		const String hint = p_username_from_url ? String::utf8(p_username_from_url) : String();
		const bool found = fill_credential(ctx, String::utf8(p_url), hint);
		if (cancel_requested) {
			return GIT_EUSER;
		}
		if (found) {
			report_progress(ctx, "Connecting...", String(), -1);
			return git_credential_userpass_plaintext_new(r_out, credential_field(ctx->credential, "username").utf8().get_data(), credential_field(ctx->credential, "password").utf8().get_data());
		}
	}
	if (p_allowed_types & GIT_CREDENTIAL_DEFAULT) {
		return git_credential_default_new(r_out);
	}

	git_error_set_str(GIT_ERROR_NET, ctx->login_prompts_allowed ? "Signing in didn't finish. If no sign-in window appeared, git has no credential helper that can ask for a login; sign in once with git in a terminal (e.g. `git fetch`), then retry." : "No saved login for this remote yet. Press Fetch to sign in.");
	return GIT_EAUTH;
}

int push_update_reference_cb(const char *p_refname, const char *p_status, void *p_payload) {
	if (p_status) {
		static_cast<RemoteContext *>(p_payload)->push_rejection = String::utf8(p_status);
	}
	return 0;
}

// The server's own messages ("Counting objects: 45% (9/20)"). Only shown until real data arrives.
int sideband_cb(const char *p_str, int p_len, void *p_payload) {
	RemoteContext *ctx = static_cast<RemoteContext *>(p_payload);
	if (!ctx->received_data && p_len > 0) {
		// Updates end in \r or \n, but one update can arrive split over several packets, and one
		// packet can hold several updates. Show the last complete one; keep the rest for later.
		ctx->sideband_buffer += String::utf8(p_str, p_len).replace("\r", "\n");
		const int end = ctx->sideband_buffer.rfind("\n");
		if (end < 0) {
			return keep_going();
		}
		const PackedStringArray lines = ctx->sideband_buffer.substr(0, end).split("\n", false);
		ctx->sideband_buffer = ctx->sideband_buffer.substr(end + 1);
		const String message = lines.is_empty() ? String() : lines[lines.size() - 1].strip_edges();
		const int colon = message.find(":");
		if (colon > 0) {
			report_progress(ctx, message.substr(0, colon + 1), message.substr(colon + 1).strip_edges(), -1);
		} else if (!message.is_empty()) {
			report_progress(ctx, message, String(), -1);
		}
	}
	return keep_going();
}

int transfer_progress_cb(const git_indexer_progress *p_stats, void *p_payload) {
	RemoteContext *ctx = static_cast<RemoteContext *>(p_payload);
	ctx->received_data = true;
	if (p_stats->total_objects == 0) {
		return keep_going();
	}
	if (p_stats->received_objects < p_stats->total_objects) {
		report_progress(ctx, "Receiving objects", vformat("%s (%s)", percent(p_stats->received_objects, p_stats->total_objects), String::humanize_size(p_stats->received_bytes)),
				(double)p_stats->received_objects / p_stats->total_objects);
	} else if (p_stats->total_deltas > 0) {
		report_progress(ctx, "Resolving changes", percent(p_stats->indexed_deltas, p_stats->total_deltas), (double)p_stats->indexed_deltas / p_stats->total_deltas);
	}
	return keep_going();
}

int pack_progress_cb(int p_stage, uint32_t p_current, uint32_t p_total, void *p_payload) {
	RemoteContext *ctx = static_cast<RemoteContext *>(p_payload);
	if (p_stage == GIT_PACKBUILDER_ADDING_OBJECTS) {
		report_progress(ctx, "Counting objects:", itos(p_current), -1);
	} else if (p_total > 0) {
		report_progress(ctx, "Compressing objects", percent(p_current, p_total), (double)p_current / p_total);
	}
	return keep_going();
}

int push_transfer_progress_cb(unsigned int p_current, unsigned int p_total, size_t p_bytes, void *p_payload) {
	RemoteContext *ctx = static_cast<RemoteContext *>(p_payload);
	ctx->received_data = true;
	if (p_total > 0) {
		report_progress(ctx, "Sending objects", vformat("%s (%s)", percent(p_current, p_total), String::humanize_size(p_bytes)), (double)p_current / p_total);
	}
	return keep_going();
}

} // namespace

void checkout_progress_cb(const char *p_path, size_t p_completed, size_t p_total, void *p_payload) {
	if (p_total > 0) {
		report_progress(static_cast<RemoteContext *>(p_payload), "Updating files", percent(p_completed, p_total), (double)p_completed / p_total, false);
	}
}

void set_remote_callbacks(git_remote_callbacks &r_callbacks, RemoteContext &p_ctx) {
	r_callbacks.credentials = credentials_cb;
	r_callbacks.push_update_reference = push_update_reference_cb;
	r_callbacks.sideband_progress = sideband_cb;
	r_callbacks.transfer_progress = transfer_progress_cb;
	r_callbacks.pack_progress = pack_progress_cb;
	r_callbacks.push_transfer_progress = push_transfer_progress_cb;
	r_callbacks.payload = &p_ctx;
}

Error finish_network_operation(RemoteContext &p_ctx, int p_err) {
	// The login got us through if the operation worked or data arrived.
	if (p_err >= 0 || p_ctx.received_data) {
		settle_credential(&p_ctx, "approve");
	}
	if (p_err < 0 && cancel_requested) {
		git_error_set_str(GIT_ERROR_NET, "Canceled. Nothing was changed.");
		return ERR_SKIP;
	}
	return p_err < 0 ? FAILED : OK;
}

void begin_network_operation() {
	cancel_requested = false;
}

void cancel_network_operation() {
	cancel_requested = true;
	const int64_t pid = running_pid.load();
	if (pid <= 0) {
		return;
	}
	if (OS::get_singleton()->get_name() == "Windows") {
		// git starts helpers (Git Credential Manager, git-lfs) as its own children, and Windows
		// doesn't end those with their parent; a sign-in window would stay open.
		PackedStringArray args;
		args.push_back("/T");
		args.push_back("/F");
		args.push_back("/PID");
		args.push_back(itos(pid));
		OS::get_singleton()->execute("taskkill", args);
	} else {
		OS::get_singleton()->kill(pid);
	}
}

void track_process(int64_t p_pid) {
	running_pid = p_pid;
}

bool is_cancel_requested() {
	return cancel_requested;
}

} // namespace godot_git
