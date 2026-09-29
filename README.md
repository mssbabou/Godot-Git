# Godot Git

A Git panel for the Godot editor: stage, commit, pull, push, switch branches and review your changes without leaving Godot.

![The Git dock and the Diff panel](https://raw.githubusercontent.com/mssbabou/Godot-Git/master/store/screenshot_diff.png)

Godot Git adds a **Git** dock next to the Inspector and a **Diff** panel at the bottom of the editor.

- **Changes**: see what's staged and unstaged, stage or discard files, commit and amend.
- **Sync**: fetch, pull and push, with progress, Cancel, and a clear result or error that stays visible.
- **Diff panel**: click a file to see what changed, with syntax highlighting, unified or side by side. Images show before and after.
- **History**: click a commit to see its message and changed files, and each file's diff.

**Sign in from the panel.** Pull or push a private repository and, with Git Credential Manager (included with Git for Windows), your browser opens to sign in once. No terminal, no tokens to paste. SSH uses your own keys and config, and hooks, commit signing and Git LFS work as they do in a terminal.

**It won't leave you in a mess.** A pull keeps your uncommitted work: if a teammate changed a file you're editing, your edit is merged into their version when the lines don't overlap, and if they do, the pull refuses up front and names the files. It never hides your changes in a stash or leaves conflict markers in them. A branch switch that can't finish (a file open in another program, say) puts everything back, and conflicting merges are fully undone.

**Native and fast.** Built in C++ on [libgit2](https://libgit2.org/): no git process per click, no polling, and it stays quick with thousands of changed files. Stage, commit, branch and history work even without git installed; the panel tells you when something needs it.

This is its own panel, not a backend for Godot's built-in version control dock (unlike the official Godot Git Plugin).

## Install

1. Download `godot_git-<version>.zip` from the [releases](https://github.com/mssbabou/Godot-Git/releases), or get Godot Git from the Godot Asset Store.
2. Extract it into your project, so you get `res://addons/godot_git/`.
3. Open (or restart) the editor. The **Git** tab appears next to the Inspector.

**Requirements:** Godot 4.7 or newer, on Windows (x86_64, arm64), Linux (x86_64, arm64, glibc 2.34+) or macOS (10.13+ Intel, 11+ Apple Silicon). Installing [git](https://git-scm.com) is recommended: sign-ins, SSH, LFS, hooks and signing need it.

**Exports on Godot 4.7:** the plugin only runs in the editor. Add `addons/godot_git/*` to your export preset's *Resources → Filters to exclude files*, or exports warn and the game logs one harmless error. From Godot 4.8 this is automatic.

**macOS, zip downloaded in a browser:** macOS may block the library. Run `xattr -dr com.apple.quarantine addons/godot_git` in your project folder, or allow it under *System Settings → Privacy & Security*.

## Verifying a download

The plugin is a compiled library, so you can't read it like a GDScript plugin. Instead, every release proves where it came from:

- **Built in public.** Releases are built by this repository's [GitHub Actions](https://github.com/mssbabou/Godot-Git/blob/master/.github/workflows/build.yml) from a tagged commit, from scratch (no cached build output), after the tests pass on all five platforms. Nobody uploads a binary by hand, and published releases are immutable: their files can't be replaced afterwards.
- **Signed build attestations.** Each release zip and each library in it has a signed record, kept in a public log, saying which commit and which build made it. With the [GitHub CLI](https://cli.github.com/):

  ```bash
  gh attestation verify godot_git-v0.4.0.zip --repo mssbabou/Godot-Git
  ```

  This works on an installed copy too, e.g. `addons/godot_git/bin/windows/libgodot_git.windows.editor.x86_64.dll`. It fails if a single byte differs.
- **Checksums.** Each release has a `SHA256SUMS` file covering the zip and every library, with paths as they are in a project. From your project folder (on Windows, in Git Bash): `sha256sum -c SHA256SUMS --ignore-missing`.
- **In the editor.** The last item in the dock's ⋮ menu says what you're running, e.g. "Godot Git v0.4.0 · Built by GitHub from 5cbbd8d", and opens that build's public log. A build you made yourself says "Local Build".

The dependencies are official, pinned releases: libgit2 1.9.7 and godot-cpp 10.0.0 (git submodules in `thirdparty/`).

Releases up to v0.3.0 were made before this process and have no attestations.

## Building from source

The first build takes a few minutes, because it also compiles libgit2. Later builds only recompile what changed.

**1. Install the tools**

- **Windows:** [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) (2022 or newer) with the *Desktop development with C++* workload, [Python 3](https://www.python.org/downloads/), [CMake](https://cmake.org/download/) and [git](https://git-scm.com). Then `pip install scons`.
- **Linux** (Debian/Ubuntu): `sudo apt install build-essential cmake python3-pip git`, then `pip install scons`. On Arch: `sudo pacman -S base-devel cmake scons git`.
- **macOS:** `xcode-select --install`, then `brew install cmake scons`.

**2. Get the source**, including the submodules:

```bash
git clone --recursive https://github.com/mssbabou/Godot-Git.git
cd Godot-Git
```

**3. Build the zip:**

```bash
scons package
```

This writes `dist/godot_git-<version>.zip`, ready to extract into a project like a downloaded release. It contains the library for the platform you built on only. A zip for every platform needs a build on each; the [CI workflow](https://github.com/mssbabou/Godot-Git/blob/master/.github/workflows/build.yml) does exactly that, and `python tools/package.py --require-all` packages the results.

Plain `scons` builds the library into `project/addons/godot_git/bin/` without zipping. `project/` is a development project with the plugin installed; open it with Godot to try your build.

Your own build is not byte-for-byte identical to a release (compilers embed paths and timestamps). To check a release, use its attestation (above), not a rebuild.

## License

MIT, see [LICENSE](https://github.com/mssbabou/Godot-Git/blob/master/LICENSE). The release zips also include the licenses of [libgit2](https://libgit2.org/) (GPLv2 with linking exception) and [godot-cpp](https://github.com/godotengine/godot-cpp) (MIT).
