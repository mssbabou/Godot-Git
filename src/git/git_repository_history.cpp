// GitRepository: History (the commits, newest first, with which of them aren't pushed yet; all of
// them, one file's, or those matching a search).

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// Reorders commits listed newest first so that within each run of commits made in the same second,
// a commit comes before its parents (keeping the order otherwise). History would otherwise show a
// merge below the commits it merged.
void order_children_first(git_repository *p_repo, LocalVector<git_oid> &r_oids) {
	LocalVector<int64_t> times;
	for (const git_oid &oid : r_oids) {
		CommitPtr commit;
		times.push_back(git_commit_lookup(commit.out(), p_repo, &oid) == 0 ? (int64_t)git_commit_time(commit) : 0);
	}
	uint32_t start = 0;
	while (start < r_oids.size()) {
		uint32_t end = start + 1;
		while (end < r_oids.size() && times[end] == times[start]) {
			end++;
		}
		if (end - start > 1) {
			// Repeatedly take the first commit that no remaining commit of the run has as a parent.
			LocalVector<git_oid> remaining;
			for (uint32_t i = start; i < end; i++) {
				remaining.push_back(r_oids[i]);
			}
			auto is_parent_of_remaining = [&](const git_oid &p_candidate) {
				for (const git_oid &other : remaining) {
					CommitPtr commit;
					if (git_commit_lookup(commit.out(), p_repo, &other) < 0) {
						continue;
					}
					for (unsigned int p = 0; p < git_commit_parentcount(commit); p++) {
						if (git_oid_equal(git_commit_parent_id(commit, p), &p_candidate)) {
							return true;
						}
					}
				}
				return false;
			};
			uint32_t out = start;
			while (!remaining.is_empty()) {
				uint32_t pick = 0;
				while (pick < remaining.size() - 1 && is_parent_of_remaining(remaining[pick])) {
					pick++;
				}
				r_oids[out++] = remaining[pick];
				remaining.remove_at(pick);
			}
		}
		start = end;
	}
}

// p_path's blob id in p_commit's tree; false where it has none.
bool path_id(git_commit *p_commit, const String &p_path, git_oid &r_id) {
	TreePtr tree;
	TreeEntryPtr entry;
	if (git_commit_tree(tree.out(), p_commit) < 0 || git_tree_entry_bypath(entry.out(), tree, p_path.utf8().get_data()) < 0) {
		git_error_clear();
		return false;
	}
	r_id = *git_tree_entry_id(entry);
	return true;
}

// Whether p_commit changed p_path, the way git log decides it: compared with its parent, and for a
// merge only if it differs from every parent (a merge that took one side's version didn't change
// anything on that side).
bool touches_path(git_commit *p_commit, const String &p_path) {
	git_oid id;
	const bool has = path_id(p_commit, p_path, id);
	const unsigned int parents = git_commit_parentcount(p_commit);
	if (parents == 0) {
		return has;
	}
	for (unsigned int p = 0; p < parents; p++) {
		CommitPtr parent;
		if (git_commit_parent(parent.out(), p_commit, p) < 0) {
			continue;
		}
		git_oid parent_id;
		const bool parent_has = path_id(parent, p_path, parent_id);
		if (has == parent_has && (!has || git_oid_equal(&id, &parent_id))) {
			return false;
		}
	}
	return true;
}

// The name p_path had before p_commit, if p_commit renamed it (like git log --follow); "" if not.
String renamed_from(git_repository *p_repo, git_commit *p_commit, const String &p_path) {
	CommitPtr parent;
	TreePtr old_tree, new_tree;
	DiffPtr diff;
	if (git_commit_parentcount(p_commit) == 0 || git_commit_parent(parent.out(), p_commit, 0) < 0 || git_commit_tree(old_tree.out(), parent) < 0 || git_commit_tree(new_tree.out(), p_commit) < 0 || git_diff_tree_to_tree(diff.out(), p_repo, old_tree, new_tree, nullptr) < 0) {
		git_error_clear();
		return String();
	}
	git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
	find.flags = GIT_DIFF_FIND_RENAMES;
	git_diff_find_similar(diff, &find);
	for (size_t i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		if (delta->status == GIT_DELTA_RENAMED && String::utf8(delta->new_file.path) == p_path) {
			return String::utf8(delta->old_file.path);
		}
	}
	return String();
}

Dictionary commit_item(git_commit *p_commit, bool p_unpushed) {
	const String hash = String(git_oid_tostr_s(git_commit_id(p_commit)));
	const git_signature *author = git_commit_author(p_commit);
	const char *summary = git_commit_summary(p_commit);
	const char *message = git_commit_message(p_commit);
	Dictionary item;
	item["id"] = hash.substr(0, 7);
	item["hash"] = hash;
	item["summary"] = summary ? String::utf8(summary) : String();
	item["message"] = message ? String::utf8(message).strip_edges() : String();
	item["author"] = author ? String::utf8(author->name) : String();
	item["time"] = (int64_t)git_commit_time(p_commit);
	item["unpushed"] = p_unpushed;
	item["merge"] = git_commit_parentcount(p_commit) > 1;
	return item;
}

} // namespace

// Returns up to p_max_count commits reachable from HEAD, newest first:
// [{ "id": String (short hash), "hash": String, "summary": String, "message": String,
//    "author": String, "time": int (unix), "unpushed": bool, "merge": bool }, ...]
// "unpushed" means the commit isn't on any remote-tracking branch yet.
// p_path: only commits that changed that file, following it back through renames (like
// git log --follow); each then has "path", the file's name in that commit. p_query: only commits
// whose message or author contains it (any case), or whose hash starts with it. Both look through
// the whole history, not just the latest commits.
Array GitRepository::get_commits(int p_max_count, const String &p_path, const String &p_query) const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	// The list only changes when HEAD or a branch moves (commit, pull, fetch, switch), so it's
	// reused while every reference points where it did. Listing them takes about a millisecond.
	String key;
	for (const char *glob : { "refs/heads/*", "refs/remotes/*" }) {
		ReferenceIteratorPtr refs;
		ReferencePtr ref;
		if (git_reference_iterator_glob_new(refs.out(), repo, glob) == 0) {
			while (git_reference_next(ref.out(), refs) == 0) {
				const git_oid *target = git_reference_target(ref); // Null for origin/HEAD, an alias.
				if (target) {
					key += vformat("|%s=%s", git_reference_name(ref), git_oid_tostr_s(target));
				}
			}
		}
	}
	ObjectPtr head;
	if (git_revparse_single(head.out(), repo, "HEAD") == 0) {
		key += vformat("|HEAD=%s", git_oid_tostr_s(git_object_id(head)));
	}
	if (key != commits_key) {
		commits_key = key;
		commits_cache.clear();
	}
	const String query = p_query.strip_edges().to_lower();
	const String request = vformat("%d|%s|%s", p_max_count, p_path, query);
	if (commits_cache.has(request)) {
		return Array(commits_cache[request]).duplicate(true);
	}

	HashSet<String> unpushed;
	if (!get_remotes().is_empty()) {
		RevwalkPtr walk;
		if (git_revwalk_new(walk.out(), repo) == 0 && git_revwalk_push_head(walk) == 0) {
			git_revwalk_hide_glob(walk, "refs/remotes/*");
			git_oid oid;
			// Capped so a huge never-pushed branch can't stall the panel.
			while (unpushed.size() < 1000 && git_revwalk_next(&oid, walk) == 0) {
				unpushed.insert(String(git_oid_tostr_s(&oid)));
			}
		}
	}

	RevwalkPtr walk;
	if (git_revwalk_new(walk.out(), repo) < 0) {
		return result;
	}
	// Newest first. Not GIT_SORT_TOPOLOGICAL: it reads the whole history before returning the first
	// commit (150 ms per refresh in Godot's own repository). Time alone leaves commits made in the
	// same second (scripts, rebases) in any order, which order_children_first fixes.
	// A file's history follows its renames, which needs every commit before its parents (else it
	// can look for the new name in an older commit made in the same second). It reads the whole
	// history anyway.
	git_revwalk_sorting(walk, p_path.is_empty() ? GIT_SORT_TIME : GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
	if (git_revwalk_push_head(walk) < 0) {
		// No commits yet.
		return result;
	}

	LocalVector<git_oid> oids;
	HashMap<String, String> paths; // Hash -> the file's name in that commit (with p_path).
	String path = p_path;
	git_oid next;
	while ((int)oids.size() < p_max_count && git_revwalk_next(&next, walk) == 0) {
		if (p_path.is_empty() && query.is_empty()) {
			oids.push_back(next);
			continue;
		}
		CommitPtr commit;
		if (git_commit_lookup(commit.out(), repo, &next) < 0) {
			continue;
		}
		if (!query.is_empty()) {
			const String hash = String(git_oid_tostr_s(&next));
			const git_signature *author = git_commit_author(commit);
			const char *message = git_commit_message(commit);
			const bool found = hash.begins_with(query) || (message && String::utf8(message).to_lower().contains(query)) || (author && String::utf8(author->name).to_lower().contains(query));
			if (!found) {
				continue;
			}
		}
		if (!path.is_empty()) {
			if (!touches_path(commit, path)) {
				continue;
			}
			paths.insert(String(git_oid_tostr_s(&next)), path);
			// Where the file appeared under this name, it may have been renamed from another one:
			// older commits have it under that.
			git_oid id;
			CommitPtr parent;
			if (git_commit_parentcount(commit) > 0 && git_commit_parent(parent.out(), commit, 0) == 0 && !path_id(parent, path, id)) {
				const String old_path = renamed_from(repo, commit, path);
				if (!old_path.is_empty()) {
					path = old_path;
				}
			}
		}
		oids.push_back(next);
	}
	order_children_first(repo, oids);

	for (const git_oid &oid : oids) {
		CommitPtr commit;
		if (git_commit_lookup(commit.out(), repo, &oid) < 0) {
			continue;
		}
		const String hash = String(git_oid_tostr_s(&oid));
		Dictionary item = commit_item(commit, unpushed.has(hash));
		if (!p_path.is_empty()) {
			item["path"] = paths.has(hash) ? paths[hash] : p_path;
		}
		result.push_back(item);
	}
	if (commits_cache.size() > 8) {
		commits_cache.clear(); // A handful of searches and files at most, not a growing pile.
	}
	commits_cache[request] = result.duplicate(true);
	return result;
}

// One commit, as get_commits lists it ({} if p_revision names none). For HEAD's, what Amend and
// Undo Last Commit need to know, whatever History is showing.
Dictionary GitRepository::get_commit(const String &p_revision) const {
	ERR_FAIL_NULL_V_MSG(repo, Dictionary(), "Repository is not open.");
	ObjectPtr object;
	CommitPtr commit;
	if (git_revparse_single(object.out(), repo, vformat("%s^{commit}", p_revision).utf8().get_data()) < 0 || git_commit_lookup(commit.out(), repo, git_object_id(object)) < 0) {
		git_error_clear();
		return Dictionary();
	}
	LocalVector<git_oid> remote_tips;
	BranchIteratorPtr it;
	if (git_branch_iterator_new(it.out(), repo, GIT_BRANCH_REMOTE) == 0) {
		ReferencePtr ref;
		git_branch_t type;
		while (git_branch_next(ref.out(), &type, it) == 0) {
			if (git_reference_type(ref) == GIT_REFERENCE_DIRECT) {
				remote_tips.push_back(*git_reference_target(ref));
			}
		}
	}
	const bool pushed = !remote_tips.is_empty() && git_graph_reachable_from_any(repo, git_commit_id(commit), remote_tips.ptr(), remote_tips.size()) == 1;
	return commit_item(commit, !get_remotes().is_empty() && !pushed);
}
