extends SceneTree
## Runs the GitRepository test suites headless:
##
##   godot --headless --path project -s res://tests/run_tests.gd [-- --online] [-- <suite name>...]
##
## --online also runs the tests that talk to GitHub. Names (e.g. "pull_safety") limit the run to
## those suites. Exits with code 1 if anything failed. Scratch repositories go to the OS temp
## folder; they're deleted after a clean run and kept (path printed) after a failure.

const SUITES := [
	"res://tests/test_local.gd",
	"res://tests/test_sync.gd",
	"res://tests/test_pull_safety.gd",
	"res://tests/test_checkout_safety.gd",
	"res://tests/test_amend.gd",
	"res://tests/test_hooks.gd",
	"res://tests/test_credentials.gd",
	"res://tests/test_ssh.gd",
	"res://tests/test_lfs.gd",
	"res://tests/test_online.gd",
]


func _initialize() -> void:
	_main()


func _main() -> void:
	var args := OS.get_cmdline_user_args()
	var online := args.has("--online")
	var only := Array(args).filter(func(a: String) -> bool: return not a.begins_with("--"))

	if not ClassDB.class_exists("GitRepository"):
		printerr("GitRepository isn't available. Build the extension (scons) and import the project once.")
		quit(1)
		return

	var root := OS.get_temp_dir().path_join("godot-git-tests").path_join("%d-%d" % [Time.get_unix_time_from_system(), Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(root)
	print("libgit2 ", GitRepository.get_libgit2_version(), ", scratch folder ", root)

	var checks := 0
	var failures := 0
	for path: String in SUITES:
		var name := path.get_file().get_basename().trim_prefix("test_")
		if not only.is_empty() and not only.has(name):
			continue
		var suite = load(path).new()
		suite.dir = root.path_join(name)
		suite.online = online
		suite.tree = self
		DirAccess.make_dir_recursive_absolute(suite.dir)
		print("\n", name)
		await suite.run()
		checks += suite.checks
		failures += suite.failures

	print("\n%d checks, %d failed" % [checks, failures])
	if failures == 0:
		_remove(root)
	else:
		print("Scratch repositories kept in ", root)
	quit(1 if failures > 0 else 0)


func _remove(path: String) -> void:
	# Git marks its object files read-only, which DirAccess can't delete on Windows.
	# One string for cmd: Godot quotes each argument, and cmd doesn't parse separately quoted words.
	if OS.get_name() == "Windows":
		OS.execute("cmd", ["/c", "rd /s /q \"%s\"" % path.replace("/", "\\")])
	else:
		OS.execute("rm", ["-rf", path])
