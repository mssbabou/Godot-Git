// GitRepository: opening a repository and reading its state (status, line stats, branches, sync
// status, what it needs git for). The rest is split by area: git_repository_local.cpp (stage,
// commit, switch), _history, _diff, _remote (fetch, push), _pull, _operation, _setup.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <string>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// Files over this many bytes count as binary in the line counts: counting them isn't worth the time.
constexpr int64_t MAX_COUNTED_SIZE = 2 * 1024 * 1024;

// Whether what's committed differs from the file on disk only by line endings (or LFS, handled
// apart): no .gitattributes asks for another filter, an ident or a text encoding. Then line counts
// can compare the bytes directly.
bool simple_attributes(git_repository *p_repo) {
	const char *workdir = git_repository_workdir(p_repo);
	if (!workdir) {
		return true;
	}
	PackedStringArray files;
	files.push_back(String::utf8(workdir).path_join(".gitattributes"));
	files.push_back(String::utf8(git_repository_path(p_repo)).path_join("info/attributes"));
	for (const String &path : files) {
		if (!FileAccess::file_exists(path)) {
			continue;
		}
		const String text = FileAccess::get_file_as_string(path);
		if (text.replace("filter=lfs", "").contains("filter=") || text.contains("ident") || text.contains("working-tree-encoding")) {
			return false;
		}
	}
	return true;
}

} // namespace

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

} // namespace

void GitRepository::_bind_methods() {
	ClassDB::bind_method(D_METHOD("call_api", "name", "args"), &GitRepository::call_api);
	ClassDB::bind_static_method("GitRepository", D_METHOD("call_static_api", "name", "args"), &GitRepository::call_static_api);
}

GitRepository::~GitRepository() {
	close();
}

void GitRepository::close() {
	commits_key = String();
	commits_cache = Dictionary();
	if (repo) {
		release_lfs(repo);
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

	ReferencePtr head;
	const int err = git_repository_head(head.out(), repo);
	if (err == GIT_EUNBORNBRANCH) {
		// Fresh repo with no commits: HEAD points at a branch that doesn't exist yet.
		if (git_reference_lookup(head.out(), repo, "HEAD") == 0 && git_reference_symbolic_target(head)) {
			return String::utf8(git_reference_symbolic_target(head)).trim_prefix("refs/heads/");
		}
		return String();
	}
	if (err < 0) {
		return String();
	}

	// Detached HEAD yields "HEAD" here.
	return String::utf8(git_reference_shorthand(head));
}

// Returns an Array of Dictionaries: { "path": String, "index": String, "worktree": String }.
// "index" is the staged change, "worktree" the unstaged one; either may be "".
Array GitRepository::get_status() const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	git_status_options opts = GIT_STATUS_OPTIONS_INIT;
	opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
	opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX;

	StatusListPtr list;
	if (git_status_list_new(list.out(), repo, &opts) < 0) {
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
	return result;
}

// Lines added/removed per changed file: { path: Vector2i(added, removed) }.
// Binary (and very large) files report Vector2i(-1, -1).
// p_staged: HEAD vs index (what the next commit contains); otherwise index vs working tree.
Dictionary GitRepository::get_line_stats(bool p_staged) const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	if (!simple_attributes(repo)) {
		return _line_stats_through_libgit2(p_staged);
	}

	// Which files changed: a diff without contents is cheap (no file is read or filtered).
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	DiffPtr diff;
	int err = 0;
	if (p_staged) {
		ObjectPtr head_tree;
		git_revparse_single(head_tree.out(), repo, "HEAD^{tree}"); // Stays null before the first commit.
		err = git_diff_tree_to_index(diff.out(), repo, (git_tree *)head_tree.get(), nullptr, &opts);
		if (err >= 0) {
			git_diff_find_similar(diff, nullptr);
		}
	} else {
		opts.flags = GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS;
		err = git_diff_index_to_workdir(diff.out(), repo, nullptr, &opts);
	}
	if (err < 0) {
		return result;
	}
	const bool uses_lfs = repo_uses_lfs(repo);
	const String workdir = get_workdir();
	const size_t count = git_diff_num_deltas(diff);
	for (size_t i = 0; i < count; i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		const char *path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
		const String key = String::utf8(path);
		if (delta->status == GIT_DELTA_TYPECHANGE || (uses_lfs && is_lfs_path(repo, path))) {
			result[key] = Vector2i(-1, -1);
			continue;
		}
		// The old side is always a blob; the new one a blob (staged) or the file on disk.
		BlobPtr old_blob, new_blob;
		const char *old_data = "";
		size_t old_size = 0;
		if (delta->status != GIT_DELTA_ADDED && delta->status != GIT_DELTA_UNTRACKED && git_blob_lookup(old_blob.out(), repo, &delta->old_file.id) == 0) {
			old_data = (const char *)git_blob_rawcontent(old_blob);
			old_size = (size_t)git_blob_rawsize(old_blob);
		}
		std::string file_data;
		const char *new_data = "";
		size_t new_size = 0;
		if (delta->status != GIT_DELTA_DELETED) {
			if (p_staged) {
				if (git_blob_lookup(new_blob.out(), repo, &delta->new_file.id) == 0) {
					new_data = (const char *)git_blob_rawcontent(new_blob);
					new_size = (size_t)git_blob_rawsize(new_blob);
				}
			} else {
				const String absolute = workdir.path_join(key);
				Ref<FileAccess> file = FileAccess::open(absolute, FileAccess::READ);
				if (file.is_null()) {
					continue;
				}
				if (file->get_length() > MAX_COUNTED_SIZE) {
					result[key] = Vector2i(-1, -1);
					continue;
				}
				const PackedByteArray bytes = file->get_buffer(file->get_length());
				file_data.assign((const char *)bytes.ptr(), bytes.size());
				// What committing would store: with core.autocrlf (or eol attributes) CRLF becomes
				// LF, so a file whose committed version has none compares without them.
				if (file_data.find('\r') != std::string::npos && memchr(old_data, '\r', old_size) == nullptr) {
					std::string stripped;
					stripped.reserve(file_data.size());
					for (size_t c = 0; c < file_data.size(); c++) {
						if (!(file_data[c] == '\r' && c + 1 < file_data.size() && file_data[c + 1] == '\n')) {
							stripped += file_data[c];
						}
					}
					file_data.swap(stripped);
				}
				new_data = file_data.data();
				new_size = file_data.size();
			}
		}
		if (old_size > MAX_COUNTED_SIZE || new_size > MAX_COUNTED_SIZE || memchr(old_data, 0, MIN(old_size, (size_t)8000)) || memchr(new_data, 0, MIN(new_size, (size_t)8000))) {
			result[key] = Vector2i(-1, -1);
			continue;
		}
		// No paths given: libgit2 then doesn't look up attributes (a diff driver) for each file,
		// which is what made counting slow (see Performance in CLAUDE.md).
		PatchPtr patch;
		size_t added = 0, removed = 0;
		if (git_patch_from_buffers(patch.out(), old_data, old_size, nullptr, new_data, new_size, nullptr, nullptr) == 0 && patch && git_patch_line_stats(nullptr, &added, &removed, patch) == 0) {
			result[key] = Vector2i((int32_t)added, (int32_t)removed);
		}
	}
	return result;
}

// get_line_stats the plain libgit2 way, for repositories whose .gitattributes ask for filters
// other than LFS (or text encodings): only libgit2's filters know what those do to a file. Slow
// (2.6 ms a file on Windows: it looks attributes up afresh for each), but exact.
Dictionary GitRepository::_line_stats_through_libgit2(bool p_staged) const {
	Dictionary result;
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	opts.max_size = MAX_COUNTED_SIZE; // Bigger files are treated as binary; counting them isn't worth the time.

	DiffPtr diff;
	int err = 0;
	if (p_staged) {
		ObjectPtr head_tree;
		git_revparse_single(head_tree.out(), repo, "HEAD^{tree}");
		err = git_diff_tree_to_index(diff.out(), repo, (git_tree *)head_tree.get(), nullptr, &opts);
		if (err >= 0) {
			git_diff_find_similar(diff, nullptr);
		}
	} else {
		opts.flags = GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS | GIT_DIFF_SHOW_UNTRACKED_CONTENT;
		err = git_diff_index_to_workdir(diff.out(), repo, nullptr, &opts);
	}
	if (err < 0) {
		return result;
	}
	const bool uses_lfs = repo_uses_lfs(repo);
	const size_t count = git_diff_num_deltas(diff);
	for (size_t i = 0; i < count; i++) {
		const git_diff_delta *lfs_delta = git_diff_get_delta(diff, i);
		const char *lfs_path = lfs_delta->new_file.path ? lfs_delta->new_file.path : lfs_delta->old_file.path;
		if (uses_lfs && is_lfs_path(repo, lfs_path)) {
			result[String::utf8(lfs_path)] = Vector2i(-1, -1);
			continue;
		}
		PatchPtr patch;
		if (git_patch_from_diff(patch.out(), diff, i) < 0 || !patch) {
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
	}
	return result;
}

// Whether some files are stored with Git LFS. Switching branches can then download files, so the
// dock runs it in the background.
bool GitRepository::uses_lfs() const {
	ERR_FAIL_NULL_V_MSG(repo, false, "Repository is not open.");
	return repo_uses_lfs(repo);
}

// Whether a commit ("HEAD", "feature", "origin/feature") contains a file.
bool GitRepository::has_file_at(const String &p_revision, const String &p_path) const {
	ERR_FAIL_NULL_V_MSG(repo, false, "Repository is not open.");
	ObjectPtr blob;
	return git_revparse_single(blob.out(), repo, vformat("%s:%s", p_revision, p_path).utf8().get_data()) == 0;
}

// What in this repository only works through the git program (see git_cli.h), so the dock can
// say up front what won't work without it: { "commit": why committing needs git, or "";
// "pre_push": bool; "lfs": bool; "ssh_remotes", "https_remotes": remote names (HTTPS logins
// come from git's credential helper) }.
Dictionary GitRepository::get_git_needs() const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	result["commit"] = commit_git_reason(repo, COMMIT_NEW);
	result["pre_push"] = has_hook(repo, "pre-push");
	result["lfs"] = repo_uses_lfs(repo);
	PackedStringArray ssh;
	PackedStringArray https;
	for (const String &name : get_remotes()) {
		RemotePtr remote;
		if (git_remote_lookup(remote.out(), repo, name.utf8().get_data()) < 0 || !git_remote_url(remote)) {
			continue;
		}
		const String url = String::utf8(git_remote_url(remote));
		if (is_ssh_url(url)) {
			ssh.push_back(name);
		} else if (url.begins_with("http://") || url.begins_with("https://")) {
			https.push_back(name);
		}
	}
	result["ssh_remotes"] = ssh;
	result["https_remotes"] = https;
	return result;
}

// The URL p_remote fetches from, or "" if there's no such remote.
String GitRepository::get_remote_url(const String &p_remote) const {
	RemotePtr remote;
	if (!repo || git_remote_lookup(remote.out(), repo, p_remote.utf8().get_data()) < 0 || !git_remote_url(remote)) {
		return String();
	}
	return String::utf8(git_remote_url(remote));
}

// Staged files (new or changed) of at least p_min_size bytes, as [{path, size}], largest first.
// Sizes are of what's staged: a Git LFS file is only its small pointer, so it never counts.
Array GitRepository::get_large_staged_files(int64_t p_min_size) const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	ObjectPtr head_tree; // Null before the first commit: everything staged is new.
	git_revparse_single(head_tree.out(), repo, "HEAD^{tree}");
	DiffPtr diff;
	OdbPtr odb;
	if (git_diff_tree_to_index(diff.out(), repo, (git_tree *)head_tree.get(), nullptr, nullptr) < 0 || git_repository_odb(odb.out(), repo) < 0) {
		return result;
	}
	for (size_t i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		if (delta->status == GIT_DELTA_DELETED) {
			continue;
		}
		size_t size = 0;
		git_object_t type;
		if (git_odb_read_header(&size, &type, odb, &delta->new_file.id) == 0 && (int64_t)size >= p_min_size) {
			Dictionary file;
			file["path"] = String::utf8(delta->new_file.path);
			file["size"] = (int64_t)size;
			int at = 0;
			while (at < result.size() && (int64_t)Dictionary(result[at])["size"] >= (int64_t)size) {
				at++;
			}
			result.insert(at, file);
		}
	}
	return result;
}

PackedStringArray GitRepository::get_branches() const {
	PackedStringArray branches;
	ERR_FAIL_NULL_V_MSG(repo, branches, "Repository is not open.");

	BranchIteratorPtr it;
	if (git_branch_iterator_new(it.out(), repo, GIT_BRANCH_LOCAL) < 0) {
		return branches;
	}

	ReferencePtr ref;
	git_branch_t type;
	while (git_branch_next(ref.out(), &type, it) == 0) {
		const char *name = nullptr;
		if (git_branch_name(&name, ref) == 0) {
			branches.push_back(String::utf8(name));
		}
	}
	return branches;
}

// Remote-tracking branches as "remote/branch" (e.g. "origin/main"), without the "origin/HEAD" alias.
PackedStringArray GitRepository::get_remote_branches() const {
	PackedStringArray branches;
	ERR_FAIL_NULL_V_MSG(repo, branches, "Repository is not open.");

	BranchIteratorPtr it;
	if (git_branch_iterator_new(it.out(), repo, GIT_BRANCH_REMOTE) < 0) {
		return branches;
	}

	ReferencePtr ref;
	git_branch_t type;
	while (git_branch_next(ref.out(), &type, it) == 0) {
		const char *name = nullptr;
		if (git_reference_type(ref) == GIT_REFERENCE_DIRECT && git_branch_name(&name, ref) == 0) {
			branches.push_back(String::utf8(name));
		}
	}
	return branches;
}

// Every branch, for the branch picker: [{ "name", "local" (false: a remote-tracking branch with no
// local branch of the same name, which picking checks out as one), "current", "upstream"
// ("origin/x", or ""), "ahead" / "behind" (commits against the upstream; 0 without one), "time"
// (its last commit's, unix; 0 for a branch without commits) }]. Local branches first (the current
// one first), then remote ones, each most recently changed first.
Array GitRepository::get_branch_list() const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	const auto commit_time = [&](const git_oid *p_oid) -> int64_t {
		CommitPtr commit;
		return p_oid && git_commit_lookup(commit.out(), repo, p_oid) == 0 ? (int64_t)git_commit_time(commit) : 0;
	};
	// The current branch first, then the most recently changed.
	const auto newest_first = [](const Variant &p_a, const Variant &p_b) {
		const Dictionary a = p_a;
		const Dictionary b = p_b;
		if (bool(a["current"]) != bool(b["current"])) {
			return bool(a["current"]);
		}
		return (int64_t)a["time"] > (int64_t)b["time"];
	};

	const String current = get_current_branch();
	Array local;
	HashSet<String> local_names;
	BranchIteratorPtr it;
	if (git_branch_iterator_new(it.out(), repo, GIT_BRANCH_LOCAL) == 0) {
		ReferencePtr ref;
		git_branch_t type;
		while (git_branch_next(ref.out(), &type, it) == 0) {
			const char *name = nullptr;
			if (git_branch_name(&name, ref) < 0) {
				continue;
			}
			Dictionary branch;
			branch["name"] = String::utf8(name);
			branch["local"] = true;
			branch["current"] = git_branch_is_head(ref) == 1;
			branch["upstream"] = String();
			branch["ahead"] = 0;
			branch["behind"] = 0;
			branch["time"] = commit_time(git_reference_target(ref));
			ReferencePtr upstream;
			const char *upstream_name = nullptr;
			if (git_branch_upstream(upstream.out(), ref) == 0 && git_branch_name(&upstream_name, upstream) == 0) {
				branch["upstream"] = String::utf8(upstream_name);
				size_t ahead = 0, behind = 0;
				if (git_reference_target(ref) && git_reference_target(upstream) && git_graph_ahead_behind(&ahead, &behind, repo, git_reference_target(ref), git_reference_target(upstream)) == 0) {
					branch["ahead"] = (int64_t)ahead;
					branch["behind"] = (int64_t)behind;
				}
			}
			git_error_clear(); // No upstream is normal.
			local_names.insert(branch["name"]);
			local.push_back(branch);
		}
	}
	if (!current.is_empty() && !local_names.has(current)) {
		// A branch without commits yet: HEAD names it, but it doesn't exist.
		Dictionary branch;
		branch["name"] = current;
		branch["local"] = true;
		branch["current"] = true;
		branch["upstream"] = String();
		branch["ahead"] = 0;
		branch["behind"] = 0;
		branch["time"] = (int64_t)0;
		local.push_back(branch);
		local_names.insert(current);
	}
	local.sort_custom(callable_mp_static(+newest_first));

	Array remote;
	if (git_branch_iterator_new(it.out(), repo, GIT_BRANCH_REMOTE) == 0) {
		ReferencePtr ref;
		git_branch_t type;
		while (git_branch_next(ref.out(), &type, it) == 0) {
			const char *name = nullptr;
			if (git_reference_type(ref) != GIT_REFERENCE_DIRECT || git_branch_name(&name, ref) < 0) {
				continue; // origin/HEAD is an alias.
			}
			const String full = String::utf8(name);
			const int slash = full.find("/");
			if (local_names.has(slash >= 0 ? full.substr(slash + 1) : full)) {
				continue; // Listed as its local branch.
			}
			Dictionary branch;
			branch["name"] = full;
			branch["local"] = false;
			branch["current"] = false;
			branch["upstream"] = String();
			branch["ahead"] = 0;
			branch["behind"] = 0;
			branch["time"] = commit_time(git_reference_target(ref));
			remote.push_back(branch);
		}
	}
	remote.sort_custom(callable_mp_static(+newest_first));
	result.append_array(local);
	result.append_array(remote);
	return result;
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
// { "branch": String, "upstream": String ("" if none), "ahead": int, "behind": int, "has_remotes": bool,
// "head": HEAD's commit id ("" without commits) }
Dictionary GitRepository::get_sync_status() const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	result["branch"] = get_current_branch();
	result["upstream"] = String();
	result["ahead"] = 0;
	result["behind"] = 0;
	result["has_remotes"] = !get_remotes().is_empty();
	// Every fetch (ours or the git CLI's) rewrites FETCH_HEAD, so its age says how fresh the
	// ahead/behind counts are. 0 if never fetched.
	const String fetch_head = String::utf8(git_repository_commondir(repo)).path_join("FETCH_HEAD");
	result["last_fetched"] = FileAccess::file_exists(fetch_head) ? (int64_t)FileAccess::get_modified_time(fetch_head) : (int64_t)0;

	git_oid head_id;
	result["head"] = git_reference_name_to_id(&head_id, repo, "HEAD") == 0 ? String(git_oid_tostr_s(&head_id)) : String();
	git_error_clear(); // No commits yet.

	ReferencePtr head;
	if (git_repository_head(head.out(), repo) < 0) {
		return result;
	}
	ReferencePtr upstream;
	if (git_reference_is_branch(head) && git_branch_upstream(upstream.out(), head) == 0) {
		result["upstream"] = String::utf8(git_reference_shorthand(upstream));

		const git_oid *local_oid = git_reference_target(head);
		const git_oid *upstream_oid = git_reference_target(upstream);
		size_t ahead = 0, behind = 0;
		if (local_oid && upstream_oid && git_graph_ahead_behind(&ahead, &behind, repo, local_oid, upstream_oid) == 0) {
			result["ahead"] = (int64_t)ahead;
			result["behind"] = (int64_t)behind;
		}
	}
	return result;
}

String GitRepository::get_last_error() {
	// Since libgit2 1.8 this is never null; "no error" comes back with class GIT_ERROR_NONE.
	const git_error *err = git_error_last();
	return (err && err->klass != GIT_ERROR_NONE && err->message) ? String::utf8(err->message) : String();
}

// Whether git itself is installed (checked once; see git_installed()).
bool GitRepository::is_git_installed() {
	return git_installed();
}

bool GitRepository::is_lfs_installed() {
	return lfs_installed();
}

// Checks again whether git is installed, e.g. when the editor gets focus back.
bool GitRepository::check_git_installed() {
	return check_git();
}

// For the tests: run p_program instead of "git", e.g. one that doesn't exist.
void GitRepository::set_git_program(const String &p_program) {
	godot_git::set_git_program(p_program);
}

String GitRepository::get_libgit2_version() {
	int major = 0, minor = 0, rev = 0;
	git_libgit2_version(&major, &minor, &rev);
	return vformat("%d.%d.%d", major, minor, rev);
}
