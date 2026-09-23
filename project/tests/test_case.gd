extends RefCounted
## Base class for test suites. A suite overrides run() (which may await) and uses check() for
## assertions. Repositories are built with the real git CLI, so results are checked against what
## git itself says, not against our own code.

var dir := "" ## This suite's scratch folder (absolute path).
var online := false ## True when network tests against GitHub are enabled.
var tree: SceneTree
var checks := 0
var failures := 0


func run() -> void:
	pass


func check(label: String, ok: bool, detail: Variant = "") -> void:
	checks += 1
	if ok:
		print("  PASS ", label)
	else:
		failures += 1
		print("  FAIL ", label, "  ->  ", detail)


## Runs git in p_repo and returns its trimmed output.
func git(repo: String, args: Array) -> String:
	var out := []
	OS.execute("git", ["-C", repo] + args, out, true)
	return "".join(out).strip_edges()


func write(path: String, text: String) -> void:
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(text)
	f.close()


## File contents with \r removed (libgit2 honors core.autocrlf on checkout).
func read(path: String) -> String:
	return FileAccess.get_file_as_string(path).replace("\r", "")


func exists(path: String) -> bool:
	return FileAccess.file_exists(path)


## An empty repository with an identity and no line-ending conversion.
func make_repo(name: String) -> String:
	var path := dir.path_join(name)
	OS.execute("git", ["init", "-q", "-b", "main", path])
	_configure(path)
	return path


## A bare "remote" with one commit (x.txt, y.txt, z.txt) and two clones of it tracking
## origin/main: { "remote", "mine", "theirs" }. "theirs" plays the teammate.
func make_shared(name: String) -> Dictionary:
	var remote := dir.path_join(name + "-remote.git")
	var mine := dir.path_join(name + "-mine")
	var theirs := dir.path_join(name + "-theirs")
	OS.execute("git", ["init", "-q", "--bare", "-b", "main", remote])
	for clone in [mine, theirs]:
		OS.execute("git", ["clone", "-q", remote, clone])
		_configure(clone)
		git(clone, ["checkout", "-q", "-b", "main"])
	for file in ["x", "y", "z"]:
		write(mine.path_join(file + ".txt"), file + "1\n")
	commit_all(mine, "Initial commit")
	git(mine, ["push", "-q", "-u", "origin", "main"])
	git(theirs, ["pull", "-q", "origin", "main"])
	git(theirs, ["branch", "-q", "-u", "origin/main"])
	return { "remote": remote, "mine": mine, "theirs": theirs }


func commit_all(repo: String, message: String) -> void:
	git(repo, ["add", "-A"])
	git(repo, ["commit", "-q", "-m", message])


## The teammate changes a file, commits and pushes.
func teammate_pushes(shared: Dictionary, file: String, text: String, message := "Teammate change") -> void:
	git(shared.theirs, ["pull", "-q", "--no-rebase"]) # Like a real teammate: up to date before pushing.
	write(shared.theirs.path_join(file), text)
	commit_all(shared.theirs, message)
	git(shared.theirs, ["push", "-q"])


func open(repo: String) -> GitRepository:
	var r := GitRepository.new()
	r.open(repo)
	return r


## Lets deferred calls (e.g. progress callbacks) run.
func frames(count := 2) -> void:
	for i in count:
		await tree.process_frame


func _configure(repo: String) -> void:
	git(repo, ["config", "user.name", "Test"])
	git(repo, ["config", "user.email", "test@example.com"])
	git(repo, ["config", "core.autocrlf", "false"])
