extends "res://tests/test_case.gd"
## Pulling into conflicts on purpose (pull(true), the panel's Start Merge): your uncommitted edit
## comes back as a conflict to resolve, or your commits leave a merge git knows; Finish and Abort
## end either; and what it still refuses. Checked against what git says.


func run() -> void:
	_your_edit_finish()
	_your_edit_abort()
	_your_commits_finish()
	_your_commits_keep_other_edits()
	_staged_refuses()
	_scene_both_add()
	_scene_same_property()
	_scene_mixed_with_text()
	_scene_edit_carried()


## You edited x.txt's line and haven't committed; the teammate pushed a change to the same line.
func _edit_against_teammate(name: String) -> Dictionary:
	var s := make_shared(name)
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-mine\n")
	write(s.mine.path_join("z.txt"), "z-mine\n") # Unrelated: must stay as it is throughout.
	return s


func _your_edit_finish() -> void:
	var s := _edit_against_teammate("edit-finish")
	var r := open(s.mine)
	check("edit: refused first, naming the file", r.pull() != OK and Array(r.get_pull_result().conflicts) == ["x.txt"], GitApi.get_last_error())
	check("edit: start merge", r.pull(true) == OK, GitApi.get_last_error())
	check("edit: pulled", git(s.mine, ["rev-parse", "HEAD"]) == git(s.mine, ["rev-parse", "origin/main"]))
	check("edit: the conflict is reported", Array(r.get_pull_result().conflicts) == ["x.txt"], r.get_pull_result())
	var op := r.get_operation()
	check("edit: shown as a merge in progress", op.kind == "pull" and Array(op.conflicts) == ["x.txt"], op)
	check("edit: git sees the conflict too", git(s.mine, ["diff", "--name-only", "--diff-filter=U"]) == "x.txt", git(s.mine, ["status", "--porcelain"]))
	var conflict := r.get_conflict("x.txt")
	check("edit: mine is my edit, theirs the pulled line", conflict.mine_label == "your changes" and conflict.theirs_label == "origin/main" and conflict.blocks[0].mine == "x-mine\n" and conflict.blocks[0].theirs == "x-theirs\n", conflict)
	check("edit: commit refuses meanwhile", r.commit("nope") != OK, GitApi.get_last_error())
	check("edit: resolve", r.resolve_conflict("x.txt", "x-both\n") == OK, GitApi.get_last_error())
	check("edit: resolved as an unstaged change, like my edit was", git(s.mine, ["status", "--porcelain", "x.txt"]) == "M x.txt", git(s.mine, ["status", "--porcelain"]))
	check("edit: finish", r.continue_operation() == OK, GitApi.get_last_error())
	check("edit: nothing in progress, no copies left", r.get_operation().kind == "" and not exists(s.mine.path_join(".git/godot-git-pull-state.json")) and not DirAccess.dir_exists_absolute(s.mine.path_join(".git/godot-git-pull")))
	check("edit: the file has the resolution, the unrelated edit stayed", read(s.mine.path_join("x.txt")) == "x-both\n" and read(s.mine.path_join("z.txt")) == "z-mine\n")
	check("edit: no stash anywhere", git(s.mine, ["stash", "list"]) == "")


func _your_edit_abort() -> void:
	var s := _edit_against_teammate("edit-abort")
	var head := git(s.mine, ["rev-parse", "HEAD"])
	var mine_bytes := FileAccess.get_file_as_bytes(s.mine.path_join("x.txt"))
	var r := open(s.mine)
	check("abort: start merge", r.pull(true) == OK, GitApi.get_last_error())
	check("abort", r.abort_operation() == OK, GitApi.get_last_error())
	check("abort: back where it was", git(s.mine, ["rev-parse", "HEAD"]) == head, git(s.mine, ["log", "--oneline", "-2"]))
	check("abort: my edit back byte for byte", FileAccess.get_file_as_bytes(s.mine.path_join("x.txt")) == mine_bytes, read(s.mine.path_join("x.txt")))
	check("abort: unrelated edit untouched", read(s.mine.path_join("z.txt")) == "z-mine\n")
	check("abort: as git sees it, just my two edits", git(s.mine, ["status", "--porcelain"]) == "M x.txt\n M z.txt", git(s.mine, ["status", "--porcelain"]))
	check("abort: nothing in progress, no copies left", r.get_operation().kind == "" and not exists(s.mine.path_join(".git/godot-git-pull-state.json")))


## Your commit and the teammate's change the same line of x.txt: a merge git knows.
func _commits_against_teammate(name: String) -> Dictionary:
	var s := make_shared(name)
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-mine\n")
	commit_all(s.mine, "My x")
	return s


func _your_commits_finish() -> void:
	var s := _commits_against_teammate("commits-finish")
	var r := open(s.mine)
	check("commits: refused first", r.pull() != OK and Array(r.get_pull_result().conflicts) == ["x.txt"], GitApi.get_last_error())
	check("commits: refused cleanly", r.get_operation().kind == "" and git(s.mine, ["status", "--porcelain"]) == "", git(s.mine, ["status", "--porcelain"]))
	check("commits: start merge", r.pull(true) == OK, GitApi.get_last_error())
	check("commits: a merge git knows, stopped at x.txt", r.get_operation().kind == "merge" and Array(r.get_operation().conflicts) == ["x.txt"], r.get_operation())
	check("commits: resolve", r.resolve_conflict_with("x.txt", "theirs") == OK, GitApi.get_last_error())
	check("commits: finish", r.continue_operation() == OK, GitApi.get_last_error())
	check("commits: a merge commit with both parents and the pull's message", git(s.mine, ["rev-list", "--parents", "-n", "1", "HEAD"]).split(" ").size() == 3 and git(s.mine, ["log", "-1", "--format=%s"]) == "Merge remote-tracking branch 'origin/main'", git(s.mine, ["log", "-1", "--format=%s %p"]))
	check("commits: x.txt is theirs", read(s.mine.path_join("x.txt")) == "x-theirs\n")


func _your_commits_keep_other_edits() -> void:
	var s := _commits_against_teammate("commits-other")
	write(s.mine.path_join("z.txt"), "z-mine\n") # Unrelated, uncommitted.
	var r := open(s.mine)
	check("other edits: start merge", r.pull(true) == OK, GitApi.get_last_error())
	check("other edits: unrelated edit still there", read(s.mine.path_join("z.txt")) == "z-mine\n")
	r.resolve_conflict_with("x.txt", "mine")
	check("other edits: finish", r.continue_operation() == OK, GitApi.get_last_error())
	check("other edits: not in the merge commit", git(s.mine, ["show", "HEAD:z.txt"]) == "z1" and read(s.mine.path_join("z.txt")) == "z-mine\n", git(s.mine, ["show", "HEAD:z.txt"]))


## Finishing commits the index, so a staged change would end up in the merge: refused up front.
func _staged_refuses() -> void:
	var s := _commits_against_teammate("staged")
	write(s.mine.path_join("z.txt"), "z-staged\n")
	git(s.mine, ["add", "z.txt"])
	var head := git(s.mine, ["rev-parse", "HEAD"])
	var r := open(s.mine)
	check("staged: start merge refuses", r.pull(true) != OK and GitApi.get_last_error().contains("staged"), GitApi.get_last_error())
	check("staged: nothing changed", git(s.mine, ["rev-parse", "HEAD"]) == head and r.get_operation().kind == "" and git(s.mine, ["diff", "--cached", "--name-only"]) == "z.txt", git(s.mine, ["status", "--porcelain"]))


## A small scene: a root, two children with unique_ids, one ext_resource. Scenes are merged section
## by section (src/scene/scene_merge.h), so two sides adding nodes at the end is no conflict.
const SCENE_BASE := "[gd_scene format=4]\n\n[ext_resource type=\"Script\" path=\"res://player.gd\" id=\"1_a\"]\n\n[node name=\"Root\" type=\"Node2D\" unique_id=100]\nscript = ExtResource(\"1_a\")\n\n[node name=\"A\" type=\"Node2D\" parent=\".\" unique_id=200]\nposition = Vector2(10, 10)\n\n[node name=\"B\" type=\"Node2D\" parent=\".\" unique_id=300]\nposition = Vector2(20, 20)\n"


## The base scene with one more node at the end.
func _scene_with_node(node: String, unique_id: int) -> String:
	return SCENE_BASE + "\n[node name=\"%s\" type=\"Node2D\" parent=\".\" unique_id=%d]\nposition = Vector2(30, 30)\n" % [node, unique_id]


## Both sides add a different node at the end of level.tscn, both committed: merged by the scene
## merge, not left to git's line merge, which would conflict on the neighboring lines.
func _scene_both_add() -> void:
	var s := make_shared("scene-both")
	write(s.mine.path_join("level.tscn"), SCENE_BASE)
	commit_all(s.mine, "Add level")
	git(s.mine, ["push", "-q"])
	git(s.theirs, ["pull", "-q", "--no-rebase"])
	write(s.mine.path_join("level.tscn"), _scene_with_node("Mine", 400))
	commit_all(s.mine, "Mine node")
	teammate_pushes(s, "level.tscn", _scene_with_node("Theirs", 500))
	var r := open(s.mine)
	check("scene both add: pull", r.pull() == OK, GitApi.get_last_error())
	var pull_result := r.get_pull_result()
	check("scene both add: merged, settled by the scene merge", pull_result.merged and Array(pull_result.scenes) == ["level.tscn"] and Array(pull_result.conflicts).is_empty(), pull_result)
	var text := read(s.mine.path_join("level.tscn"))
	check("scene both add: both nodes in the file", text.contains("name=\"Mine\"") and text.contains("name=\"Theirs\""), text)
	check("scene both add: nothing left over", git(s.mine, ["status", "--porcelain"]) == "", git(s.mine, ["status", "--porcelain"]))
	check("scene both add: a merge commit with both parents", git(s.mine, ["log", "-1", "--format=%P"]).split(" ").size() == 2, git(s.mine, ["log", "-1", "--format=%P"]))
	check("scene both add: HEAD has the file as written", git(s.mine, ["show", "HEAD:level.tscn"]) == text.strip_edges(), git(s.mine, ["show", "HEAD:level.tscn"]))


## Both sides change the same property of the same node differently: a real clash, so nothing
## changes and the file is reported as a conflict.
func _scene_same_property() -> void:
	var s := make_shared("scene-same")
	write(s.mine.path_join("level.tscn"), SCENE_BASE)
	commit_all(s.mine, "Add level")
	git(s.mine, ["push", "-q"])
	git(s.theirs, ["pull", "-q", "--no-rebase"])
	write(s.mine.path_join("level.tscn"), SCENE_BASE.replace("position = Vector2(10, 10)", "position = Vector2(11, 11)"))
	commit_all(s.mine, "Mine moves A")
	var head := git(s.mine, ["rev-parse", "HEAD"])
	var before := read(s.mine.path_join("level.tscn"))
	teammate_pushes(s, "level.tscn", SCENE_BASE.replace("position = Vector2(10, 10)", "position = Vector2(12, 12)"))
	var r := open(s.mine)
	check("scene same property: refused", r.pull() != OK, GitApi.get_last_error())
	var pull_result := r.get_pull_result()
	check("scene same property: reported as a conflict", Array(pull_result.conflicts) == ["level.tscn"] and Array(pull_result.scenes).is_empty(), pull_result)
	check("scene same property: HEAD and the file unchanged", git(s.mine, ["rev-parse", "HEAD"]) == head and read(s.mine.path_join("level.tscn")) == before, git(s.mine, ["status", "--porcelain"]))


## Mixed: the scene merges on its own, the text file conflicts. Start merge stops at notes.txt only.
func _scene_mixed_with_text() -> void:
	var s := make_shared("scene-mixed")
	write(s.mine.path_join("level.tscn"), SCENE_BASE)
	write(s.mine.path_join("notes.txt"), "note-1\n")
	commit_all(s.mine, "Add level and notes")
	git(s.mine, ["push", "-q"])
	git(s.theirs, ["pull", "-q", "--no-rebase"])
	write(s.mine.path_join("level.tscn"), _scene_with_node("Mine", 400))
	write(s.mine.path_join("notes.txt"), "note-mine\n")
	commit_all(s.mine, "Mine: node and note")
	teammate_pushes(s, "level.tscn", _scene_with_node("Theirs", 500))
	write(s.theirs.path_join("notes.txt"), "note-theirs\n")
	commit_all(s.theirs, "Theirs: note")
	git(s.theirs, ["push", "-q"])
	var r := open(s.mine)
	check("scene mixed: start merge", r.pull(true) == OK, GitApi.get_last_error())
	var pull_result := r.get_pull_result()
	check("scene mixed: scene settled, text conflicts", Array(pull_result.scenes) == ["level.tscn"] and Array(pull_result.conflicts) == ["notes.txt"], pull_result)
	check("scene mixed: git sees only notes.txt conflicted", git(s.mine, ["diff", "--name-only", "--diff-filter=U"]) == "notes.txt", git(s.mine, ["status", "--porcelain"]))
	check("scene mixed: level.tscn staged", git(s.mine, ["diff", "--cached", "--name-only"]).split("\n").has("level.tscn"), git(s.mine, ["status", "--porcelain"]))
	var text := read(s.mine.path_join("level.tscn"))
	check("scene mixed: staged file has both nodes", text.contains("name=\"Mine\"") and text.contains("name=\"Theirs\""), text)


## An uncommitted edit to the scene, the teammate adds a node at the same place: the edit is
## carried (merged into the new version), stays uncommitted, and nothing is reported as a conflict.
func _scene_edit_carried() -> void:
	var s := make_shared("scene-carry")
	write(s.mine.path_join("level.tscn"), SCENE_BASE)
	commit_all(s.mine, "Add level")
	git(s.mine, ["push", "-q"])
	git(s.theirs, ["pull", "-q", "--no-rebase"])
	write(s.mine.path_join("level.tscn"), _scene_with_node("Local", 400))
	teammate_pushes(s, "level.tscn", _scene_with_node("Theirs", 500))
	var r := open(s.mine)
	check("scene carry: no conflicts to ask about", r.get_pull_conflicts().is_empty(), r.get_pull_conflicts())
	check("scene carry: pull", r.pull() == OK, GitApi.get_last_error())
	check("scene carry: reported as carried", Array(r.get_pull_result().carried) == ["level.tscn"], r.get_pull_result())
	var text := read(s.mine.path_join("level.tscn"))
	check("scene carry: the file has both nodes", text.contains("name=\"Local\"") and text.contains("name=\"Theirs\""), text)
	# git() strips the leading space of " M" (unstaged); a staged edit would read "M  level.tscn".
	check("scene carry: the edit is still uncommitted, not staged", git(s.mine, ["status", "--porcelain"]) == "M level.tscn", git(s.mine, ["status", "--porcelain"]))
	var head_text := git(s.mine, ["show", "HEAD:level.tscn"])
	check("scene carry: HEAD has Theirs but not Local", head_text.contains("name=\"Theirs\"") and not head_text.contains("name=\"Local\""), head_text)
