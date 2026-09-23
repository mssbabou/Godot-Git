#include "git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <atomic>

namespace {

String index_status_name(unsigned int p_status) {
	if (p_status & GIT_STATUS_INDEX_NEW) {
		return "new";
	}
	if (p_status & GIT_STATUS_INDEX_MODIFIED) {
		return "modified";
	}
	if (p_status & GIT_STATUS_INDEX_DELETED) {
		return "deleted";
	}
	if (p_status & GIT_STATUS_INDEX_RENAMED) {
		return "renamed";
	}
	if (p_status & GIT_STATUS_INDEX_TYPECHANGE) {
		return "typechange";
	}
	return "";
}

String worktree_status_name(unsigned int p_status) {
	if (p_status & GIT_STATUS_WT_NEW) {
		return "untracked";
	}
	if (p_status & GIT_STATUS_WT_MODIFIED) {
		return "modified";
	}
	if (p_status & GIT_STATUS_WT_DELETED) {
		return "deleted";
	}
	if (p_status & GIT_STATUS_WT_RENAMED) {
		return "renamed";
	}
	if (p_status & GIT_STATUS_WT_TYPECHANGE) {
		return "typechange";
	}
	if (p_status & GIT_STATUS_CONFLICTED) {
		return "conflicted";
	}
	return "";
}

Error to_error(int p_git_error) {
	if (p_git_error >= 0) {
		return OK;
	}
	return p_git_error == GIT_ENOTFOUND ? ERR_FILE_NOT_FOUND : FAILED;
}

// Sets the message GitRepository::get_last_error() returns, for failures libgit2 doesn't describe well.
Error fail(const String &p_message) {
	git_error_set_str(GIT_ERROR_INVALID, p_message.utf8().get_data());
	return FAILED;
}

String buf_to_string(git_buf &p_buf) {
	const String result = p_buf.ptr ? String::utf8(p_buf.ptr) : String();
	git_buf_dispose(&p_buf);
	return result;
}

// Owns the UTF-8 buffer a single-entry git_strarray points into.
struct SinglePathspec {
	CharString utf8;
	char *entry = nullptr;
	git_strarray array = { nullptr, 0 };

	explicit SinglePathspec(const String &p_path) :
			utf8(p_path.utf8()) {
		entry = const_cast<char *>(utf8.get_data());
		array.strings = &entry;
		array.count = 1;
	}
};

// Looks up the branch HEAD points at. Fails with a readable message on a detached HEAD
// or a branch with no commits yet.
int head_branch(git_reference **r_ref, git_repository *p_repo) {
	int err = git_repository_head(r_ref, p_repo);
	if (err == GIT_EUNBORNBRANCH) {
		fail("This branch has no commits yet.");
		return err;
	}
	if (err >= 0 && !git_reference_is_branch(*r_ref)) {
		git_reference_free(*r_ref);
		*r_ref = nullptr;
		fail("You're not on a branch (detached HEAD).");
		return GIT_ERROR;
	}
	return err;
}

// Paths that differ between commit p_from and commit p_to (HEAD when p_to is null).
HashSet<String> changed_paths(git_repository *p_repo, const git_oid *p_from, const git_oid *p_to) {
	HashSet<String> paths;
	git_commit *from = nullptr;
	git_commit *to = nullptr;
	git_tree *from_tree = nullptr;
	git_tree *to_tree = nullptr;
	git_diff *diff = nullptr;

	bool ok = git_commit_lookup(&from, p_repo, p_from) == 0 && git_commit_tree(&from_tree, from) == 0;
	if (ok && p_to) {
		ok = git_commit_lookup(&to, p_repo, p_to) == 0;
	} else if (ok) {
		git_object *head = nullptr;
		ok = git_revparse_single(&head, p_repo, "HEAD^{commit}") == 0;
		to = (git_commit *)head;
	}
	ok = ok && git_commit_tree(&to_tree, to) == 0 && git_diff_tree_to_tree(&diff, p_repo, from_tree, to_tree, nullptr) == 0;
	if (ok) {
		for (size_t i = 0; i < git_diff_num_deltas(diff); i++) {
			const git_diff_delta *delta = git_diff_get_delta(diff, i);
			paths.insert(String::utf8(delta->old_file.path));
			paths.insert(String::utf8(delta->new_file.path));
		}
	}

	git_diff_free(diff);
	git_tree_free(to_tree);
	git_tree_free(from_tree);
	git_commit_free(to);
	git_commit_free(from);
	return paths;
}

// Paths a stash entry changed, relative to the commit it was made on.
PackedStringArray stashed_paths(git_repository *p_repo, const git_oid *p_stash) {
	PackedStringArray result;
	git_commit *stash = nullptr;
	if (git_commit_lookup(&stash, p_repo, p_stash) == 0 && git_commit_parentcount(stash) > 0) {
		for (const String &path : changed_paths(p_repo, git_commit_parent_id(stash, 0), p_stash)) {
			result.push_back(path);
		}
	}
	git_commit_free(stash);
	return result;
}

// --- Remote callbacks -------------------------------------------------------

// PID of a running `git credential fill` (0 if none), so the editor can stop it when closing
// instead of waiting forever on e.g. a login window nobody finishes.
std::atomic<int64_t> credential_pid{ 0 };

struct RemoteContext {
	String workdir;
	int credential_attempts = 0;
	String push_rejection;
};

// Asks git's configured credential helper (e.g. Git Credential Manager, osxkeychain) for a
// login, so the panel uses the same saved credentials as the git command line.
bool credential_fill(const String &p_workdir, const String &p_url, const String &p_username, String &r_username, String &r_password) {
	// Never let git fall back to a terminal prompt; there is no terminal. GUI helpers still work.
	OS::get_singleton()->set_environment("GIT_TERMINAL_PROMPT", "0");

	PackedStringArray args;
	args.push_back("-C");
	args.push_back(p_workdir);
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
		String username, password;
		const String hint = p_username_from_url ? String::utf8(p_username_from_url) : String();
		if (credential_fill(ctx->workdir, String::utf8(p_url), hint, username, password)) {
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

void set_remote_callbacks(git_remote_callbacks &r_callbacks, RemoteContext &p_ctx) {
	r_callbacks.credentials = credentials_cb;
	r_callbacks.push_update_reference = push_update_reference_cb;
	r_callbacks.payload = &p_ctx;
}

} // namespace

void GitRepository::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "path"), &GitRepository::open);
	ClassDB::bind_method(D_METHOD("is_open"), &GitRepository::is_open);
	ClassDB::bind_method(D_METHOD("get_workdir"), &GitRepository::get_workdir);
	ClassDB::bind_method(D_METHOD("get_current_branch"), &GitRepository::get_current_branch);
	ClassDB::bind_method(D_METHOD("get_branches"), &GitRepository::get_branches);
	ClassDB::bind_method(D_METHOD("get_remote_branches"), &GitRepository::get_remote_branches);
	ClassDB::bind_method(D_METHOD("get_remotes"), &GitRepository::get_remotes);
	ClassDB::bind_method(D_METHOD("get_sync_status"), &GitRepository::get_sync_status);
	ClassDB::bind_method(D_METHOD("get_status"), &GitRepository::get_status);
	ClassDB::bind_method(D_METHOD("get_line_stats", "staged"), &GitRepository::get_line_stats);
	ClassDB::bind_method(D_METHOD("get_commits", "max_count"), &GitRepository::get_commits, DEFVAL(50));

	ClassDB::bind_method(D_METHOD("stage", "path"), &GitRepository::stage);
	ClassDB::bind_method(D_METHOD("unstage", "path"), &GitRepository::unstage);
	ClassDB::bind_method(D_METHOD("stage_all"), &GitRepository::stage_all);
	ClassDB::bind_method(D_METHOD("unstage_all"), &GitRepository::unstage_all);
	ClassDB::bind_method(D_METHOD("discard", "path"), &GitRepository::discard);
	ClassDB::bind_method(D_METHOD("commit", "message"), &GitRepository::commit);
	ClassDB::bind_method(D_METHOD("checkout_branch", "branch"), &GitRepository::checkout_branch);
	ClassDB::bind_method(D_METHOD("create_branch", "name"), &GitRepository::create_branch);

	ClassDB::bind_method(D_METHOD("fetch"), &GitRepository::fetch);
	ClassDB::bind_method(D_METHOD("pull"), &GitRepository::pull);
	ClassDB::bind_method(D_METHOD("push"), &GitRepository::push);
	ClassDB::bind_method(D_METHOD("get_notice"), &GitRepository::get_notice);

	ClassDB::bind_static_method("GitRepository", D_METHOD("get_last_error"), &GitRepository::get_last_error);
	ClassDB::bind_static_method("GitRepository", D_METHOD("get_libgit2_version"), &GitRepository::get_libgit2_version);
}

GitRepository::~GitRepository() {
	close();
}

void GitRepository::close() {
	if (repo) {
		git_repository_free(repo);
		repo = nullptr;
	}
}

// Opens the repository containing p_path, searching parent folders like the git CLI does.
// Accepts res:// and user:// paths as well as absolute OS paths.
Error GitRepository::open(const String &p_path) {
	close();

	const String path = ProjectSettings::get_singleton()->globalize_path(p_path);
	const int err = git_repository_open_ext(&repo, path.utf8().get_data(), 0, nullptr);
	if (err < 0) {
		repo = nullptr;
		return err == GIT_ENOTFOUND ? ERR_FILE_NOT_FOUND : FAILED;
	}
	return OK;
}

bool GitRepository::is_open() const {
	return repo != nullptr;
}

String GitRepository::get_workdir() const {
	ERR_FAIL_NULL_V_MSG(repo, String(), "Repository is not open.");
	const char *workdir = git_repository_workdir(repo);
	return workdir ? String::utf8(workdir) : String();
}

String GitRepository::get_current_branch() const {
	ERR_FAIL_NULL_V_MSG(repo, String(), "Repository is not open.");

	git_reference *head = nullptr;
	const int err = git_repository_head(&head, repo);
	if (err == GIT_EUNBORNBRANCH) {
		// Fresh repo with no commits: HEAD points at a branch that doesn't exist yet.
		String name;
		if (git_reference_lookup(&head, repo, "HEAD") == 0) {
			const char *target = git_reference_symbolic_target(head);
			if (target) {
				name = String::utf8(target).trim_prefix("refs/heads/");
			}
			git_reference_free(head);
		}
		return name;
	}
	if (err < 0) {
		return String();
	}

	// Detached HEAD yields "HEAD" here.
	const String name = String::utf8(git_reference_shorthand(head));
	git_reference_free(head);
	return name;
}

// Returns an Array of Dictionaries: { "path": String, "index": String, "worktree": String }.
// "index" is the staged change, "worktree" the unstaged one; either may be "".
Array GitRepository::get_status() const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	git_status_options opts = GIT_STATUS_OPTIONS_INIT;
	opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
	opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX;

	git_status_list *list = nullptr;
	if (git_status_list_new(&list, repo, &opts) < 0) {
		ERR_FAIL_V_MSG(result, "git status failed: " + get_last_error());
	}

	const size_t count = git_status_list_entrycount(list);
	for (size_t i = 0; i < count; i++) {
		const git_status_entry *entry = git_status_byindex(list, i);
		if (entry->status == GIT_STATUS_CURRENT || (entry->status & GIT_STATUS_IGNORED)) {
			continue;
		}

		const git_diff_delta *delta = entry->head_to_index ? entry->head_to_index : entry->index_to_workdir;
		const char *path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;

		Dictionary item;
		item["path"] = String::utf8(path);
		item["index"] = index_status_name(entry->status);
		item["worktree"] = worktree_status_name(entry->status);
		result.push_back(item);
	}

	git_status_list_free(list);
	return result;
}

// Lines added/removed per changed file: { path: Vector2i(added, removed) }.
// Binary (and very large) files report Vector2i(-1, -1).
// p_staged: HEAD vs index (what the next commit contains); otherwise index vs working tree.
Dictionary GitRepository::get_line_stats(bool p_staged) const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	opts.max_size = 2 * 1024 * 1024; // Bigger files are treated as binary; counting them isn't worth the time.

	git_diff *diff = nullptr;
	int err = 0;
	if (p_staged) {
		git_object *head_tree = nullptr;
		git_revparse_single(&head_tree, repo, "HEAD^{tree}"); // Stays null before the first commit.
		err = git_diff_tree_to_index(&diff, repo, (git_tree *)head_tree, nullptr, &opts);
		git_object_free(head_tree);
		if (err >= 0) {
			git_diff_find_similar(diff, nullptr);
		}
	} else {
		opts.flags = GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS | GIT_DIFF_SHOW_UNTRACKED_CONTENT;
		err = git_diff_index_to_workdir(&diff, repo, nullptr, &opts);
	}
	if (err < 0) {
		return result;
	}

	const size_t count = git_diff_num_deltas(diff);
	for (size_t i = 0; i < count; i++) {
		git_patch *patch = nullptr;
		if (git_patch_from_diff(&patch, diff, i) < 0 || !patch) {
			continue;
		}
		const git_diff_delta *delta = git_patch_get_delta(patch);
		const char *path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;

		size_t added = 0, removed = 0;
		if (delta->flags & GIT_DIFF_FLAG_BINARY) {
			result[String::utf8(path)] = Vector2i(-1, -1);
		} else if (git_patch_line_stats(nullptr, &added, &removed, patch) == 0) {
			result[String::utf8(path)] = Vector2i((int32_t)added, (int32_t)removed);
		}
		git_patch_free(patch);
	}

	git_diff_free(diff);
	return result;
}

PackedStringArray GitRepository::get_branches() const {
	PackedStringArray branches;
	ERR_FAIL_NULL_V_MSG(repo, branches, "Repository is not open.");

	git_branch_iterator *it = nullptr;
	if (git_branch_iterator_new(&it, repo, GIT_BRANCH_LOCAL) < 0) {
		return branches;
	}

	git_reference *ref = nullptr;
	git_branch_t type;
	while (git_branch_next(&ref, &type, it) == 0) {
		const char *name = nullptr;
		if (git_branch_name(&name, ref) == 0) {
			branches.push_back(String::utf8(name));
		}
		git_reference_free(ref);
	}
	git_branch_iterator_free(it);
	return branches;
}

// Remote-tracking branches as "remote/branch" (e.g. "origin/main"), without the "origin/HEAD" alias.
PackedStringArray GitRepository::get_remote_branches() const {
	PackedStringArray branches;
	ERR_FAIL_NULL_V_MSG(repo, branches, "Repository is not open.");

	git_branch_iterator *it = nullptr;
	if (git_branch_iterator_new(&it, repo, GIT_BRANCH_REMOTE) < 0) {
		return branches;
	}

	git_reference *ref = nullptr;
	git_branch_t type;
	while (git_branch_next(&ref, &type, it) == 0) {
		const char *name = nullptr;
		if (git_reference_type(ref) == GIT_REFERENCE_DIRECT && git_branch_name(&name, ref) == 0) {
			branches.push_back(String::utf8(name));
		}
		git_reference_free(ref);
	}
	git_branch_iterator_free(it);
	return branches;
}

PackedStringArray GitRepository::get_remotes() const {
	PackedStringArray remotes;
	ERR_FAIL_NULL_V_MSG(repo, remotes, "Repository is not open.");

	git_strarray list = { nullptr, 0 };
	if (git_remote_list(&list, repo) == 0) {
		for (size_t i = 0; i < list.count; i++) {
			remotes.push_back(String::utf8(list.strings[i]));
		}
		git_strarray_dispose(&list);
	}
	return remotes;
}

// How the current branch relates to its upstream:
// { "branch": String, "upstream": String ("" if none), "ahead": int, "behind": int, "has_remotes": bool }
Dictionary GitRepository::get_sync_status() const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	result["branch"] = get_current_branch();
	result["upstream"] = String();
	result["ahead"] = 0;
	result["behind"] = 0;
	result["has_remotes"] = !get_remotes().is_empty();

	git_reference *head = nullptr;
	if (git_repository_head(&head, repo) < 0) {
		return result;
	}
	git_reference *upstream = nullptr;
	if (git_reference_is_branch(head) && git_branch_upstream(&upstream, head) == 0) {
		result["upstream"] = String::utf8(git_reference_shorthand(upstream));

		const git_oid *local_oid = git_reference_target(head);
		const git_oid *upstream_oid = git_reference_target(upstream);
		size_t ahead = 0, behind = 0;
		if (local_oid && upstream_oid && git_graph_ahead_behind(&ahead, &behind, repo, local_oid, upstream_oid) == 0) {
			result["ahead"] = (int64_t)ahead;
			result["behind"] = (int64_t)behind;
		}
		git_reference_free(upstream);
	}
	git_reference_free(head);
	return result;
}

// Returns up to p_max_count commits reachable from HEAD, newest first:
// [{ "id": String (short hash), "hash": String, "summary": String, "message": String,
//    "author": String, "time": int (unix), "unpushed": bool }, ...]
// "unpushed" means the commit isn't on any remote-tracking branch yet.
Array GitRepository::get_commits(int p_max_count) const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	HashSet<String> unpushed;
	if (!get_remotes().is_empty()) {
		git_revwalk *walk = nullptr;
		if (git_revwalk_new(&walk, repo) == 0) {
			if (git_revwalk_push_head(walk) == 0) {
				git_revwalk_hide_glob(walk, "refs/remotes/*");
				git_oid oid;
				// Capped so a huge never-pushed branch can't stall the panel.
				while (unpushed.size() < 1000 && git_revwalk_next(&oid, walk) == 0) {
					unpushed.insert(String(git_oid_tostr_s(&oid)));
				}
			}
			git_revwalk_free(walk);
		}
	}

	git_revwalk *walk = nullptr;
	if (git_revwalk_new(&walk, repo) < 0) {
		return result;
	}
	git_revwalk_sorting(walk, GIT_SORT_TIME);
	if (git_revwalk_push_head(walk) < 0) {
		// No commits yet.
		git_revwalk_free(walk);
		return result;
	}

	git_oid oid;
	while (result.size() < p_max_count && git_revwalk_next(&oid, walk) == 0) {
		git_commit *commit = nullptr;
		if (git_commit_lookup(&commit, repo, &oid) < 0) {
			continue;
		}

		const String hash = String(git_oid_tostr_s(&oid));
		const git_signature *author = git_commit_author(commit);
		const char *summary = git_commit_summary(commit);
		const char *message = git_commit_message(commit);

		Dictionary item;
		item["id"] = hash.substr(0, 7);
		item["hash"] = hash;
		item["summary"] = summary ? String::utf8(summary) : String();
		item["message"] = message ? String::utf8(message).strip_edges() : String();
		item["author"] = author ? String::utf8(author->name) : String();
		item["time"] = (int64_t)git_commit_time(commit);
		item["unpushed"] = unpushed.has(hash);
		result.push_back(item);

		git_commit_free(commit);
	}

	git_revwalk_free(walk);
	return result;
}

Error GitRepository::stage(const String &p_path) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	const CharString path = p_path.utf8();
	unsigned int flags = 0;
	int err = git_status_file(&flags, repo, path.get_data());
	if (err < 0) {
		return to_error(err);
	}

	git_index *index = nullptr;
	err = git_repository_index(&index, repo);
	if (err < 0) {
		return to_error(err);
	}

	// A file deleted from disk is staged by removing it from the index.
	if (flags & GIT_STATUS_WT_DELETED) {
		err = git_index_remove_bypath(index, path.get_data());
	} else {
		err = git_index_add_bypath(index, path.get_data());
	}
	if (err >= 0) {
		err = git_index_write(index);
	}
	git_index_free(index);
	return to_error(err);
}

Error GitRepository::unstage(const String &p_path) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	// With no commits yet there's no HEAD; a null target just drops the path from the index.
	git_object *head = nullptr;
	git_revparse_single(&head, repo, "HEAD");

	SinglePathspec pathspec(p_path);
	const int err = git_reset_default(repo, head, &pathspec.array);
	git_object_free(head);
	return to_error(err);
}

Error GitRepository::stage_all() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	git_index *index = nullptr;
	int err = git_repository_index(&index, repo);
	if (err < 0) {
		return to_error(err);
	}

	// add_all picks up new and modified files, update_all picks up deletions.
	const git_strarray everything = { nullptr, 0 };
	err = git_index_add_all(index, &everything, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr);
	if (err >= 0) {
		err = git_index_update_all(index, &everything, nullptr, nullptr);
	}
	if (err >= 0) {
		err = git_index_write(index);
	}
	git_index_free(index);
	return to_error(err);
}

Error GitRepository::unstage_all() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	// Like `git reset`: the index goes back to HEAD, files on disk are untouched.
	// (git_reset_default can't do "all paths"; it requires a non-empty pathspec.)
	git_object *head = nullptr;
	if (git_revparse_single(&head, repo, "HEAD^{commit}") == 0) {
		const int err = git_reset(repo, head, GIT_RESET_MIXED, nullptr);
		git_object_free(head);
		return to_error(err);
	}

	// No commits yet: everything staged is new, so unstaging all means an empty index.
	git_index *index = nullptr;
	int err = git_repository_index(&index, repo);
	if (err >= 0) {
		err = git_index_clear(index);
	}
	if (err >= 0) {
		err = git_index_write(index);
	}
	git_index_free(index);
	return to_error(err);
}

// Throws away unstaged changes to p_path (restores it from the index).
// Untracked files are deleted. This can't be undone.
Error GitRepository::discard(const String &p_path) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	unsigned int flags = 0;
	int err = git_status_file(&flags, repo, p_path.utf8().get_data());
	if (err < 0) {
		return to_error(err);
	}

	if (flags & GIT_STATUS_WT_NEW) {
		return DirAccess::remove_absolute(get_workdir().path_join(p_path));
	}

	SinglePathspec pathspec(p_path);
	git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
	opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
	opts.paths = pathspec.array;
	err = git_checkout_index(repo, nullptr, &opts);
	return to_error(err);
}

// Commits what's staged. Author/committer come from git config (user.name / user.email).
Error GitRepository::commit(const String &p_message) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	git_oid oid;
	return to_error(git_commit_create_from_stage(&oid, repo, p_message.utf8().get_data(), nullptr));
}

// Switches to a branch. Accepts a local branch ("feature") or a remote-tracking one
// ("origin/feature"); for the latter a local branch tracking it is created if needed.
// Refuses (and changes nothing) if local changes would be overwritten.
Error GitRepository::checkout_branch(const String &p_branch) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();

	git_reference *ref = nullptr;
	int err = git_branch_lookup(&ref, repo, p_branch.utf8().get_data(), GIT_BRANCH_LOCAL);
	if (err == GIT_ENOTFOUND) {
		git_reference *remote_ref = nullptr;
		if (git_branch_lookup(&remote_ref, repo, p_branch.utf8().get_data(), GIT_BRANCH_REMOTE) < 0) {
			return fail(vformat("There is no branch called \"%s\".", p_branch));
		}
		// "origin/feature" -> "feature".
		const int slash = p_branch.find("/");
		const String local_name = slash >= 0 ? p_branch.substr(slash + 1) : p_branch;

		// A local branch with that name may already exist; use it rather than failing.
		err = git_branch_lookup(&ref, repo, local_name.utf8().get_data(), GIT_BRANCH_LOCAL);
		if (err == GIT_ENOTFOUND) {
			git_commit *commit = nullptr;
			err = git_commit_lookup(&commit, repo, git_reference_target(remote_ref));
			if (err >= 0) {
				err = git_branch_create(&ref, repo, local_name.utf8().get_data(), commit, 0);
				git_commit_free(commit);
			}
			if (err >= 0) {
				git_branch_set_upstream(ref, p_branch.utf8().get_data());
			}
		}
		git_reference_free(remote_ref);
	}
	if (err < 0) {
		return to_error(err);
	}

	git_object *target = nullptr;
	err = git_reference_peel(&target, ref, GIT_OBJECT_COMMIT);
	if (err >= 0) {
		git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
		opts.checkout_strategy = GIT_CHECKOUT_SAFE;
		err = git_checkout_tree(repo, target, &opts);
		if (err == GIT_ECONFLICT) {
			fail("Your local changes would be overwritten by switching branches. Commit or discard them first.");
		}
	}
	if (err >= 0) {
		err = git_repository_set_head(repo, git_reference_name(ref));
	}

	git_object_free(target);
	git_reference_free(ref);
	return to_error(err);
}

// Creates a branch at the current commit and switches to it.
Error GitRepository::create_branch(const String &p_name) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();

	int valid = 0;
	if (git_branch_name_is_valid(&valid, p_name.utf8().get_data()) < 0 || !valid) {
		return fail(vformat("\"%s\" isn't a valid branch name.", p_name));
	}

	git_object *head = nullptr;
	if (git_revparse_single(&head, repo, "HEAD^{commit}") < 0) {
		return fail("Make a first commit before creating branches.");
	}
	git_reference *ref = nullptr;
	int err = git_branch_create(&ref, repo, p_name.utf8().get_data(), (git_commit *)head, 0);
	git_object_free(head);
	if (err == GIT_EEXISTS) {
		return fail(vformat("A branch called \"%s\" already exists.", p_name));
	}
	if (err < 0) {
		return to_error(err);
	}
	err = git_repository_set_head(repo, git_reference_name(ref));
	git_reference_free(ref);
	return to_error(err);
}

Error GitRepository::_fetch_remote(const String &p_remote) {
	git_remote *remote = nullptr;
	int err = git_remote_lookup(&remote, repo, p_remote.utf8().get_data());
	if (err < 0) {
		return to_error(err);
	}

	RemoteContext ctx;
	ctx.workdir = get_workdir();
	git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
	set_remote_callbacks(opts.callbacks, ctx);

	err = git_remote_fetch(remote, nullptr, &opts, "fetch");
	git_remote_free(remote);
	return to_error(err);
}

// Fetches the current branch's remote, or every remote if the branch has no upstream.
Error GitRepository::fetch() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();

	const PackedStringArray remotes = get_remotes();
	if (remotes.is_empty()) {
		return fail("This repository has no remote.");
	}

	git_reference *head = nullptr;
	if (git_repository_head(&head, repo) == 0) {
		git_buf remote_name = GIT_BUF_INIT;
		const int err = git_branch_upstream_remote(&remote_name, repo, git_reference_name(head));
		git_reference_free(head);
		if (err == 0) {
			return _fetch_remote(buf_to_string(remote_name));
		}
		git_buf_dispose(&remote_name);
	}

	for (const String &remote : remotes) {
		const Error err = _fetch_remote(remote);
		if (err != OK) {
			return err;
		}
	}
	return OK;
}

// Fetches the upstream, then fast-forwards, or creates a merge commit when both sides have
// new commits. If the merge would conflict, nothing is changed and an error is returned.
Error GitRepository::pull() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	notice = String();

	git_reference *head = nullptr;
	int err = head_branch(&head, repo);
	if (err < 0) {
		return FAILED;
	}

	git_buf remote_buf = GIT_BUF_INIT;
	if (git_branch_upstream_remote(&remote_buf, repo, git_reference_name(head)) < 0) {
		git_buf_dispose(&remote_buf);
		git_reference_free(head);
		return fail("This branch isn't tracking a remote branch yet. Push it first.");
	}
	const String remote_name = buf_to_string(remote_buf);

	Error fetch_err = _fetch_remote(remote_name);
	if (fetch_err != OK) {
		git_reference_free(head);
		return fetch_err;
	}

	git_reference *upstream = nullptr;
	git_annotated_commit *theirs = nullptr;
	err = git_branch_upstream(&upstream, head);
	if (err < 0) {
		git_reference_free(head);
		return fail("The upstream branch doesn't exist on the remote anymore.");
	}
	err = git_annotated_commit_from_ref(&theirs, repo, upstream);

	git_merge_analysis_t analysis = GIT_MERGE_ANALYSIS_NONE;
	git_merge_preference_t preference = GIT_MERGE_PREFERENCE_NONE;
	if (err >= 0) {
		err = git_merge_analysis(&analysis, &preference, repo, (const git_annotated_commit **)&theirs, 1);
	}

	Error result = to_error(err);
	if (err < 0 || (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)) {
		// Error or nothing to do.
	} else if ((analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) && !(preference & GIT_MERGE_PREFERENCE_NO_FASTFORWARD)) {
		const git_oid *target_oid = git_annotated_commit_id(theirs);
		git_object *target = nullptr;
		err = git_object_lookup(&target, repo, target_oid, GIT_OBJECT_COMMIT);
		if (err >= 0) {
			git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
			opts.checkout_strategy = GIT_CHECKOUT_SAFE;
			err = git_checkout_tree(repo, target, &opts);
			if (err == GIT_ECONFLICT) {
				fail("Your local changes would be overwritten by the pull. Commit or discard them first.");
			}
		}
		if (err >= 0) {
			git_reference *moved = nullptr;
			err = git_reference_set_target(&moved, head, target_oid, "pull: fast-forward");
			git_reference_free(moved);
		}
		git_object_free(target);
		result = to_error(err);
	} else {
		// Both sides have new commits: merge. Uncommitted changes to tracked files (normal in Godot,
		// which rewrites project.godot and scenes) are set aside first, like `git pull --autostash`,
		// so a conflicting merge can be undone completely, then put back afterwards.
		git_status_options status_opts = GIT_STATUS_OPTIONS_INIT;
		status_opts.flags = 0; // Tracked files only; untracked files are left where they are.
		git_status_list *status = nullptr;
		const bool dirty = git_status_list_new(&status, repo, &status_opts) == 0 && git_status_list_entrycount(status) > 0;
		git_status_list_free(status);

		bool stashed = false;
		git_oid stash_id;
		if (dirty) {
			git_signature *stasher = nullptr;
			err = git_signature_default(&stasher, repo);
			if (err >= 0) {
				err = git_stash_save(&stash_id, repo, stasher, "godot-git: your changes, set aside while pulling", GIT_STASH_DEFAULT);
			}
			git_signature_free(stasher);
			stashed = err >= 0;
			if (!stashed) {
				result = to_error(err);
			}
		}

		if (!dirty || stashed) {
			git_merge_options merge_opts = GIT_MERGE_OPTIONS_INIT;
			git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
			checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_ALLOW_CONFLICTS;
			err = git_merge(repo, (const git_annotated_commit **)&theirs, 1, &merge_opts, &checkout_opts);

			git_index *index = nullptr;
			if (err >= 0) {
				err = git_repository_index(&index, repo);
			}
			if (err >= 0 && git_index_has_conflicts(index)) {
				git_object *head_commit = nullptr;
				git_revparse_single(&head_commit, repo, "HEAD");
				git_reset(repo, head_commit, GIT_RESET_HARD, nullptr);
				git_object_free(head_commit);
				git_repository_state_cleanup(repo);
				result = fail("Pulling would cause merge conflicts, so nothing was changed. Resolving conflicts isn't supported in the panel yet; use git in a terminal for this one.");
			} else if (err >= 0) {
				git_oid tree_oid, commit_oid;
				git_tree *tree = nullptr;
				git_signature *signature = nullptr;
				git_commit *ours = nullptr;
				git_commit *their_commit = nullptr;

				err = git_index_write_tree(&tree_oid, index);
				if (err >= 0) {
					err = git_tree_lookup(&tree, repo, &tree_oid);
				}
				if (err >= 0) {
					err = git_signature_default(&signature, repo);
				}
				if (err >= 0) {
					err = git_commit_lookup(&ours, repo, git_reference_target(head));
				}
				if (err >= 0) {
					err = git_commit_lookup(&their_commit, repo, git_annotated_commit_id(theirs));
				}
				if (err >= 0) {
					const git_commit *parents[2] = { ours, their_commit };
					const String message = vformat("Merge remote-tracking branch '%s'", String::utf8(git_reference_shorthand(upstream)));
					err = git_commit_create(&commit_oid, repo, "HEAD", signature, signature, nullptr, message.utf8().get_data(), tree, 2, parents);
				}
				git_repository_state_cleanup(repo);

				git_commit_free(their_commit);
				git_commit_free(ours);
				git_signature_free(signature);
				git_tree_free(tree);
				result = to_error(err);
			} else {
				// git_merge itself refused (e.g. it would overwrite an untracked file); nothing changed.
				git_repository_state_cleanup(repo);
				result = to_error(err);
			}
			git_index_free(index);

			// Put the set-aside changes back, including what was staged. Keep the pull's own error
			// message: libgit2 may overwrite it while restoring.
			if (stashed) {
				const String pull_error = result == OK ? String() : get_last_error();

				// libgit2 (unlike the git CLI) writes conflict markers into files and drops the stash
				// when putting changes back conflicts. So if the pull touched any file the user had
				// changed, leave their changes untouched in the stash instead.
				PackedStringArray overlap;
				if (result == OK) {
					const HashSet<String> pulled = changed_paths(repo, git_reference_target(head), nullptr);
					for (const String &path : stashed_paths(repo, &stash_id)) {
						if (pulled.has(path)) {
							overlap.push_back(path);
						}
					}
				}

				bool restored = false;
				if (overlap.is_empty()) {
					git_stash_apply_options apply_opts = GIT_STASH_APPLY_OPTIONS_INIT;
					apply_opts.flags = GIT_STASH_APPLY_REINSTATE_INDEX;
					restored = git_stash_pop(repo, 0, &apply_opts) >= 0;
				}

				if (result == OK && !restored) {
					notice = vformat("Pulled, but it also changed %s, which you had uncommitted edits to. Your edits are kept in a git stash, untouched; run `git stash pop` in a terminal to merge them back.",
							overlap.is_empty() ? String("files") : String(", ").join(overlap.slice(0, 3)) + (overlap.size() > 3 ? String(", ...") : String()));
				} else if (result != OK) {
					fail(restored ? pull_error : vformat("%s Your uncommitted changes are saved in a git stash; run `git stash pop` in a terminal to get them back.", pull_error));
				}
			}
		}
	}

	git_annotated_commit_free(theirs);
	git_reference_free(upstream);
	git_reference_free(head);
	return result;
}

// Pushes the current branch to its upstream. A branch without one is published to "origin"
// (or the only remote) under the same name and starts tracking it.
Error GitRepository::push() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();

	git_reference *head = nullptr;
	if (head_branch(&head, repo) < 0) {
		return FAILED;
	}
	const String local_ref = String::utf8(git_reference_name(head));
	const String branch = String::utf8(git_reference_shorthand(head));

	String remote_name;
	String remote_ref;
	bool publish = false;

	git_buf buf = GIT_BUF_INIT;
	if (git_branch_upstream_remote(&buf, repo, local_ref.utf8().get_data()) == 0) {
		remote_name = buf_to_string(buf);
		if (git_branch_upstream_merge(&buf, repo, local_ref.utf8().get_data()) == 0) {
			remote_ref = buf_to_string(buf);
		}
	}
	git_buf_dispose(&buf);

	if (remote_name.is_empty() || remote_ref.is_empty()) {
		const PackedStringArray remotes = get_remotes();
		if (remotes.is_empty()) {
			git_reference_free(head);
			return fail("This repository has no remote to push to.");
		}
		remote_name = remotes.has("origin") ? String("origin") : remotes[0];
		remote_ref = local_ref;
		publish = true;
	}

	git_remote *remote = nullptr;
	int err = git_remote_lookup(&remote, repo, remote_name.utf8().get_data());
	if (err < 0) {
		git_reference_free(head);
		return to_error(err);
	}

	RemoteContext ctx;
	ctx.workdir = get_workdir();
	git_push_options opts = GIT_PUSH_OPTIONS_INIT;
	set_remote_callbacks(opts.callbacks, ctx);

	SinglePathspec refspec(vformat("%s:%s", local_ref, remote_ref));
	err = git_remote_push(remote, &refspec.array, &opts);
	git_remote_free(remote);

	Error result = to_error(err);
	if (err < 0 && get_last_error().contains("non-fast")) {
		result = fail("The remote has commits you don't have yet. Pull first, then push.");
	} else if (err >= 0 && !ctx.push_rejection.is_empty()) {
		result = fail("The remote rejected the push: " + ctx.push_rejection);
	} else if (err >= 0 && publish) {
		git_branch_set_upstream(head, vformat("%s/%s", remote_name, branch).utf8().get_data());
	}

	git_reference_free(head);
	return result;
}

// A warning from the last operation that still succeeded (e.g. a pull whose local changes had
// to stay in a stash), or "".
String GitRepository::get_notice() const {
	return notice;
}

// Stops a `git credential fill` that a network operation is waiting on, so that operation fails
// quickly instead of blocking (used when the editor closes during a login).
void GitRepository::cancel_pending_login() {
	const int64_t pid = credential_pid.load();
	if (pid > 0) {
		OS::get_singleton()->kill(pid);
	}
}

String GitRepository::get_last_error() {
	// Since libgit2 1.8 this is never null; "no error" comes back with class GIT_ERROR_NONE.
	const git_error *err = git_error_last();
	return (err && err->klass != GIT_ERROR_NONE && err->message) ? String::utf8(err->message) : String();
}

String GitRepository::get_libgit2_version() {
	int major = 0, minor = 0, rev = 0;
	git_libgit2_version(&major, &minor, &rev);
	return vformat("%d.%d.%d", major, minor, rev);
}
