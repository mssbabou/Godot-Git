extends "res://tests/test_case.gd"
## Git LFS file locking against a stand-in LFS server (fake_lfs_lock_server.py, which needs
## Python): listing locks, yours and others', locking, unlocking, and refusing to take someone
## else's lock without force. Skipped without git-lfs or Python.

var server_pid := -1


func run() -> void:
	var out := []
	if OS.execute("git", ["lfs", "version"], out) != 0:
		print("  (skipped; git-lfs isn't installed)")
		return
	var python := ""
	for program in ["python3", "python"]:
		if OS.execute(program, ["--version"], out) == 0:
			python = program
			break
	if python.is_empty():
		print("  (skipped; Python isn't installed)")
		return
	var port := 40000 + randi() % 20000
	server_pid = OS.create_process(python, [ProjectSettings.globalize_path("res://tests/fake_lfs_lock_server.py"), str(port)])
	OS.delay_msec(1500)
	_locks(port)
	OS.kill(server_pid)


func _locks(port: int) -> void:
	var repo := make_repo("lfs-locks")
	git(repo, ["config", "lfs.url", "http://127.0.0.1:%d/" % port])
	git(repo, ["config", "lfs.locksverify", "true"])
	write(repo.path_join("level.tscn"), "[gd_scene]\n")
	commit_all(repo, "first")
	var r := open(repo)
	var locks := r.get_lfs_locks()
	check("locks: alice's lock is listed, not yours", locks.size() == 1 and locks[0].path == "music.ogg" and locks[0].owner == "alice" and not locks[0].mine, [locks, GitRepository.get_last_error()])
	check("locks: lock a file", r.lock_file("level.tscn") == OK, GitRepository.get_last_error())
	locks = r.get_lfs_locks()
	var mine := locks.filter(func(lock): return lock.mine)
	check("locks: yours is listed as yours", mine.size() == 1 and mine[0].path == "level.tscn", locks)
	check("locks: locking it again is refused", r.lock_file("level.tscn") != OK, GitRepository.get_last_error())
	check("locks: unlock yours", r.unlock_file("level.tscn") == OK, GitRepository.get_last_error())
	check("locks: unlocking someone else's needs force", r.unlock_file("music.ogg") != OK, GitRepository.get_last_error())
	check("locks: only alice's is left", r.get_lfs_locks().size() == 1)
