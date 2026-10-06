extends "res://tests/test_case.gd"
## get_diff, get_commit_files, get_commit_diff, get_file_bytes: what the Diff panel and History
## show, checked against `git diff` and `git show`.


func run() -> void:
	_unstaged_edit()
	_untracked_file()
	_staged_changes()
	_special_files()
	_settings_files()
	_line_endings()
	_commits()
	_history_order()
	_history_cache()
	_file_bytes()
	_line_changes()


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


## `.import` and `.uid` files: which settings changed, for the Diff panel's settings view.
func _settings_files() -> void:
	var repo := make_repo("settings")
	var import_text := "[remap]\n\nimporter=\"texture\"\nuid=\"uid://aaa\"\npath=\"res://.godot/imported/a.png-1.ctex\"\nmetadata={\n\"vram_texture\": false\n}\n\n[deps]\n\nsource_file=\"res://a.png\"\n\n[params]\n\ncompress/mode=0\nmipmaps/generate=false\nold/option=1\n"
	write(repo.path_join("a.png.import"), import_text)
	write(repo.path_join("a.gd.uid"), "uid://first\n")
	commit_all(repo, "first")
	write(repo.path_join("a.png.import"), import_text.replace("compress/mode=0", "compress/mode=2").replace("\"vram_texture\": false", "\"vram_texture\": true").replace("old/option=1\n", "new/option=3\n"))
	write(repo.path_join("a.gd.uid"), "uid://second\n")
	var r := open(repo)

	var diff := r.get_diff("a.png.import", false)
	var found := {}
	for change: Dictionary in diff.get("settings", []):
		found[change.section + "/" + change.key] = [change.get("old"), change.get("new")]
	check("settings: importer", diff.get("importer") == "texture", diff.get("importer"))
	check("settings: changed value", found.get("params/compress/mode") == ["0", "2"], found)
	check("settings: multi-line value", found.get("remap/metadata") == ["{\n\"vram_texture\": false\n}", "{\n\"vram_texture\": true\n}"], found.get("remap/metadata"))
	check("settings: added and removed keys", found.get("params/new/option") == [null, "3"] and found.get("params/old/option") == ["1", null], found)
	check("settings: unchanged keys left out", found.size() == 4, found.keys())

	var uid := r.get_diff("a.gd.uid", false)
	check("settings: uid", uid.get("settings") == [{ "section": "", "key": "uid", "old": "uid://first", "new": "uid://second" }], uid.get("settings"))
	check("settings: not for other files", not r.get_diff("a.gd.uid", true).has("settings") and not diff.is_empty(), "")

	git(repo, ["add", "-A"])
	git(repo, ["commit", "-q", "-m", "second"])
	r = open(repo)
	var in_commit := r.get_commit_diff(git(repo, ["rev-parse", "HEAD"]), "a.gd.uid")
	check("settings: in a commit", in_commit.get("settings") == uid.get("settings"), in_commit.get("settings"))


# Files checked out with CRLF (core.autocrlf on Windows) must not show "\r" at line ends.
func _line_endings() -> void:
	var repo := make_repo("crlf")
	git(repo, ["config", "core.autocrlf", "true"])
	write(repo.path_join("a.txt"), "one\r\ntwo\r\n")
	commit_all(repo, "init")
	write(repo.path_join("a.txt"), "one\r\nTWO\r\n")
	var diff := open(repo).get_diff("a.txt", false)
	check("CRLF file: lines without \\r", _our_lines(diff).texts == ["one", "two", "TWO"], _our_lines(diff).texts)


## `git show --name-status` of a commit as {path: letter}, against its first parent.
func _git_name_status(repo: String, commit: String) -> Dictionary:
	var result := {}
	var out := git(repo, ["diff", "--name-status", "-M", "--no-color", commit + "^1", commit]) if git(repo, ["rev-list", "--parents", "-n", "1", commit]).split(" ").size() > 1 else git(repo, ["show", "--name-status", "-M", "--no-color", "--format=", commit])
	for line: String in out.split("\n"):
		if line.is_empty():
			continue
		var parts := line.split("	")
		result[parts[parts.size() - 1]] = parts[0].left(1)
	return result


func _our_name_status(files: Array) -> Dictionary:
	var letters := { "new": "A", "modified": "M", "deleted": "D", "renamed": "R", "copied": "C", "typechange": "T" }
	var result := {}
	for file: Dictionary in files:
		result[file.path] = letters.get(file.status, "?")
	return result


func _commits() -> void:
	var repo := make_repo("commits")
	var body := ""
	for i in 20:
		body += "same line %d\n" % i
	write(repo.path_join("a.txt"), "one\ntwo\nthree\n")
	write(repo.path_join("old.txt"), body)
	write(repo.path_join("gone.txt"), "bye\n")
	commit_all(repo, "first")
	var first := git(repo, ["rev-parse", "HEAD"])
	write(repo.path_join("a.txt"), "one\nTWO\nthree\nfour\n")
	git(repo, ["mv", "old.txt", "renamed.txt"])
	git(repo, ["rm", "-q", "gone.txt"])
	write(repo.path_join("sub/new.txt"), "hello\n")
	commit_all(repo, "second")
	var second := git(repo, ["rev-parse", "HEAD"])
	var r := open(repo)

	var files := r.get_commit_files(second)
	check("commit files: same as git show", _our_name_status(files) == _git_name_status(repo, second), [_our_name_status(files), _git_name_status(repo, second)])
	var by_path := {}
	for file: Dictionary in files:
		by_path[file.path] = file
	check("commit files: rename keeps its old path", by_path.get("renamed.txt", {}).get("old_path") == "old.txt", by_path.get("renamed.txt"))
	check("commit files: line counts", by_path.get("a.txt", {}).get("added") == 2 and by_path.get("a.txt", {}).get("removed") == 1, by_path.get("a.txt"))
	var first_files := r.get_commit_files(first)
	check("first commit: every file added", _our_name_status(first_files) == { "a.txt": "A", "old.txt": "A", "gone.txt": "A" }, _our_name_status(first_files))
	check("short hash works", r.get_commit_files(second.left(7)).size() == files.size())
	check("unknown commit: nothing", r.get_commit_files("0123456789abcdef0123456789abcdef01234567").is_empty())

	var diff := r.get_commit_diff(second, "a.txt")
	var theirs := _git_lines(repo, [second + "^", second, "--", "a.txt"])
	check("commit diff: same lines as git", _our_lines(diff).origins == theirs.origins and _our_lines(diff).texts == theirs.texts, [_our_lines(diff), theirs])
	check("commit diff: line numbers", _numbers_consistent(diff))
	check("commit diff of a deleted file", r.get_commit_diff(second, "gone.txt").get("status") == "deleted" and _our_lines(r.get_commit_diff(second, "gone.txt")).origins == "-")
	check("commit diff of a file it didn't touch", r.get_commit_diff(second, "zzz.txt").get("kind") == "unchanged")

	# A merge shows what it brought into the branch: its changes against the first parent.
	git(repo, ["checkout", "-q", "-b", "feature"])
	write(repo.path_join("feature.txt"), "feature\n")
	commit_all(repo, "feature work")
	git(repo, ["checkout", "-q", "main"])
	write(repo.path_join("main.txt"), "main\n")
	commit_all(repo, "main work")
	git(repo, ["merge", "-q", "--no-edit", "feature"])
	var merge := git(repo, ["rev-parse", "HEAD"])
	check("merge commit: files against the first parent", _our_name_status(r.get_commit_files(merge)) == { "feature.txt": "A" }, _our_name_status(r.get_commit_files(merge)))


# History lists a commit before its parents, even when they were all made in the same second
# (sorting by time alone left them in any order).
func _history_order() -> void:
	var repo := make_repo("order")
	OS.set_environment("GIT_AUTHOR_DATE", "2026-01-01T12:00:00")
	OS.set_environment("GIT_COMMITTER_DATE", "2026-01-01T12:00:00")
	for i in 4:
		write(repo.path_join("f%d.txt" % i), "x\n")
		commit_all(repo, "commit %d" % i)
	git(repo, ["checkout", "-q", "-b", "side", "HEAD~2"])
	write(repo.path_join("side.txt"), "x\n")
	commit_all(repo, "side")
	git(repo, ["checkout", "-q", "main"])
	git(repo, ["merge", "-q", "--no-edit", "side"])
	OS.unset_environment("GIT_AUTHOR_DATE")
	OS.unset_environment("GIT_COMMITTER_DATE")

	var position := {}
	var commits := open(repo).get_commits(50)
	for i in commits.size():
		position[commits[i].hash] = i
	var ok := commits.size() == 6
	for c: Dictionary in commits:
		for parent in git(repo, ["rev-list", "--parents", "-n", "1", c.hash]).split(" ").slice(1):
			ok = ok and position.get(parent, -1) > position[c.hash]
	check("history: every commit before its parents, same-second commits too", ok, commits.map(func(c): return c.summary))


# get_commits reuses its last result while no branch moved; anything that moves one must show.
func _history_cache() -> void:
	var shared := make_shared("cache")
	var r := open(shared.mine)
	check("history: first commit listed", r.get_commits(50).size() == 1)
	write(shared.mine.path_join("x.txt"), "changed in a terminal\n")
	commit_all(shared.mine, "From a terminal")
	var commits := r.get_commits(50)
	check("history: a commit made elsewhere shows", commits.size() == 2 and commits[0].summary == "From a terminal", commits.map(func(c): return c.summary))
	check("history: not pushed yet", commits[0].unpushed)
	git(shared.mine, ["push", "-q"])
	check("history: pushed from a terminal, no longer marked", not r.get_commits(50)[0].unpushed)
	git(shared.mine, ["commit", "-q", "--amend", "-m", "Amended in a terminal"])
	check("history: an amend shows", r.get_commits(50)[0].summary == "Amended in a terminal")
	var result := r.get_commits(50)
	result[0].summary = "changed by the caller"
	check("history: callers get their own copy", r.get_commits(50)[0].summary == "Amended in a terminal")


func _bytes(path: String, data: PackedByteArray) -> void:
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer(data)
	f.close()


## Whether p_bytes are exactly git's version p_spec ("HEAD:art/icon.png"): git hashes the bytes,
## and the hash must be the blob id git has for that version. (Reading binary output through a
## shell redirect doesn't survive OS.execute's quoting; see gotcha 25.)
func _is_git_version(repo: String, spec: String, bytes: PackedByteArray) -> bool:
	var file := dir.path_join("hash-me.bin")
	_bytes(file, bytes)
	return git(repo, ["hash-object", file]) == git(repo, ["rev-parse", spec])


# The image previews read a file's bytes in each version. Binary content, each side different.
func _file_bytes() -> void:
	var repo := make_repo("bytes")
	var v1 := PackedByteArray([0x89, 0x50, 0x4E, 0x47, 0, 1, 2, 3, 255, 13, 10, 0])
	var v2 := v1.duplicate()
	v2.append_array(PackedByteArray([4, 5, 6]))
	var v3 := v2.duplicate()
	v3[5] = 99
	_bytes(repo.path_join("art/icon.png"), v1)
	write(repo.path_join("gone.txt"), "bye\n")
	commit_all(repo, "first")
	_bytes(repo.path_join("art/icon.png"), v2)
	git(repo, ["rm", "-q", "gone.txt"])
	commit_all(repo, "second")
	var second := git(repo, ["rev-parse", "HEAD"])
	_bytes(repo.path_join("art/icon.png"), v3)
	git(repo, ["add", "art/icon.png"])
	var v4 := v3.duplicate()
	v4.append(7)
	_bytes(repo.path_join("art/icon.png"), v4)
	var r := open(repo)

	check("bytes: HEAD is exactly git's version", _is_git_version(repo, "HEAD:art/icon.png", r.get_file_bytes("HEAD", "art/icon.png").bytes) and r.get_file_bytes("HEAD", "art/icon.png").bytes == v2)
	check("bytes: the staged version", r.get_file_bytes("index", "art/icon.png").bytes == v3)
	check("bytes: the file on disk", r.get_file_bytes("workdir", "art/icon.png").bytes == v4)
	check("bytes: a commit's first parent", r.get_file_bytes(second + "^1", "art/icon.png").bytes == v1)
	check("bytes: a file deleted in that commit is gone from it", not r.get_file_bytes(second, "gone.txt").exists and r.get_file_bytes(second + "^1", "gone.txt").exists)
	check("bytes: no first parent for the first commit", not r.get_file_bytes(git(repo, ["rev-list", "--max-parents=0", "HEAD"]) + "^1", "art/icon.png").exists)
	check("bytes: not LFS", r.get_file_bytes("HEAD", "art/icon.png").lfs == "")


# The script editor's change marks: diff_lines, checked against the hunk headers of git diff -U0.
func _line_changes() -> void:
	var old := "a\nb\nc\nd\ne\n"
	var h := GitRepository.diff_lines(old, "a\nB\nc\nd\ne\n")
	check("one changed line", h.size() == 1 and h[0].old_start == 2 and h[0].old_count == 1 and h[0].new_start == 2 and h[0].new_count == 1 and h[0].old_lines == PackedStringArray(["b"]), h)
	h = GitRepository.diff_lines(old, "a\nb\nx\ny\nc\nd\ne\n")
	check("added lines: no old lines", h.size() == 1 and h[0].old_count == 0 and h[0].new_start == 3 and h[0].new_count == 2, h)
	h = GitRepository.diff_lines(old, "a\nd\ne\n")
	check("deleted lines: after the line they followed", h.size() == 1 and h[0].new_count == 0 and h[0].new_start == 1 and h[0].old_lines == PackedStringArray(["b", "c"]), h)
	h = GitRepository.diff_lines(old, "c\nd\ne\n")
	check("deleted at the top", h.size() == 1 and h[0].new_start == 0 and h[0].new_count == 0, h)
	check("CRLF and a missing final newline don't count", GitRepository.diff_lines(old, "a\nb\nc\nd\ne").is_empty(), GitRepository.diff_lines(old, "a\nb\nc\nd\ne"))
	check("nothing changed", GitRepository.diff_lines(old, old).is_empty())
	check("new file: all added", GitRepository.diff_lines("", "x\ny\n")[0].new_count == 2)

	# A bigger edit, against git itself.
	var before := PackedStringArray()
	for i in 40:
		before.append("line %d" % i)
	var after := before.duplicate()
	after[3] = "changed 3"
	after.remove_at(10)
	after.insert(20, "new A")
	after.insert(21, "new B")
	after[30] = "changed 30"
	after.remove_at(35)
	after.remove_at(35)
	var dir_path := dir.path_join("line-changes")
	DirAccess.make_dir_recursive_absolute(dir_path)
	write(dir_path.path_join("old.txt"), "\n".join(before) + "\n")
	write(dir_path.path_join("new.txt"), "\n".join(after) + "\n")
	var out := []
	OS.execute("git", ["-C", dir_path, "diff", "--no-index", "-U0", "old.txt", "new.txt"], out, true)
	var git_headers := PackedStringArray()
	for line in "".join(out).split("\n"):
		if line.begins_with("@@"):
			git_headers.append(line.get_slice(" @@", 0))
	var ours := PackedStringArray()
	for hunk in GitRepository.diff_lines("\n".join(before) + "\n", "\n".join(after) + "\n"):
		ours.append("@@ -%d,%d +%d,%d" % [hunk.old_start, hunk.old_count, hunk.new_start, hunk.new_count])
	# git leaves out ",1"; spell its headers out the same way.
	var normalized := PackedStringArray()
	for header in git_headers:
		var parts := header.split(" ")
		for i in [1, 2]:
			if not parts[i].contains(","):
				parts[i] += ",1"
		normalized.append(" ".join(parts))
	check("same hunks as git diff -U0", ours == normalized and not ours.is_empty(), [ours, normalized])

