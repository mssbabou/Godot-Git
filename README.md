# godot-git

A Git panel for the Godot 4.7 editor, built as a GDExtension (C++, [godot-cpp](https://github.com/godotengine/godot-cpp)) on top of [libgit2](https://libgit2.org/).

It adds a **Git** dock next to the Inspector:

- Branch picker with local and remote branches (picking a remote one checks it out as a tracking branch) and *New Branch…*; ⟳ fetches
- Under the commit message: *Commit*, plus *Pull* (↓ commits to get) and *Push* (↑ commits to send, or *Publish* for a new branch). Pull and Push only appear when the repository has a remote.
- Staged Changes / Changes sections with +/− line totals; hover a file for stage/unstage/discard, right-click for more
- Recent History (unpushed commits highlighted); stage/unstage/discard all, new branch and more in the ⋮ menu

Network operations run in the background. Pull fast-forwards, or makes a merge commit when both sides have new commits. Uncommitted changes are set aside during the merge and put back afterwards (like `git pull --autostash`); if the pull changed a file you have uncommitted edits to, your edits stay untouched in a git stash and the panel tells you. If the merge itself would conflict, nothing changes and the panel says so (resolving conflicts in the panel isn't supported yet).

Authentication: HTTPS remotes use git's own credential helper (e.g. Git Credential Manager), so if `git fetch` works in a terminal it works here. SSH remotes use the system `ssh` client with your keys/agent.

## Install (users)

1. Download `godot_git-<version>.zip` from the [releases](../../releases).
2. Extract it into your Godot project, so you get `res://addons/godot_git/`.
3. Open (or restart) the editor. The **Git** tab appears next to the Inspector.

The plugin only runs in the editor. **On Godot 4.7**, add `addons/godot_git/*` to your export preset's *Resources → Filters to exclude files*. Otherwise exports show a warning and the game logs a harmless error on startup. From Godot 4.8 this is automatic.

Supported: Windows (x86_64, arm64), Linux (x86_64, arm64), macOS (universal).

## Layout

| Path | What |
|---|---|
| `src/` | Extension source: `GitRepository` (libgit2 wrapper), `GitDock` (the panel), `GitEditorPlugin` |
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

## CI and releases

`.github/workflows/build.yml` runs on every push to `master` and every pull request:

1. Builds the plugin on Windows x86_64/arm64, Linux x86_64/arm64 and macOS (universal).
2. Packages everything into one `godot_git` zip, downloadable from the workflow run's *Artifacts*.

To release: create a GitHub release (e.g. tag `v0.1.0`). The same workflow runs and attaches `godot_git-v0.1.0.zip` to the release.

## GDScript API

```gdscript
var repo := GitRepository.new()
repo.open("res://")            # also accepts absolute paths; searches parent folders
repo.get_workdir()
repo.get_current_branch()
repo.get_branches()            # local branch names
repo.get_remote_branches()     # ["origin/master", ...]
repo.get_remotes()
repo.get_sync_status()         # { branch, upstream, ahead, behind, has_remotes }
repo.get_status()              # [{ path, index, worktree }, ...]
repo.get_line_stats(staged)    # { path: Vector2i(added, removed) }, binary = (-1, -1)
repo.get_commits(50)           # [{ id, hash, summary, message, author, time, unpushed }, ...]
repo.stage(path) / repo.unstage(path) / repo.stage_all() / repo.unstage_all()
repo.discard(path)             # can't be undone
repo.commit(message)           # uses user.name / user.email from git config
repo.checkout_branch(name)     # local, or "origin/x" to create a tracking branch
repo.create_branch(name)       # from HEAD, and switches to it
repo.fetch() / repo.pull() / repo.push()   # blocking; the dock runs them on a thread
GitRepository.get_last_error()
GitRepository.get_libgit2_version()
```

## License

MIT, see [LICENSE](LICENSE). Release zips also include the licenses of libgit2 (GPLv2 with linking exception) and godot-cpp (MIT).
