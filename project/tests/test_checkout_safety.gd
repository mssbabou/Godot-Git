extends "res://tests/test_case.gd"
## A checkout that fails halfway (on Windows: a file another program has open, e.g. Godot
## importing it, or an antivirus scan) must leave the working tree exactly as it was. libgit2
## stops at the first error without undoing what it already changed.
## Found in real use: a branch switch deleted files, then failed, leaving fake "changes".


func run() -> void:
	if OS.get_name() != "Windows":
		# Deleting an open file works elsewhere, so there's no easy way to make checkout fail.
		print("  (skipped; the lock only exists on Windows)")
		return
	_switch_branch_with_locked_file()
	_pull_with_locked_file()
	_merge_with_locked_file()
	_lock_released_soon()


## main has a.txt ... e.txt; "other" drops a, b, d and e, changes c, and adds f.
func _make_branches(name: String) -> Dictionary:
	var shared := make_shared(name)
	for file in ["a", "b", "c", "d", "e"]:
		write(shared.theirs.path_join(file + ".txt"), file + "\n")
	commit_all(shared.theirs, "Five files")
	git(shared.theirs, ["push", "-q"])
	git(shared.theirs, ["checkout", "-q", "-b", "other"])
	for file in ["a", "b", "d", "e"]:
		DirAccess.remove_absolute(shared.theirs.path_join(file + ".txt"))
	write(shared.theirs.path_join("c.txt"), "c changed\n")
	write(shared.theirs.path_join("f.txt"), "f\n")
	commit_all(shared.theirs, "Other")
	git(shared.theirs, ["push", "-q", "-u", "origin", "other"])
	git(shared.theirs, ["checkout", "-q", "main"])
	git(shared.mine, ["pull", "-q"])
	git(shared.mine, ["fetch", "-q"])
	return shared


func _tree_snapshot(repo: String) -> Dictionary:
	var files := {}
	for file in ["a", "b", "c", "d", "e", "f", "x", "y", "z"]:
		var path := repo.path_join(file + ".txt")
		files[file] = read(path) if exists(path) else null
	return files


func _switch_branch_with_locked_file() -> void:
	var shared := _make_branches("switch")
	write(shared.mine.path_join("x.txt"), "my uncommitted edit\n")
	var before := _tree_snapshot(shared.mine)
	var status_before := git(shared.mine, ["status", "--porcelain"])

	var lock := FileAccess.open(shared.mine.path_join("d.txt"), FileAccess.READ) # Can't be deleted while open.
	var r := open(shared.mine)
	var err := r.checkout_branch("origin/other")
	lock.close()

	check("switch fails while a file is locked", err != OK)
	check("says which file", GitRepository.get_last_error().contains("d.txt"), GitRepository.get_last_error())
	check("still on main", git(shared.mine, ["symbolic-ref", "--short", "HEAD"]) == "main")
	check("working tree exactly as before", _tree_snapshot(shared.mine) == before, [_tree_snapshot(shared.mine), before])
	check("git status as before", git(shared.mine, ["status", "--porcelain"]) == status_before, git(shared.mine, ["status", "--porcelain"]))

	check("switch works once the file is free", r.checkout_branch("origin/other") == OK, GitRepository.get_last_error())
	check("switched", git(shared.mine, ["symbolic-ref", "--short", "HEAD"]) == "other" and read(shared.mine.path_join("c.txt")) == "c changed\n")


func _pull_with_locked_file() -> void:
	# The same for a pull: the teammate deletes several files.
	var shared := _make_branches("pull")
	git(shared.theirs, ["pull", "-q"])
	for file in ["a", "b", "d", "e"]:
		DirAccess.remove_absolute(shared.theirs.path_join(file + ".txt"))
	commit_all(shared.theirs, "Remove files")
	git(shared.theirs, ["push", "-q"])
	write(shared.mine.path_join("x.txt"), "my uncommitted edit\n")
	var before := _tree_snapshot(shared.mine)
	var head := git(shared.mine, ["rev-parse", "HEAD"])

	var lock := FileAccess.open(shared.mine.path_join("d.txt"), FileAccess.READ)
	var r := open(shared.mine)
	var err := r.pull()
	lock.close()

	check("pull fails while a file is locked", err != OK)
	check("HEAD unchanged", git(shared.mine, ["rev-parse", "HEAD"]) == head)
	check("working tree exactly as before", _tree_snapshot(shared.mine) == before, [_tree_snapshot(shared.mine), before])
	check("pull works once the file is free", r.pull() == OK, GitRepository.get_last_error())


func _merge_with_locked_file() -> void:
	# Diverged, so the pull merges: git_merge's own checkout hits the locked file.
	var shared := _make_branches("merge")
	git(shared.theirs, ["pull", "-q"])
	for file in ["a", "b", "d", "e"]:
		DirAccess.remove_absolute(shared.theirs.path_join(file + ".txt"))
	commit_all(shared.theirs, "Remove files")
	git(shared.theirs, ["push", "-q"])
	write(shared.mine.path_join("y.txt"), "y2\n")
	commit_all(shared.mine, "My change")
	write(shared.mine.path_join("x.txt"), "my uncommitted edit\n")
	var before := _tree_snapshot(shared.mine)
	var head := git(shared.mine, ["rev-parse", "HEAD"])

	var lock := FileAccess.open(shared.mine.path_join("d.txt"), FileAccess.READ)
	var r := open(shared.mine)
	var err := r.pull()
	lock.close()

	check("merging pull fails while a file is locked", err != OK)
	check("HEAD unchanged", git(shared.mine, ["rev-parse", "HEAD"]) == head)
	check("working tree exactly as before, uncommitted edit included", _tree_snapshot(shared.mine) == before, [_tree_snapshot(shared.mine), before])
	check("no merge in progress", not exists(shared.mine.path_join(".git/MERGE_HEAD")))
	check("no stash left behind", git(shared.mine, ["stash", "list"]).is_empty(), git(shared.mine, ["stash", "list"]))
	check("merging pull works once the file is free", r.pull() == OK and r.get_pull_result().merged, GitRepository.get_last_error())


func _lock_released_soon() -> void:
	# A file busy for a moment (an import, an antivirus scan): the switch waits and succeeds.
	var shared := _make_branches("brief")
	var lock := FileAccess.open(shared.mine.path_join("d.txt"), FileAccess.READ)
	var releaser := Thread.new()
	releaser.start(func():
		OS.delay_msec(300)
		lock.close())
	var r := open(shared.mine)
	var err := r.checkout_branch("origin/other")
	releaser.wait_to_finish()
	check("switch succeeds when the file is free again soon", err == OK, GitRepository.get_last_error())
	check("fully switched", read(shared.mine.path_join("c.txt")) == "c changed\n" and not exists(shared.mine.path_join("d.txt")))
