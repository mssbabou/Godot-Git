# Godot Git

A Git panel for the Godot 4.7 editor: stage, commit, pull, push, switch branches and review your changes without leaving Godot.

## What it does

- **Git dock** next to the Inspector: your staged and unstaged changes, commit (and amend), pull, push, fetch, and a branch picker.
- **Diff panel** at the bottom: click any file to see what changed, with the script editor's colors and syntax highlighting, unified or side by side. Images show before and after.
- **History**: click a commit to see its message and the files it changed, and click a file to see that commit's change.
- **Clear feedback**: what the panel is doing shows with progress and Cancel, and the last result or error stays until the next one.

## It won't leave you in a mess

- A pull either completes with your uncommitted work untouched, or refuses up front and names the files in the way. It never hides your changes in a stash.
- A branch switch that can't finish (for example because a file is open elsewhere) puts everything back.
- Merges that would conflict are undone completely, not left half-done.

## Works with your existing git setup

- **Logins** come from git's own credential helper. If `git fetch` works in a terminal, it works here, and with Git Credential Manager you can sign in from the panel.
- **SSH** uses your keys, agent and `~/.ssh/config`, like git in a terminal.
- **Hooks and commit signing** run exactly as they do in a terminal.
- **Git LFS** files are handled like the git command line does (git-lfs must be installed).

## Install

1. Download `godot_git-<version>.zip` from the [releases](https://github.com/mssbabou/Godot-Git/releases).
2. Extract it into your project, so you get `res://addons/godot_git/`.
3. Open (or restart) the editor. The **Git** tab appears next to the Inspector.

**Requirements:** Godot 4.7 or newer, on Windows (x86_64, arm64), Linux (x86_64, arm64, glibc 2.34+) or macOS (10.13+ Intel, 11+ Apple Silicon). Installing [git](https://git-scm.com) is recommended: logins, SSH, LFS, hooks and signing need it. Everything else works without it, and the panel tells you when something needs git.

**Exports on Godot 4.7:** the plugin only runs in the editor. Add `addons/godot_git/*` to your export preset's *Resources → Filters to exclude files*, or exports warn and the game logs one harmless error. From Godot 4.8 this is automatic.

**macOS, zip downloaded in a browser:** macOS may block the library. Run `xattr -dr com.apple.quarantine addons/godot_git` in your project folder, or allow it under *System Settings → Privacy & Security*.

## Building from source

```bash
git submodule update --init --recursive
scons            # builds into project/addons/godot_git/bin/
```

You need a C++17 compiler, Python with SCons (`pip install scons`) and CMake. Development notes (architecture, tests, gotchas) are in [CLAUDE.md](https://github.com/mssbabou/Godot-Git/blob/master/CLAUDE.md).

## License

MIT, see [LICENSE](LICENSE). The release zips also include the licenses of [libgit2](https://libgit2.org/) (GPLv2 with linking exception) and [godot-cpp](https://github.com/godotengine/godot-cpp) (MIT).
