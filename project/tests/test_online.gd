extends "res://tests/test_case.gd"
## Talks to GitHub over HTTPS. Only runs with --online.

const SMALL_REPO := "https://github.com/godotengine/godot-cpp-template.git"
const BIG_REPO := "https://github.com/godotengine/godot.git" # Big enough to cancel mid-download.


func run() -> void:
	if not online:
		print("  (skipped; pass --online to run)")
		return
	_https_fetch()
	await _no_login_prompts()
	await _cancel_fetch()


func _https_fetch() -> void:
	var repo := make_repo("https")
	git(repo, ["remote", "add", "origin", SMALL_REPO])
	var r := open(repo)
	check("public HTTPS fetch", r.fetch() == OK, GitRepository.get_last_error())
	check("remote branches arrived", r.get_remote_branches().has("origin/main"), r.get_remote_branches())


# Background fetches must never open a sign-in window. GitHub asks for a login for repositories
# that don't exist (it won't say whether a private one does); with prompts off, the fetch has
# to fail on its own, quickly, instead of waiting for someone to sign in.
func _no_login_prompts() -> void:
	var repo := make_repo("no-prompt")
	git(repo, ["remote", "add", "origin", "https://github.com/godotengine/this-repository-does-not-exist-godot-git-test.git"])
	var thread := Thread.new()
	thread.start(func():
		var r := open(repo)
		r.set_login_prompts_allowed(false)
		return [r.fetch(), GitRepository.get_last_error()])
	var deadline := Time.get_ticks_msec() + 30000
	while thread.is_alive() and Time.get_ticks_msec() < deadline:
		await tree.process_frame
	var finished := not thread.is_alive()
	if not finished:
		GitRepository.cancel_network() # Don't leave a sign-in window hanging.
	var result: Array = thread.wait_to_finish()
	check("finishes on its own without a sign-in", finished, "still waiting after 30 s")
	check("fails with an explanation", result[0] != OK and not String(result[1]).is_empty(), result)

	# Prompts off must not break anything that needs no login.
	var small := make_repo("no-prompt-public")
	git(small, ["remote", "add", "origin", SMALL_REPO])
	var r := open(small)
	r.set_login_prompts_allowed(false)
	check("public fetch still works with prompts off", r.fetch() == OK, GitRepository.get_last_error())


func _cancel_fetch() -> void:
	var repo := make_repo("cancel")
	git(repo, ["remote", "add", "origin", BIG_REPO])
	var steps := []
	var thread := Thread.new()
	thread.start(func():
		var r := open(repo)
		r.set_progress_callback(func(step: String, _fraction: float, _cancellable: bool): steps.append(step))
		var err := r.fetch()
		return [err, GitRepository.get_last_error()])

	# Cancel once data is actually flowing.
	var deadline := Time.get_ticks_msec() + 60000
	while Time.get_ticks_msec() < deadline and not steps.any(func(x): return x.begins_with("Receiving")):
		await tree.process_frame
	GitRepository.cancel_network()
	var result: Array = thread.wait_to_finish()
	await frames()

	check("data was flowing before the cancel", steps.any(func(x): return x.begins_with("Receiving")), steps.slice(-5))
	check("fetch returns ERR_SKIP", result[0] == ERR_SKIP, result)
	check("says it was canceled", String(result[1]).begins_with("Canceled"), result[1])
	check("server messages arrive whole", not steps.any(func(x): return x.begins_with("Count") and not x.begins_with("Counting objects")), steps.slice(0, 8))
	check("nothing was fetched", open(repo).get_remote_branches().is_empty())
