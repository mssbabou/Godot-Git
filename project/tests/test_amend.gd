extends "res://tests/test_case.gd"
## Amending the last commit: new message, staged files added, and never once it's pushed.


func run() -> void:
	_message_only()
	_with_staged_files()
	_refused_once_pushed()


func _message_only() -> void:
	var shared := make_shared("message")
	write(shared.mine.path_join("a.txt"), "a\n")
	commit_all(shared.mine, "Tpyo")
	var parent := git(shared.mine, ["rev-parse", "HEAD~1"])
	var tree := git(shared.mine, ["rev-parse", "HEAD^{tree}"])

	var r := open(shared.mine)
	check("unpushed commit isn't pushed", not r.is_head_pushed())
	check("amend succeeds", r.amend("Typo fixed") == OK, GitRepository.get_last_error())
	check("message replaced", git(shared.mine, ["log", "-1", "--format=%s"]) == "Typo fixed", git(shared.mine, ["log", "-1", "--format=%s"]))
	check("same parent (replaced, not added)", git(shared.mine, ["rev-parse", "HEAD~1"]) == parent)
	check("same files", git(shared.mine, ["rev-parse", "HEAD^{tree}"]) == tree)
	check("author kept", git(shared.mine, ["log", "-1", "--format=%an"]) == "Test")


func _with_staged_files() -> void:
	var shared := make_shared("staged")
	write(shared.mine.path_join("a.txt"), "a\n")
	commit_all(shared.mine, "Add a")
	write(shared.mine.path_join("forgotten.txt"), "oops\n")

	var r := open(shared.mine)
	r.stage("forgotten.txt")
	check("amend with a staged file", r.amend("Add a and the forgotten file") == OK, GitRepository.get_last_error())
	var files := git(shared.mine, ["show", "--name-only", "--format=", "HEAD"]).split("\n")
	check("amended commit has both files", files.has("a.txt") and files.has("forgotten.txt"), files)
	check("nothing left staged", r.get_status().is_empty(), r.get_status())
	check("still one commit on top", git(shared.mine, ["rev-list", "--count", "origin/main..HEAD"]) == "1")


func _refused_once_pushed() -> void:
	var shared := make_shared("pushed")
	write(shared.mine.path_join("a.txt"), "a\n")
	commit_all(shared.mine, "Shared with the team")
	git(shared.mine, ["push", "-q"])
	var head := git(shared.mine, ["rev-parse", "HEAD"])

	var r := open(shared.mine)
	check("pushed commit counts as pushed", r.is_head_pushed())
	check("amend refused", r.amend("Rewritten") != OK)
	check("says why", GitRepository.get_last_error().contains("already pushed"), GitRepository.get_last_error())
	check("commit untouched", git(shared.mine, ["rev-parse", "HEAD"]) == head)
