extends "res://tests/test_case.gd"
## Fetch, pull, push against a local "remote": counts, results, progress, refusals.


func run() -> void:
	await _fetch_and_fast_forward()
	_merge_and_push()
	_conflict_refused()
	_publish_and_remote_branches()


func _fetch_and_fast_forward() -> void:
	var s := make_shared("ff")
	var r := open(s.mine)
	check("tracks origin/main", r.get_sync_status().upstream == "origin/main", r.get_sync_status())

	for i in 3:
		teammate_pushes(s, "t%d.txt" % i, "t\n", "Teammate %d" % i)
	check("fetch", r.fetch() == OK, GitRepository.get_last_error())
	var status := r.get_sync_status()
	check("behind 3 after fetch", status.behind == 3 and status.ahead == 0, status)
	check("fetch time recorded", absi(int(Time.get_unix_time_from_system()) - status.last_fetched) < 120, status.last_fetched)

	var steps := []
	r.set_progress_callback(func(step: String, _fraction: float, _cancellable: bool): steps.append(step))
	check("pull fast-forwards", r.pull() == OK, GitRepository.get_last_error())
	var result := r.get_pull_result()
	check("pull result: 3 commits, no merge", result.commits == 3 and not result.merged, result)
	check("files arrived", exists(s.mine.path_join("t2.txt")))
	check("in sync", r.get_sync_status().behind == 0)
	await frames()
	check("progress was reported", steps.has("Connecting...") and steps.any(func(x): return x.begins_with("Updating files")), steps)

	check("pull when up to date", r.pull() == OK and r.get_pull_result().commits == 0, r.get_pull_result())


func _merge_and_push() -> void:
	var s := make_shared("merge")
	var r := open(s.mine)
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("y.txt"), "y-mine\n")
	commit_all(s.mine, "My change")

	check("diverged pull merges", r.pull() == OK, GitRepository.get_last_error())
	check("pull result: 1 commit, merged", r.get_pull_result().commits == 1 and r.get_pull_result().merged, r.get_pull_result())
	var commits := r.get_commits(5)
	check("merge commit on top", commits[0].summary.begins_with("Merge remote-tracking branch 'origin/main'"), commits[0].summary)
	check("both changes present", read(s.mine.path_join("x.txt")) == "x-theirs\n" and read(s.mine.path_join("y.txt")) == "y-mine\n")
	check("ahead 2 (mine + merge)", r.get_sync_status().ahead == 2, r.get_sync_status())
	var unpushed := commits.filter(func(c): return c.unpushed).map(func(c): return c.summary)
	check("my commit and the merge are marked unpushed, the teammate's isn't", unpushed.size() == 2 and unpushed.has("My change"), unpushed)

	check("push", r.push() == OK, GitRepository.get_last_error())
	check("in sync after push", r.get_sync_status().ahead == 0 and r.get_sync_status().behind == 0, r.get_sync_status())
	check("nothing marked unpushed", not r.get_commits(1)[0].unpushed)
	check("remote has the merge", git(s.remote, ["log", "-1", "--format=%s", "main"]).begins_with("Merge remote-tracking"))

	# Behind the remote: push is refused with a clear message.
	teammate_pushes(s, "z.txt", "z-theirs\n")
	write(s.mine.path_join("y.txt"), "y-again\n")
	commit_all(s.mine, "Another change")
	check("push refused when behind", r.push() != OK and GitRepository.get_last_error().contains("Pull first"), GitRepository.get_last_error())


func _conflict_refused() -> void:
	var s := make_shared("conflict")
	var r := open(s.mine)
	teammate_pushes(s, "x.txt", "x-theirs\n")
	write(s.mine.path_join("x.txt"), "x-mine\n")
	commit_all(s.mine, "My x")
	var head := git(s.mine, ["rev-parse", "HEAD"])

	check("conflicting pull refused", r.pull() != OK and GitRepository.get_last_error().contains("conflict"), GitRepository.get_last_error())
	check("HEAD unchanged", git(s.mine, ["rev-parse", "HEAD"]) == head)
	check("working tree clean", r.get_status().is_empty(), r.get_status())
	check("not stuck mid-merge", not exists(s.mine.path_join(".git/MERGE_HEAD")))
	check("my version kept", read(s.mine.path_join("x.txt")) == "x-mine\n")


func _publish_and_remote_branches() -> void:
	var s := make_shared("branches")
	var r := open(s.mine)
	r.create_branch("feature/x")
	check("new branch has no upstream", r.get_sync_status().upstream == "")
	check("publish", r.push() == OK, GitRepository.get_last_error())
	check("now tracks origin/feature/x", r.get_sync_status().upstream == "origin/feature/x", r.get_sync_status())
	check("remote has the branch", git(s.remote, ["branch", "--list", "feature/x"]).contains("feature/x"))

	git(s.theirs, ["checkout", "-q", "-b", "experiment"])
	teammate_pushes(s, "exp.txt", "e\n")
	git(s.theirs, ["push", "-q", "-u", "origin", "experiment"])
	r.fetch()
	check("remote branch listed", r.get_remote_branches().has("origin/experiment"), r.get_remote_branches())
	check("check out a remote branch", r.checkout_branch("origin/experiment") == OK, GitRepository.get_last_error())
	check("local branch tracks it", r.get_current_branch() == "experiment" and r.get_sync_status().upstream == "origin/experiment", r.get_sync_status())
	check("its files are there", exists(s.mine.path_join("exp.txt")))
