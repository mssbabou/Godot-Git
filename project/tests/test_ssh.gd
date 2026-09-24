extends "res://tests/test_case.gd"
## SSH remotes. Fetch and push for them go through git, which runs ssh, shows ssh's own errors
## and uses the same ssh as git in a terminal. A fake ssh (core.sshCommand) runs git's remote
## command locally instead of on a server, and can act out ssh failures: a rejected key, an
## unknown server, a connection that hangs.

## Ignores the host and runs the remote command here. What it does is chosen by a "mode" file.
const FAKE_SSH := """#!/bin/sh
d=$(dirname "$0")
echo "$1" >> "$d/hosts"
case "$(cat "$d/mode" 2>/dev/null)" in
denied) echo "git@$1: Permission denied (publickey)." >&2; exit 255 ;;
unknown) echo "Host key verification failed." >&2; exit 255 ;;
hang) sleep 60; exit 255 ;;
esac
shift
exec sh -c "$*"
"""


func run() -> void:
	var shared := make_shared("ssh")
	var fake := dir.path_join("fake-ssh")
	write(fake.path_join("ssh.sh"), FAKE_SSH)
	_use_fake_ssh(shared.mine, fake, shared.remote)
	_use_fake_ssh(shared.theirs, fake, shared.remote)

	_fetch_and_pull(shared, fake)
	_push(shared)
	_ssh_errors(shared, fake)
	await _cancel_hanging_connection(shared, fake)


## Points "origin" at an scp-style SSH address ("fakehost:/path/remote.git") served by the fake.
## Written straight into the config file (Godot mangles quotes in arguments on Windows).
func _use_fake_ssh(repo: String, fake: String, remote: String) -> void:
	var config := FileAccess.open(repo.path_join(".git/config"), FileAccess.READ_WRITE)
	config.seek_end()
	# "simple": git passes just the host and the command, no OpenSSH options.
	config.store_string("[core]\n\tsshCommand = sh '%s'\n[ssh]\n\tvariant = simple\n" % fake.path_join("ssh.sh"))
	config.close()
	git(repo, ["remote", "set-url", "origin", "fakehost:" + remote])


func _set_mode(fake: String, mode: String) -> void:
	write(fake.path_join("mode"), mode)


func _fetch_and_pull(shared: Dictionary, fake: String) -> void:
	teammate_pushes(shared, "x.txt", "x2\n")
	var r := open(shared.mine)
	check("fetch over SSH", r.fetch() == OK, GitRepository.get_last_error())
	check("went through the configured ssh command", read(fake.path_join("hosts")).contains("fakehost"))
	check("new commit arrived", r.get_sync_status().behind == 1, r.get_sync_status())
	check("pull over SSH", r.pull() == OK, GitRepository.get_last_error())
	check("pulled the change", read(shared.mine.path_join("x.txt")) == "x2\n")


func _push(shared: Dictionary) -> void:
	write(shared.mine.path_join("a.txt"), "a\n")
	commit_all(shared.mine, "Add a")
	var r := open(shared.mine)
	check("push over SSH", r.push() == OK, GitRepository.get_last_error())
	check("remote got it", git(shared.remote, ["rev-parse", "main"]) == git(shared.mine, ["rev-parse", "HEAD"]))


func _ssh_errors(shared: Dictionary, fake: String) -> void:
	var r := open(shared.mine)
	_set_mode(fake, "denied")
	check("rejected key: fetch fails", r.fetch() != OK)
	check("says SSH couldn't log in, with ssh's own words", GitRepository.get_last_error().contains("SSH couldn't log in") and GitRepository.get_last_error().contains("Permission denied (publickey)"), GitRepository.get_last_error())
	check("rejected key: push fails the same way", r.push() != OK and GitRepository.get_last_error().contains("SSH couldn't log in"), GitRepository.get_last_error())

	_set_mode(fake, "unknown")
	check("unknown server: fetch fails", r.fetch() != OK)
	check("says to confirm the server once", GitRepository.get_last_error().contains("doesn't know this server"), GitRepository.get_last_error())
	_set_mode(fake, "")


func _cancel_hanging_connection(shared: Dictionary, fake: String) -> void:
	# ssh stuck connecting: Cancel must end it right away, not when ssh gives up.
	_set_mode(fake, "hang")
	var thread := Thread.new()
	var start := Time.get_ticks_msec()
	thread.start(func():
		var r := open(shared.mine)
		return [r.fetch(), GitRepository.get_last_error()])
	while Time.get_ticks_msec() - start < 1500:
		await tree.process_frame
	GitRepository.cancel_network()
	while thread.is_alive() and Time.get_ticks_msec() - start < 20000:
		await tree.process_frame
	var finished := not thread.is_alive()
	var result: Array = thread.wait_to_finish() if finished else [OK, ""]
	_set_mode(fake, "")
	check("cancel ends a hanging SSH connection quickly", finished and Time.get_ticks_msec() - start < 10000, "%d ms" % (Time.get_ticks_msec() - start))
	check("reported as canceled", result[0] == ERR_SKIP, result)
