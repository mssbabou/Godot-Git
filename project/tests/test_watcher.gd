extends "res://tests/test_case.gd"
## GitWatcher: changes made outside the panel are reported (once things are quiet), changes the
## dock doesn't show are not (Godot's .godot folder, files git ignores, git's objects).

var reports: Array = []


func run() -> void:
	var repo := make_repo("watch")
	write(repo.path_join(".gitignore"), "build/\n*.tmp\n")
	write(repo.path_join("a.txt"), "a\n")
	DirAccess.make_dir_recursive_absolute(repo.path_join("sub"))
	write(repo.path_join("sub/b.txt"), "b\n")
	commit_all(repo, "First")
	DirAccess.make_dir_recursive_absolute(repo.path_join(".godot"))
	DirAccess.make_dir_recursive_absolute(repo.path_join("build"))

	var watcher := GitWatcher.new()
	check("start", watcher.start(repo, _on_changed) == OK and watcher.is_watching())
	await _settle()
	# macOS can still deliver the commit above (.git/refs/heads/main) after starting: fseventsd
	# numbers events when it gets to them, so "since now" lets a just-earlier one through. Harmless
	# for the dock (one more refresh at startup), but not what this check is about.
	reports.clear()

	write(repo.path_join("a.txt"), "a2\n")
	write(repo.path_join("sub/b.txt"), "b2\n")
	await _settle()
	check("edits reported once, together", reports.size() == 1 and _paths().has("a.txt") and _paths().has("sub/b.txt"), reports)

	reports.clear()
	write(repo.path_join(".godot/cache.bin"), "x")
	write(repo.path_join("build/out.exe"), "x")
	write(repo.path_join("notes.tmp"), "x")
	await _settle()
	check("ignored changes not reported (.godot, ignored folder, ignored file)", reports.is_empty(), reports)

	reports.clear()
	DirAccess.make_dir_recursive_absolute(repo.path_join("new_folder"))
	await _settle()
	reports.clear()
	write(repo.path_join("new_folder/c.txt"), "c\n")
	await _settle()
	check("a file in a folder made after starting is reported", _paths().has("new_folder/c.txt"), reports)

	reports.clear()
	git(repo, ["add", "-A"])
	git(repo, ["commit", "-q", "-m", "From a terminal"])
	await _settle()
	check("a terminal commit is reported (HEAD, index, refs)", Array(_paths()).any(func(p: String) -> bool: return p.begins_with(".git/")), reports)
	check("git's objects aren't", not Array(_paths()).any(func(p: String) -> bool: return p.begins_with(".git/objects")), _paths())

	watcher.stop()
	reports.clear()
	write(repo.path_join("a.txt"), "a3\n")
	await _settle()
	check("nothing after stop", reports.is_empty() and not watcher.is_watching(), reports)


func _on_changed(paths: PackedStringArray, first_change_msec: int) -> void:
	reports.append(paths)


func _paths() -> PackedStringArray:
	var all := PackedStringArray()
	for report: PackedStringArray in reports:
		all.append_array(report)
	return all


## Long enough for the notifications, the quiet period and the deferred call.
func _settle() -> void:
	var until := Time.get_ticks_msec() + 1200
	while Time.get_ticks_msec() < until:
		await tree.process_frame
