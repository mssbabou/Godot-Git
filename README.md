# Godot Git

A Git panel for the Godot editor that knows it's in Godot, and won't leave your project in a mess.

![The Git dock and the Git Diff panel](https://raw.githubusercontent.com/mssbabou/Godot-Git/master/store/screenshot_diff.png)

## What it brings

- **Pulls that can't wreck your work.** Your uncommitted edits survive a pull: merged into your teammate's version when the lines don't overlap; when they clash, the pull asks first and changes nothing until you say so. No stash you didn't ask for, no half-switched branches.
- **Conflicts resolved in the editor.** Each clash shows what you and your teammate each did to the original, with one click for yours, theirs or both, and the result editable below. Conflicting images show both versions. Abort Merge always puts everything back.
- **Made for Godot projects.** `.uid` and `.import` files ride along with the file they belong to instead of doubling your change list. Open scenes and scripts reload after a pull or branch switch, so saving can't undo it. Import settings show as settings ("Compress › Mode  Lossless → VRAM Compressed"), images as before and after.
- **Changes where you work.** Changed lines are marked in the script editor as you type, with a click to see or undo them. Changed files are colored in the FileSystem dock.
- **Always says what it's doing.** Every pull, push and fetch shows progress, can be canceled, and leaves a result or error that stays until you've read it.
- **Sign in without a terminal.** Private repositories open a browser sign-in once (through Git Credential Manager). SSH, hooks, commit signing and Git LFS work as they do in a terminal.
- **Native and fast.** C++ on [libgit2](https://libgit2.org/): no git process per click, no polling, quick with thousands of changed files.

Plus the everyday things: stage, commit, amend, fetch, pull, push; branches with search and ahead/behind; stash; history with search, one file's history, undo last commit, revert and restore.

## Install

1. Download `godot_git-<version>.zip` from the [releases](https://github.com/mssbabou/Godot-Git/releases), or get it from the Godot Asset Store.
2. Extract it into your project, so you get `res://addons/godot_git/`.
3. Restart the editor. The **Git** tab appears next to the Inspector, **Git Diff** at the bottom.

Needs Godot 4.7+ on Windows, Linux (glibc 2.34+) or macOS (10.13+). Installing [git](https://git-scm.com) is recommended: sign-ins, SSH, LFS, hooks and signing use it.

Want to check a download came from this repository? See [Verifying a release](#verifying-a-release).

## Build it yourself

Rather not run a binary you didn't build? It's three commands:

```bash
git clone --recursive https://github.com/mssbabou/Godot-Git.git
cd Godot-Git
scons package
```

That writes `dist/godot_git-<version>.zip` for your platform, ready to extract into a project. The first build takes a few minutes (it compiles libgit2 too). Plain `scons` builds into `project/`, a test project with the plugin installed.

You need a C++ compiler, Python 3 with SCons, CMake and git:
- **Windows:** [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) with *Desktop development with C++*, [Python](https://www.python.org/downloads/), [CMake](https://cmake.org/download/), then `pip install scons`.
- **Linux:** `sudo apt install build-essential cmake python3-pip git && pip install scons` (Arch: `sudo pacman -S base-devel cmake scons git`).
- **macOS:** `xcode-select --install`, then `brew install cmake scons`.

## Good to know

- **Settings:** script editor marks, FileSystem colors and automatic fetching are on by default; turn them off under *⋮ → Settings...*. The whole plugin can be switched off per project under *Project Settings → Plugins*.
- **Exports on Godot 4.7:** add `addons/godot_git/*` to your export preset's *Filters to exclude files*, or the game logs one harmless error. Automatic from Godot 4.8.
- **macOS, zip from a browser:** if macOS blocks the library, run `xattr -dr com.apple.quarantine addons/godot_git` in your project folder.

## License

MIT, see [LICENSE](https://github.com/mssbabou/Godot-Git/blob/master/LICENSE). Release zips include the licenses of libgit2 (GPLv2 with linking exception) and godot-cpp (MIT).

## Verifying a release

Releases are built from a tagged commit by this repository's public [CI](https://github.com/mssbabou/Godot-Git/blob/master/.github/workflows/build.yml), never uploaded by hand. Each has signed build attestations and a `SHA256SUMS` file:

```bash
gh attestation verify godot_git-<version>.zip --repo mssbabou/Godot-Git
```

The last item in the dock's ⋮ menu says which build you're running.
