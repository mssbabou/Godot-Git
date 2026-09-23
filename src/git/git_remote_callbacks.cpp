#include "git/git_remote_callbacks.h"

#include <git2/sys/errors.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <atomic>

namespace godot_git {

namespace {

// PID of a running `git credential fill` (0 if none), so the editor can stop it when closing
// instead of waiting forever on e.g. a login window nobody finishes.
std::atomic<int64_t> credential_pid{ 0 };

// Set by cancel_network_operation(); the network callbacks see it and make libgit2 stop.
std::atomic<bool> cancel_requested{ false };

} // namespace

void report_progress(RemoteContext *p_ctx, const String &p_step, const String &p_detail, double p_fraction, bool p_cancellable) {
	if (!p_ctx || !p_ctx->progress.is_valid()) {
		return;
	}
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (p_step == p_ctx->last_step && p_fraction < 1.0 && now - p_ctx->last_report_msec < 100) {
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

// Asks git's configured credential helper (e.g. Git Credential Manager, osxkeychain) for a
// login, so the panel uses the same saved credentials as the git command line.
bool credential_fill(const String &p_workdir, const String &p_url, const String &p_username, bool p_prompt, String &r_username, String &r_password) {
	// Never let git fall back to a terminal prompt; there is no terminal. GUI helpers still work.
	OS::get_singleton()->set_environment("GIT_TERMINAL_PROMPT", "0");

	PackedStringArray args;
	args.push_back("-C");
	args.push_back(p_workdir);
	if (!p_prompt) {
		// Git Credential Manager's switch for "saved logins only, never open a sign-in window".
		// Other helpers (osxkeychain, libsecret, store) never prompt anyway.
		args.push_back("-c");
		args.push_back("credential.interactive=never");
	}
	args.push_back("credential");
	args.push_back("fill");
	Dictionary process = OS::get_singleton()->execute_with_pipe("git", args, true);
	Ref<FileAccess> io = process.get("stdio", Variant());
	if (io.is_null()) {
		return false;
	}

	const int64_t pid = process.get("pid", -1);
	credential_pid = pid;

	String request = vformat("url=%s\n", p_url);
	if (!p_username.is_empty()) {
		request += vformat("username=%s\n", p_username);
	}
	io->store_string(request + String("\n"));
	io->flush();

	// Read until git exits. Godot's pipes never report eof_reached() (it's hard-coded to false
	// on Windows and Unix); a closed pipe shows up as a read error instead.
	while (true) {
		const String line = io->get_line();
		if (line.begins_with("username=")) {
			r_username = line.substr(9);
		} else if (line.begins_with("password=")) {
			r_password = line.substr(9);
		}
		if (io->get_error() != OK) {
			break;
		}
	}
	credential_pid = 0;
	OS::get_singleton()->get_process_exit_code(pid);
	return !r_password.is_empty();
}

int credentials_cb(git_credential **r_out, const char *p_url, const char *p_username_from_url, unsigned int p_allowed_types, void *p_payload) {
	RemoteContext *ctx = static_cast<RemoteContext *>(p_payload);

	// libgit2 asks again when the server rejects what we gave it; don't loop forever.
	if (ctx->credential_attempts++ > 0) {
		git_error_set_str(GIT_ERROR_NET, "The server rejected the saved login. Sign in again with git in a terminal, then retry.");
		return GIT_EAUTH;
	}

	if (p_allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) {
		// The credential helper may open a sign-in window; say so, or it looks like a hang.
		report_progress(ctx, "Waiting for login...", String(), -1);
		String username, password;
		const String hint = p_username_from_url ? String::utf8(p_username_from_url) : String();
		const bool found = credential_fill(ctx->workdir, String::utf8(p_url), hint, ctx->login_prompts_allowed, username, password);
		if (cancel_requested) {
			return GIT_EUSER;
		}
		if (found) {
			report_progress(ctx, "Connecting...", String(), -1);
			return git_credential_userpass_plaintext_new(r_out, username.utf8().get_data(), password.utf8().get_data());
		}
	}
	if (p_allowed_types & GIT_CREDENTIAL_DEFAULT) {
		return git_credential_default_new(r_out);
	}

	git_error_set_str(GIT_ERROR_NET, "No saved login for this remote. Sign in once with git in a terminal (e.g. `git fetch`), then retry.");
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

Error remote_error(int p_err) {
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
	const int64_t pid = credential_pid.load();
	if (pid > 0) {
		OS::get_singleton()->kill(pid);
	}
}

} // namespace godot_git
