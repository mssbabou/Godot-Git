extends "res://tests/test_case.gd"
## Git LFS: files stored with LFS must come out as the real files (never the small pointer files
## git holds for them), commit into LFS (never into git itself), and reach the remote on push.
## Uses a local remote, which git-lfs serves without an LFS server. Skipped without git-lfs.


func run() -> void:
	var out := []
	if OS.execute("git", ["lfs", "version"], out) != 0:
		print("  (skipped; git-lfs isn't installed)")
		return
	await _pull_fast_forward()
	await _pull_merge()
	await _commit_and_push()
	await _switch_branch()
	await _changes()
	_file_bytes()


## Like make_shared(), with *.png stored in LFS and one image (icon.png) in the first commit.
func _make_lfs_shared(name: String) -> Dictionary:
	var remote := dir.path_join(name + "-remote.git")
	var mine := dir.path_join(name + "-mine")
	var theirs := dir.path_join(name + "-theirs")
	OS.execute("git", ["init", "-q", "--bare", "-b", "main", remote])
	for clone in [mine, theirs]:
		OS.execute("git", ["clone", "-q", remote, clone])
		_configure(clone)
		git(clone, ["checkout", "-q", "-b", "main"])
		git(clone, ["lfs", "install", "--local"])
	git(theirs, ["lfs", "track", "*.png"])
	write(theirs.path_join("readme.txt"), "hello\n")
	_write_image(theirs.path_join("icon.png"), 2000)
	commit_all(theirs, "Initial commit")
	git(theirs, ["push", "-q", "-u", "origin", "main"])
	git(mine, ["pull", "-q", "origin", "main"])
	git(mine, ["branch", "-q", "-u", "origin/main"])
	return { "remote": remote, "mine": mine, "theirs": theirs }


func _write_image(path: String, size: int) -> PackedByteArray:
	var rng := RandomNumberGenerator.new()
	var data := PackedByteArray([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])
	for i in size:
		data.push_back(rng.randi() % 256)
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer(data)
	f.close()
	return data


func _is_pointer(path: String) -> bool:
	return FileAccess.get_file_as_bytes(path).slice(0, 7).get_string_from_ascii() == "version"


func _pull_fast_forward() -> void:
	var shared := _make_lfs_shared("ff")
	git(shared.theirs, ["pull", "-q"])
	var image := _write_image(shared.theirs.path_join("new.png"), 3000)
	commit_all(shared.theirs, "Add new.png")
	git(shared.theirs, ["push", "-q"])

	var r := open(shared.mine)
	check("repository uses LFS", r.uses_lfs())
	check("pull succeeds", r.pull() == OK, GitRepository.get_last_error())
	check("pulled image is the real file", FileAccess.get_file_as_bytes(shared.mine.path_join("new.png")) == image, "a pointer file" if _is_pointer(shared.mine.path_join("new.png")) else "different bytes")
	check("status clean after pull", r.get_status().is_empty(), r.get_status())
	check("git agrees it's clean", git(shared.mine, ["status", "--porcelain"]).is_empty(), git(shared.mine, ["status", "--porcelain"]))


func _pull_merge() -> void:
	var shared := _make_lfs_shared("merge")
	var image := _write_image(shared.theirs.path_join("theirs.png"), 1500)
	commit_all(shared.theirs, "Teammate art")
	git(shared.theirs, ["push", "-q"])
	write(shared.mine.path_join("notes.txt"), "mine\n")
	commit_all(shared.mine, "My notes")

	var r := open(shared.mine)
	check("diverged pull merges", r.pull() == OK and r.get_pull_result().merged, GitRepository.get_last_error())
	check("merged-in image is the real file", FileAccess.get_file_as_bytes(shared.mine.path_join("theirs.png")) == image)
	check("status clean after merge", r.get_status().is_empty(), r.get_status())


func _commit_and_push() -> void:
	var shared := _make_lfs_shared("commit")
	var image := _write_image(shared.mine.path_join("mine.png"), 5000)
	var r := open(shared.mine)
	check("stage and commit an image", r.stage("mine.png") == OK and r.commit("Add mine.png") == OK, GitRepository.get_last_error())
	check("committed into LFS", git(shared.mine, ["lfs", "ls-files", "-n"]).split("\n").has("mine.png"), git(shared.mine, ["lfs", "ls-files", "-n"]))
	check("git holds only a pointer", git(shared.mine, ["cat-file", "-s", "HEAD:mine.png"]).to_int() < 200, git(shared.mine, ["cat-file", "-s", "HEAD:mine.png"]))
	check("status clean after commit", r.get_status().is_empty(), r.get_status())
	check("push succeeds", r.push() == OK, GitRepository.get_last_error())

	# A teammate gets the real image from the remote, so the file itself was uploaded too.
	git(shared.theirs, ["pull", "-q"])
	check("teammate gets the real image", FileAccess.get_file_as_bytes(shared.theirs.path_join("mine.png")) == image, "a pointer file" if _is_pointer(shared.theirs.path_join("mine.png")) else "missing or different")


func _switch_branch() -> void:
	var shared := _make_lfs_shared("switch")
	git(shared.theirs, ["checkout", "-q", "-b", "feature"])
	var image := _write_image(shared.theirs.path_join("feature.png"), 1200)
	commit_all(shared.theirs, "Feature art")
	git(shared.theirs, ["push", "-q", "-u", "origin", "feature"])

	var r := open(shared.mine)
	check("fetch", r.fetch() == OK, GitRepository.get_last_error())
	check("switch to the remote branch", r.checkout_branch("origin/feature") == OK, GitRepository.get_last_error())
	check("switched-to image is the real file", FileAccess.get_file_as_bytes(shared.mine.path_join("feature.png")) == image)
	check("status clean after switching", r.get_status().is_empty(), r.get_status())


func _changes() -> void:
	var shared := _make_lfs_shared("changes")
	_write_image(shared.mine.path_join("icon.png"), 2500)
	var r := open(shared.mine)
	var status := r.get_status()
	check("edited image shows as changed", status.size() == 1 and status[0].path == "icon.png", status)
	check("image counts as binary", r.get_line_stats(false).get("icon.png") == Vector2i(-1, -1), r.get_line_stats(false))


# The Diff panel's image previews read old versions with get_file_bytes: an LFS file must come back
# as the real image (from git-lfs's local cache), never the pointer git stores for it.
func _file_bytes() -> void:
	var shared := _make_lfs_shared("bytes")
	var first := FileAccess.get_file_as_bytes(shared.mine.path_join("icon.png"))
	var changed := _write_image(shared.mine.path_join("icon.png"), 2500)
	var r := open(shared.mine)
	var head := r.get_file_bytes("HEAD", "icon.png")
	check("LFS: HEAD's version is the real image, from the cache", head.lfs == "cached" and head.bytes == first, [head.lfs, head.bytes.size(), first.size()])
	var disk := r.get_file_bytes("workdir", "icon.png")
	check("LFS: the file on disk is read as is", disk.lfs == "" and disk.bytes == changed)
	# A version that was never downloaded: remove it from the cache.
	var oid := git(shared.mine, ["lfs", "ls-files", "--long"]).split(" ")[0]
	var object: String = shared.mine.path_join(".git/lfs/objects").path_join(oid.substr(0, 2)).path_join(oid.substr(2, 2)).path_join(oid)
	check("LFS: found the cached object to remove", DirAccess.remove_absolute(object) == OK, object)
	var missing := r.get_file_bytes("HEAD", "icon.png")
	check("LFS: a version not in the cache says so, with no bytes", missing.exists and missing.lfs == "missing" and missing.bytes.is_empty(), missing)
