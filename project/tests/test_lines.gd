extends "res://tests/test_case.gd"
## Staging, unstaging and discarding part of a file (apply_line_changes): a hunk, or chosen lines,
## as the Diff panel hands them over. Results are checked against git diff / git diff --cached.


func run() -> void:
	_stage_one_hunk()
	_stage_added_line_only()
	_unstage_hunk()
	_discard_lines()
	_crlf_kept()
	_new_file()
	_stale_refused()
	_binary_refused()


## Twenty lines, two far apart changes: two hunks.
func _two_hunk_repo(name: String) -> Array:
	var repo := make_repo(name)
	var lines := PackedStringArray()
	for i in 20:
		lines.append("line %d" % (i + 1))
	write(repo.path_join("a.txt"), "\n".join(lines) + "\n")
	commit_all(repo, "First")
	lines[1] = "line 2 changed"
	lines[17] = "line 18 changed"
	write(repo.path_join("a.txt"), "\n".join(lines) + "\n")
	return [repo, open(repo)]


## The changed lines of hunk p_hunk of p_path's diff, as the Diff panel sends them.
func _hunk_lines(r: GitRepository, path: String, staged: bool, hunk_index: int) -> Array:
	var hunk: Dictionary = r.get_diff(path, staged).hunks[hunk_index]
	var result := []
	for i in hunk.origins.size():
		var origin: int = hunk.origins[i]
		if origin == 43: # '+'
			result.append({ "old": -1, "new": hunk.new_numbers[i], "text": hunk.text[i] })
		elif origin == 45: # '-'
			result.append({ "old": hunk.old_numbers[i], "new": -1, "text": hunk.text[i] })
	return result


func _stage_one_hunk() -> void:
	var setup := _two_hunk_repo("hunk")
	var repo: String = setup[0]
	var r: GitRepository = setup[1]
	check("hunk: two hunks", r.get_diff("a.txt", false).hunks.size() == 2)
	check("hunk: staged the first", r.apply_line_changes("a.txt", false, "stage", _hunk_lines(r, "a.txt", false, 0)) == OK, GitRepository.get_last_error())
	var cached := git(repo, ["diff", "--cached", "-U0"])
	var unstaged := git(repo, ["diff", "-U0"])
	check("hunk: only line 2 is staged", cached.contains("+line 2 changed") and not cached.contains("line 18"), cached)
	check("hunk: line 18 is still unstaged", unstaged.contains("+line 18 changed") and not unstaged.contains("line 2 changed"), unstaged)


func _stage_added_line_only() -> void:
	var setup := _two_hunk_repo("added")
	var repo: String = setup[0]
	var r: GitRepository = setup[1]
	var lines := _hunk_lines(r, "a.txt", false, 0).filter(func(l: Dictionary) -> bool: return l.new > 0)
	check("added only: staged", r.apply_line_changes("a.txt", false, "stage", lines) == OK, GitRepository.get_last_error())
	var staged := git(repo, ["show", ":a.txt"]).split("\n")
	check("added only: the index has the old line and the new one", staged[1] == "line 2" and staged[2] == "line 2 changed" and staged.size() == 21, staged)


func _unstage_hunk() -> void:
	var setup := _two_hunk_repo("unstage")
	var repo: String = setup[0]
	var r: GitRepository = setup[1]
	r.stage("a.txt")
	check("unstage: unstaged the second hunk", r.apply_line_changes("a.txt", true, "unstage", _hunk_lines(r, "a.txt", true, 1)) == OK, GitRepository.get_last_error())
	var cached := git(repo, ["diff", "--cached", "-U0"])
	var unstaged := git(repo, ["diff", "-U0"])
	check("unstage: line 2 still staged, line 18 not", cached.contains("+line 2 changed") and not cached.contains("line 18"), cached)
	check("unstage: line 18 now unstaged, the file untouched", unstaged.contains("+line 18 changed") and read(repo.path_join("a.txt")).contains("line 18 changed"), unstaged)
	check("unstage: wrong side refused", r.apply_line_changes("a.txt", true, "stage", _hunk_lines(r, "a.txt", true, 0)) != OK)


func _discard_lines() -> void:
	var setup := _two_hunk_repo("discard")
	var repo: String = setup[0]
	var r: GitRepository = setup[1]
	check("discard: discarded the second hunk", r.apply_line_changes("a.txt", false, "discard", _hunk_lines(r, "a.txt", false, 1)) == OK, GitRepository.get_last_error())
	var text := read(repo.path_join("a.txt")).replace("\r", "")
	check("discard: line 18 is back, line 2 still changed", text.contains("line 2 changed") and text.contains("line 18\n") and not text.contains("line 18 changed"), text)
	check("discard: nothing staged", git(repo, ["diff", "--cached"]) == "")


func _crlf_kept() -> void:
	var repo := make_repo("crlf")
	git(repo, ["config", "core.autocrlf", "true"])
	var lines := PackedStringArray()
	for i in 20:
		lines.append("line %d" % (i + 1))
	write(repo.path_join("w.txt"), "
".join(lines) + "
")
	commit_all(repo, "First")
	lines[1] = "LINE 2"
	lines[18] = "LINE 19"
	write(repo.path_join("w.txt"), "
".join(lines) + "
")
	var r := open(repo)
	check("crlf: two hunks", r.get_diff("w.txt", false).hunks.size() == 2)
	check("crlf: discarded the first change", r.apply_line_changes("w.txt", false, "discard", _hunk_lines(r, "w.txt", false, 0)) == OK, GitRepository.get_last_error())
	var on_disk := FileAccess.get_file_as_string(repo.path_join("w.txt"))
	check("crlf: line 2 back, line 19 still changed, CRLF line endings kept", on_disk.begins_with("line 1
line 2
") and on_disk.contains("LINE 19
") and on_disk.count("
") == 20, on_disk.c_escape())
	check("crlf: staged the other", r.apply_line_changes("w.txt", false, "stage", _hunk_lines(r, "w.txt", false, 0)) == OK, GitRepository.get_last_error())
	check("crlf: git stores LF, nothing left unstaged", git(repo, ["show", ":w.txt"]).contains("LINE 19
line 20") and git(repo, ["diff"]) == "", git(repo, ["diff"]))


func _new_file() -> void:
	var repo := make_repo("new")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "First")
	write(repo.path_join("n.txt"), "keep 1\nleave out\nkeep 2\n")
	var r := open(repo)
	var lines := _hunk_lines(r, "n.txt", false, 0).filter(func(l: Dictionary) -> bool: return l.text != "leave out")
	check("new: staged two of its lines", r.apply_line_changes("n.txt", false, "stage", lines) == OK, GitRepository.get_last_error())
	check("new: the index has just those", git(repo, ["show", ":n.txt"]) == "keep 1\nkeep 2", git(repo, ["show", ":n.txt"]))
	check("new: unstaged all of it: not staged at all", r.apply_line_changes("n.txt", true, "unstage", _hunk_lines(r, "n.txt", true, 0)) == OK and git(repo, ["status", "--porcelain"]) == "?? n.txt", git(repo, ["status", "--porcelain"]))
	check("new: discarding every line deletes it", r.apply_line_changes("n.txt", false, "discard", _hunk_lines(r, "n.txt", false, 0)) == OK and not exists(repo.path_join("n.txt")), GitRepository.get_last_error())


func _stale_refused() -> void:
	var setup := _two_hunk_repo("stale")
	var repo: String = setup[0]
	var r: GitRepository = setup[1]
	var lines := _hunk_lines(r, "a.txt", false, 0)
	write(repo.path_join("a.txt"), read(repo.path_join("a.txt")).replace("line 2 changed", "line 2 changed again"))
	check("stale: refused", r.apply_line_changes("a.txt", false, "stage", lines) != OK and GitRepository.get_last_error().contains("changed since"), GitRepository.get_last_error())
	check("stale: nothing staged", git(repo, ["diff", "--cached"]) == "")


func _binary_refused() -> void:
	var repo := make_repo("binary")
	var f := FileAccess.open(repo.path_join("b.bin"), FileAccess.WRITE)
	f.store_buffer(PackedByteArray([0, 1, 2, 3, 0, 10]))
	f.close()
	commit_all(repo, "First")
	f = FileAccess.open(repo.path_join("b.bin"), FileAccess.WRITE)
	f.store_buffer(PackedByteArray([0, 1, 9, 3, 0, 10]))
	f.close()
	var r := open(repo)
	check("binary: refused", r.apply_line_changes("b.bin", false, "stage", [{ "old": 1, "new": -1, "text": "x" }]) != OK and GitRepository.get_last_error().contains("binary"), GitRepository.get_last_error())
