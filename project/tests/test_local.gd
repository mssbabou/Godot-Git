extends "res://tests/test_case.gd"
## Everything that doesn't talk to a remote: status, staging, discarding, committing, line
## stats, branches.


func run() -> void:
	_status_and_staging()
	_unstage_all()
	_discard()
	_line_stats()
	_commit_and_history()
	_branches()


func _status_and_staging() -> void:
	var repo := make_repo("status")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "init")
	write(repo.path_join("a.txt"), "changed\n")
	write(repo.path_join("new/b.txt"), "b\n")
	var r := open(repo)

	var states := {}
	for entry: Dictionary in r.get_status():
		states[entry.path] = [entry.index, entry.worktree]
	check("modified file is listed", states.get("a.txt") == ["", "modified"], states)
	check("new file in a new folder is listed by path", states.get("new/b.txt") == ["", "untracked"], states)

	check("stage", r.stage("a.txt") == OK, GitRepository.get_last_error())
	check("git sees it staged", git(repo, ["diff", "--cached", "--name-only"]) == "a.txt")
	check("unstage", r.unstage("a.txt") == OK, GitRepository.get_last_error())
	check("git sees nothing staged", git(repo, ["diff", "--cached", "--name-only"]) == "")
	check("stage all", r.stage_all() == OK, GitRepository.get_last_error())
	check("stage all includes new files", git(repo, ["diff", "--cached", "--name-only"]).split("\n").size() == 2, git(repo, ["status", "--short"]))


# "Unstage all" once failed with a libgit2 assertion (empty pathspec). Both with commits and
# before the first commit, where there's no HEAD to reset to.
func _unstage_all() -> void:
	for kind in ["with commits", "no commits yet"]:
		var repo := make_repo("unstage-" + kind.replace(" ", "-"))
		if kind == "with commits":
			write(repo.path_join("a.txt"), "a\n")
			commit_all(repo, "init")
			write(repo.path_join("a.txt"), "changed\n")
		write(repo.path_join("b.txt"), "new\n")
		var r := open(repo)
		r.stage_all()
		check("unstage all (%s)" % kind, r.unstage_all() == OK, GitRepository.get_last_error())
		check("nothing staged afterwards (%s)" % kind, git(repo, ["diff", "--cached", "--name-only"]) == "", git(repo, ["status", "--short"]))
		check("files untouched (%s)" % kind, read(repo.path_join("b.txt")) == "new\n")


func _discard() -> void:
	var repo := make_repo("discard")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "init")
	write(repo.path_join("a.txt"), "changed\n")
	write(repo.path_join("new.txt"), "new\n")
	var r := open(repo)
	check("discard modified", r.discard("a.txt") == OK, GitRepository.get_last_error())
	check("modified file is back to committed version", read(repo.path_join("a.txt")) == "a\n", read(repo.path_join("a.txt")))
	check("discard untracked", r.discard("new.txt") == OK, GitRepository.get_last_error())
	check("untracked file is deleted", not exists(repo.path_join("new.txt")))
	check("working tree clean", r.get_status().is_empty(), r.get_status())


func _line_stats() -> void:
	var repo := make_repo("stats")
	write(repo.path_join("a.txt"), "line1\nline2\nline3\n")
	commit_all(repo, "init")
	write(repo.path_join("a.txt"), "line1\nCHANGED\nline3\nline4\n")
	write(repo.path_join("new.txt"), "1\n2\n3\n")
	var r := open(repo)
	var stats := r.get_line_stats(false)
	check("unstaged stats of an edit", stats.get("a.txt") == Vector2i(2, 1), stats)
	check("unstaged stats count a new file's lines", stats.get("new.txt") == Vector2i(3, 0), stats)
	r.stage("a.txt")
	check("staged stats", r.get_line_stats(true).get("a.txt") == Vector2i(2, 1), r.get_line_stats(true))


func _commit_and_history() -> void:
	var repo := make_repo("commit")
	var r := open(repo)
	check("no commits yet", r.get_commits(10).is_empty())
	write(repo.path_join("a.txt"), "a\n")
	r.stage("a.txt")
	check("first commit", r.commit("First commit") == OK, GitRepository.get_last_error())
	write(repo.path_join("a.txt"), "b\n")
	r.stage("a.txt")
	check("second commit", r.commit("Second commit\n\nWith a body.") == OK, GitRepository.get_last_error())
	var commits := r.get_commits(10)
	check("history newest first", commits.size() == 2 and commits[0].summary == "Second commit", commits)
	check("full message kept", commits[0].message.contains("With a body."), commits[0].message)
	check("git agrees", git(repo, ["log", "-1", "--format=%s"]) == "Second commit")
	check("no remote: nothing marked unpushed", commits.all(func(c): return not c.unpushed), commits)
	check("never fetched", r.get_sync_status().last_fetched == 0, r.get_sync_status())


func _branches() -> void:
	var repo := make_repo("branches")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "init")
	var r := open(repo)
	check("create branch", r.create_branch("feature/x") == OK, GitRepository.get_last_error())
	check("switched to it", r.get_current_branch() == "feature/x")
	check("listed", r.get_branches().has("feature/x") and r.get_branches().has("main"), r.get_branches())
	check("switch back", r.checkout_branch("main") == OK and r.get_current_branch() == "main", GitRepository.get_last_error())
