// GitRepository: ignoring files (which .gitignore a rule goes into, which files a rule would hide,
// with git's own matching, before anything is written, and adding it) and tracking files with Git
// LFS: what the Ignore dialog and the large-file question at commit offer.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/file_access.hpp>

#include "git/git_lfs.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// A gitignore line written in the .gitignore of p_dir (repository-relative, "" for the root), as
// a rule relative to the repository's root, which is what libgit2's internal rules are. Lines with
// a slash are anchored to their file's folder; lines without one match in any folder below it.
String root_relative_rule(const String &p_dir, const String &p_line) {
	if (p_dir.is_empty()) {
		return p_line;
	}
	const String body = p_line.trim_suffix("/");
	if (p_line.begins_with("/") || body.contains("/")) {
		return vformat("/%s/%s", p_dir, p_line.trim_prefix("/"));
	}
	return vformat("/%s/**/%s", p_dir, p_line);
}

} // namespace

// The .gitignore a rule for p_dir (repository-relative folder, "" for the root) goes into: the
// nearest existing one at p_dir or above, else p_dir's own (created when the rule is added).
// Repository-relative.
String GitRepository::get_ignore_file(const String &p_dir) const {
	ERR_FAIL_NULL_V_MSG(repo, String(), "Repository is not open.");
	const String workdir = get_workdir();
	String dir = p_dir.trim_suffix("/");
	while (true) {
		const String file = dir.is_empty() ? String(".gitignore") : dir.path_join(".gitignore");
		if (FileAccess::file_exists(workdir.path_join(file))) {
			return file;
		}
		if (dir.is_empty()) {
			break;
		}
		dir = dir.contains("/") ? dir.get_base_dir() : String();
	}
	const String own = p_dir.trim_suffix("/");
	return own.is_empty() ? String(".gitignore") : own.path_join(".gitignore");
}

// Of p_paths (repository-relative), the ones that p_lines would ignore once added to p_ignore_file.
// Matched by libgit2 as rules of its own, added for the question and cleared after it, so the
// answer is git's matching (folders, globs, rules already in place), not a guess of ours.
PackedStringArray GitRepository::get_paths_ignored_by(const String &p_ignore_file, const PackedStringArray &p_lines, const PackedStringArray &p_paths) {
	PackedStringArray result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	const String dir = p_ignore_file.contains("/") ? p_ignore_file.get_base_dir() : String();
	PackedStringArray rules;
	for (const String &line : p_lines) {
		rules.push_back(root_relative_rule(dir, line));
	}
	PackedStringArray already;
	for (const String &path : p_paths) {
		int ignored = 0;
		git_ignore_path_is_ignored(&ignored, repo, path.utf8().get_data());
		if (ignored) {
			already.push_back(path);
		}
	}
	if (git_ignore_add_rule(repo, String("\n").join(rules).utf8().get_data()) < 0) {
		ERR_FAIL_V_MSG(result, "Couldn't check the ignore rule: " + get_last_error());
	}
	for (const String &path : p_paths) {
		int ignored = 0;
		if (git_ignore_path_is_ignored(&ignored, repo, path.utf8().get_data()) == 0 && ignored && !already.has(path)) {
			result.push_back(path);
		}
	}
	git_ignore_clear_internal_rules(repo);
	return result;
}

// Appends p_lines to p_ignore_file (repository-relative; created if missing), each on its own
// line, leaving what's there untouched.
Error GitRepository::add_ignore_lines(const String &p_ignore_file, const PackedStringArray &p_lines) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	const String path = get_workdir().path_join(p_ignore_file);
	String text;
	if (FileAccess::file_exists(path)) {
		text = FileAccess::get_file_as_string(path);
	}
	// Keep the file's line endings.
	const String newline = text.contains("\r\n") ? String("\r\n") : String("\n");
	if (!text.is_empty() && !text.ends_with("\n")) {
		text += newline;
	}
	for (const String &line : p_lines) {
		text += line + newline;
	}
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		return fail(vformat("Couldn't write %s.", p_ignore_file));
	}
	file->store_string(text);
	return OK;
}

// Stores files matching p_patterns ("*.psd") with Git LFS from now on, as `git lfs track` does:
// a line each in the repository's root .gitattributes (the one repo_uses_lfs reads), staged, then
// p_paths staged again, which the LFS filter turns into pointers. Files already committed keep
// their old versions in the history; only new versions go to LFS.
Error GitRepository::track_with_lfs(const PackedStringArray &p_patterns, const PackedStringArray &p_paths) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (!lfs_installed()) {
		return fail("Git LFS isn't installed, so files can't be stored with it. Install it from git-lfs.com, then restart the editor.");
	}
	const String path = get_workdir().path_join(".gitattributes");
	String text = FileAccess::file_exists(path) ? FileAccess::get_file_as_string(path) : String();
	const String newline = text.contains("\r\n") ? String("\r\n") : String("\n");
	if (!text.is_empty() && !text.ends_with("\n")) {
		text += newline;
	}
	for (const String &pattern : p_patterns) {
		const String line = vformat("%s filter=lfs diff=lfs merge=lfs -text", pattern);
		if (!text.contains(line)) {
			text += line + newline;
		}
	}
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		return fail("Couldn't write .gitattributes.");
	}
	file->store_string(text);
	file.unref();
	Error err = stage(".gitattributes");
	for (int i = 0; err == OK && i < p_paths.size(); i++) {
		err = stage(p_paths[i]);
	}
	return err;
}
