# godot-git

A Git panel for the Godot 4.7 editor, built as a GDExtension (C++, [godot-cpp](https://github.com/godotengine/godot-cpp)) on top of [libgit2](https://libgit2.org/).

It adds a **Git** dock next to the Inspector:

- Branch picker with local and remote branches (picking a remote one checks it out as a tracking branch) and *New Branch…*
- *Fetch*, *Pull* (↓ commits to get) and *Push* (↑ commits to send, or *Publish* for a new branch) in one row above the commit message, when the repository has a remote
- *Commit* under the message, with *Amend* to redo the last commit (new message, or add what you forgot to stage) as long as it isn't pushed yet
- Staged Changes / Changes sections with +/− line totals (per file in the tooltip); hover a file for stage/unstage/discard, right-click for more
- Recent History (unpushed commits highlighted); stage/unstage/discard all, new branch and more in the ⋮ menu
- A status line under the branch picker: what's running (with progress and Cancel), then the last result or error, which stays until the next one
- Automatic fetching every few minutes in the background (never opens a sign-in window; can be turned off in the ⋮ menu)

Network operations run in the background. Pull fast-forwards, or makes a merge commit when both sides have new commits. Your uncommitted changes to other files are kept as they are. If the new commits change a file you have uncommitted changes to, the pull is refused up front and names the files, so nothing is changed and nothing ends up in a stash; commit or discard those changes, then pull. If the merge itself would conflict, nothing changes and the panel says so (resolving conflicts in the panel isn't supported yet).

Authentication: HTTPS remotes use git's own credential helper (e.g. Git Credential Manager), so if `git fetch` works in a terminal it works here. With Git Credential Manager you can also sign in from the panel: the first fetch, pull or push opens its sign-in window (e.g. "Sign in with your browser" for GitHub), and the login is saved for next time. SSH remotes go through git, so they use the same `ssh`, keys, agent and `~/.ssh/config` as git in a terminal. The panel can't ask for a key's passphrase, so such a key must be in ssh-agent; the panel says so when that's the problem.

Hooks and signing: if your repository has commit or push hooks (e.g. a `pre-commit` linter) or signs commits (`commit.gpgsign`), the panel commits and pushes through git itself, so they run exactly as in a terminal, and a hook's message shows in the panel when it stops something.

Git LFS: projects that store files with [Git LFS](https://git-lfs.com) work as with the git command line (checkout, pull, commit and push all go through the real `git-lfs`). Git LFS must be installed; without it the panel refuses the actions that would damage LFS files.

## Install (users)

1. Download `godot_git-<version>.zip` from the [releases](../../releases).
2. Extract it into your Godot project, so you get `res://addons/godot_git/`.
3. Open (or restart) the editor. The **Git** tab appears next to the Inspector.

The plugin only runs in the editor. **On Godot 4.7**, add `addons/godot_git/*` to your export preset's *Resources → Filters to exclude files*. Otherwise exports show a warning and the game logs a harmless error on startup. From Godot 4.8 this is automatic.

Supported: Windows (x86_64, arm64), Linux (x86_64, arm64), macOS (universal).

## Layout

| Path | What |
|---|---|
| `src/git/` | `GitRepository`, the libgit2 wrapper |
| `src/editor/` | The Git dock and the editor plugin that adds it |
| `project/` | Godot project used to develop and test the plugin |
| `project/addons/godot_git/` | The plugin itself: this folder is what gets shipped |
| `project/addons/godot_git/godot_git.gdextension` | Tells Godot which library to load per platform |
| `thirdparty/godot-cpp` | C++ bindings (submodule, 10.0.0-stable, targets API 4.7) |
| `thirdparty/libgit2` | libgit2 v1.9.7 (submodule), built as a static lib via CMake |
| `tools/libgit2.py` | SCons helper that configures, builds, and links libgit2 |
| `tools/package.py` | Zips the addon into `dist/` |
| `.github/workflows/build.yml` | CI: builds all platforms, packages, attaches to releases |

## Prerequisites (Windows)

- Visual Studio 2026 Build Tools with the **Desktop development with C++** workload (2022 also works)
- Python 3.9+ and SCons: `pip install scons`
- CMake 3.16+
- Godot 4.7.x

Linux/macOS: a C++17 compiler, Python + SCons, CMake.

## Build

```bash
git submodule update --init --recursive
scons                      # editor build, output in project/addons/godot_git/bin/<platform>/
scons package              # build, then zip the addon into dist/godot_git-<version>.zip
scons compiledb=yes        # also write compile_commands.json for clangd / IDEs
```

The first build compiles libgit2 into `build/libgit2/<platform>.<arch>/` (a few minutes); later builds reuse it. Pass `rebuild_libgit2=yes` to rebuild it.

A local `scons package` only contains your own platform. Full multi-platform zips come from CI.

## Test in the editor

`project/` is the test project. Open it in Godot 4.7 (`godot -e --path project`) and use the **Git** tab next to the Inspector. It works on this repo. Double-click a file to open it.

Dev loop: edit C++ → run `scons` → click back into the editor. Hot reload is on, so the editor picks up the new library without restarting. If a change doesn't show (or the editor misbehaves after a reload), restart the editor.

## Tests

`project/tests/` holds headless tests for `GitRepository`. They build throwaway repositories with the git CLI and check the results against what git says. (`GitRepository` is exposed to GDScript only for these tests; it isn't a stable API and may change.) Build first, open `project/` in the editor once (so the extension is registered), then:

```bash
godot --headless --path project -s res://tests/run_tests.gd                 # local tests only
godot --headless --path project -s res://tests/run_tests.gd -- --online     # also the ones that talk to GitHub
godot --headless --path project -s res://tests/run_tests.gd -- pull_safety  # just one suite
```

The exit code is 1 if anything failed.

## CI and releases

`.github/workflows/build.yml` runs on every push to `master` and every pull request:

1. Builds the plugin on Windows x86_64/arm64, Linux x86_64/arm64 and macOS (universal).
2. Runs the test suite (`project/tests`) with the official Godot 4.7.2, headless, on Windows x86_64/arm64, Linux x86_64/arm64 and macOS.
3. Packages everything into one `godot_git` zip, downloadable from the workflow run's *Artifacts*.

To release: create a GitHub release (e.g. tag `v0.1.0`). The same workflow runs and attaches `godot_git-v0.1.0.zip` to the release.

## License

MIT, see [LICENSE](LICENSE). Release zips also include the licenses of libgit2 (GPLv2 with linking exception) and godot-cpp (MIT).
