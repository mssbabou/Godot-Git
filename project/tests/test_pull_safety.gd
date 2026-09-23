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


func _refused_when_incoming_touches_uncommitted_edit() -> void:
	var s := make_shared("uncommitted")
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("y.txt"), "y-mine\n")
	commit_all(s.mine, "My y") # Diverged, so this would be a merge.
	write(s.mine.path_join("x.txt"), "x-my-uncommitted\n")
	var head := git(s.mine, ["rev-parse", "HEAD"])
	var r := open(s.mine)

	var err := r.pull()
	var message := GitRepository.get_last_error()
	check("refused", err != OK, message)
	check("message names the file and what to do", message.contains("x.txt") and message.contains("Commit or discard"), message)
	check("HEAD unchanged", git(s.mine, ["rev-parse", "HEAD"]) == head)
	check("my edit untouched", read(s.mine.path_join("x.txt")) == "x-my-uncommitted\n", read(s.mine.path_join("x.txt")))
	check("no stash left behind", git(s.mine, ["stash", "list"]) == "", git(s.mine, ["stash", "list"]))
	check("no commits counted as pulled", r.get_pull_result().commits == 0)

	# After committing the edit, the same pull goes ahead (and conflicts are then git's business).
	git(s.mine, ["checkout", "-q", "--", "x.txt"])
	check("pull works once the edit is gone", r.pull() == OK, GitRepository.get_last_error())


func _refused_when_incoming_touches_staged_edit() -> void:
	var s := make_shared("staged")
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-staged\n")
	git(s.mine, ["add", "x.txt"])
	var r := open(s.mine)
	check("refused", r.pull() != OK and GitRepository.get_last_error().contains("x.txt"), GitRepository.get_last_error())
	check("still staged, unchanged", git(s.mine, ["diff", "--cached", "--name-only"]) == "x.txt" and read(s.mine.path_join("x.txt")) == "x-staged\n")
	check("no stash left behind", git(s.mine, ["stash", "list"]) == "")


func _refused_when_incoming_adds_a_file_you_created() -> void:
	var s := make_shared("untracked")
	teammate_pushes(s, "level.tscn", "theirs\n")
	write(s.mine.path_join("level.tscn"), "mine, never committed\n")
	var r := open(s.mine)
	check("refused", r.pull() != OK and GitRepository.get_last_error().contains("level.tscn"), GitRepository.get_last_error())
	check("my file untouched", read(s.mine.path_join("level.tscn")) == "mine, never committed\n")


func _refused_on_fast_forward_too() -> void:
	var s := make_shared("fastforward")
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-my-uncommitted\n")
	var r := open(s.mine)
	check("refused with the same clear message", r.pull() != OK and GitRepository.get_last_error().contains("Commit or discard"), GitRepository.get_last_error())
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

	check("pull merges", r.pull() == OK, GitRepository.get_last_error())
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

	check("conflicting pull refused", r.pull() != OK and GitRepository.get_last_error().contains("conflict"), GitRepository.get_last_error())
	check("HEAD unchanged", git(s.mine, ["rev-parse", "HEAD"]) == head)
	check("uncommitted edit restored", read(s.mine.path_join("z.txt")) == "z-uncommitted\n")
	check("staged edit restored and still staged", git(s.mine, ["diff", "--cached", "--name-only"]) == "y.txt", git(s.mine, ["status", "--short"]))
	check("no stash left behind", git(s.mine, ["stash", "list"]) == "")
	check("not stuck mid-merge", not exists(s.mine.path_join(".git/MERGE_HEAD")))
