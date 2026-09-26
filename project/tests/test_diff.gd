extends "res://tests/test_case.gd"
## get_diff: what the diff view shows for a file, checked against `git diff`.


func run() -> void:
	_unstaged_edit()
	_untracked_file()
	_staged_changes()
	_special_files()
	_line_endings()


## `git diff` output as [origins, texts, hunk headers], for comparing with get_diff.
func _git_lines(repo: String, args: Array) -> Dictionary:
	var origins := ""
	var texts := []
	var headers := []
	var in_hunk := false
	for line: String in git(repo, ["diff", "--no-color", "--no-ext-diff"] + args).split("\n"):
		if line.begins_with("@@"):
			in_hunk = true
			headers.append(line)
		elif in_hunk and (line.begins_with("+") or line.begins_with("-") or line.begins_with(" ")):
			origins += line[0]
			texts.append(line.substr(1))
	return { "origins": origins, "texts": texts, "headers": headers }


## get_diff's lines in the same shape as _git_lines.
func _our_lines(diff: Dictionary) -> Dictionary:
	var origins := ""
	var texts := []
	var headers := []
	for hunk: Dictionary in diff.get("hunks", []):
		var header := "@@ -%d,%d +%d,%d @@" % [hunk.old_start, hunk.old_lines, hunk.new_start, hunk.new_lines]
		if hunk.context != "":
			header += " " + hunk.context
		headers.append(header)
		for i in hunk.origins.size():
			origins += char(hunk.origins[i])
			texts.append(hunk.text[i])
	return { "origins": origins, "texts": texts, "headers": headers }


## Line numbers must follow from the hunk's starts: each side counts up on its own lines.
func _numbers_consistent(diff: Dictionary) -> bool:
	for hunk: Dictionary in diff.hunks:
		var old_line: int = hunk.old_start
		var new_line: int = hunk.new_start
		for i in hunk.origins.size():
			var origin := char(hunk.origins[i])
			var expected_old := old_line if origin != "+" else -1
			var expected_new := new_line if origin != "-" else -1
			if hunk.old_numbers[i] != expected_old or hunk.new_numbers[i] != expected_new:
				return false
			if origin != "+":
				old_line += 1
			if origin != "-":
				new_line += 1
	return true


func _unstaged_edit() -> void:
	var repo := make_repo("edit")
	var lines := []
	for i in 40:
		lines.append("line %d" % i)
	lines[20] = "func _ready():"
	write(repo.path_join("a.gd"), "\n".join(lines) + "\n")
	commit_all(repo, "init")
	lines[2] = "changed 2"
	lines.insert(25, "inserted")
	lines.remove_at(35)
	write(repo.path_join("a.gd"), "\n".join(lines) + "\n")

	var diff := open(repo).get_diff("a.gd", false)
	var ours := _our_lines(diff)
	var theirs := _git_lines(repo, ["--", "a.gd"])
	check("edit: kind and status", diff.get("kind") == "text" and diff.get("status") == "modified", diff)
	check("edit: same lines as git diff", ours.origins == theirs.origins and ours.texts == theirs.texts, [ours, theirs])
	check("edit: same hunks as git diff (incl. the function they're in)", ours.headers == theirs.headers, [ours.headers, theirs.headers])
	check("edit: line numbers", _numbers_consistent(diff))
	check("edit: counts", diff.added == 2 and diff.removed == 2, [diff.added, diff.removed])
	check("edit: nothing staged", open(repo).get_diff("a.gd", true).get("kind") == "unchanged")


func _untracked_file() -> void:
	var repo := make_repo("untracked")
	write(repo.path_join("keep.txt"), "x\n")
	commit_all(repo, "init")
	write(repo.path_join("new/deep/b.txt"), "one\ntwo\n")
	var diff := open(repo).get_diff("new/deep/b.txt", false)
	check("untracked file in a new folder: status", diff.get("status") == "untracked", diff)
	check("untracked file: every line added", _our_lines(diff).origins == "++" and _our_lines(diff).texts == ["one", "two"], _our_lines(diff))
	check("untracked file: numbers", diff.hunks[0].new_numbers == PackedInt32Array([1, 2]) and diff.hunks[0].old_numbers == PackedInt32Array([-1, -1]), diff.hunks)


func _staged_changes() -> void:
	var repo := make_repo("staged")
	var body := ""
	for i in 30:
		body += "shared line %d\n" % i
	write(repo.path_join("old_name.txt"), body)
	write(repo.path_join("gone.txt"), "bye\n")
	write(repo.path_join("both.txt"), "1\n2\n3\n")
	commit_all(repo, "init")
	git(repo, ["mv", "old_name.txt", "new_name.txt"])
	git(repo, ["rm", "-q", "gone.txt"])
	write(repo.path_join("added.txt"), "hello\n")
	git(repo, ["add", "added.txt"])
	write(repo.path_join("both.txt"), "1\nstaged\n3\n")
	git(repo, ["add", "both.txt"])
	write(repo.path_join("both.txt"), "1\nstaged\n3\nunstaged\n")
	var r := open(repo)

	var renamed := r.get_diff("new_name.txt", true)
	check("staged rename: found as a rename", renamed.get("status") == "renamed" and renamed.get("old_path") == "old_name.txt", renamed)
	check("staged rename: no line changes", renamed.get("kind") == "text" and renamed.hunks.is_empty(), renamed)
	var deleted := r.get_diff("gone.txt", true)
	check("staged deletion", deleted.get("status") == "deleted" and _our_lines(deleted).origins == "-", deleted)
	var added := r.get_diff("added.txt", true)
	check("staged new file", added.get("status") == "new" and _our_lines(added).texts == ["hello"], added)

	# One file, different changes staged and unstaged: each side shows only its own.
	var staged := _our_lines(r.get_diff("both.txt", true))
	var unstaged := _our_lines(r.get_diff("both.txt", false))
	check("staged side matches git diff --cached", staged == _git_lines(repo, ["--cached", "--", "both.txt"]), [staged, _git_lines(repo, ["--cached", "--", "both.txt"])])
	check("unstaged side matches git diff", unstaged == _git_lines(repo, ["--", "both.txt"]), [unstaged, _git_lines(repo, ["--", "both.txt"])])

	check("a file without changes", r.get_diff("new_name.txt", false).get("kind") == "unchanged")


func _special_files() -> void:
	var repo := make_repo("special")
	commit_all(repo, "empty")
	var png := PackedByteArray([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0, 0xFF])
	var f := FileAccess.open(repo.path_join("icon.png"), FileAccess.WRITE)
	f.store_buffer(png)
	f.close()
	var big := "0123456789abcdef\n".repeat(160 * 1024) # About 2.7 MB of text.
	write(repo.path_join("big.txt"), big)
	var r := open(repo)
	check("binary file", r.get_diff("icon.png", false).get("kind") == "binary", r.get_diff("icon.png", false))
	check("file over 2 MB", r.get_diff("big.txt", false).get("kind") == "too_large", r.get_diff("big.txt", false).get("kind"))


# Files checked out with CRLF (core.autocrlf on Windows) must not show "\r" at line ends.
func _line_endings() -> void:
	var repo := make_repo("crlf")
	git(repo, ["config", "core.autocrlf", "true"])
	write(repo.path_join("a.txt"), "one\r\ntwo\r\n")
	commit_all(repo, "init")
	write(repo.path_join("a.txt"), "one\r\nTWO\r\n")
	var diff := open(repo).get_diff("a.txt", false)
	check("CRLF file: lines without \\r", _our_lines(diff).texts == ["one", "two", "TWO"], _our_lines(diff).texts)
