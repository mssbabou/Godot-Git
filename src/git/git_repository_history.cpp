// GitRepository: History (the commits, newest first, with which of them aren't pushed yet).

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

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

} // namespace

// Returns up to p_max_count commits reachable from HEAD, newest first:
// [{ "id": String (short hash), "hash": String, "summary": String, "message": String,
//    "author": String, "time": int (unix), "unpushed": bool, "merge": bool }, ...]
// "unpushed" means the commit isn't on any remote-tracking branch yet.
Array GitRepository::get_commits(int p_max_count) const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	// The list only changes when HEAD or a branch moves (commit, pull, fetch, switch), so it's
	// reused while every reference points where it did. Listing them takes about a millisecond.
	String key = itos(p_max_count);
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
	if (key == commits_key) {
		return commits_cache.duplicate(true);
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
	git_revwalk_sorting(walk, GIT_SORT_TIME);
	if (git_revwalk_push_head(walk) < 0) {
		// No commits yet.
		return result;
	}

	LocalVector<git_oid> oids;
	git_oid next;
	while ((int)oids.size() < p_max_count && git_revwalk_next(&next, walk) == 0) {
		oids.push_back(next);
	}
	order_children_first(repo, oids);

	for (const git_oid &oid : oids) {
		CommitPtr commit;
		if (git_commit_lookup(commit.out(), repo, &oid) < 0) {
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
		item["merge"] = git_commit_parentcount(commit) > 1;
		result.push_back(item);
	}
	commits_key = key;
	commits_cache = result.duplicate(true);
	return result;
}
