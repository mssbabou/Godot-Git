extends "res://tests/test_case.gd"
## Setting a repository up from the panel: initializing one, adding its remote, and the name and
## email commits need. Runs against an empty global git config of its own (for libgit2 and for
## the git CLI), so "git doesn't know who you are" can be tested on any machine.

var _home := ""


func run() -> void:
	_home = dir.path_join("config-home")
	DirAccess.make_dir_recursive_absolute(_home)
	GitRepository.set_config_home(_home)
	OS.set_environment("GIT_CONFIG_GLOBAL", _home.path_join(".gitconfig"))
	OS.set_environment("GIT_CONFIG_NOSYSTEM", "1")

	_init_project_folder()
	_init_parent_folder()
	_init_default_branch()
	_identity()
	_add_remote_and_publish()
	_pull_without_identity()

	GitRepository.set_config_home("")
	OS.unset_environment("GIT_CONFIG_GLOBAL")
	OS.unset_environment("GIT_CONFIG_NOSYSTEM")


func _project(path: String) -> void:
	write(path.path_join("project.godot"), "config_version=5\n")


func _init_project_folder() -> void:
	var project := dir.path_join("fresh")
	_project(project)
	check("init", GitRepository.init_repository(project, project) == OK, GitRepository.get_last_error())
	check("git sees a repository", git(project, ["rev-parse", "--show-toplevel"]).ends_with("fresh"), git(project, ["rev-parse", "--show-toplevel"]))
	check("first branch is main", git(project, ["symbolic-ref", "HEAD"]) == "refs/heads/main")
	check("Godot's .gitignore added", read(project.path_join(".gitignore")) == "# Godot 4+ specific ignores\n.godot/\n/android/\n")
	check("Godot's .gitattributes added", read(project.path_join(".gitattributes")).contains("* text=auto eol=lf"))
	check("nothing committed", git(project, ["rev-list", "--all"]) == "")
	var r := open(project)
	check("opens", r.is_open())
	var paths := r.get_status().map(func(s: Dictionary) -> String: return s.path)
	check("files show as changes", paths.has("project.godot") and paths.has(".gitignore"), paths)
	check("init again refused", GitRepository.init_repository(project, project) != OK)
	check("says it's already one", GitRepository.get_last_error().contains("already"), GitRepository.get_last_error())

	# A project that already has Godot's files (the project manager wrote them) keeps its own.
	var existing := dir.path_join("existing")
	_project(existing)
	write(existing.path_join(".gitignore"), ".godot/\nmy-own-rule/\n")
	GitRepository.init_repository(existing, existing)
	check("existing .gitignore kept", read(existing.path_join(".gitignore")) == ".godot/\nmy-own-rule/\n")


# MegaGame/ as the repository, MegaGame/game/ as the Godot project.
func _init_parent_folder() -> void:
	var mega := dir.path_join("MegaGame")
	var game := mega.path_join("game")
	_project(game)
	write(mega.path_join("docs/design.md"), "# Design\n")
	check("init in the parent folder", GitRepository.init_repository(mega, game) == OK, GitRepository.get_last_error())
	check("repository is the parent", exists(mega.path_join(".git/HEAD")) and not exists(game.path_join(".git")))
	check(".gitignore goes in the project folder", exists(game.path_join(".gitignore")) and not exists(mega.path_join(".gitignore")))
	var r := open(game)
	check("found from the project folder", r.is_open() and r.get_workdir().trim_suffix("/").ends_with("MegaGame"), r.get_workdir())
	var paths := r.get_status().map(func(s: Dictionary) -> String: return s.path)
	check("files outside the project show", paths.has("docs/design.md") and paths.has("game/project.godot"), paths)
	write(game.path_join(".godot/editor/cache.cfg"), "cache
")
	git(mega, ["add", "-A"])
	var added := git(mega, ["diff", "--cached", "--name-only"])
	check(".godot/ ignored from the parent too", added.contains("game/project.godot") and not added.contains("game/.godot/"), added)


func _init_default_branch() -> void:
	write(_home.path_join(".gitconfig"), "[init]\n\tdefaultBranch = trunk\n")
	var project := dir.path_join("trunk")
	_project(project)
	GitRepository.init_repository(project, project)
	check("init.defaultBranch respected", git(project, ["symbolic-ref", "HEAD"]) == "refs/heads/trunk", git(project, ["symbolic-ref", "HEAD"]))
	DirAccess.remove_absolute(_home.path_join(".gitconfig"))


func _identity() -> void:
	var project := dir.path_join("who")
	_project(project)
	GitRepository.init_repository(project, project)
	var r := open(project)
	var identity := r.get_identity()
	check("no identity yet", identity.name == "" and identity.email == "", identity)
	r.stage("project.godot")
	check("commit refused without a name and email", r.commit("First") != OK)
	check("says so plainly", GitRepository.get_last_error().contains("name and email"), GitRepository.get_last_error())
	check("nothing committed", git(project, ["rev-list", "--all"]) == "")
	check("empty name refused", r.set_identity("  ", "a@b.c", true) != OK)

	# Only for this repository.
	check("set for this repository", r.set_identity("Ada Local", "ada@local.test", false) == OK, GitRepository.get_last_error())
	check("git sees it locally", git(project, ["config", "--local", "user.name"]) == "Ada Local")
	check("global untouched", not exists(_home.path_join(".gitconfig")))
	check("first commit", r.commit("First") == OK, GitRepository.get_last_error())
	check("made by that name", git(project, ["log", "-1", "--format=%an <%ae>"]) == "Ada Local <ada@local.test>")
	check("shows in history", r.get_commits(10).size() == 1)

	# For every repository: creates the global config file git itself reads.
	var other := dir.path_join("who-global")
	_project(other)
	GitRepository.init_repository(other, other)
	var r2 := open(other)
	check("set globally", r2.set_identity("Ada Global", "ada@global.test", true) == OK, GitRepository.get_last_error())
	check("git CLI sees it globally", git(other, ["config", "--global", "user.email"]) == "ada@global.test")
	check("not written locally", git(other, ["config", "--local", "user.name"]) == "")
	check("read back", open(other).get_identity().name == "Ada Global")
	r2.stage("project.godot")
	check("commit with the global identity", r2.commit("First") == OK, GitRepository.get_last_error())
	DirAccess.remove_absolute(_home.path_join(".gitconfig"))


func _add_remote_and_publish() -> void:
	var project := dir.path_join("publish")
	_project(project)
	GitRepository.init_repository(project, project)
	var remote := dir.path_join("publish-remote.git")
	OS.execute("git", ["init", "-q", "--bare", remote])
	var r := open(project)
	r.set_identity("Ada", "ada@example.com", false)
	r.stage("project.godot")
	r.commit("First")
	check("empty URL refused", r.add_remote("origin", "  ") != OK)
	check("add remote", r.add_remote("origin", remote) == OK, GitRepository.get_last_error())
	check("git sees it", git(project, ["remote", "get-url", "origin"]) == remote)
	check("adding it twice refused", r.add_remote("origin", remote) != OK)
	check("says it exists", GitRepository.get_last_error().contains("already"), GitRepository.get_last_error())
	check("sync row would show", r.get_sync_status().has_remotes)
	check("publish", r.push() == OK, GitRepository.get_last_error())
	check("remote has the commit", git(remote, ["log", "-1", "--format=%s", "main"]) == "First")
	check("branch now tracks it", r.get_sync_status().upstream == "origin/main", r.get_sync_status())


# A pull that has to merge makes a commit, so it needs a name and email too; without, it must
# refuse before touching anything.
func _pull_without_identity() -> void:
	var shared := make_shared("pull-who")
	teammate_pushes(shared, "x.txt", "x2\n")
	write(shared.mine.path_join("y.txt"), "y2\n")
	commit_all(shared.mine, "My change")
	git(shared.mine, ["config", "--unset", "user.name"])
	git(shared.mine, ["config", "--unset", "user.email"])
	var head := git(shared.mine, ["rev-parse", "HEAD"])
	var r := open(shared.mine)
	check("merging pull refused", r.pull() != OK)
	check("says it needs a name and email", GitRepository.get_last_error().contains("name and email") and GitRepository.get_last_error().contains("Nothing was pulled"), GitRepository.get_last_error())
	check("HEAD unchanged", git(shared.mine, ["rev-parse", "HEAD"]) == head)
	check("teammate's change not applied", read(shared.mine.path_join("x.txt")) == "x1\n")
