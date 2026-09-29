// GitRepository: setting a repository up (git init, the first remote, your name and email).

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

// Adds a remote (e.g. "origin") to push to and pull from. Nothing is contacted yet.
Error GitRepository::add_remote(const String &p_name, const String &p_url) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	const String url = p_url.strip_edges();
	if (url.is_empty()) {
		return fail("Paste the repository's URL first.");
	}
	int valid = 0;
	if (git_remote_name_is_valid(&valid, p_name.utf8().get_data()) < 0 || !valid) {
		return fail(vformat("\"%s\" isn't a valid remote name.", p_name));
	}
	RemotePtr remote;
	const int err = git_remote_create(remote.out(), repo, p_name.utf8().get_data(), url.utf8().get_data());
	if (err == GIT_EEXISTS) {
		return fail(vformat("This repository already has a remote called \"%s\".", p_name));
	}
	return to_error(err);
}

// The name and email commits are made with: { "name", "email" }, "" where git has none.
Dictionary GitRepository::get_identity() const {
	Dictionary result;
	result["name"] = String();
	result["email"] = String();
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	ConfigPtr config;
	if (git_repository_config_snapshot(config.out(), repo) < 0) {
		return result;
	}
	for (const char *key : { "name", "email" }) {
		git_buf value = GIT_BUF_INIT;
		if (git_config_get_string_buf(&value, config, vformat("user.%s", key).utf8().get_data()) == 0) {
			result[key] = buf_to_string(value).strip_edges();
		} else {
			git_buf_dispose(&value);
		}
	}
	return result;
}

// Sets the name and email commits are made with, like `git config [--global] user.name`: for
// every repository (the user's global git config, shared with the git CLI and other tools), or
// only for this one.
Error GitRepository::set_identity(const String &p_name, const String &p_email, bool p_global) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	const String name = p_name.strip_edges();
	const String email = p_email.strip_edges();
	if (name.is_empty() || email.is_empty()) {
		return fail("Fill in both your name and your email.");
	}
	ConfigPtr config;
	int err = 0;
	if (p_global) {
		String path;
		git_buf found = GIT_BUF_INIT;
		if (git_config_find_global(&found) == 0) {
			path = buf_to_string(found);
		} else {
			// No global config file yet: create it where git looks first (~/.gitconfig).
			git_buf_dispose(&found);
			git_buf dirs = GIT_BUF_INIT;
			git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &dirs);
			const String first = buf_to_string(dirs).get_slice(String::chr(GIT_PATH_LIST_SEPARATOR), 0);
			if (first.is_empty()) {
				return fail("Couldn't find your home folder to save your name and email in.");
			}
			path = first.path_join(".gitconfig");
		}
		err = git_config_open_ondisk(config.out(), path.utf8().get_data());
	} else {
		ConfigPtr all;
		err = git_repository_config(all.out(), repo);
		if (err >= 0) {
			err = git_config_open_level(config.out(), all, GIT_CONFIG_LEVEL_LOCAL);
		}
	}
	if (err >= 0) {
		err = git_config_set_string(config, "user.name", name.utf8().get_data());
	}
	if (err >= 0) {
		err = git_config_set_string(config, "user.email", email.utf8().get_data());
	}
	return to_error(err);
}

// Makes the folder p_path a new, empty git repository. Its first branch is named like the git
// CLI would (init.defaultBranch, else "main"). The .gitignore and .gitattributes Godot's project
// manager writes are added to p_project_path when missing (p_path is that folder or one above it).
Error GitRepository::init_repository(const String &p_path, const String &p_project_path) {
	git_error_clear();
	const String path = ProjectSettings::get_singleton()->globalize_path(p_path);
	const String project_path = ProjectSettings::get_singleton()->globalize_path(p_project_path);

	String branch = "main";
	ConfigPtr config;
	git_buf configured = GIT_BUF_INIT;
	if (git_config_open_default(config.out()) == 0 && git_config_get_string_buf(&configured, config, "init.defaultBranch") == 0) {
		branch = buf_to_string(configured);
	} else {
		git_buf_dispose(&configured);
	}
	const CharString branch_utf8 = branch.utf8();

	git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
	options.flags = GIT_REPOSITORY_INIT_NO_REINIT | GIT_REPOSITORY_INIT_MKPATH;
	options.initial_head = branch_utf8.get_data();
	git_repository *created = nullptr;
	const int err = git_repository_init_ext(&created, path.utf8().get_data(), &options);
	git_repository_free(created);
	if (err == GIT_EEXISTS) {
		return fail("That folder is already a git repository.");
	}
	if (err < 0) {
		return to_error(err);
	}

	// Godot's own files, word for word (editor/version_control/editor_vcs_interface.cpp).
	const String ignore = project_path.path_join(".gitignore");
	if (!FileAccess::file_exists(ignore)) {
		Ref<FileAccess> file = FileAccess::open(ignore, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string("# Godot 4+ specific ignores\n.godot/\n/android/\n");
		}
	}
	const String attributes = project_path.path_join(".gitattributes");
	if (!FileAccess::file_exists(attributes)) {
		Ref<FileAccess> file = FileAccess::open(attributes, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string("# Normalize EOL for all files that Git considers text files.\n* text=auto eol=lf\n");
		}
	}
	return OK;
}

// For the tests: read the global and system git config from p_path instead of the user's own
// ("" goes back to normal), so they can see what happens when git has no name and email.
void GitRepository::set_config_home(const String &p_path) {
	const CharString path = p_path.utf8();
	for (git_config_level_t level : { GIT_CONFIG_LEVEL_GLOBAL, GIT_CONFIG_LEVEL_XDG, GIT_CONFIG_LEVEL_SYSTEM, GIT_CONFIG_LEVEL_PROGRAMDATA }) {
		git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, p_path.is_empty() ? nullptr : path.get_data());
	}
}
