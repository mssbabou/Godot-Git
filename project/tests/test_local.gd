extends "res://tests/test_case.gd"
## Everything that doesn't talk to a remote: status, staging, discarding, committing, line
## stats, branches.


func run() -> void:
	_status_and_staging()
	_unstage_all()
	_discard()
	_line_stats()
	_line_stats_match_git()
	_commit_and_history()
	_branches()
	_rename_and_delete_branches()
	_branch_list()
	_large_staged_files()


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


## The line counts are worked out in memory (not through libgit2's diff, which was slow): they
## must say what `git diff --numstat` says, CRLF files with core.autocrlf, deletions and binary
## files included.
func _line_stats_match_git() -> void:
	var repo := make_repo("stats-numstat")
	git(repo, ["config", "core.autocrlf", "true"])
	write(repo.path_join("a.txt"), "one
two
three
four
")
	write(repo.path_join("gone.txt"), "x
y
")
	write(repo.path_join("s.txt"), "s1
s2
")
	var bin := FileAccess.open(repo.path_join("bin.dat"), FileAccess.WRITE)
	bin.store_buffer(PackedByteArray([0, 1, 2, 3]))
	bin.close()
	commit_all(repo, "init")
	write(repo.path_join("a.txt"), "one
TWO
three
four
five
")
	DirAccess.remove_absolute(repo.path_join("gone.txt"))
	bin = FileAccess.open(repo.path_join("bin.dat"), FileAccess.WRITE)
	bin.store_buffer(PackedByteArray([0, 9, 9, 9]))
	bin.close()
	write(repo.path_join("s.txt"), "s1
S2
s3
")
	git(repo, ["add", "s.txt"])
	write(repo.path_join("untracked.txt"), "u1
u2
")
	var r := open(repo)
	for staged in [false, true]:
		var stats := r.get_line_stats(staged)
		var expected := {}
		for line in git(repo, ["diff", "--numstat"] + (["--cached"] if staged else [])).split("
", false):
			var parts := line.split("	")
			expected[parts[2]] = Vector2i(-1, -1) if parts[0] == "-" else Vector2i(parts[0].to_int(), parts[1].to_int())
		if not staged:
			expected["untracked.txt"] = Vector2i(2, 0)
		check("line counts match git diff --numstat (%s)" % ("staged" if staged else "unstaged"), stats == expected, [stats, expected])


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

	# The dock asks before switching to a branch that would delete the addon itself.
	git(repo, ["checkout", "-q", "-b", "no-addon"])
	git(repo, ["rm", "-q", "a.txt"])
	write(repo.path_join("b.txt"), "b\n")
	commit_all(repo, "drop a")
	git(repo, ["checkout", "-q", "main"])
	check("file at HEAD", r.has_file_at("HEAD", "a.txt"))
	check("file missing on another branch", not r.has_file_at("no-addon", "a.txt"))
	check("file only on another branch", r.has_file_at("no-addon", "b.txt") and not r.has_file_at("HEAD", "b.txt"))
	check("nested path", not r.has_file_at("HEAD", "addons/godot_git/godot_git.gdextension"))


# Rename and delete, checked against what git says; delete tells what it would lose first.
func _rename_and_delete_branches() -> void:
	var shared := make_shared("branch-edit")
	var repo: String = shared.mine
	var r := open(repo)

	check("rename the current branch", r.rename_branch("main", "trunk") == OK, GitRepository.get_last_error())
	check("HEAD follows the rename", git(repo, ["symbolic-ref", "--short", "HEAD"]).strip_edges() == "trunk", git(repo, ["symbolic-ref", "HEAD"]))
	check("upstream moves with it, like git branch -m", git(repo, ["rev-parse", "--abbrev-ref", "trunk@{upstream}"]).strip_edges() == "origin/main")
	check("old name is gone", not r.get_branches().has("main"), r.get_branches())
	r.create_branch("other")
	r.checkout_branch("trunk")
	check("rename to an existing name refused", r.rename_branch("trunk", "other") != OK and GitRepository.get_last_error().contains("already exists"), GitRepository.get_last_error())
	check("invalid name refused", r.rename_branch("trunk", "a..b") != OK and GitRepository.get_last_error().contains("valid"), GitRepository.get_last_error())

	# "other" points at the same commit as trunk: nothing to lose, and no upstream.
	var details := r.get_branch_details("other")
	check("nothing lost when its commits are elsewhere", details.unique == 0 and details.upstream == "", details)
	details = r.get_branch_details("trunk")
	check("upstream named", details.upstream == "origin/main", details)

	# Two commits only "lonely" has.
	git(repo, ["checkout", "-q", "-b", "lonely"])
	for i in 2:
		write(repo.path_join("lonely.txt"), str(i))
		commit_all(repo, "lonely %d" % i)
	git(repo, ["checkout", "-q", "trunk"])
	details = r.get_branch_details("lonely")
	check("counts commits no other branch has", details.unique == 2, details)
	git(repo, ["branch", "-q", "keeper", "lonely"])
	check("not lost while another branch has them", r.get_branch_details("lonely").unique == 0, r.get_branch_details("lonely"))
	git(repo, ["branch", "-q", "-D", "keeper"])

	check("can't delete the current branch", r.delete_branch("trunk") != OK and GitRepository.get_last_error().contains("Switch to another branch"), GitRepository.get_last_error())
	check("delete", r.delete_branch("lonely") == OK, GitRepository.get_last_error())
	check("gone for git too", git(repo, ["branch", "--list", "lonely"]).strip_edges() == "")
	check("deleting a missing branch refused", r.delete_branch("lonely") != OK)
	check("remote branch untouched", git(repo, ["branch", "-r"]).contains("origin/main"))


# The branch picker's list: local first (newest first), each with ahead/behind against its
# upstream, then remote branches that have no local branch.
func _branch_list() -> void:
	var shared := make_shared("branch-list")
	var mine: String = shared.mine
	git(shared.theirs, ["checkout", "-q", "-b", "their-feature"])
	write(shared.theirs.path_join("f.txt"), "f
")
	commit_all(shared.theirs, "Feature")
	git(shared.theirs, ["push", "-q", "-u", "origin", "their-feature"])
	teammate_pushes(shared, "x.txt", "x2
")
	git(mine, ["checkout", "-q", "-b", "older"])
	git(mine, ["checkout", "-q", "main"])
	write(mine.path_join("y.txt"), "mine
")
	commit_all(mine, "Mine")
	git(mine, ["fetch", "-q"])
	var list := open(mine).get_branch_list()
	var names := list.map(func(b): return b.name)
	check("local branches first, newest first", names.slice(0, 2) == ["main", "older"], names)
	check("a remote branch without a local one is listed", names.has("origin/their-feature") and not list[names.find("origin/their-feature")].local, names)
	check("origin/main isn't listed twice", not names.has("origin/main"), names)
	var main: Dictionary = list[0]
	var counts: String = git(mine, ["rev-list", "--left-right", "--count", "main...origin/main"]).strip_edges()
	check("current, with ahead/behind as git counts them", main.current and main.upstream == "origin/main" and "%d	%d" % [main.ahead, main.behind] == counts, [main, counts])


# The commit warning for big files: what's staged and at least the given size, largest first.
func _large_staged_files() -> void:
	var repo := make_repo("large")
	write(repo.path_join("small.txt"), "small\n")
	commit_all(repo, "init")
	var r := open(repo)
	var big := FileAccess.open(repo.path_join("big.bin"), FileAccess.WRITE)
	big.store_buffer(_bytes(3000))
	big.close()
	var bigger := FileAccess.open(repo.path_join("bigger.bin"), FileAccess.WRITE)
	bigger.store_buffer(_bytes(5000))
	bigger.close()
	check("nothing staged, nothing large", r.get_large_staged_files(1000).is_empty())
	r.stage_all()
	var large: Array = r.get_large_staged_files(2000)
	check("large staged files, largest first", large.map(func(f): return f.path) == ["bigger.bin", "big.bin"], large)
	check("sizes", large[0].size == 5000 and large[1].size == 3000, large)
	check("above the limit only", r.get_large_staged_files(4000).size() == 1)


func _bytes(count: int) -> PackedByteArray:
	var data := PackedByteArray()
	data.resize(count)
	return data
