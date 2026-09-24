extends "res://tests/test_case.gd"
## Hooks and commit signing. libgit2 runs no hooks and never signs, so commits, amends, pull's
## merge commits and pushes go through git itself when a hook or signing applies. These check
## that hooks really run (and can stop things) and that commits really get signed.


func run() -> void:
	_no_hooks_no_git()
	_pre_commit_stops_commit()
	_hooks_run_on_commit_and_amend()
	_merge_commit_hooks()
	_pre_push()
	_signing()


func _install_hook(repo: String, name: String, body: String) -> void:
	var path := repo.path_join(".git/hooks").path_join(name)
	write(path, "#!/bin/sh\n" + body + "\n")
	if OS.get_name() != "Windows":
		OS.execute("chmod", ["+x", path])


func _no_hooks_no_git() -> void:
	var shared := make_shared("plain")
	var r := open(shared.mine)
	check("no hooks: commits stay in libgit2", not r.commit_runs_git(false))
	_install_hook(shared.mine, "commit-msg", "exit 0")
	check("with a hook: commits go through git", r.commit_runs_git(false))


func _pre_commit_stops_commit() -> void:
	var shared := make_shared("pre-commit")
	_install_hook(shared.mine, "pre-commit", "echo \"lint: player.gd has 2 errors\" >&2\nexit 1")
	var head := git(shared.mine, ["rev-parse", "HEAD"])
	write(shared.mine.path_join("player.gd"), "extends Node\n")
	var r := open(shared.mine)
	r.stage("player.gd")
	check("pre-commit hook stops the commit", r.commit("Add player") != OK)
	check("the hook's own words are shown", GitRepository.get_last_error().contains("player.gd has 2 errors"), GitRepository.get_last_error())
	check("no commit made", git(shared.mine, ["rev-parse", "HEAD"]) == head)
	var status := r.get_status()
	check("file still staged", status.size() == 1 and status[0].index == "new", status)


func _hooks_run_on_commit_and_amend() -> void:
	var shared := make_shared("commit-msg")
	_install_hook(shared.mine, "pre-commit", "echo ran >> .git/pre-commit-ran")
	_install_hook(shared.mine, "commit-msg", "echo \"\" >> \"$1\"\necho \"Checked-by: hook\" >> \"$1\"")
	write(shared.mine.path_join("a.txt"), "a\n")
	var r := open(shared.mine)
	r.stage("a.txt")
	check("commit with hooks succeeds", r.commit("Add a") == OK, GitRepository.get_last_error())
	check("pre-commit hook ran", exists(shared.mine.path_join(".git/pre-commit-ran")))
	check("commit-msg hook changed the message", git(shared.mine, ["log", "-1", "--format=%B"]).contains("Checked-by: hook"), git(shared.mine, ["log", "-1", "--format=%B"]))
	check("status clean afterwards", r.get_status().is_empty(), r.get_status())

	check("amend with hooks succeeds", r.amend("Add a, properly") == OK, GitRepository.get_last_error())
	check("amended message went through the hook", git(shared.mine, ["log", "-1", "--format=%B"]).contains("Add a, properly") and git(shared.mine, ["log", "-1", "--format=%B"]).contains("Checked-by: hook"))
	check("still one new commit", git(shared.mine, ["rev-list", "--count", "origin/main..HEAD"]) == "1")


func _merge_commit_hooks() -> void:
	# A commit-msg hook that refuses merge commits: the pull must change nothing.
	var shared := make_shared("merge")
	teammate_pushes(shared, "x.txt", "x2\n")
	write(shared.mine.path_join("y.txt"), "y2\n")
	commit_all(shared.mine, "My change")
	write(shared.mine.path_join("z.txt"), "my uncommitted edit\n")
	_install_hook(shared.mine, "commit-msg", "if grep -q '^Merge' \"$1\"; then echo 'no merge commits here' >&2; exit 1; fi")
	var head := git(shared.mine, ["rev-parse", "HEAD"])
	var r := open(shared.mine)
	check("pull refused when the merge commit is", r.pull() != OK)
	check("says nothing was pulled, and why", GitRepository.get_last_error().contains("Nothing was pulled") and GitRepository.get_last_error().contains("no merge commits here"), GitRepository.get_last_error())
	check("HEAD unchanged", git(shared.mine, ["rev-parse", "HEAD"]) == head)
	check("teammate's change not applied", read(shared.mine.path_join("x.txt")) == "x1\n")
	check("uncommitted edit kept", read(shared.mine.path_join("z.txt")) == "my uncommitted edit\n")
	check("no merge in progress", not exists(shared.mine.path_join(".git/MERGE_HEAD")))

	# Allowed: the merge commit goes through the hook.
	_install_hook(shared.mine, "commit-msg", "echo \"\" >> \"$1\"\necho \"Checked-by: hook\" >> \"$1\"")
	check("pull merges with the hook", r.pull() == OK and r.get_pull_result().merged, GitRepository.get_last_error())
	check("merge commit has two parents", git(shared.mine, ["rev-list", "--parents", "-1", "HEAD"]).split(" ").size() == 3)
	check("merge commit went through the hook", git(shared.mine, ["log", "-1", "--format=%B"]).contains("Checked-by: hook"))
	check("uncommitted edit still there", read(shared.mine.path_join("z.txt")) == "my uncommitted edit\n")


func _pre_push() -> void:
	var shared := make_shared("pre-push")
	write(shared.mine.path_join("a.txt"), "a\n")
	commit_all(shared.mine, "Add a")
	_install_hook(shared.mine, "pre-push", "echo \"tests failed: 3 of 40\" >&2\nexit 1")
	var remote_head := git(shared.remote, ["rev-parse", "main"])
	var r := open(shared.mine)
	check("pre-push hook stops the push", r.push() != OK)
	check("the hook's words are shown", GitRepository.get_last_error().contains("tests failed: 3 of 40"), GitRepository.get_last_error())
	check("remote unchanged", git(shared.remote, ["rev-parse", "main"]) == remote_head)

	_install_hook(shared.mine, "pre-push", "echo ran >> .git/pre-push-ran")
	check("push with a passing hook", r.push() == OK, GitRepository.get_last_error())
	check("pre-push hook ran", exists(shared.mine.path_join(".git/pre-push-ran")))
	check("remote got the commit", git(shared.remote, ["rev-parse", "main"]) == git(shared.mine, ["rev-parse", "HEAD"]))

	teammate_pushes(shared, "x.txt", "x2\n")
	write(shared.mine.path_join("b.txt"), "b\n")
	commit_all(shared.mine, "Add b")
	check("behind the remote: refused", r.push() != OK)
	check("says to pull first", GitRepository.get_last_error().contains("Pull first"), GitRepository.get_last_error())


func _signing() -> void:
	var key := dir.path_join("signing-key")
	var out := []
	var code: int
	if OS.get_name() == "Windows":
		# Godot drops empty arguments on Windows (CLAUDE.md gotcha 25), so -N "" goes through cmd.
		code = OS.execute("cmd", ["/c", "ssh-keygen -q -t ed25519 -N \"\" -C test -f \"%s\"" % key], out, true)
	else:
		code = OS.execute("ssh-keygen", ["-q", "-t", "ed25519", "-N", "", "-C", "test", "-f", key], out, true)
	if not exists(key):
		print("  (signing skipped; ssh-keygen failed with code %d: %s)" % [code, "".join(out).strip_edges()])
		return
	var shared := make_shared("signing")
	for setting in [["gpg.format", "ssh"], ["user.signingkey", key], ["commit.gpgsign", "true"]]:
		git(shared.mine, ["config", setting[0], setting[1]])
	var r := open(shared.mine)
	check("signing: commits go through git", r.commit_runs_git(false))

	write(shared.mine.path_join("a.txt"), "a\n")
	r.stage("a.txt")
	check("signed commit succeeds", r.commit("Signed work") == OK, GitRepository.get_last_error())
	check("commit is signed", git(shared.mine, ["cat-file", "-p", "HEAD"]).contains("BEGIN SSH SIGNATURE"))
	check("amend succeeds", r.amend("Signed work, amended") == OK, GitRepository.get_last_error())
	check("amended commit is signed", git(shared.mine, ["cat-file", "-p", "HEAD"]).contains("BEGIN SSH SIGNATURE"))

	teammate_pushes(shared, "x.txt", "x2\n")
	check("pull merges", r.pull() == OK and r.get_pull_result().merged, GitRepository.get_last_error())
	check("merge commit is signed", git(shared.mine, ["cat-file", "-p", "HEAD"]).contains("BEGIN SSH SIGNATURE"))
