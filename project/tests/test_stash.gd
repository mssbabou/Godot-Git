extends "res://tests/test_case.gd"
## Stashes: stash what's staged or everything, list them (terminal stashes too), their files and
## diffs, restore all or nothing, delete. Checked against what `git stash list` and git's status say.


func run() -> void:
	_nothing_to_stash()
	_stash_staged_and_restore()
	_stash_everything_and_restore()
	_stash_staged_half_of_a_file()
	_restore_refused_for_your_edits()
	_restore_refused_for_conflicting_commits()
	_restore_after_unrelated_commits()
	_terminal_stashes_and_delete()
	_switch_refusal_names_files()
	_restore_over_regenerated_uid()


## a.txt and b.txt, five lines each, committed.
func _repo(name: String) -> String:
	var repo := make_repo(name)
	write(repo.path_join("a.txt"), "a1\na2\na3\na4\na5\n")
	write(repo.path_join("b.txt"), "b1\nb2\nb3\nb4\nb5\n")
	commit_all(repo, "first")
	return repo


func _nothing_to_stash() -> void:
	var r := open(_repo("stash-nothing"))
	check("stash: none yet", r.get_stashes().is_empty(), r.get_stashes())
	check("stash: nothing to stash", r.stash(false) != OK and GitRepository.get_last_error().contains("nothing to stash"), GitRepository.get_last_error())


func _stash_staged_and_restore() -> void:
	var repo := _repo("stash-staged")
	write(repo.path_join("a.txt"), "a1\nA2 staged\na3\na4\na5\n")
	git(repo, ["add", "a.txt"])
	write(repo.path_join("b.txt"), "b1\nb2\nB3 unstaged\nb4\nb5\n")
	write(repo.path_join("c.txt"), "new\n")
	var r := open(repo)

	check("stash staged", r.stash(true) == OK, GitRepository.get_last_error())
	var stashes := r.get_stashes()
	check("stash staged: listed, named after its file, on main", stashes.size() == 1 and stashes[0].message == "a.txt" and stashes[0].branch == "main", stashes)
	check("stash staged: git sees it too", git(repo, ["stash", "list"]).split("\n").size() == 1, git(repo, ["stash", "list"]))
	check("stash staged: a.txt set aside", read(repo.path_join("a.txt")) == "a1\na2\na3\na4\na5\n" and git(repo, ["diff", "--cached", "--name-only"]) == "", git(repo, ["status", "--short"]))
	check("stash staged: the rest untouched", read(repo.path_join("b.txt")).contains("B3 unstaged") and exists(repo.path_join("c.txt")), git(repo, ["status", "--short"]))

	var files := r.get_stash_files(stashes[0].hash)
	check("stash staged: its files", files.size() == 1 and files[0].path == "a.txt" and files[0].added == 1 and files[0].removed == 1, files)
	var diff := r.get_stash_diff(stashes[0].hash, "a.txt")
	check("stash staged: its diff", diff.get("kind") == "text" and "A2 staged" in diff.hunks[0].text, diff)

	check("restore", r.restore_stash(stashes[0].hash) == OK, GitRepository.get_last_error())
	check("restore: the edit is back, staged again", read(repo.path_join("a.txt")).contains("A2 staged") and git(repo, ["diff", "--cached", "--name-only"]) == "a.txt", git(repo, ["status", "--short"]))
	check("restore: the stash is gone", r.get_stashes().is_empty() and git(repo, ["stash", "list"]) == "", git(repo, ["stash", "list"]))
	check("restore: the other edits untouched", read(repo.path_join("b.txt")).contains("B3 unstaged") and exists(repo.path_join("c.txt")))


func _stash_everything_and_restore() -> void:
	var repo := _repo("stash-all")
	write(repo.path_join("a.txt"), "a1\nA2\na3\na4\na5\n")
	git(repo, ["add", "a.txt"])
	write(repo.path_join("b.txt"), "b1\nb2\nB3\nb4\nb5\n")
	write(repo.path_join("c.txt"), "new\n")
	var r := open(repo)
	check("stash all", r.stash(false) == OK, GitRepository.get_last_error())
	check("stash all: nothing left, new file too", git(repo, ["status", "--porcelain"]) == "", git(repo, ["status", "--porcelain"]))
	var stash: Dictionary = r.get_stashes()[0]
	check("stash all: named after its files", stash.message == "a.txt, b.txt, c.txt", stash.message)
	write(repo.path_join("d.txt"), "another\n")
	check("stash with a name", r.stash(false, "  half-done jump rework ") == OK and r.get_stashes()[0].message == "half-done jump rework", r.get_stashes()[0])
	check("named stash: git shows the name too", git(repo, ["stash", "list"]).contains("half-done jump rework"), git(repo, ["stash", "list"]))
	r.restore_stash(r.get_stashes()[0].hash)
	DirAccess.remove_absolute(repo.path_join("d.txt"))
	var paths := []
	for file: Dictionary in r.get_stash_files(stash.hash):
		paths.append(file.path)
	paths.sort()
	check("stash all: its files, the new one included", paths == ["a.txt", "b.txt", "c.txt"], paths)
	check("stash all: a new file's diff", r.get_stash_diff(stash.hash, "c.txt").get("kind") == "text", r.get_stash_diff(stash.hash, "c.txt"))
	check("restore all", r.restore_stash(stash.hash) == OK, GitRepository.get_last_error())
	check("restore all: everything back as it was", git(repo, ["status", "--porcelain"]).replace("\r", "") == "M  a.txt\n M b.txt\n?? c.txt", git(repo, ["status", "--porcelain"]))


# One file, one change staged and another not: stashing what's staged takes only the staged one.
# (libgit2's own path-limited stash lost unstaged edits here, which is why this goes through git.)
func _stash_staged_half_of_a_file() -> void:
	var repo := _repo("stash-half")
	write(repo.path_join("a.txt"), "a1\nA2 staged\na3\na4\na5\n")
	git(repo, ["add", "a.txt"])
	write(repo.path_join("a.txt"), "a1\nA2 staged\na3\na4\nA5 not staged\n")
	var r := open(repo)
	check("half staged: stash", r.stash(true) == OK, GitRepository.get_last_error())
	check("half staged: the unstaged change stays", read(repo.path_join("a.txt")) == "a1\na2\na3\na4\nA5 not staged\n" and git(repo, ["diff", "--cached", "--name-only"]) == "", read(repo.path_join("a.txt")))
	git(repo, ["checkout", "--", "a.txt"]) # Out of the way: a restore refuses while the file has edits.
	check("half staged: restore", r.restore_stash(r.get_stashes()[0].hash) == OK, GitRepository.get_last_error())
	check("half staged: the staged change is back, staged", read(repo.path_join("a.txt")) == "a1\nA2 staged\na3\na4\na5\n" and git(repo, ["diff", "--cached", "--name-only"]) == "a.txt", git(repo, ["status", "--short"]))


func _restore_refused_for_your_edits() -> void:
	var repo := _repo("stash-busy")
	write(repo.path_join("b.txt"), "b1\nb2\nB3 stashed\nb4\nb5\n")
	var r := open(repo)
	r.stash(false)
	write(repo.path_join("b.txt"), "b1\nb2\nb3\nb4\nB5 mine now\n")
	var hash: String = r.get_stashes()[0].hash
	check("restore refused: your edits to the same file", r.restore_stash(hash) != OK and GitRepository.get_last_error().contains("b.txt"), GitRepository.get_last_error())
	check("restore refused: nothing changed", read(repo.path_join("b.txt")) == "b1\nb2\nb3\nb4\nB5 mine now\n" and r.get_stashes().size() == 1, read(repo.path_join("b.txt")))


func _restore_refused_for_conflicting_commits() -> void:
	var repo := _repo("stash-conflict")
	write(repo.path_join("a.txt"), "a1\nA2 stashed\na3\na4\na5\n")
	var r := open(repo)
	r.stash(false)
	write(repo.path_join("a.txt"), "a1\nA2 committed\na3\na4\na5\n")
	commit_all(repo, "same line")
	var head := git(repo, ["rev-parse", "HEAD"])
	var hash: String = r.get_stashes()[0].hash
	check("restore refused: conflicts with a newer commit", r.restore_stash(hash) != OK and GitRepository.get_last_error().contains("same lines"), GitRepository.get_last_error())
	check("restore refused: nothing changed, stash kept", git(repo, ["status", "--porcelain"]) == "" and git(repo, ["rev-parse", "HEAD"]) == head and r.get_stashes().size() == 1, git(repo, ["status", "--porcelain"]))
	check("restore refused: no conflict markers", not read(repo.path_join("a.txt")).contains("<<<<<<<"))


func _restore_after_unrelated_commits() -> void:
	var repo := _repo("stash-moved-on")
	write(repo.path_join("a.txt"), "a1\nA2 stashed\na3\na4\na5\n")
	var r := open(repo)
	r.stash(false)
	write(repo.path_join("a.txt"), "a1\na2\na3\na4\nA5 committed\n")
	commit_all(repo, "other line")
	check("restore after new commits", r.restore_stash(r.get_stashes()[0].hash) == OK, GitRepository.get_last_error())
	check("restore after new commits: both changes", read(repo.path_join("a.txt")) == "a1\nA2 stashed\na3\na4\nA5 committed\n", read(repo.path_join("a.txt")))


func _terminal_stashes_and_delete() -> void:
	var repo := _repo("stash-terminal")
	write(repo.path_join("a.txt"), "changed\n")
	git(repo, ["stash", "push", "-q", "-m", "trying something"])
	write(repo.path_join("b.txt"), "changed\n")
	git(repo, ["stash", "-q"])
	var r := open(repo)
	var stashes := r.get_stashes()
	check("terminal stashes: listed, newest first", stashes.size() == 2 and stashes[1].message == "trying something" and stashes[1].branch == "main", stashes)
	check("terminal stashes: git's own WIP message", String(stashes[0].message).ends_with("first") and stashes[0].branch == "main", stashes[0])
	check("delete", r.delete_stash(stashes[1].hash) == OK, GitRepository.get_last_error())
	check("delete: that one is gone, the other kept", r.get_stashes().size() == 1 and r.get_stashes()[0].hash == stashes[0].hash and git(repo, ["stash", "list"]).split("\n").size() == 1, git(repo, ["stash", "list"]))
	check("delete: a stash that's gone", r.delete_stash(stashes[1].hash) != OK)


# A switch your changes are in the way of: ERR_BUSY (the panel then offers to stash them), naming
# the files. After stashing, the switch goes through.
func _switch_refusal_names_files() -> void:
	var repo := _repo("stash-switch")
	git(repo, ["checkout", "-q", "-b", "other"])
	write(repo.path_join("a.txt"), "other branch\n")
	commit_all(repo, "other")
	git(repo, ["checkout", "-q", "main"])
	write(repo.path_join("a.txt"), "my edit\n")
	var r := open(repo)
	check("switch refused: ERR_BUSY", r.checkout_branch("other") == ERR_BUSY, GitRepository.get_last_error())
	check("switch refused: names the file", GitRepository.get_last_error().contains("a.txt") and GitRepository.get_last_error().contains("stash"), GitRepository.get_last_error())
	check("switch after stashing", r.stash(false) == OK and r.checkout_branch("other") == OK and read(repo.path_join("a.txt")) == "other branch\n", GitRepository.get_last_error())


# A new script and its .uid, stashed. Godot writes a fresh .uid as soon as the stashed one is
# gone; restoring must put the stashed one back (it's the uid scenes refer to), not refuse.
func _restore_over_regenerated_uid() -> void:
	var repo := _repo("stash-uid")
	write(repo.path_join("hero.gd"), "extends Node\n")
	write(repo.path_join("hero.gd.uid"), "uid://original\n")
	git(repo, ["add", "hero.gd", "hero.gd.uid"])
	var r := open(repo)
	check("uid: stash", r.stash(true) == OK, GitRepository.get_last_error())
	write(repo.path_join("hero.gd.uid"), "uid://regenerated\n") # What the editor does.
	check("uid: restore over Godot's new one", r.restore_stash(r.get_stashes()[0].hash) == OK, GitRepository.get_last_error())
	check("uid: the stashed uid is back", read(repo.path_join("hero.gd.uid")) == "uid://original\n" and exists(repo.path_join("hero.gd")), read(repo.path_join("hero.gd.uid")))
	# A tracked .import you changed is yours, though: that still blocks.
	write(repo.path_join("a.txt"), "stashed\n")
	r.stash(false)
	write(repo.path_join("a.txt"), "mine\n")
	check("uid: your own edits still block", r.restore_stash(r.get_stashes()[0].hash) != OK)
