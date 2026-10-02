extends "res://tests/test_case.gd"
## Pulling into conflicts on purpose (pull(true), the panel's Start Merge): your uncommitted edit
## comes back as a conflict to resolve, or your commits leave a merge git knows; Finish and Abort
## end either; and what it still refuses. Checked against what git says.


func run() -> void:
	_your_edit_finish()
	_your_edit_abort()
	_your_commits_finish()
	_your_commits_keep_other_edits()
	_staged_refuses()


## You edited x.txt's line and haven't committed; the teammate pushed a change to the same line.
func _edit_against_teammate(name: String) -> Dictionary:
	var s := make_shared(name)
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-mine\n")
	write(s.mine.path_join("z.txt"), "z-mine\n") # Unrelated: must stay as it is throughout.
	return s


func _your_edit_finish() -> void:
	var s := _edit_against_teammate("edit-finish")
	var r := open(s.mine)
	check("edit: refused first, naming the file", r.pull() != OK and Array(r.get_pull_result().conflicts) == ["x.txt"], GitRepository.get_last_error())
	check("edit: start merge", r.pull(true) == OK, GitRepository.get_last_error())
	check("edit: pulled", git(s.mine, ["rev-parse", "HEAD"]) == git(s.mine, ["rev-parse", "origin/main"]))
	check("edit: the conflict is reported", Array(r.get_pull_result().conflicts) == ["x.txt"], r.get_pull_result())
	var op := r.get_operation()
	check("edit: shown as a merge in progress", op.kind == "pull" and Array(op.conflicts) == ["x.txt"], op)
	check("edit: git sees the conflict too", git(s.mine, ["diff", "--name-only", "--diff-filter=U"]) == "x.txt", git(s.mine, ["status", "--porcelain"]))
	var conflict := r.get_conflict("x.txt")
	check("edit: mine is my edit, theirs the pulled line", conflict.mine_label == "your changes" and conflict.theirs_label == "origin/main" and conflict.blocks[0].mine == "x-mine\n" and conflict.blocks[0].theirs == "x-theirs\n", conflict)
	check("edit: commit refuses meanwhile", r.commit("nope") != OK, GitRepository.get_last_error())
	check("edit: resolve", r.resolve_conflict("x.txt", "x-both\n") == OK, GitRepository.get_last_error())
	check("edit: resolved as an unstaged change, like my edit was", git(s.mine, ["status", "--porcelain", "x.txt"]) == "M x.txt", git(s.mine, ["status", "--porcelain"]))
	check("edit: finish", r.continue_operation() == OK, GitRepository.get_last_error())
	check("edit: nothing in progress, no copies left", r.get_operation().kind == "" and not exists(s.mine.path_join(".git/godot-git-pull-state.json")) and not DirAccess.dir_exists_absolute(s.mine.path_join(".git/godot-git-pull")))
	check("edit: the file has the resolution, the unrelated edit stayed", read(s.mine.path_join("x.txt")) == "x-both\n" and read(s.mine.path_join("z.txt")) == "z-mine\n")
	check("edit: no stash anywhere", git(s.mine, ["stash", "list"]) == "")


func _your_edit_abort() -> void:
	var s := _edit_against_teammate("edit-abort")
	var head := git(s.mine, ["rev-parse", "HEAD"])
	var mine_bytes := FileAccess.get_file_as_bytes(s.mine.path_join("x.txt"))
	var r := open(s.mine)
	check("abort: start merge", r.pull(true) == OK, GitRepository.get_last_error())
	check("abort", r.abort_operation() == OK, GitRepository.get_last_error())
	check("abort: back where it was", git(s.mine, ["rev-parse", "HEAD"]) == head, git(s.mine, ["log", "--oneline", "-2"]))
	check("abort: my edit back byte for byte", FileAccess.get_file_as_bytes(s.mine.path_join("x.txt")) == mine_bytes, read(s.mine.path_join("x.txt")))
	check("abort: unrelated edit untouched", read(s.mine.path_join("z.txt")) == "z-mine\n")
	check("abort: as git sees it, just my two edits", git(s.mine, ["status", "--porcelain"]) == "M x.txt\n M z.txt", git(s.mine, ["status", "--porcelain"]))
	check("abort: nothing in progress, no copies left", r.get_operation().kind == "" and not exists(s.mine.path_join(".git/godot-git-pull-state.json")))


## Your commit and the teammate's change the same line of x.txt: a merge git knows.
func _commits_against_teammate(name: String) -> Dictionary:
	var s := make_shared(name)
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-mine\n")
	commit_all(s.mine, "My x")
	return s


func _your_commits_finish() -> void:
	var s := _commits_against_teammate("commits-finish")
	var r := open(s.mine)
	check("commits: refused first", r.pull() != OK and Array(r.get_pull_result().conflicts) == ["x.txt"], GitRepository.get_last_error())
	check("commits: refused cleanly", r.get_operation().kind == "" and git(s.mine, ["status", "--porcelain"]) == "", git(s.mine, ["status", "--porcelain"]))
	check("commits: start merge", r.pull(true) == OK, GitRepository.get_last_error())
	check("commits: a merge git knows, stopped at x.txt", r.get_operation().kind == "merge" and Array(r.get_operation().conflicts) == ["x.txt"], r.get_operation())
	check("commits: resolve", r.resolve_conflict_with("x.txt", "theirs") == OK, GitRepository.get_last_error())
	check("commits: finish", r.continue_operation() == OK, GitRepository.get_last_error())
	check("commits: a merge commit with both parents and the pull's message", git(s.mine, ["rev-list", "--parents", "-n", "1", "HEAD"]).split(" ").size() == 3 and git(s.mine, ["log", "-1", "--format=%s"]) == "Merge remote-tracking branch 'origin/main'", git(s.mine, ["log", "-1", "--format=%s %p"]))
	check("commits: x.txt is theirs", read(s.mine.path_join("x.txt")) == "x-theirs\n")


func _your_commits_keep_other_edits() -> void:
	var s := _commits_against_teammate("commits-other")
	write(s.mine.path_join("z.txt"), "z-mine\n") # Unrelated, uncommitted.
	var r := open(s.mine)
	check("other edits: start merge", r.pull(true) == OK, GitRepository.get_last_error())
	check("other edits: unrelated edit still there", read(s.mine.path_join("z.txt")) == "z-mine\n")
	r.resolve_conflict_with("x.txt", "mine")
	check("other edits: finish", r.continue_operation() == OK, GitRepository.get_last_error())
	check("other edits: not in the merge commit", git(s.mine, ["show", "HEAD:z.txt"]) == "z1" and read(s.mine.path_join("z.txt")) == "z-mine\n", git(s.mine, ["show", "HEAD:z.txt"]))


## Finishing commits the index, so a staged change would end up in the merge: refused up front.
func _staged_refuses() -> void:
	var s := _commits_against_teammate("staged")
	write(s.mine.path_join("z.txt"), "z-staged\n")
	git(s.mine, ["add", "z.txt"])
	var head := git(s.mine, ["rev-parse", "HEAD"])
	var r := open(s.mine)
	check("staged: start merge refuses", r.pull(true) != OK and GitRepository.get_last_error().contains("staged"), GitRepository.get_last_error())
	check("staged: nothing changed", git(s.mine, ["rev-parse", "HEAD"]) == head and r.get_operation().kind == "" and git(s.mine, ["diff", "--cached", "--name-only"]) == "z.txt", git(s.mine, ["status", "--porcelain"]))
