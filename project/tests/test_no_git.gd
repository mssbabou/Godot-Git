extends "res://tests/test_case.gd"
## A machine without git. Most of the panel runs on libgit2 and keeps working; what needs git
## itself (hooks, signing, SSH, LFS, logins) must refuse with a message saying so, never skip a
## hook or fail with an unrelated error. The repositories are built with the real git first, then
## the extension is pointed at a git program that doesn't exist.

const MISSING := "git-not-installed-for-tests"


func run() -> void:
	var shared := make_shared("no-git")
	var hooked := make_shared("no-git-hook")
	var lfs := make_repo("no-git-lfs")
	write(lfs.path_join(".gitattributes"), "*.png filter=lfs diff=lfs merge=lfs -text\n")
	write(lfs.path_join("art.png"), "not really a png\n")
	_install_hook(hooked.mine, "pre-commit", "exit 0")
	_install_hook(hooked.mine, "pre-push", "exit 0")
	git(shared.mine, ["remote", "add", "github-ssh", "git@github.com:godot-git/does-not-exist.git"])
	git(shared.mine, ["remote", "add", "github-https", "https://github.com/godot-git/does-not-exist.git"])

	check("git found normally", GitRepository.is_git_installed())
	GitRepository.set_git_program(MISSING)
	check("missing git noticed", not GitRepository.is_git_installed())

	_libgit2_still_works(shared)
	_needs(shared, hooked, lfs)
	_hooks_refuse(hooked)
	_ssh_refuses(shared)
	_lfs_refuses(lfs)

	GitRepository.set_git_program("git")
	check("git found again once it's back", GitRepository.is_git_installed())


func _install_hook(repo: String, name: String, body: String) -> void:
	var path := repo.path_join(".git/hooks").path_join(name)
	write(path, "#!/bin/sh\n" + body + "\n")
	if OS.get_name() != "Windows":
		OS.execute("chmod", ["+x", path])


func _says_install_git(label: String, message: String, reason: String) -> void:
	check(label, message.contains(reason) and message.contains("git-scm.com"), message)


# No hooks, a local remote: commit, push, fetch and pull all work on libgit2 alone.
func _libgit2_still_works(shared: Dictionary) -> void:
	var r := open(shared.mine)
	write(shared.mine.path_join("a.txt"), "a\n")
	check("stage without git", r.stage("a.txt") == OK, GitRepository.get_last_error())
	check("commit without git", r.commit("Add a") == OK, GitRepository.get_last_error())
	check("push without git", r.push() == OK, GitRepository.get_last_error())
	check("pushed for real", git(shared.remote, ["log", "-1", "--format=%s", "main"]) == "Add a")
	teammate_pushes(shared, "x.txt", "x2\n")
	check("pull without git", r.pull() == OK, GitRepository.get_last_error())
	check("pulled for real", read(shared.mine.path_join("x.txt")) == "x2\n")


func _needs(shared: Dictionary, hooked: Dictionary, lfs: String) -> void:
	var plain: Dictionary = open(shared.mine).get_git_needs()
	check("plain: commit doesn't need git", plain.commit == "", plain)
	check("plain: no LFS, no pre-push", not plain.lfs and not plain.pre_push, plain)
	check("SSH remote listed", Array(plain.ssh_remotes) == ["github-ssh"], plain)
	check("HTTPS remote listed", Array(plain.https_remotes) == ["github-https"], plain)
	var with_hooks: Dictionary = open(hooked.mine).get_git_needs()
	check("hooked: commit needs git, and says why", with_hooks.commit.contains("pre-commit"), with_hooks)
	check("hooked: pre-push", with_hooks.pre_push, with_hooks)
	check("LFS noticed", open(lfs).get_git_needs().lfs)


func _hooks_refuse(hooked: Dictionary) -> void:
	var r := open(hooked.mine)
	var head := git(hooked.mine, ["rev-parse", "HEAD"])
	write(hooked.mine.path_join("b.txt"), "b\n")
	r.stage("b.txt")
	check("commit refused: the pre-commit hook can't run", r.commit("Add b") != OK)
	_says_install_git("says it's the hook, and to install git", GitRepository.get_last_error(), "pre-commit")
	check("no commit made", git(hooked.mine, ["rev-parse", "HEAD"]) == head)
	check("still staged", r.get_status().size() == 1 and r.get_status()[0].index == "new", r.get_status())
	check("amend refused too", r.amend("Changed") != OK)
	check("amend changed nothing", git(hooked.mine, ["rev-parse", "HEAD"]) == head)

	# The pre-push hook, with a commit made by git itself.
	commit_all(hooked.mine, "Add b")
	var remote_head := git(hooked.remote, ["rev-parse", "main"])
	check("push refused: the pre-push hook can't run", r.push() != OK)
	_says_install_git("says it's the pre-push hook", GitRepository.get_last_error(), "pre-push")
	check("nothing pushed", git(hooked.remote, ["rev-parse", "main"]) == remote_head)


func _ssh_refuses(shared: Dictionary) -> void:
	var r := open(shared.mine)
	git(shared.mine, ["remote", "set-url", "origin", "git@github.com:godot-git/does-not-exist.git"])
	var started := Time.get_ticks_msec()
	check("SSH fetch refused", r.fetch() != OK)
	_says_install_git("says SSH needs git", GitRepository.get_last_error(), "SSH")
	check("refused at once", Time.get_ticks_msec() - started < 2000, Time.get_ticks_msec() - started)
	write(shared.mine.path_join("c.txt"), "c\n")
	r.stage("c.txt")
	r.commit("Add c")
	check("SSH push refused", r.push() != OK)
	_says_install_git("says SSH needs git (push)", GitRepository.get_last_error(), "SSH")
	git(shared.mine, ["remote", "set-url", "origin", shared.remote])


func _lfs_refuses(lfs: String) -> void:
	var r := open(lfs)
	check("staging an LFS file refused", r.stage("art.png") != OK)
	_says_install_git("says LFS needs git", GitRepository.get_last_error(), "Git LFS")
	check("nothing staged", git(lfs, ["diff", "--cached", "--name-only"]) == "")
