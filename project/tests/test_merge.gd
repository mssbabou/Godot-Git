extends "res://tests/test_case.gd"
## Merging a branch into the current one (merge_branch), and the preview the panel shows first
## (get_merge_branches, get_merge_preview). Results are checked against what git says: the merged
## tree against `git merge-tree`, messages against git's own.


func run() -> void:
	_fast_forward()
	_merge_commit()
	_message_into_other_branch()
	_commits_conflict()
	_edit_carried()
	_edit_conflicts()
	_staged_refuses()
	_remote_branch()
	_nothing_to_merge()


## A repository on main with x.txt, y.txt and z.txt (five lines each), and a branch "feature"
## made from it.
func _repo_with_feature(name: String) -> String:
	var repo := make_repo(name)
	for file in ["x", "y", "z"]:
		write(repo.path_join(file + ".txt"), "%s1\n%s2\n%s3\n%s4\n%s5\n" % [file, file, file, file, file])
	commit_all(repo, "Initial commit")
	git(repo, ["branch", "feature"])
	return repo


## Commits p_text as p_file on p_branch, then goes back to main.
func _commit_on(repo: String, branch: String, file: String, text: String) -> void:
	git(repo, ["checkout", "-q", branch])
	write(repo.path_join(file), text)
	commit_all(repo, "Change %s on %s" % [file, branch])
	git(repo, ["checkout", "-q", "main"])


func _fast_forward() -> void:
	var repo := _repo_with_feature("ff")
	_commit_on(repo, "feature", "x.txt", "x1\nx2-feature\nx3\nx4\nx5\n")
	_commit_on(repo, "feature", "w.txt", "new\n")
	var r := open(repo)
	var branches := r.get_merge_branches()
	check("ff: feature listed with 2 commits to merge, none behind", branches.size() == 1 and branches[0].name == "feature" and branches[0].commits == 2 and branches[0].behind == 0, branches)
	var preview := r.get_merge_preview("feature")
	check("ff: preview: 2 commits, 2 files, a fast-forward, nothing in the way", preview.commits == 2 and preview.files == 2 and preview.fast_forward and Array(preview.conflicts).is_empty() and preview.problem == "", preview)
	check("ff: merge", r.merge_branch("feature") == OK, GitApi.get_last_error())
	check("ff: main is where feature is", git(repo, ["rev-parse", "main"]) == git(repo, ["rev-parse", "feature"]))
	check("ff: no merge commit, files there", not r.get_pull_result().merged and r.get_pull_result().commits == 2 and read(repo.path_join("w.txt")) == "new\n")
	check("ff: nothing left over", git(repo, ["status", "--porcelain"]) == "", git(repo, ["status", "--porcelain"]))


func _merge_commit() -> void:
	var repo := _repo_with_feature("merge")
	_commit_on(repo, "feature", "x.txt", "x1\nx2-feature\nx3\nx4\nx5\n")
	_commit_on(repo, "main", "y.txt", "y1\ny2-main\ny3\ny4\ny5\n")
	var expected_tree := git(repo, ["merge-tree", "--write-tree", "main", "feature"])
	var r := open(repo)
	var preview := r.get_merge_preview("feature")
	check("merge: preview: 1 commit, not a fast-forward, no conflicts", preview.commits == 1 and not preview.fast_forward and Array(preview.conflicts).is_empty() and preview.problem == "", preview)
	check("merge: merge", r.merge_branch("feature") == OK, GitApi.get_last_error())
	check("merge: a merge commit with both parents", git(repo, ["rev-list", "--parents", "-n", "1", "HEAD"]).split(" ").size() == 3 and r.get_pull_result().merged)
	check("merge: git's message", git(repo, ["log", "-1", "--format=%s"]) == "Merge branch 'feature'", git(repo, ["log", "-1", "--format=%s"]))
	check("merge: the same tree git merges to", git(repo, ["rev-parse", "HEAD^{tree}"]) == expected_tree, expected_tree)
	check("merge: nothing left over", git(repo, ["status", "--porcelain"]) == "" and r.get_operation().kind == "", git(repo, ["status", "--porcelain"]))


func _message_into_other_branch() -> void:
	var repo := _repo_with_feature("into")
	git(repo, ["checkout", "-q", "-b", "dev"])
	write(repo.path_join("y.txt"), "y1\ny2-dev\ny3\ny4\ny5\n")
	commit_all(repo, "Dev change")
	_commit_on(repo, "feature", "x.txt", "x1\nx2-feature\nx3\nx4\nx5\n")
	git(repo, ["checkout", "-q", "dev"])
	var r := open(repo)
	check("into: merge", r.merge_branch("feature") == OK, GitApi.get_last_error())
	check("into: message names the branch merged into, like git", git(repo, ["log", "-1", "--format=%s"]) == "Merge branch 'feature' into dev", git(repo, ["log", "-1", "--format=%s"]))


func _commits_conflict() -> void:
	var repo := _repo_with_feature("conflict")
	_commit_on(repo, "feature", "x.txt", "x1-feature\nx2\nx3\nx4\nx5\n")
	_commit_on(repo, "main", "x.txt", "x1-main\nx2\nx3\nx4\nx5\n")
	var head := git(repo, ["rev-parse", "HEAD"])
	var r := open(repo)
	var preview := r.get_merge_preview("feature")
	check("conflict: preview names the file, no problem", Array(preview.conflicts) == ["x.txt"] and preview.problem == "", preview)
	check("conflict: refused first, naming the file", r.merge_branch("feature") != OK and Array(r.get_pull_result().conflicts) == ["x.txt"], GitApi.get_last_error())
	check("conflict: refused cleanly", git(repo, ["rev-parse", "HEAD"]) == head and git(repo, ["status", "--porcelain"]) == "" and r.get_operation().kind == "", git(repo, ["status", "--porcelain"]))
	check("conflict: start merge", r.merge_branch("feature", true) == OK, GitApi.get_last_error())
	check("conflict: a merge git knows, stopped at x.txt", r.get_operation().kind == "merge" and Array(r.get_operation().conflicts) == ["x.txt"], r.get_operation())
	check("conflict: subject is git's message", r.get_operation().subject == "Merge branch 'feature'", r.get_operation())
	check("conflict: resolve", r.resolve_conflict_with("x.txt", "theirs") == OK, GitApi.get_last_error())
	check("conflict: finish", r.continue_operation() == OK, GitApi.get_last_error())
	check("conflict: a merge commit with git's message", git(repo, ["rev-list", "--parents", "-n", "1", "HEAD"]).split(" ").size() == 3 and git(repo, ["log", "-1", "--format=%s"]) == "Merge branch 'feature'", git(repo, ["log", "-1", "--format=%s %p"]))
	check("conflict: x.txt is feature's", read(repo.path_join("x.txt")) == "x1-feature\nx2\nx3\nx4\nx5\n")


func _edit_carried() -> void:
	var repo := _repo_with_feature("carry")
	_commit_on(repo, "feature", "x.txt", "x1-feature\nx2\nx3\nx4\nx5\n")
	write(repo.path_join("x.txt"), "x1\nx2\nx3\nx4\nx5-mine\n")
	var r := open(repo)
	var preview := r.get_merge_preview("feature")
	check("carry: preview says my edit is merged in", Array(preview.carried) == ["x.txt"] and Array(preview.conflicts).is_empty() and preview.problem == "", preview)
	check("carry: merge", r.merge_branch("feature") == OK, GitApi.get_last_error())
	check("carry: both changes in the file, mine uncommitted", read(repo.path_join("x.txt")) == "x1-feature\nx2\nx3\nx4\nx5-mine\n" and git(repo, ["status", "--porcelain"]) == "M x.txt", git(repo, ["status", "--porcelain"]))
	check("carry: reported", Array(r.get_pull_result().carried) == ["x.txt"], r.get_pull_result())
	check("carry: no stash, no copies", git(repo, ["stash", "list"]) == "" and not DirAccess.dir_exists_absolute(repo.path_join(".git/godot-git-pull")))


func _edit_conflicts() -> void:
	var repo := _repo_with_feature("edit")
	_commit_on(repo, "feature", "x.txt", "x1-feature\nx2\nx3\nx4\nx5\n")
	write(repo.path_join("x.txt"), "x1-mine\nx2\nx3\nx4\nx5\n")
	var head := git(repo, ["rev-parse", "HEAD"])
	var mine_bytes := FileAccess.get_file_as_bytes(repo.path_join("x.txt"))
	var r := open(repo)
	check("edit: preview names the file", Array(r.get_merge_preview("feature").conflicts) == ["x.txt"], r.get_merge_preview("feature"))
	check("edit: refused first", r.merge_branch("feature") != OK and Array(r.get_pull_result().conflicts) == ["x.txt"], GitApi.get_last_error())
	check("edit: start merge", r.merge_branch("feature", true) == OK, GitApi.get_last_error())
	var op := r.get_operation()
	check("edit: the panel's merge of my edit with feature", op.kind == "pull" and Array(op.conflicts) == ["x.txt"] and op.subject.contains("feature"), op)
	check("edit: theirs is labeled feature", r.get_conflict("x.txt").theirs_label == "feature", r.get_conflict("x.txt"))
	check("edit: abort", r.abort_operation() == OK, GitApi.get_last_error())
	check("edit: back where it was, my edit byte for byte", git(repo, ["rev-parse", "HEAD"]) == head and FileAccess.get_file_as_bytes(repo.path_join("x.txt")) == mine_bytes, git(repo, ["status", "--porcelain"]))


func _staged_refuses() -> void:
	var repo := _repo_with_feature("staged")
	_commit_on(repo, "feature", "x.txt", "x1-feature\nx2\nx3\nx4\nx5\n")
	write(repo.path_join("x.txt"), "x1\nx2\nx3\nx4\nx5-mine\n")
	git(repo, ["add", "x.txt"])
	var head := git(repo, ["rev-parse", "HEAD"])
	var r := open(repo)
	var preview := r.get_merge_preview("feature")
	check("staged: preview says why it can't", preview.problem.contains("x.txt"), preview)
	check("staged: merge refuses", r.merge_branch("feature", true) != OK, GitApi.get_last_error())
	check("staged: nothing changed", git(repo, ["rev-parse", "HEAD"]) == head and git(repo, ["status", "--porcelain"]) == "M  x.txt", git(repo, ["status", "--porcelain"]))


func _remote_branch() -> void:
	var s := make_shared("remote")
	git(s.theirs, ["checkout", "-q", "-b", "dialogue"])
	write(s.theirs.path_join("d.txt"), "hello\n")
	commit_all(s.theirs, "Dialogue")
	git(s.theirs, ["push", "-q", "-u", "origin", "dialogue"])
	write(s.mine.path_join("y.txt"), "y-mine\n")
	commit_all(s.mine, "My y")
	git(s.mine, ["fetch", "-q"])
	var r := open(s.mine)
	var names := r.get_merge_branches().map(func(b): return b.name)
	check("remote: origin/dialogue listed", names.has("origin/dialogue"), names)
	check("remote: merge", r.merge_branch("origin/dialogue") == OK, GitApi.get_last_error())
	check("remote: git's message for a remote-tracking branch", git(s.mine, ["log", "-1", "--format=%s"]) == "Merge remote-tracking branch 'origin/dialogue'", git(s.mine, ["log", "-1", "--format=%s"]))
	check("remote: its file is here", read(s.mine.path_join("d.txt")) == "hello\n")

	# A local branch behind its remote branch: the preview says what it leaves out.
	git(s.mine, ["branch", "-q", "--track", "dialogue", "origin/dialogue"])
	git(s.theirs, ["checkout", "-q", "dialogue"])
	write(s.theirs.path_join("d.txt"), "hello again\n")
	commit_all(s.theirs, "More dialogue")
	git(s.theirs, ["push", "-q"])
	git(s.mine, ["fetch", "-q"])
	var preview := r.get_merge_preview("dialogue")
	check("remote: local branch behind its remote: says so", preview.behind_remote == 1 and preview.remote_branch == "origin/dialogue", preview)


func _nothing_to_merge() -> void:
	var repo := _repo_with_feature("nothing")
	_commit_on(repo, "main", "x.txt", "x1-main\nx2\nx3\nx4\nx5\n") # feature is behind main.
	var head := git(repo, ["rev-parse", "HEAD"])
	var r := open(repo)
	check("nothing: listed with nothing to merge, 1 behind", r.get_merge_branches()[0].commits == 0 and r.get_merge_branches()[0].behind == 1, r.get_merge_branches())
	check("nothing: preview says 0 commits", r.get_merge_preview("feature").commits == 0, r.get_merge_preview("feature"))
	check("nothing: merge changes nothing", r.merge_branch("feature") == OK and git(repo, ["rev-parse", "HEAD"]) == head)
	check("nothing: can't merge the current branch", r.merge_branch("main") != OK, GitApi.get_last_error())
