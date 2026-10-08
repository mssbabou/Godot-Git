extends "res://tests/test_case.gd"
## A pull must never lose or strand your uncommitted work. It either completes with your edits
## still in place, or refuses up front and changes nothing (and never leaves a stash behind).


func run() -> void:
	_refused_when_incoming_touches_uncommitted_edit()
	_refused_when_incoming_touches_staged_edit()
	_refused_when_incoming_adds_a_file_you_created()
	_refused_on_fast_forward_too()
	_unrelated_edits_survive_a_merge()
	_unrelated_edits_survive_a_refused_conflict()
	_edit_on_other_lines_carried_on_fast_forward()
	_edit_on_other_lines_carried_through_a_merge()
	_carried_edit_restored_when_the_merge_conflicts()
	_carried_edit_keeps_crlf()
	_leftovers()


const DOC := "a\nb\nc\nd\ne\nf\ng\n"


## Both sides have doc.txt (seven lines); the teammate then changes its first line.
func _shared_doc(name: String) -> Dictionary:
	var s := make_shared(name)
	teammate_pushes(s, "doc.txt", DOC, "Add doc")
	git(s.mine, ["pull", "-q", "--no-rebase"])
	teammate_pushes(s, "doc.txt", DOC.replace("a\n", "A-theirs\n"), "Change doc's first line")
	return s


# The teammate's change and your uncommitted edit are in the same file but on different lines:
# pull merges your edit into the new version instead of refusing (Godit's "stash, pull,
# re-apply", without its risks: checked in memory first, never markers or a leftover stash).
func _edit_on_other_lines_carried_on_fast_forward() -> void:
	var s := _shared_doc("carry-ff")
	write(s.mine.path_join("doc.txt"), DOC.replace("g\n", "G-mine\n"))
	var r := open(s.mine)
	check("carry: fetch", r.fetch() == OK, GitApi.get_last_error())
	check("carry: no blockers for an edit on other lines", r.get_pull_blockers().is_empty(), r.get_pull_blockers())
	check("carry: pull goes ahead", r.pull() == OK, GitApi.get_last_error())
	check("carry: both changes in the file", read(s.mine.path_join("doc.txt")) == DOC.replace("a\n", "A-theirs\n").replace("g\n", "G-mine\n"), read(s.mine.path_join("doc.txt")))
	check("carry: HEAD is the teammate's commit", git(s.mine, ["rev-parse", "HEAD"]) == git(s.mine, ["rev-parse", "origin/main"]))
	check("carry: my edit is still uncommitted, unstaged", git(s.mine, ["status", "--porcelain"]) == "M doc.txt", git(s.mine, ["status", "--porcelain"]))
	check("carry: git sees only my line as changed", git(s.mine, ["diff", "--numstat"]) == "1\t1\tdoc.txt", git(s.mine, ["diff", "--numstat"]))
	check("carry: the pull says so", Array(r.get_pull_result().get("carried", [])) == ["doc.txt"], r.get_pull_result())
	check("carry: no stash, no backups left", git(s.mine, ["stash", "list"]) == "" and not DirAccess.dir_exists_absolute(s.mine.path_join(".git/godot-git-pull")))


func _edit_on_other_lines_carried_through_a_merge() -> void:
	var s := _shared_doc("carry-merge")
	write(s.mine.path_join("y.txt"), "y-mine\n")
	commit_all(s.mine, "My y") # Diverged: the pull makes a merge commit.
	write(s.mine.path_join("doc.txt"), DOC.replace("g\n", "G-mine\n"))
	var r := open(s.mine)
	check("carry merge: pull goes ahead", r.pull() == OK, GitApi.get_last_error())
	check("carry merge: a merge commit", git(s.mine, ["rev-list", "--parents", "-n", "1", "HEAD"]).split(" ").size() == 3)
	check("carry merge: both changes in the file", read(s.mine.path_join("doc.txt")) == DOC.replace("a\n", "A-theirs\n").replace("g\n", "G-mine\n"), read(s.mine.path_join("doc.txt")))
	check("carry merge: my edit is still uncommitted", git(s.mine, ["status", "--porcelain"]) == "M doc.txt", git(s.mine, ["status", "--porcelain"]))
	check("carry merge: no stash left", git(s.mine, ["stash", "list"]) == "")


# The edit merges fine, but the pull itself can't (both committed y.txt's line): nothing may
# change, and the carried file must be back exactly as it was.
func _carried_edit_restored_when_the_merge_conflicts() -> void:
	var s := _shared_doc("carry-conflict")
	teammate_pushes(s, "y.txt", "y-theirs\n")
	write(s.mine.path_join("y.txt"), "y-mine\n")
	commit_all(s.mine, "My y")
	var mine := DOC.replace("g\n", "G-mine\n")
	write(s.mine.path_join("doc.txt"), mine)
	var head := git(s.mine, ["rev-parse", "HEAD"])
	var r := open(s.mine)
	check("carry conflict: refused, naming the conflicting file", r.pull() != OK and Array(r.get_pull_result().conflicts) == ["y.txt"], [GitApi.get_last_error(), r.get_pull_result().conflicts])
	check("carry conflict: HEAD unchanged", git(s.mine, ["rev-parse", "HEAD"]) == head)
	check("carry conflict: my file exactly as it was", FileAccess.get_file_as_string(s.mine.path_join("doc.txt")) == mine, FileAccess.get_file_as_string(s.mine.path_join("doc.txt")))
	check("carry conflict: no stash, no backups left", git(s.mine, ["stash", "list"]) == "" and not DirAccess.dir_exists_absolute(s.mine.path_join(".git/godot-git-pull")))


# With core.autocrlf the file on disk has CRLF and git's copy LF: the merged file must come out
# with CRLF like every other checked-out file, not LF, and not as a whole-file change.
func _carried_edit_keeps_crlf() -> void:
	var s := _shared_doc("carry-crlf")
	git(s.mine, ["config", "core.autocrlf", "true"])
	DirAccess.remove_absolute(s.mine.path_join("doc.txt"))
	git(s.mine, ["checkout", "--", "doc.txt"]) # Checked out again, now with CRLF.
	write(s.mine.path_join("doc.txt"), DOC.replace("g\n", "G-mine\n").replace("\n", "\r\n"))
	var r := open(s.mine)
	check("carry crlf: pull goes ahead", r.pull() == OK, GitApi.get_last_error())
	var raw := FileAccess.get_file_as_string(s.mine.path_join("doc.txt"))
	check("carry crlf: CRLF throughout", raw == DOC.replace("a\n", "A-theirs\n").replace("g\n", "G-mine\n").replace("\n", "\r\n"), raw.c_escape())
	check("carry crlf: only my line differs", git(s.mine, ["diff", "--numstat"]) == "1\t1\tdoc.txt", git(s.mine, ["diff", "--numstat"]))


func _refused_when_incoming_touches_uncommitted_edit() -> void:
	var s := make_shared("uncommitted")
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("y.txt"), "y-mine\n")
	commit_all(s.mine, "My y") # Diverged, so this would be a merge.
	write(s.mine.path_join("x.txt"), "x-my-uncommitted\n")
	var head := git(s.mine, ["rev-parse", "HEAD"])
	var r := open(s.mine)

	var err := r.pull()
	var message := GitApi.get_last_error()
	check("refused", err != OK, message)
	check("message names the file, and the conflict is reported for the panel to ask", message.contains("x.txt") and Array(r.get_pull_result().conflicts) == ["x.txt"], [message, r.get_pull_result().conflicts])
	check("HEAD unchanged", git(s.mine, ["rev-parse", "HEAD"]) == head)
	check("my edit untouched", read(s.mine.path_join("x.txt")) == "x-my-uncommitted\n", read(s.mine.path_join("x.txt")))
	check("no stash left behind", git(s.mine, ["stash", "list"]) == "", git(s.mine, ["stash", "list"]))
	check("no commits counted as pulled", r.get_pull_result().commits == 0)

	# After committing the edit, the same pull goes ahead (and conflicts are then git's business).
	git(s.mine, ["checkout", "-q", "--", "x.txt"])
	check("pull works once the edit is gone", r.pull() == OK, GitApi.get_last_error())


func _refused_when_incoming_touches_staged_edit() -> void:
	var s := make_shared("staged")
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-staged\n")
	git(s.mine, ["add", "x.txt"])
	var r := open(s.mine)
	check("refused", r.pull() != OK and GitApi.get_last_error().contains("x.txt"), GitApi.get_last_error())
	check("still staged, unchanged", git(s.mine, ["diff", "--cached", "--name-only"]) == "x.txt" and read(s.mine.path_join("x.txt")) == "x-staged\n")
	check("no stash left behind", git(s.mine, ["stash", "list"]) == "")


func _refused_when_incoming_adds_a_file_you_created() -> void:
	var s := make_shared("untracked")
	teammate_pushes(s, "level.tscn", "theirs\n")
	write(s.mine.path_join("level.tscn"), "mine, never committed\n")
	var r := open(s.mine)
	check("refused", r.pull() != OK and GitApi.get_last_error().contains("level.tscn"), GitApi.get_last_error())
	check("my file untouched", read(s.mine.path_join("level.tscn")) == "mine, never committed\n")


func _refused_on_fast_forward_too() -> void:
	var s := make_shared("fastforward")
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-my-uncommitted\n")
	var r := open(s.mine)
	check("refused the same way", r.pull() != OK and Array(r.get_pull_result().conflicts) == ["x.txt"], GitApi.get_last_error())
	check("my edit untouched", read(s.mine.path_join("x.txt")) == "x-my-uncommitted\n")


# Godot rewrites project.godot and scenes all the time, so pulling with unrelated local edits
# has to just work.
func _unrelated_edits_survive_a_merge() -> void:
	var s := make_shared("unrelated")
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("y.txt"), "y-mine\n")
	commit_all(s.mine, "My y")
	write(s.mine.path_join("z.txt"), "z-uncommitted\n")
	write(s.mine.path_join("y.txt"), "y-staged\n")
	git(s.mine, ["add", "y.txt"])
	write(s.mine.path_join("notes.txt"), "untracked\n")
	var r := open(s.mine)

	check("pull merges", r.pull() == OK, GitApi.get_last_error())
	check("no warning", r.get_notice() == "", r.get_notice())
	check("their change arrived", read(s.mine.path_join("x.txt")) == "x-theirs\n")
	check("uncommitted edit kept", read(s.mine.path_join("z.txt")) == "z-uncommitted\n")
	check("staged edit kept and still staged", git(s.mine, ["diff", "--cached", "--name-only"]) == "y.txt", git(s.mine, ["status", "--short"]))
	check("untracked file kept", read(s.mine.path_join("notes.txt")) == "untracked\n")
	check("no stash left behind", git(s.mine, ["stash", "list"]) == "")


func _unrelated_edits_survive_a_refused_conflict() -> void:
	var s := make_shared("conflict")
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-mine\n")
	commit_all(s.mine, "My x")
	write(s.mine.path_join("z.txt"), "z-uncommitted\n")
	write(s.mine.path_join("y.txt"), "y-staged\n")
	git(s.mine, ["add", "y.txt"])
	var head := git(s.mine, ["rev-parse", "HEAD"])
	var r := open(s.mine)

	check("conflicting pull refused", r.pull() != OK and GitApi.get_last_error().contains("same lines"), GitApi.get_last_error())
	check("HEAD unchanged", git(s.mine, ["rev-parse", "HEAD"]) == head)
	check("uncommitted edit restored", read(s.mine.path_join("z.txt")) == "z-uncommitted\n")
	check("staged edit restored and still staged", git(s.mine, ["diff", "--cached", "--name-only"]) == "y.txt", git(s.mine, ["status", "--short"]))
	check("no stash left behind", git(s.mine, ["stash", "list"]) == "")
	check("not stuck mid-merge", not exists(s.mine.path_join(".git/MERGE_HEAD")))


## A pull that never finished (the editor went down) leaves your edits in .git/godot-git-pull:
## listed, put back byte for byte, or deleted.
func _leftovers() -> void:
	var repo := make_repo("leftovers")
	write(repo.path_join("a.txt"), "committed\n")
	commit_all(repo, "first")
	var folder := repo.path_join(".git/godot-git-pull")
	DirAccess.make_dir_recursive_absolute(folder)
	write(folder.path_join("0"), "my edit\n")
	write(folder.path_join("README.txt"), "Your uncommitted edits, saved by the Godot Git panel while it pulled.\n\n0  a.txt\n")
	var r := open(repo)
	var leftovers := r.get_pull_leftovers()
	check("leftovers: listed", leftovers.size() == 1 and leftovers[0].files.size() == 1 and leftovers[0].files[0].path == "a.txt" and not leftovers[0].files[0].back, leftovers)
	check("leftovers: put back", r.resolve_pull_leftovers(leftovers[0].folder, true) == OK, GitApi.get_last_error())
	check("leftovers: the edit is back, byte for byte", FileAccess.get_file_as_string(repo.path_join("a.txt")) == "my edit\n")
	check("leftovers: the copies are gone", not DirAccess.dir_exists_absolute(folder) and r.get_pull_leftovers().is_empty())
