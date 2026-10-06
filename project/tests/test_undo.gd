extends "res://tests/test_case.gd"
## Undo (get_undo, undo_last_operation): what the last thing that moved HEAD was, and taking it
## back without touching uncommitted changes or anything pushed. Results are checked against git.


func run() -> void:
	_commit()
	_amend()
	_pull_fast_forward()
	_pull_merge()
	_pull_edit_in_the_way()
	_merge_branch()
	_switch()
	_pushed()
	_nothing()
	_from_terminal()


func _head(repo: String) -> String:
	return git(repo, ["rev-parse", "HEAD"])


func _commit() -> void:
	var repo := make_repo("commit")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "First")
	var first := _head(repo)
	var r := open(repo)
	write(repo.path_join("a.txt"), "a2\n")
	r.stage("a.txt")
	check("commit: committed", r.commit("Second") == OK, GitRepository.get_last_error())
	var undo := r.get_undo()
	check("commit: offered as Undo Commit", undo.get("kind") == "commit" and undo.get("label") == "Undo Commit" and undo.get("reason") == "", undo)
	check("commit: undone", r.undo_last_operation() == OK, GitRepository.get_last_error())
	check("commit: HEAD back at the first commit, the change staged", _head(repo) == first and git(repo, ["status", "--porcelain"]) == "M  a.txt", git(repo, ["status", "--porcelain"]))
	check("commit: nothing more to undo", r.get_undo().is_empty(), r.get_undo())


func _amend() -> void:
	var repo := make_repo("amend")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "First")
	write(repo.path_join("b.txt"), "b\n")
	commit_all(repo, "Second")
	var second := _head(repo)
	var r := open(repo)
	write(repo.path_join("c.txt"), "c\n")
	r.stage("c.txt")
	check("amend: amended", r.amend("Second, amended") == OK, GitRepository.get_last_error())
	var undo := r.get_undo()
	check("amend: offered as Undo Amend", undo.get("kind") == "amend" and undo.get("reason") == "", undo)
	check("amend: undone", r.undo_last_operation() == OK, GitRepository.get_last_error())
	check("amend: the commit as it was, what the amend added staged", _head(repo) == second and git(repo, ["status", "--porcelain"]) == "A  c.txt", git(repo, ["status", "--porcelain"]))


func _pull_fast_forward() -> void:
	var shared := make_shared("ff")
	var mine: String = shared.mine
	var before := _head(mine)
	teammate_pushes(shared, "x.txt", "x-theirs\n")
	teammate_pushes(shared, "w.txt", "new\n", "Teammate adds w")
	write(mine.path_join("y.txt"), "y-mine\n") # Uncommitted, in a file the pull doesn't touch.
	var r := open(mine)
	check("ff: pulled", r.pull() == OK, GitRepository.get_last_error())
	var undo := r.get_undo()
	check("ff: offered as Undo Pull, naming 2 commits", undo.get("kind") == "pull" and undo.get("label") == "Undo Pull" and String(undo.get("detail")).contains("2 commits") and undo.get("reason") == "", undo)
	check("ff: undone", r.undo_last_operation() == OK, GitRepository.get_last_error())
	check("ff: main back where it was", _head(mine) == before)
	check("ff: the pulled files back too, your edit kept", read(mine.path_join("x.txt")).replace("\r", "") == "x1\n" and not exists(mine.path_join("w.txt")) and read(mine.path_join("y.txt")).replace("\r", "") == "y-mine\n")
	check("ff: only your edit left", git(mine, ["status", "--porcelain"]) == "M y.txt", git(mine, ["status", "--porcelain"]))
	check("ff: nothing more to undo", r.get_undo().is_empty(), r.get_undo())
	check("ff: pulls again", r.pull() == OK and read(mine.path_join("w.txt")).replace("\r", "") == "new\n", GitRepository.get_last_error())


func _pull_merge() -> void:
	var shared := make_shared("merge")
	var mine: String = shared.mine
	write(mine.path_join("y.txt"), "y-mine\n")
	commit_all(mine, "Mine")
	var before := _head(mine)
	teammate_pushes(shared, "x.txt", "x-theirs\n")
	var r := open(mine)
	check("pull merge: pulled with a merge commit", r.pull() == OK and r.get_pull_result().merged, GitRepository.get_last_error())
	check("pull merge: git's reflog message", git(mine, ["reflog", "-1", "--format=%gs"]) == "pull: Merge made by the 'ort' strategy.", git(mine, ["reflog", "-1", "--format=%gs"]))
	var undo := r.get_undo()
	check("pull merge: offered as Undo Pull, counting the one commit pulled (not the merge commit)", undo.get("kind") == "pull" and undo.get("reason") == "" and String(undo.get("detail")).contains("the commit it brought in"), undo)
	check("pull merge: undone", r.undo_last_operation() == OK, GitRepository.get_last_error())
	check("pull merge: main back at your commit, x.txt as before, nothing left over", _head(mine) == before and read(mine.path_join("x.txt")).replace("\r", "") == "x1\n" and git(mine, ["status", "--porcelain"]) == "", git(mine, ["status", "--porcelain"]))


func _pull_edit_in_the_way() -> void:
	var shared := make_shared("inway")
	var mine: String = shared.mine
	teammate_pushes(shared, "x.txt", "x1\nx2-theirs\n")
	var r := open(mine)
	check("in the way: pulled", r.pull() == OK, GitRepository.get_last_error())
	var after := _head(mine)
	write(mine.path_join("x.txt"), "x1\nx2-theirs\nx3-mine\n")
	var undo := r.get_undo()
	check("in the way: refused up front, naming the file", undo.get("kind") == "pull" and String(undo.get("reason")).contains("x.txt"), undo)
	check("in the way: undo refuses", r.undo_last_operation() != OK)
	check("in the way: nothing changed", _head(mine) == after and read(mine.path_join("x.txt")).replace("\r", "") == "x1\nx2-theirs\nx3-mine\n")


func _merge_branch() -> void:
	var repo := make_repo("branch")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "First")
	git(repo, ["checkout", "-q", "-b", "feature"])
	write(repo.path_join("f.txt"), "f\n")
	commit_all(repo, "Feature")
	git(repo, ["checkout", "-q", "main"])
	write(repo.path_join("m.txt"), "m\n")
	commit_all(repo, "Main")
	var before := _head(repo)
	var r := open(repo)
	check("merge: merged", r.merge_branch("feature") == OK, GitRepository.get_last_error())
	var undo := r.get_undo()
	check("merge: offered as Undo Merge, naming feature", undo.get("kind") == "merge" and undo.get("label") == "Undo Merge" and String(undo.get("detail")).contains("feature"), undo)
	check("merge: undone", r.undo_last_operation() == OK, GitRepository.get_last_error())
	check("merge: main back, feature's file gone, feature itself kept", _head(repo) == before and not exists(repo.path_join("f.txt")) and git(repo, ["rev-parse", "--verify", "-q", "feature"]) != "")


func _switch() -> void:
	var repo := make_repo("switch")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "First")
	git(repo, ["branch", "other"])
	var r := open(repo)
	check("switch: switched", r.checkout_branch("other") == OK, GitRepository.get_last_error())
	var undo := r.get_undo()
	check("switch: offered as Switch Back to main", undo.get("kind") == "switch" and undo.get("label") == "Switch Back to main" and undo.get("branch") == "main", undo)
	check("switch: undone", r.undo_last_operation() == OK and r.get_current_branch() == "main", GitRepository.get_last_error())


func _pushed() -> void:
	var shared := make_shared("pushed")
	var mine: String = shared.mine
	write(mine.path_join("y.txt"), "y-mine\n")
	commit_all(mine, "Mine")
	git(mine, ["push", "-q"])
	var r := open(mine)
	var undo := r.get_undo()
	check("pushed: a pushed commit isn't undone", undo.get("kind") == "commit" and String(undo.get("reason")).contains("pushed"), undo)
	check("pushed: undo refuses", r.undo_last_operation() != OK)


func _nothing() -> void:
	var repo := make_repo("nothing")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "First")
	check("nothing: the first commit can't be undone", open(repo).get_undo().is_empty(), open(repo).get_undo())


## A commit made in a terminal counts too.
func _from_terminal() -> void:
	var repo := make_repo("terminal")
	write(repo.path_join("a.txt"), "a\n")
	commit_all(repo, "First")
	write(repo.path_join("a.txt"), "a2\n")
	commit_all(repo, "From the terminal")
	var undo := open(repo).get_undo()
	check("terminal: a terminal commit is offered too", undo.get("kind") == "commit" and String(undo.get("detail")).contains("From the terminal"), undo)
