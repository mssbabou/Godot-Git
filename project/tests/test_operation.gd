extends "res://tests/test_case.gd"
## Operations left in progress by git itself (a merge, rebase or cherry-pick stopped at
## conflicts, as a terminal leaves them): get_operation, abort_operation, continue_operation, and
## the actions that must refuse meanwhile.


func run() -> void:
	_nothing_in_progress()
	_merge_abort()
	_merge_continue()
	_merge_commit()
	_rebase_continue()
	_cherry_pick_abort()


## A repository on main with a.txt, and a branch "feature" that changes the same line as main.
func _conflicting_branches(name: String) -> String:
	var repo := make_repo(name)
	write(repo.path_join("a.txt"), "one\n")
	commit_all(repo, "first")
	git(repo, ["checkout", "-q", "-b", "feature"])
	write(repo.path_join("a.txt"), "feature\n")
	commit_all(repo, "Feature change")
	git(repo, ["checkout", "-q", "main"])
	write(repo.path_join("a.txt"), "main\n")
	commit_all(repo, "Main change")
	return repo


func _nothing_in_progress() -> void:
	var repo := make_repo("no-operation")
	write(repo.path_join("a.txt"), "one\n")
	commit_all(repo, "first")
	var r := open(repo)
	check("operation: none", r.get_operation().kind == "", r.get_operation())
	check("operation: nothing to abort", r.abort_operation() != OK, GitRepository.get_last_error())


func _merge_abort() -> void:
	var repo := _conflicting_branches("merge-abort")
	var head := git(repo, ["rev-parse", "HEAD"])
	git(repo, ["merge", "feature"])
	var r := open(repo)
	var op := r.get_operation()
	check("merge: seen", op.kind == "merge", op)
	check("merge: its message", op.subject == "Merge branch 'feature'", op.subject)
	check("merge: conflicts listed", op.conflicts == PackedStringArray(["a.txt"]), op.conflicts)

	check("merge: commit refuses while conflicted, naming the file", r.commit("plain") != OK and "a.txt" in GitRepository.get_last_error(), GitRepository.get_last_error())
	check("merge: amend refuses", r.amend("plain") != OK and "merge is in progress" in GitRepository.get_last_error(), GitRepository.get_last_error())
	check("merge: switching refuses", r.checkout_branch("feature") != OK, GitRepository.get_last_error())
	check("merge: continue refuses while conflicted", r.continue_operation() != OK and "a.txt" in GitRepository.get_last_error(), GitRepository.get_last_error())

	check("merge: abort", r.abort_operation() == OK, GitRepository.get_last_error())
	check("merge: aborted, like git merge --abort", r.get_operation().kind == "" and git(repo, ["rev-parse", "HEAD"]) == head and read(repo.path_join("a.txt")) == "main\n", read(repo.path_join("a.txt")))


func _merge_continue() -> void:
	var repo := _conflicting_branches("merge-continue")
	git(repo, ["merge", "feature"])
	write(repo.path_join("a.txt"), "both\n")
	var r := open(repo)
	r.stage("a.txt")
	check("merge: resolved once staged", r.get_operation().conflicts.is_empty(), r.get_operation())
	check("merge: continue", r.continue_operation() == OK, GitRepository.get_last_error())
	check("merge: a merge commit with git's message", git(repo, ["rev-list", "--parents", "-n", "1", "HEAD"]).split(" ").size() == 3 and git(repo, ["log", "-1", "--format=%s"]) == "Merge branch 'feature'", git(repo, ["log", "-1", "--format=%s %p"]))
	check("merge: finished", r.get_operation().kind == "" and git(repo, ["status", "--porcelain"]) == "", git(repo, ["status", "--porcelain"]))


## Committing once the conflicts are resolved finishes the merge, like `git commit` does: both
## parents, your message; also when you kept your side everywhere and no file changes.
func _merge_commit() -> void:
	for keep_mine in [false, true]:
		var name := "merge-commit-mine" if keep_mine else "merge-commit"
		var repo := _conflicting_branches(name)
		var head := git(repo, ["rev-parse", "HEAD"])
		var feature := git(repo, ["rev-parse", "feature"])
		git(repo, ["merge", "feature"])
		var r := open(repo)
		check(name + ": resolve", r.resolve_conflict_with("a.txt", "mine" if keep_mine else "theirs") == OK, GitRepository.get_last_error())
		check(name + ": commit", r.commit("Merged feature, my way") == OK, GitRepository.get_last_error())
		check(name + ": both parents, my message", git(repo, ["rev-list", "--parents", "-n", "1", "HEAD"]) == "%s %s %s" % [git(repo, ["rev-parse", "HEAD"]), head, feature] and git(repo, ["log", "-1", "--format=%s"]) == "Merged feature, my way", git(repo, ["log", "-1", "--format=%s %p"]))
		check(name + ": finished, nothing left over", r.get_operation().kind == "" and git(repo, ["status", "--porcelain"]) == "", git(repo, ["status", "--porcelain"]))
		check(name + ": git agrees feature is merged", git(repo, ["branch", "--merged"]).contains("feature"), git(repo, ["branch", "--merged"]))


func _rebase_continue() -> void:
	var repo := _conflicting_branches("rebase-continue")
	git(repo, ["checkout", "-q", "feature"])
	git(repo, ["rebase", "main"])
	var r := open(repo)
	var op := r.get_operation()
	check("rebase: seen", op.kind == "rebase" and op.subject == "feature" and op.conflicts == PackedStringArray(["a.txt"]), op)
	check("rebase: pull refuses", r.pull() != OK and "rebase is in progress" in GitRepository.get_last_error(), GitRepository.get_last_error())
	write(repo.path_join("a.txt"), "both\n")
	r.stage("a.txt")
	check("rebase: continue (no editor opens)", r.continue_operation() == OK, GitRepository.get_last_error())
	check("rebase: finished on top of main", r.get_operation().kind == "" and git(repo, ["rev-parse", "HEAD~1"]) == git(repo, ["rev-parse", "main"]) and git(repo, ["rev-parse", "--abbrev-ref", "HEAD"]) == "feature", git(repo, ["log", "--oneline", "-3"]))


func _cherry_pick_abort() -> void:
	var repo := _conflicting_branches("cherry-pick")
	var picked := git(repo, ["rev-parse", "--short=7", "feature"])
	git(repo, ["cherry-pick", "feature"])
	var r := open(repo)
	var op := r.get_operation()
	check("cherry-pick: seen", op.kind == "cherry-pick" and op.subject == picked + " Feature change", op)
	check("cherry-pick: abort", r.abort_operation() == OK and r.get_operation().kind == "" and read(repo.path_join("a.txt")) == "main\n", GitRepository.get_last_error())
