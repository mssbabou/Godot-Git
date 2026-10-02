extends "res://tests/test_case.gd"
## Ignoring files: which .gitignore a rule goes into, and that the files a rule would hide (asked
## before anything is written) are exactly the ones git stops listing once it is written.


func run() -> void:
	_ignore_file()
	_preview_matches_git("type, project in a subfolder", "game", ["*.psd"])
	_preview_matches_git("one file", "game", ["/art/boss.psd", "/art/boss.psd.import"])
	_preview_matches_git("a folder", "game", ["/art/raw/"])
	_preview_matches_git("type, at the root", "", ["*.psd"])
	_line_endings()


# The nearest .gitignore at the folder or above it, else the folder's own.
func _ignore_file() -> void:
	var repo := make_repo("ignore-file")
	write(repo.path_join("game/project.godot"), "")
	var r := open(repo)
	check("no .gitignore anywhere: the folder's own", r.get_ignore_file("game") == "game/.gitignore", r.get_ignore_file("game"))
	write(repo.path_join(".gitignore"), "*.tmp\n")
	check("one at the root: that one", r.get_ignore_file("game") == ".gitignore", r.get_ignore_file("game"))
	write(repo.path_join("game/.gitignore"), ".godot/\n")
	check("one in the folder: that one", r.get_ignore_file("game") == "game/.gitignore", r.get_ignore_file("game"))
	check("the root itself", r.get_ignore_file("") == ".gitignore", r.get_ignore_file(""))


func _untracked(repo: String) -> PackedStringArray:
	var out := PackedStringArray()
	for line in git(repo, ["status", "--porcelain=v1", "-uall"]).split("\n", false):
		if line.begins_with("?? "):
			out.append(line.substr(3).strip_edges())
	out.sort()
	return out


func _preview_matches_git(label: String, dir: String, lines: PackedStringArray) -> void:
	var repo := make_repo("ignore-" + label.replace(" ", "-").replace(",", ""))
	var base := repo.path_join(dir) if dir else repo
	write(repo.path_join("README.md"), "x\n")
	write(repo.path_join(".gitignore"), "*.log\n")
	commit_all(repo, "First")
	for file in ["art/boss.psd", "art/boss.psd.import", "art/raw/sky.psd", "art/raw/notes.txt", "art/hero.png", "debug.log", "x.psd"]:
		write(base.path_join(file), "data\n")
	write(repo.path_join("outside.psd"), "data\n") # Outside the project folder.

	var r := open(repo)
	var file := r.get_ignore_file(dir)
	# Lines are given relative to the project folder; anchored ones are written relative to the
	# chosen file's folder, as the Ignore dialog does.
	var file_dir := file.get_base_dir()
	var written := PackedStringArray()
	for line in lines:
		if line.begins_with("/"):
			var full := (dir + line) if dir else line.substr(1)
			line = "/" + (full.trim_prefix(file_dir + "/") if file_dir else full)
		written.append(line)
	lines = written
	var before := _untracked(repo)
	var preview := r.get_paths_ignored_by(file, lines, before)
	preview.sort()
	check("%s: the rule hides something" % label, not preview.is_empty(), preview)
	check("%s: nothing written yet" % label, _untracked(repo) == before)
	check("%s: added" % label, r.add_ignore_lines(file, lines) == OK, r.get_last_error())
	var after := _untracked(repo)
	var hidden := PackedStringArray()
	for path in before:
		if not after.has(path):
			hidden.append(path)
	hidden.sort()
	hidden.erase(file) # The .gitignore it wrote is new itself when it didn't exist.
	check("%s: the preview names exactly what git hides" % label, preview == hidden, "preview %s, git %s" % [preview, hidden])


# A file with CRLF endings keeps them; a missing last newline gets one first.
func _line_endings() -> void:
	var repo := make_repo("ignore-crlf")
	write(repo.path_join("README.md"), "x\n")
	commit_all(repo, "First")
	var f := FileAccess.open(repo.path_join(".gitignore"), FileAccess.WRITE)
	f.store_string(".godot/\r\n*.tmp")
	f.close()
	var r := open(repo)
	check("added to a CRLF file", r.add_ignore_lines(".gitignore", ["*.psd"]) == OK)
	var text := FileAccess.get_file_as_string(repo.path_join(".gitignore"))
	check("CRLF kept, the last line finished first", text == ".godot/\r\n*.tmp\r\n*.psd\r\n", text.c_escape())
