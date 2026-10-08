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
	# A Python that really runs a script: on Windows runners "python3" can be the Microsoft Store
	# stub, which answers --version but runs nothing.
	var python := ""
	for program in ["python3", "python", "py"]:
		out.clear()
		if OS.execute(program, ["-c", "print('ok')"], out) == 0 and "".join(out).strip_edges() == "ok":
			python = program
			break
	if python.is_empty():
		print("  (skipped; Python isn't installed)")
		return
	# The server picks a free port itself and writes it to this file.
	var port_file := dir.path_join("lfs_server_port")
	server_pid = OS.create_process(python, [ProjectSettings.globalize_path("res://tests/fake_lfs_lock_server.py"), port_file])
	var port := 0
	for attempt in 200:
		if FileAccess.file_exists(port_file):
			port = FileAccess.get_file_as_string(port_file).strip_edges().to_int()
			break
		OS.delay_msec(100)
	# Up when it accepts a connection (it writes the file once it listens, so this is quick).
	var up := false
	for attempt in (100 if port > 0 else 0):
		var peer := StreamPeerTCP.new()
		if peer.connect_to_host("127.0.0.1", port) == OK:
			for i in 10:
				peer.poll()
				if peer.get_status() == StreamPeerTCP.STATUS_CONNECTED:
					up = true
					break
				OS.delay_msec(10)
			peer.disconnect_from_host()
		if up:
			break
		OS.delay_msec(100)
	check("locks: the stand-in LFS server started", up, "%s on port %d" % [python, port])
	if up:
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
	check("locks: alice's lock is listed, not yours", locks.size() == 1 and locks[0].path == "music.ogg" and locks[0].owner == "alice" and not locks[0].mine, [locks, GitApi.get_last_error()])
	check("locks: lock a file", r.lock_file("level.tscn") == OK, GitApi.get_last_error())
	locks = r.get_lfs_locks()
	var mine := locks.filter(func(lock): return lock.mine)
	check("locks: yours is listed as yours", mine.size() == 1 and mine[0].path == "level.tscn", locks)
	check("locks: locking it again is refused", r.lock_file("level.tscn") != OK, GitApi.get_last_error())
	check("locks: unlock yours", r.unlock_file("level.tscn") == OK, GitApi.get_last_error())
	check("locks: unlocking someone else's needs force", r.unlock_file("music.ogg") != OK, GitApi.get_last_error())
	check("locks: only alice's is left", r.get_lfs_locks().size() == 1)
