extends "res://tests/test_case.gd"
## Logins through git's credential helper: that a login that worked is confirmed (so a helper
## like Git Credential Manager saves it), and a rejected one is removed and, if allowed, asked
## for again. Uses a fake helper that logs what git asks it, and a small local HTTP server that
## wants a password. Nothing here touches the real credential store.

const USER := "tester"
const RIGHT := "right-password"
const WRONG := "wrong-password"

## The fake credential helper. Logs each request, answers "get" with the login in ./password,
## and on "erase" replaces that login with ./next (what a fresh sign-in would give).
const HELPER := """#!/bin/sh
d=$(dirname "$0")
cat >/dev/null
echo "$1" >> "$d/log"
case "$1" in
get)
	p=$(cat "$d/password")
	if [ -n "$p" ]; then
		echo "username={user}"
		echo "password=$p"
	fi
	;;
erase)
	cp "$d/next" "$d/password"
	;;
esac
"""

var _server := TCPServer.new()
var _thread := Thread.new()
var _stop := false
var _bare := ""


func run() -> void:
	var shared := make_shared("login")
	_bare = shared.remote
	if _server.listen(0, "127.0.0.1") != OK:
		check("local test server started", false)
		return
	_thread.start(_serve)

	_saved_login_works()
	_stale_login_is_replaced()
	_stale_login_without_prompts()
	_rejected_twice()
	_no_login()
	_no_helper()
	_no_git()

	_stop = true
	_thread.wait_to_finish()
	_server.stop()


# A saved login that works is confirmed to the helper.
func _saved_login_works() -> void:
	var repo := _clone_with_helper("works", RIGHT, RIGHT)
	var r := open(repo)
	check("fetch with a saved login", r.fetch() == OK, GitRepository.get_last_error())
	check("login confirmed to the helper", _log(repo) == ["get", "store"], _log(repo))


# The server rejects the saved login: the helper forgets it and is asked again (which is where
# Git Credential Manager opens its sign-in window), and the new login is saved.
func _stale_login_is_replaced() -> void:
	var repo := _clone_with_helper("stale", WRONG, RIGHT)
	var r := open(repo)
	check("fetch recovers from a rejected login", r.fetch() == OK, GitRepository.get_last_error())
	check("old login removed, new one asked for and saved", _log(repo) == ["get", "erase", "get", "store"], _log(repo))


# Background fetches never ask for a new login, but still remove a rejected one.
func _stale_login_without_prompts() -> void:
	var repo := _clone_with_helper("stale-quiet", WRONG, RIGHT)
	var r := open(repo)
	r.set_login_prompts_allowed(false)
	check("quiet fetch with a rejected login fails", r.fetch() != OK)
	check("says the login was removed", GitRepository.get_last_error().contains("removed"), GitRepository.get_last_error())
	check("removed without asking again", _log(repo) == ["get", "erase"], _log(repo))


# A fresh login rejected too: give up instead of asking forever.
func _rejected_twice() -> void:
	var repo := _clone_with_helper("rejected", WRONG, WRONG)
	var r := open(repo)
	check("fetch with logins that never work fails", r.fetch() != OK)
	check("says the server rejected it", GitRepository.get_last_error().contains("rejected"), GitRepository.get_last_error())
	check("asked twice, never saved", _log(repo) == ["get", "erase", "get", "erase"], _log(repo))


# The helper has no login to give.
func _no_login() -> void:
	var repo := _clone_with_helper("none", "", "")
	var r := open(repo)
	check("fetch without any login fails", r.fetch() != OK)
	check("explains it", not GitRepository.get_last_error().is_empty(), GitRepository.get_last_error())
	check("nothing saved", not _log(repo).has("store"), _log(repo))
	check("doesn't claim there's no helper", not GitRepository.get_last_error().contains("no credential helper"), GitRepository.get_last_error())


# No credential helper at all (common on macOS and Linux): say how to set one up. Signing in
# once in a terminal wouldn't help, since nothing would save that login.
func _no_helper() -> void:
	var repo := _clone_with_helper("no-helper", RIGHT, RIGHT)
	var config := FileAccess.open(repo.path_join(".git/config"), FileAccess.READ_WRITE)
	config.seek_end()
	config.store_string("[credential]\n\thelper =\n")
	config.close()
	var r := open(repo)
	check("fetch without a helper fails", r.fetch() != OK)
	var message := GitRepository.get_last_error()
	check("says there's no helper, and how to get one", message.contains("no credential helper") and message.contains("gh auth login") and message.contains("Git Credential Manager"), message)
	check("helper not asked", _log(repo).is_empty(), _log(repo))


# Without git there's no credential helper to ask: say so, instead of blaming the helper.
func _no_git() -> void:
	var repo := _clone_with_helper("no-git", RIGHT, RIGHT)
	var r := open(repo)
	GitRepository.set_git_program("git-not-installed-for-tests")
	check("fetch needing a login fails without git", r.fetch() != OK)
	var message := GitRepository.get_last_error()
	check("says logins need git", message.contains("credential helper") and message.contains("git-scm.com"), message)
	GitRepository.set_git_program("git")
	check("works again with git", r.fetch() == OK, GitRepository.get_last_error())


## A clone of the test remote, fetched over HTTP from the local server, with a fake credential
## helper that answers p_first until asked to erase it, and p_after_erase after that.
func _clone_with_helper(name: String, first: String, after_erase: String) -> String:
	var repo := dir.path_join(name)
	OS.execute("git", ["clone", "-q", _bare, repo])
	var state := dir.path_join(name + "-helper")
	DirAccess.make_dir_recursive_absolute(state)
	write(state.path_join("password"), first)
	write(state.path_join("next"), after_erase)
	write(state.path_join("log"), "")
	git(repo, ["remote", "set-url", "origin", "http://127.0.0.1:%d/repo.git" % _server.get_local_port()])
	write(state.path_join("helper.sh"), HELPER.replace("{user}", USER))
	# Written straight into the config file: Godot drops empty arguments, so
	# `git config credential.helper ""` can't be run from here. The empty entry clears helpers
	# from the global and system config (e.g. the real Git Credential Manager), so only the
	# fake one is ever asked.
	var config := FileAccess.open(repo.path_join(".git/config"), FileAccess.READ_WRITE)
	config.seek_end()
	config.store_string("[credential]\n\thelper =\n\thelper = \"!sh '%s'\"\n" % state.path_join("helper.sh"))
	config.close()
	return repo


func _log(repo: String) -> Array:
	return Array(read(repo + "-helper/log").split("\n", false))


# Serves the test remote's refs over smart HTTP to requests with the right password. The clones
# already have every commit, so a fetch needs nothing more than the ref list.
func _serve() -> void:
	var expected := "Basic " + Marshalls.utf8_to_base64("%s:%s" % [USER, RIGHT])
	while not _stop:
		if not _server.is_connection_available():
			OS.delay_msec(5)
			continue
		var peer := _server.take_connection()
		var request := _read_request(peer)
		if request.contains("\r\nauthorization: %s\r\n" % expected.to_lower()):
			var body := "001e# service=git-upload-pack\n0000".to_utf8_buffer()
			body.append_array(_advertised_refs())
			var head := "HTTP/1.1 200 OK\r\nContent-Type: application/x-git-upload-pack-advertisement\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % body.size()
			peer.put_data(head.to_utf8_buffer())
			peer.put_data(body)
		else:
			peer.put_data("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"test\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".to_utf8_buffer())
		OS.delay_msec(20)
		peer.disconnect_from_host()


# Request line and headers, lowercased (header names are case-insensitive).
func _read_request(peer: StreamPeerTCP) -> String:
	var data := PackedByteArray()
	var deadline := Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() < deadline:
		peer.poll()
		var available := peer.get_available_bytes()
		if available > 0:
			data.append_array(peer.get_data(available)[1])
			if data.get_string_from_utf8().contains("\r\n\r\n"):
				break
		else:
			OS.delay_msec(2)
	return data.get_string_from_utf8().to_lower()


# `git upload-pack --advertise-refs` output. Binary-safe (the first line holds a NUL), so it's
# read from a pipe rather than through OS.execute's String output. Byte by byte: get_buffer()
# drops a partial last chunk when the pipe closes, which cut off the final flush packet.
func _advertised_refs() -> PackedByteArray:
	var process := OS.execute_with_pipe("git", ["upload-pack", "--stateless-rpc", "--advertise-refs", _bare], true)
	var io: FileAccess = process.stdio
	var out := PackedByteArray()
	while true:
		var byte := io.get_8()
		if io.get_error() != OK:
			break
		out.push_back(byte)
	OS.get_process_exit_code(process.pid)
	return out

