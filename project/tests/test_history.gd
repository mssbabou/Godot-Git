extends "res://tests/test_case.gd"
## History beyond the list: one file's commits (through renames), search, the commit a line comes
## from, and the commit actions (restore a file's version, undo the last commit, revert, branch
## here). Checked against what the git CLI says.


func run() -> void:
	_file_history()
	_search()
	_line_commit()
	_restore_version()
	_undo_last_commit()
	_revert()
	_branch_here()


func _hashes(commits: Array) -> PackedStringArray:
	var out := PackedStringArray()
	for c in commits:
		out.append(c.hash)
	return out


# One file's commits: the same list git log --follow gives, with the file's name in each.
func _file_history() -> void:
	var repo := make_repo("file-history")
	write(repo.path_join("a.gd"), "one\n")
	write(repo.path_join("other.gd"), "x\n")
	commit_all(repo, "Add a")
	write(repo.path_join("other.gd"), "y\n")
	commit_all(repo, "Only other")
	write(repo.path_join("a.gd"), "one\ntwo\n")
	commit_all(repo, "Edit a")
	git(repo, ["mv", "a.gd", "player.gd"])
	commit_all(repo, "Rename a to player")
	write(repo.path_join("player.gd"), "one\ntwo\nthree\n")
	commit_all(repo, "Edit player")

	var r := open(repo)
	var commits := r.get_commits(50, "player.gd")
	var expected := git(repo, ["log", "--follow", "--format=%H", "--", "player.gd"]).split("\n", false)
	check("same commits as git log --follow", _hashes(commits) == expected, [_hashes(commits), expected])
	check("each with the file's name then", commits.size() == 4 and commits[0].path == "player.gd" and commits[3].path == "a.gd", commits.map(func(c): return c.path))
	check("a commit that didn't touch it is left out", not commits.any(func(c): return c.summary == "Only other"))
	check("limited like the whole list", r.get_commits(2, "player.gd").size() == 2)
	check("a file with no commits: none", r.get_commits(50, "nope.gd").is_empty())
	check("unfiltered history unchanged", r.get_commits(50).size() == 5)


func _search() -> void:
	var repo := make_repo("search")
	write(repo.path_join("a.txt"), "1\n")
	commit_all(repo, "Add the jump")
	write(repo.path_join("a.txt"), "2\n")
	commit_all(repo, "Fix DOUBLE jump height")
	write(repo.path_join("a.txt"), "3\n")
	commit_all(repo, "Unrelated")
	var r := open(repo)
	var found := r.get_commits(50, "", "jump")
	check("search by message, any case", found.size() == 2 and found[0].summary == "Fix DOUBLE jump height", found.map(func(c): return c.summary))
	var head: String = git(repo, ["rev-parse", "HEAD"]).strip_edges()
	check("search by hash prefix", r.get_commits(50, "", head.left(8)).size() == 1)
	check("search by author", r.get_commits(50, "", "test").size() == 3)
	check("nothing found", r.get_commits(50, "", "zzz-not-there").is_empty())
	check("HEAD's commit on its own", r.get_commit("HEAD").summary == "Unrelated" and not r.get_commit("HEAD").unpushed)


func _line_commit() -> void:
	var repo := make_repo("line-commit")
	write(repo.path_join("a.gd"), "first\nsecond\n")
	commit_all(repo, "Two lines")
	write(repo.path_join("a.gd"), "first\nsecond changed\n")
	commit_all(repo, "Change the second")
	var r := open(repo)
	var text := "first\nsecond changed\nnot committed\n"
	var first := r.get_line_commit("a.gd", text, 0)
	var second := r.get_line_commit("a.gd", text, 1)
	check("line from the first commit", first.get("summary", "") == "Two lines", first)
	check("line from the second", second.get("summary", "") == "Change the second", second)
	check("an uncommitted line has none", r.get_line_commit("a.gd", text, 2).is_empty())
	check("an uncommitted file has none", r.get_line_commit("new.gd", "x\n", 0).is_empty())


func _restore_version() -> void:
	var repo := make_repo("restore")
	write(repo.path_join("a.txt"), "v1\n")
	commit_all(repo, "v1")
	var v1: String = git(repo, ["rev-parse", "HEAD"]).strip_edges()
	write(repo.path_join("a.txt"), "v2\n")
	write(repo.path_join("b.txt"), "b\n")
	commit_all(repo, "v2 and b")
	var v2: String = git(repo, ["rev-parse", "HEAD"]).strip_edges()
	var r := open(repo)
	check("restore an older version", r.restore_file_version(v1, "a.txt") == OK and read(repo.path_join("a.txt")).replace("\r", "") == "v1\n", GitApi.get_last_error())
	check("as an unstaged change", git(repo, ["status", "--porcelain", "a.txt"]).strip_edges() == "M a.txt")
	check("refused while it has uncommitted changes", r.restore_file_version(v2, "a.txt") != OK and GitApi.get_last_error().contains("uncommitted"), GitApi.get_last_error())
	git(repo, ["checkout", "--", "a.txt"])
	check("before the commit that added it: deleted", r.restore_file_version(v2 + "^1", "b.txt") == OK and not exists(repo.path_join("b.txt")))
	check("an unknown version refused", r.restore_file_version("0000000", "a.txt") != OK)


func _undo_last_commit() -> void:
	var shared := make_shared("undo")
	var mine: String = shared.mine
	var r := open(mine)
	check("a pushed commit can't be undone", r.undo_last_commit() != OK and GitApi.get_last_error().contains("pushed"), GitApi.get_last_error())
	write(mine.path_join("x.txt"), "local\n")
	commit_all(mine, "Local commit")
	var before: String = git(mine, ["rev-parse", "HEAD~1"]).strip_edges()
	check("undo", r.undo_last_commit() == OK, GitApi.get_last_error())
	check("the branch is back one commit", git(mine, ["rev-parse", "HEAD"]).strip_edges() == before)
	check("its changes are staged", git(mine, ["status", "--porcelain", "x.txt"]).strip_edges() == "M  x.txt")

	var solo := make_repo("undo-first")
	write(solo.path_join("a.txt"), "a\n")
	commit_all(solo, "First")
	check("the first commit can't be undone", open(solo).undo_last_commit() != OK and GitApi.get_last_error().contains("first commit"), GitApi.get_last_error())


func _revert() -> void:
	var repo := make_repo("revert")
	write(repo.path_join("a.txt"), "1\n2\n3\n")
	write(repo.path_join("b.txt"), "b\n")
	commit_all(repo, "Base")
	write(repo.path_join("a.txt"), "1\nTWO\n3\n")
	commit_all(repo, "Change two")
	var change: String = git(repo, ["rev-parse", "HEAD"]).strip_edges()
	write(repo.path_join("b.txt"), "b2\n")
	commit_all(repo, "Change b")
	var r := open(repo)

	write(repo.path_join("a.txt"), "1\nTWO\n3\nmine\n")
	check("refused with uncommitted changes in its files", r.revert_commit(change) != OK and GitApi.get_last_error().contains("a.txt"), GitApi.get_last_error())
	check("and nothing changed", read(repo.path_join("a.txt")).replace("\r", "") == "1\nTWO\n3\nmine\n" and git(repo, ["log", "-1", "--format=%s"]).strip_edges() == "Change b")
	git(repo, ["checkout", "--", "a.txt"])

	check("revert", r.revert_commit(change) == OK, GitApi.get_last_error())
	check("a new commit, git's message", git(repo, ["log", "-1", "--format=%s"]).strip_edges() == "Revert \"Change two\"")
	check("the change is undone", read(repo.path_join("a.txt")).replace("\r", "") == "1\n2\n3\n" and read(repo.path_join("b.txt")).replace("\r", "") == "b2\n")
	check("nothing left over", git(repo, ["status", "--porcelain"]).strip_edges().is_empty(), git(repo, ["status", "--porcelain"]))

	# A later commit changed the same line: refused, nothing touched.
	write(repo.path_join("a.txt"), "1\n2 again\n3\n")
	commit_all(repo, "Change two again")
	var head: String = git(repo, ["rev-parse", "HEAD"]).strip_edges()
	check("refused when later commits changed the same lines", r.revert_commit(change) != OK and GitApi.get_last_error().contains("a.txt"), GitApi.get_last_error())
	check("and nothing changed", git(repo, ["rev-parse", "HEAD"]).strip_edges() == head and git(repo, ["status", "--porcelain"]).strip_edges().is_empty())


func _branch_here() -> void:
	var repo := make_repo("branch-here")
	write(repo.path_join("a.txt"), "1\n")
	commit_all(repo, "One")
	var one: String = git(repo, ["rev-parse", "HEAD"]).strip_edges()
	write(repo.path_join("a.txt"), "2\n")
	commit_all(repo, "Two")
	var r := open(repo)
	check("branch at an older commit", r.create_branch_at("old-idea", one) == OK, GitApi.get_last_error())
	check("it points there", git(repo, ["rev-parse", "old-idea"]).strip_edges() == one)
	check("and we stay where we were", r.get_current_branch() == "main")
	check("an existing name refused", r.create_branch_at("old-idea", one) != OK)
