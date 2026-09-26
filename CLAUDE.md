# CLAUDE.md

Guidance for Claude (and humans) working on **godot-git**: a Git panel for the Godot 4.7 editor, written as a C++ GDExtension on top of [godot-cpp](https://github.com/godotengine/godot-cpp) and [libgit2](https://libgit2.org/).

Repository: https://github.com/mssbabou/Godot-Git (public, default branch `master`).

The first half is reference (what, where, how to build and test, conventions). The second half is the hard-won stuff: engine and library gotchas, design decisions with their reasons, known gaps, and where I'd take it next.

---

## Product philosophy (read this first)

These are the maintainer's principles. They outrank convenience, cleverness, and feature count.

1. **Never overpromise.** The panel shows what it can do, and nothing more. Don't show a feature that looks complete but isn't, e.g. a pull that silently leaves you with a stash the panel can't manage, or a menu that hints at stashing when there is no stash support. If something can't be done in the current state, hide it or disable it with a tooltip that says why. A smaller tool that always delivers beats a bigger one that sometimes doesn't.
2. **The UI follows Godot's design language, and it must be sleek, readable, modern, functional and good-looking.** Clean UI is paramount, not a nice-to-have. Use the editor's own controls, theme colors, icons, fonts and spacing, but hold them to a high bar: nothing cramped, nothing clipped, nothing that reads as "programmer UI". Look at a screenshot after every visual change.
3. **You must be able to trust your tools.** The panel has to show what it's doing and what just happened, at all times. Anything that takes time (pull, push, fetch, maybe even commit) needs visible progress and a clear result, not just a button that quietly greys out and a toast that disappears. The maintainer has said plainly that right now they *don't* fully trust the UI to have their back, and that fixing this is a requirement, not a want. The status strip is the answer so far; see "Status and feedback" under Known gaps.

---

## What it is

An editor-only plugin that adds a **Git** dock (default slot: right side, next to the Inspector). It is *not* an `EditorVCSInterface` backend for Godot's built-in VCS panels; it draws its own UI.

What the dock does today:

- Branch picker: local branches, remote-tracking branches (picking `origin/x` creates and checks out a tracking branch), "New Branch…".
- A status strip under the toolbar. It shows what's running, with real progress and Cancel, then keeps the last result, error or "last fetched" time.
- A ⋮ menu next to the branch picker (stage/unstage/discard all, new branch, refresh, open repo folder).
- A sync row, **⟳ Fetch**, **↓ Pull N** and **↑ Push N / Publish**, sharing the width (hidden without a remote).
- Setting a repository up: **Initialize Repository...** when the project isn't in one (in the project folder or the folder above it), **Add Remote...** in the ⋮ menu while there's no remote, and a **name and email** dialog the first time a commit (or a merging pull) needs them.
- Commit message box (Ctrl+Enter commits), then **☐ Amend** and **Commit**. Amend redoes the last commit (new message, plus whatever is staged). It's only enabled while that commit isn't on any remote-tracking branch, and ticking it fills in the last message.
- "Staged Changes" and "Changes" sections (Godot `FoldableContainer`s) with file counts and **+/− line totals** in the header (each file's own +/− is in its tooltip), discard-all / stage-all / unstage-all header buttons, and per-file hover buttons (stage / unstage / discard). Both sections always show; when empty they show a plain dim label ("Nothing staged." / "No changes.").
- Right-click menus on files (open, stage/unstage, discard, show in FileSystem / file manager, copy paths) and commits (copy hash / message).
- History, 50 commits at a time ("Load More Commits" adds 50); commits not yet on any remote are highlighted with the accent color. Clicking a commit expands it: author and time, the rest of its message, and the files it changed (rows like the Changes rows). Clicking one of those files shows its change in the Diff panel.
- **A Diff panel at the bottom** (next to Output, Debugger, ...): clicking a file in Staged Changes or Changes shows its diff there, looking like the script editor (its colors and syntax highlighting), with added/removed lines tinted, old and new line numbers, and a Unified / Side by Side view. It follows a file you stage or unstage, updates as you save, and says plainly when there are no lines to show (binary, LFS, over 2 MB, rename only).
- Double-click opens a file: in Godot if it's a scene/script/resource, otherwise in the external editor from Godot's settings, or VS Code.
- Pull fast-forwards or creates a merge commit; uncommitted changes to other files stay put. It's refused up front (naming the files) if the new commits touch files you have uncommitted changes to, and conflicting merges are refused and fully undone. It never leaves anything in a stash.
- Auto-fetch every few minutes, quietly, never opening a sign-in window (toggle in the ⋮ menu).

Supported targets: Windows x86_64/arm64, Linux x86_64/arm64, macOS universal. All five build in CI. The backend test suite (including HTTPS fetches from GitHub) passes on all five in CI (first confirmed 2026-09-23). The dock UI has been seen on **Windows x86_64** (the original dev machine), on **Linux x86_64** (the maintainer's Arch/KDE Wayland machine with Godot 4.7.1, 2026-09-25: "feels and looks good", dialogs and buttons fine, pull and fetch work on StorageWars through `gh`'s credential helper) and on an **Apple Silicon Mac** (a friend of the maintainer's, 2026-09-24: "worked perfectly fine"; no screenshots or details on how it was installed). It's in real use on the maintainer's StorageWars project, a private GitHub repo over HTTPS, and on this repo itself: commit `39a940b` was made from the panel (an early build, before the status strip).

Minimum OS versions of the built libraries (check with `pyelftools`/`macholib` on a CI zip):
- **macOS 10.13 (Intel) / 11.0 (Apple Silicon)**, matching Godot 4.7. Set via `macos_deployment_target` in `SConstruct` and passed to libgit2's CMake. Without it the library requires the CI runner's macOS version (the first CI build required macOS 26).
- **Linux: glibc 2.34+** (Ubuntu 22.04+, Debian 12+, Fedora 35+), because CI builds on Ubuntu 22.04. Older distros would need a build in an older container. Runtime deps are only libc/libm/libstdc++; OpenSSL is loaded at runtime.

---

## Repository layout

| Path | What |
|---|---|
| `src/git/` | **Backend**, no editor dependencies. All git logic lives here. |
| `src/git/git_repository.h` | `GitRepository` (RefCounted): the libgit2 wrapper's whole API. Exposed to GDScript only so the test suite can drive the shipped library; deliberately not documented as a public API (decided 2026-09-24), so it can change freely. |
| `src/git/git_repository.cpp` | Opening, reading state (status, line stats, branches, history), local changes (stage, discard, commit, checkout). |
| `src/git/git_repository_diff.cpp` | `get_diff`, `get_commit_diff`: one file's hunks and lines for the Diff panel; `get_commit_files` for History; `get_file_bytes`: a file's content in any version (LFS from git-lfs's cache), for image previews. |
| `src/git/git_repository_remote.cpp` | Fetch, pull, push. `pull()` is split into `paths_blocking_pull` / `fast_forward` / `merge_and_commit` / `merge_with_autostash`. |
| `src/git/git_remote_callbacks.{h,cpp}` | libgit2 remote callbacks: logins via `git credential fill/approve/reject`, progress reporting (`RemoteContext`, `report_progress`), cancel. |
| `src/git/git_cli.{h,cpp}` | Steps handed to the git CLI because libgit2 would skip hooks or signing, or handles SSH badly: `commit_needs_git`, `has_hook`, `commit_with_git`, and `run_git_command` (runs git with merged output, live progress lines, Cancel). |
| `src/git/git_lfs.{h,cpp}` | Git LFS: a libgit2 filter for `filter=lfs` backed by `git lfs filter-process`, plus `git lfs fetch`/`push` with progress. |
| `src/git/git_util.{h,cpp}` | Small libgit2 helpers shared by the above (`Owned` pointers like `CommitPtr`, `fail`, `to_error`, `head_branch`, `changed_paths`, `uncommitted_paths`, ...). |
| `src/editor/` | **Editor UI.** |
| `src/editor/git_dock.h` | `GitDock` (EditorDock). Its private methods are grouped by the file that implements them. |
| `src/editor/git_dock.cpp` | Building the dock, `refresh()`, toolbar and action row, local actions (stage, discard, commit, branches). |
| `src/editor/git_dock_lists.cpp` | Staged Changes / Changes / History: row drawing, header alignment, hover buttons, clicks, context menu, expandable commits, and which file the Diff panel shows. |
| `src/editor/git_dock_status.cpp` | The status strip. |
| `src/editor/git_dock_network.cpp` | Fetch / pull / push on a worker thread, auto-fetch. |
| `src/editor/git_dock_setup.cpp` | Setting a repository up: the empty state with Initialize Repository, Add Remote, the name and email dialog. |
| `src/editor/git_diff_dock.{h,cpp}` | `GitDiffDock` (the Diff panel at the bottom) and `GitDiffHighlighter`. Only shows what `GitDock` hands it; never reads the repository. |
| `src/editor/file_opener.{h,cpp}` | `open_file()`: in Godot if it can edit the file, else the configured external editor or VS Code. |
| `src/editor/ui_text.{h,cpp}` | Wording helpers: `plural`, `time_ago`, `status_letter`, ... |
| `src/editor/git_editor_plugin.{h,cpp}` | `GitEditorPlugin`: adds/removes the dock. |
| `src/addon_files.{h,cpp}` | Finds the addon's `godot_git.gdextension` next to the library, and keeps the library mapped after Godot unloads it (see gotcha 30). |
| `src/register_types.cpp` | Extension entry `godot_git_library_init`. SCENE level: `git_libgit2_init()` + `GitRepository`. EDITOR level: `GitDock`/`GitEditorPlugin` (internal classes) + `EditorPlugins::add_by_type`. |
| `project/` | Dev/test Godot project. Has no main scene on purpose. |
| `project/tests/` | Backend test suites (`run_tests.gd` + `test_*.gd`); see Testing. Not part of the shipped addon. |
| `project/addons/godot_git/` | **The shipped addon.** `godot_git.gdextension` + `bin/<platform>/` (build output, git-ignored). |
| `thirdparty/godot-cpp` | Submodule, pinned to `10.0.0-stable`. Targets `api_version=4.7`. |
| `thirdparty/libgit2` | Submodule, pinned to `v1.9.7`. |
| `tools/libgit2.py` | Configures + builds libgit2 as a static lib with CMake, per platform, and links it into the SCons env. |
| `tools/package.py` | Zips the addon (+ LICENSE + third-party licenses) into `dist/godot_git-<version>.zip`. `--require-all` fails if any platform library is missing. |
| `SConstruct` | Build entry. Default target `editor`, hot reload on, `SCONS_CACHE` support, `package` alias. |
| `.github/workflows/build.yml` | CI: builds 5 targets in parallel, runs the tests on all 5, packages one zip, attaches it to published releases. |

Build by-products (all git-ignored) stay out of `src/`: `build/obj/` (object files, via `VariantDir`), `build/gen/` (generated class-reference source), `build/libgit2/<platform>.<arch>/`, `dist/`, `project/.godot/`, `project/addons/godot_git/bin/`.

---

## Build

```bash
git submodule update --init --recursive
scons                         # editor build -> project/addons/godot_git/bin/<platform>/
scons package                 # build, then zip into dist/ (local zip only has your platform)
scons rebuild_libgit2=yes     # wipe build/libgit2/ and rebuild it (needed after changing tools/libgit2.py)
scons compiledb=yes           # compile_commands.json for clangd
```

- The first build compiles libgit2 via CMake (a few minutes); later builds reuse `build/libgit2/<platform>.<arch>/install/`. Changing `LIBGIT2_OPTIONS` in `tools/libgit2.py` does **not** trigger a rebuild by itself; pass `rebuild_libgit2=yes`.
- The target is always `editor` (the addon is editor-only). The `.gdextension` only lists `*.editor.*` libraries.
- `use_hot_reload` is forced on for non-release builds. Without it, rebuilding while the editor is open **crashes the editor** ("Unable to recreate GDExtension instance").
- On Windows, MSVC links libgit2 compiled with `/GL`; the "restarting link with /LTCG" message is harmless.

**This machine** (Windows 11, 4K display at 2× editor scale): VS 2026 Build Tools (MSVC 14.51, VS 2022 also installed), Python 3.13, SCons 4.11 (in the Python install's `Scripts`), CMake 4.4. Godot is `C:\Users\Markus\Desktop\Godot_v4.7.2-stable_win64.exe`. PowerShell sessions spawned by tools may need `$env:Path` refreshed from the registry to see these.

**Engine source**: `C:\Users\Markus\Desktop\godot` is Godot **master (4.8-dev)**, newer than 4.7.2. When checking engine behavior for 4.7, use `git show 4.7.2-stable:<path>` rather than the working tree (example: `include_tags` exists in master but not in 4.7.2).

Run the editor on the dev project:

```bash
C:\Users\Markus\Desktop\Godot_v4.7.2-stable_win64.exe -e --path project
```

Dev loop: edit C++ → `scons` → click back into the editor (hot reload). If something looks stale or odd after a reload, restart the editor.

---

## Architecture notes

### GitRepository (backend)

- Wraps one `git_repository*`. **Not thread-safe**: one instance per thread. The dock keeps one on the main thread and opens a fresh one on the worker thread for network ops.
- Paths in and out are **relative to the repo workdir** (not `res://`). `open()` accepts `res://` and searches parent folders like the git CLI, so a Godot project inside a larger repo works (this repo's own `project/` is exactly that case).
- **Which repository.** The dock always opens the repository containing `res://`, searching parent folders. That covers a project at its repo's root (~80% per the maintainer) and a project in a subfolder of its repo (`MegaGame/godot-project/`, which the maintainer considers common; this repo is laid out that way). **Picking a different repository was dropped** (decided 2026-09-25): no setting, no picker. A project that isn't in a repo at all is handled separately (see Missing features: "Initialize Repository").
- Errors: methods return Godot `Error`. The human-readable reason is `GitRepository::get_last_error()` (libgit2's thread-local last error). Custom messages are set with the file-local `fail()` helper (`git_error_set_str`). `get_notice()` carries a warning from an operation that *succeeded* (only the pull's stash safety net, which shouldn't trigger anymore). `get_pull_result()` says how many commits the last pull brought in and whether it merged. A canceled network op returns `ERR_SKIP`.
- Network (fetch / pull / push) uses `RemoteContext` callbacks:
  - **HTTPS credentials** come from git's own credential helper by running `git credential fill` (with `GIT_TERMINAL_PROMPT=0`). On this machine that's Git Credential Manager. If git isn't installed, HTTPS auth can't work.
  - **SSH** uses libgit2's `USE_SSH=exec` backend, i.e. the system `ssh` client (keys, agent, `~/.ssh/config`).
  - **Like the git CLI, the login is settled after each operation**: `finish_network_operation` runs `git credential approve` when it worked. That's what makes Git Credential Manager (GCM) save a login it just asked for. Without it, a sign-in through the panel was never saved (found 2026-09-24). When the server rejects a login, libgit2 asks the callback again. The callback then runs `git credential reject`, so the helper forgets the stale login, and asks once more. That's where GCM opens its sign-in window, so an expired token recovers without a terminal. A second rejection, or any rejection with prompts off, fails.
  - Prompts allowed: `git -c credential.interactive=always credential fill`. **The `always` is required**: started from the editor, GCM has no console, concludes nobody can see a window ("user interactivity has been disabled"), and fails or hangs instead of prompting. This is also why the official godot-git-plugin can't do the browser sign-in; it never calls a credential helper at all.
  - `set_login_prompts_allowed(false)` (used for background fetches) passes `credential.interactive=never`: GCM then only uses saved logins. Other common helpers (osxkeychain, libsecret, store) never prompt anyway. A configured `askpass` program could still pop up; not handled.
- `pull()`:
  1. fetches the upstream's remote;
  2. **refuses up front** if the incoming commits (merge base → theirs) touch any path with uncommitted changes (staged, unstaged, or an untracked file the pull would add). The message names the files. Nothing is touched, and no stash is made;
  3. up to date → nothing; fast-forward → safe checkout + move the ref;
  4. diverged → autostash the (by now guaranteed unrelated) tracked changes, `git_merge`, then either create the merge commit or, on conflicts, `git_reset --hard` + `state_cleanup` (nothing changes), and restore the stash. It applies cleanly because of step 2; the old "keep it in the stash if files overlap" logic remains only as a safety net.
- `push()` pushes to the upstream, or **publishes** (pushes to `origin`, or the only remote, under the same name, then sets upstream) when there is none. Non-fast-forward (`GIT_ENONFASTFORWARD`, or a server "fetch first" rejection) is reported as "pull first".
- `get_line_stats(staged)` diffs HEAD↔index or index↔workdir (untracked content included) and returns `{path: Vector2i(added, removed)}`; binary or >2 MB files are `(-1,-1)`.

### Git LFS

libgit2 has no LFS support. On its own it checks out the ~130-byte pointer files instead of the real files, reports a clean status, and commits whole files into git (verified 2026-09-24 before the fix: a pull turned PNGs into pointer text while the panel said "Pulled 1 commit"). `git_lfs.cpp` fixes this by using the real git-lfs, the way git does:
- **A libgit2 filter named "lfs"** (registered in `register_types.cpp`, attribute `filter=lfs`) sends file contents through **`git lfs filter-process`**. That's git's long-running filter protocol (pkt-lines, `clean` = file→pointer, `smudge` = pointer→file). One process per `git_repository*`, started lazily and stopped in `GitRepository::close()`. Because it's a filter, checkout, pull, merge, stash, discard, staging and status all handle LFS files correctly without special cases.
- **Downloads happen before checkout**: `pull()` and `checkout_branch()` run `git lfs fetch <remote> <commit>` first, with progress ("Downloading LFS files... 45% (9/20)"; `GIT_LFS_FORCE_PROGRESS=1`, since git-lfs is quiet without a terminal) and Cancel. The filter process runs with `credential.interactive=never`, so a download inside a checkout can never wait on a sign-in window. **`push()` runs `git lfs push` first**, so commits never reach the remote before their files.
- **Without git-lfs installed**, the filter passes files through (libgit2's old behavior), and staging, committing, pulling, switching branches and pushing in an LFS repo refuse with an explanation (`require_lfs`). Fetch still works.
- **The dock switches branches in the background** in LFS repos (`NETWORK_SWITCH`), because a switch may download files. Elsewhere it stays instant, on the main thread.
- LFS files count as binary in the line stats (their diff would be of the pointer text).
- Why the filter process is started through `cmd /c` / `sh -c` with `2>logfile`: Godot's `execute_with_pipe` gives stderr its own pipe. If nobody reads it, it fills up (a few KB) and git-lfs blocks forever, which would freeze the editor. The log (`.git/godot-git-lfs.log`) is also where error messages come from.
- Limits: `.gitattributes` is only checked at the repo root (`repo_uses_lfs`). Each file is held in memory while filtered. Only tested against local remotes; **a real LFS server (GitHub) with a sign-in is untested** (StorageWars doesn't use LFS).

### Hooks and commit signing

libgit2 runs no hooks and never signs. Committing that way would silently skip a team's `pre-commit` linter or leave commits unsigned despite `commit.gpgsign`. So (`git_cli.cpp`):
- **Commit, amend and pull's merge commit** go through `git commit --quiet -F <file>` (`--amend` for amend) when `commit.gpgsign` is on or one of `pre-commit`, `prepare-commit-msg`, `commit-msg`, `post-commit` (plus `post-rewrite` for amend) exists. Hooks come from `core.hooksPath` or `.git/hooks`. For the merge, libgit2's `git_merge` leaves `MERGE_HEAD`, so `git commit` makes a two-parent commit. If git refuses, the merge is undone like a conflicting one and the pull reports "Nothing was pulled".
- **Push** goes through `git push --progress` when a `pre-push` hook exists (with the same `credential.interactive` setting as our own logins). "Pull first" is recognized from git's `[rejected] ... (fetch first / non-fast-forward)` output.
- Otherwise libgit2 does it as before, so repositories without hooks or signing are unaffected.
- A failure shows git's last lines, which is where the hook's own message is (e.g. "player.gd:1: error: use tabs").
- **The dock commits in the background** when `commit_runs_git()` says so (`NETWORK_COMMIT`), because a hook can take a while. The strip shows the hook's output live, with Cancel. After a Cancel it only says "Commit canceled.", because a post-commit hook may already have run.
- `run_git_command` merges stdout and stderr through `cmd /c ... 2>&1` / `sh -c`, for the same reason as the LFS filter: an unread stderr pipe blocks the process. git-lfs fetch/push use it too.
- Not covered: `pre-merge-commit` (only `git merge` runs it; we commit merges with `git commit`), and `post-checkout`/`post-merge` after libgit2 checkouts. GPG signing needs a pinentry that can show a window (e.g. Gpg4win's); a terminal-only pinentry fails, since there's no terminal.

### Without git installed

The git program is needed for logins, LFS, hooks, signing and SSH; everything else runs on libgit2. `git_installed()` (`git_cli.cpp`) checks once: it first looks for git on the PATH itself, because starting a missing program makes Godot print a red error to the editor's Output every time, then runs `git --version`. It's **not** checked on every refresh: on a Mac without the developer tools, `/usr/bin/git` is a stub that pops up an install offer each time it runs. The dock checks on READY and, while git is missing, on every focus-in (so installing it needs no restart).
- Each place that needs git refuses with a message saying why and pointing to git-scm.com (`require_git`): commit/amend/merge commit with hooks or signing (never silently skipping the hook), fetch/push with an SSH remote, push with a pre-push hook, logins (credential helper), LFS (`require_lfs`). `run_git_command` has a generic backstop.
- The dock shows one warning listing what won't work in *this* repository (`GitRepository::get_git_needs`), and disables Commit / Fetch / Pull / Push with the reason in the tooltip when they can't work. A repository without hooks, LFS, SSH or HTTPS remotes gets no warning at all. Auto-fetch skips a remote it can't reach.
- Seen on screen 2026-09-25 (editor started with git removed from PATH).

### SSH remotes

libgit2 runs ssh itself (`USE_SSH=exec`), but badly for us. On Windows it ignores "shell" mode, so a `GIT_SSH_COMMAND`/`core.sshCommand` with options is taken as one program name and fails. It starts the first `ssh` on PATH (Windows' own), not git's. And it discards ssh's stderr (`capture_err = 0`), so every failure read "could not read refs". So **fetch and push for SSH remotes go through `git fetch`/`git push`** (`is_ssh_url`, `_fetch_with_git`, `_push_with_git`):
- git runs its own ssh (checked: `/usr/bin/ssh` inside Git for Windows), with the user's keys, agent and `~/.ssh/config`, exactly as in a terminal.
- Unless the user set `GIT_SSH_COMMAND`, `GIT_SSH` or `core.sshCommand` (then used as is), we pass `core.sshCommand=ssh -o BatchMode=yes -o ConnectTimeout=15`. BatchMode means ssh fails instead of waiting on a passphrase or host-key prompt nobody can see.
- `network_failure` turns the usual ssh errors into instructions: unknown server (connect once with `ssh -T`), rejected key (use ssh-agent), can't connect. ssh's own lines are kept below.
- **Cancel on Windows**: an ssh started through Git's shell isn't in git's Windows process tree (the MSYS process in between has exited), so `taskkill /T` can't reach it, and it keeps our pipe open. `run_git_command` therefore reads without blocking and finishes when git exits or on Cancel, not when the pipe closes. An orphaned ssh ends by itself (ConnectTimeout).

### GitDock (UI)

- Layout, top to bottom: toolbar (branch `OptionButton`, ⋮ `MenuButton`) → status strip → sync row (Fetch, Pull, Push; `SIZE_EXPAND_FILL` each) → commit `TextEdit` → commit row (Amend `CheckBox`, Commit) → `ScrollContainer` holding three `FoldableContainer`s (Staged Changes, Changes, History).
- The file trees have **scrolling disabled** so they report their full height, and the outer `ScrollContainer` scrolls everything as one list. Sections are exactly as tall as their content.
- File rows are **one cell** (`CELL_MODE_CUSTOM`). `_draw_file_row` paints the status letter, icon, name and dimmed folder. The cell's text is set to the file name with a transparent color, so row height, type-to-search and accessibility still work.
- Hover buttons: `_set_hovered` adds stage/unstage/discard buttons to the row under the mouse and clears them from the previous one (tracked by instance id, reset on every refresh). Only rows whose metadata is a `String` path count as file rows.
- Each section's content is built by `_make_body`: a `VBoxContainer` holding the tree plus a `MarginContainer`/`Label` empty state. When a section is empty, the tree is hidden and the label shown. Empty states are **never** tree rows: rows can be hovered, selected and given buttons, which made "No changes." behave like a file.
- Header "all" buttons use the Tree's own button styleboxes and spacing, and `_align_header_buttons` shifts them (via a `MarginContainer`'s right margin) so they line up **exactly** with the row buttons at any scale.
- `refresh()` rebuilds everything: sync status, branches, status for both panes (line counts follow from a worker thread), history (only if the commits changed), action buttons. It runs on:
  - `READY`;
  - `filesystem_changed` (every scene/script/resource save in Godot ends up there, via `ResourceSaver` → `EditorFileSystem::update_file`);
  - `NOTIFICATION_APPLICATION_FOCUS_IN` (changes made outside Godot);
  - a change of `project.godot`'s modified time, checked every second, because Project Settings saves it without notifying anything;
  - after every action.

  There is no other polling.
- Network ops run on a `Thread` (`_network_worker`) and report back with `call_deferred` to `_network_done`. `_finish_network_thread` joins on `EXIT_TREE`. Commit, Amend, the branch picker and the ⋮ menu are disabled while a pull, push or LFS branch switch runs; the worker is rewriting the repo.
- **Line counts** (`_start_line_stats` and friends, `git_dock_lists.cpp`): the +/− totals in the list headers and each file's counts in its tooltip come from `get_line_stats`, which takes seconds on big change sets. `refresh()` fills the lists at once with the last counts (so the numbers don't flicker on every save) and starts `stats_thread`, which opens its own `GitRepository` and hands both results back with `call_deferred`. A refresh while it counts sets `stats_again` (one more count afterwards, never a pile of threads). If counting takes over 0.25 s, the totals dim and their tooltip says "Counting lines again…". `EXIT_TREE` joins the thread.
- **Auto-fetch** (`_on_auto_fetch_timer`): checks 10 s after opening, then every minute, and fetches when `last_fetched` (FETCH_HEAD's age, so CLI fetches count) is 5+ minutes old. After a failed attempt it also waits 5 minutes. Per-project setting in the ⋮ menu (`EditorSettings` project metadata `godot_git/auto_fetch`, default on). It runs **quietly** (`network_quiet`): no strip, buttons stay enabled, and no sign-in windows (`set_login_prompts_allowed(false)`). Pressing Fetch during it just shows it. Pressing Pull/Push queues the op (`queued_op`), shown as busy with "Waiting for a background fetch...". A failure shows one warning, cleared by the next success. `_shown_network_op()` is what the buttons reflect.
- **Status strip** (`_set_status` / `_update_status` / `_update_status_style`), under the toolbar. Every result and error goes here. It's one wrapping `RichTextLabel` (never trimmed; selectable so errors can be copied), an icon, a Cancel/Dismiss button, and a thin progress bar while busy. Kinds: idle ("Last fetched 3h ago", from `last_fetched` in the sync status), busy, success ("· just now", kept current by a 30 s timer), neutral (canceled), warning and error (tinted background, stay until dismissed or replaced). Results never time out. Toasts are only used when the dock is hidden behind another tab, so an outcome is never missed.
- Progress: the worker's `GitRepository` gets `set_progress_callback(callable_mp(dock, &_network_progress))`. The backend throttles to ~10 updates/s and uses `call_deferred`, so the dock is only touched on the main thread. Cancel calls the static `GitRepository::cancel_network()`, which sets an atomic flag the libgit2 callbacks check (returning `GIT_EUSER`) and kills the child process being waited on (`git credential fill`, `git lfs fetch/push`; tracked with `track_process`). On Windows that's `taskkill /T`, because git's own children (Git Credential Manager's window, git-lfs) outlive their parent otherwise. The op then fails with `ERR_SKIP`.

### History

- `get_commits` walks `GIT_SORT_TIME` (not `TOPOLOGICAL`, which reads the whole history first) and then `order_children_first` puts commits made in the same second child-first. Its result is cached while HEAD and every branch point at the same commits (`commits_key`).
- `_fill_history` asks for `history_limit + 1` commits (50, plus 50 per "Load More Commits") and **only rebuilds the tree when the commits changed** (`history_shown`); otherwise it just updates the ages. Every save refreshes the dock, so without this, expanded commits and the selection would reset constantly. When it does rebuild, `history_expanded` (hashes) re-expands what was open.
- Expanding (click or arrow) and Load More build rows deferred, never inside the Tree's mouse handling (gotcha 38).
- Rows carry meta `git_row`: `commit` (metadata = the commit dictionary), `placeholder`, `note`, `file`, `more`. A commit starts collapsed with a placeholder child (so it shows the arrow); expanding it (`item_collapsed`) calls `_fill_commit`, which reads `get_commit_files` once per hash (`commit_files` cache; commits never change). A click on a commit row toggles it, like VS Code.
- `_fill_commit` adds notes (author and time, up to 6 lines of the message body, a merge note) as single trimmed lines with the full text in the tooltip, then file rows drawn by the same `_draw_file_row` as the change lists (up to 500, then "...and N more files"). Notes are not autowrapped: see gotcha 36.
- A merge commit's files and diffs are against its first parent: what it brought into the branch.
- Clicking a commit's file sets `diff_commit` and shows `get_commit_diff` in the Diff panel ("Commit eabb44a"). Commit diffs never change, so `_update_diff` doesn't re-read them on refresh (`diff_commit_shown`). Selecting in any list clears the other two.

### GitDiffDock (the Diff panel)

- An `EditorDock` in `DOCK_SLOT_BOTTOM` (layouts: horizontal, floating), title "Diff", layout key `GodotGitDiff`, minimum height 240 px scaled (the bottom panel otherwise opens one line tall). Created by `GitEditorPlugin` next to the Git dock, which gets a pointer (`set_diff_dock`) and is deleted first.
- **Who does what.** `GitDock` owns the choice of file (`diff_path`, `diff_staged`) and reads the diff (`_show_diff` / `_update_diff`, in `git_dock_lists.cpp`). A file row's `multi_selected` shows its diff; a left click (`item_mouse_selected`) also calls `make_visible()` to bring the panel up. `_update_diff` runs at the end of every `refresh()`, so the panel updates on save, focus-in and after actions. When the shown side has no changes left but the other side does (you just staged or unstaged it), it follows the file there. `_select_diff_row` re-selects the shown file's row after the lists are rebuilt; selecting in one list clears the other.
- **`set_diff` skips redrawing** when the dictionary is equal to the one shown (a refresh that changed nothing keeps scroll and selection). A changed diff of the same file keeps the scroll position.
- **Rendering.** Read-only `CodeEdit`s: one for the unified view, two in an `HSplitContainer` for side by side, which scroll together (both sides have the same row count, with blank filler rows opposite one-sided lines). In the editor theme a `CodeEdit` already has the script editor's font, background and colors; only `font_readonly_color` is overridden (read-only text is dimmed otherwise). Each `CodeEdit` sits in a `PanelContainer` that draws the code editor's stylebox (background, padding, corners), and the `CodeEdit` itself gets empty styleboxes (gotcha 35). Row tints are `set_line_background_color` (text area only), and the gutters (`GUTTER_TYPE_CUSTOM`, `_draw_gutter`) paint the same tint plus right-aligned numbers and the +/− sign, so each row reads as one band. Hunk headers (`@@ -12,7 +12,9 @@ func _ready():`) are dimmed rows; a new or deleted file's single `@@ -0,0 ...` header is left out.
- **Syntax highlighting** (`GitDiffHighlighter`): `.gd` gets Godot's own `GDScriptSyntaxHighlighter` (exposed to extensions), other code and Godot's text formats a `CodeHighlighter` with the editor's highlighting colors, strings and the file type's comment markers; `.txt`/`.md` none. A highlighter reads its lines from the TextEdit it's attached to, so the real highlighter sits on a hidden mirror `CodeEdit` whose hunk headers and fillers are blank lines (so an `@@` line can't open a string), and `GitDiffHighlighter` on the visible one forwards to it, dimming headers.
- The view (Unified / Side by Side) is saved per project in `EditorSettings` project metadata `godot_git/diff_view`.
- **Images** (png, jpg, webp, bmp, tga, svg, exr, dds; `GitDiffDock::is_image_path`) show before | after instead of "binary file". `GitDock::_add_image_versions` attaches both versions (`image_old`, `image_new`: `GitRepository::get_file_bytes`) to the diff: unstaged is index → workdir, staged HEAD → index, a commit `<hash>^1` → `<hash>` (the old side under its old name for a rename). Each side is a caption ("Before · 32×32 · 128 bytes"), the picture drawn by `_draw_image_side` (as large as fits, on a checkerboard exactly its size), or a note: new file, deleted, not downloaded (LFS), can't be read. Images of 256 px or less are scaled up without filtering (pixel art); SVGs are rendered at 1024 px and filtered. Identical pixels in a changed file say "same pixels" (re-saved, re-compressed). An SVG is also text: the view choice then has Image / Unified / Side by Side (`images_as_text`, set only by a choice made on an image).
- Not done yet: downloading LFS versions that aren't in the local cache, scene structure, audio, glTF models (see "Diffs for Godot's file types" under Big features 1), word-level highlights within changed lines.

---

## Conventions

- Code style follows godot-cpp/Godot: tabs, `p_` params, `r_` out-params, `_private_method`, `snake_case`. `.clang-format` is copied from godot-cpp.
- UI strings use Godot's Title Case for buttons and menus ("New Branch...", "Discard All Changes..."), full sentences for tooltips and toasts.
- Use editor theme colors/icons (`get_theme_color("success_color", "Editor")`, `get_theme_icon("Reload", "EditorIcons")`); never hardcode colors. Scale pixel sizes by `EditorInterface::get_editor_scale()`.
- Comments explain *why* (non-obvious engine or library behavior), not what.
- **Code organization.**
  - `src/git/` never includes editor classes; `src/editor/` talks to git only through `GitRepository`.
  - Internal helpers live in `namespace godot_git`; file-local ones in an anonymous namespace.
  - A class may span several `.cpp` files when it has clear areas (the dock does: lists, status, network). Split along those seams, not by line count. Keep the header's method list grouped by file.
  - Hold libgit2 objects in the `Owned` aliases from `git_util.h` (`ReferencePtr head; git_repository_head(head.out(), repo);`), which free them on every return. Don't call `git_*_free` by hand; the only exceptions are the repository itself (`GitRepository::close`) and `head_branch`'s out-param. Add an alias when you need a new type. `git_buf` goes through `buf_to_string` or `git_buf_dispose`.
- **Formatting**: run clang-format (the repo's `.clang-format`, from godot-cpp) on changed C++ files. On this machine it's at `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-format.exe`.
- **Commits**: the maintainer commits and pushes themselves. Don't commit, create repos, or push unless explicitly asked. Leaving work staged has been the norm.

---

## Testing

1. **Backend tests: `project/tests/`, in the repo and in CI.** `run_tests.gd` (a `SceneTree` script) runs the suites `test_local.gd`, `test_diff.gd` (`get_diff` against `git diff`), `test_sync.gd`, `test_pull_safety.gd`, `test_checkout_safety.gd` (Windows only), `test_amend.gd`, `test_hooks.gd` (real hooks, and SSH signing with a throwaway key), `test_credentials.gd`, `test_ssh.gd` (fake ssh), `test_lfs.gd` (skipped without git-lfs; CI installs it), `test_no_git.gd` (points the extension at a git that doesn't exist via `GitRepository.set_git_program`), `test_setup.gd` (init, add remote, name and email, against an empty global config via `GitRepository.set_config_home`) and `test_online.gd` (the last only with `-- --online`). Each suite extends `test_case.gd`, which builds throwaway repos with the real git CLI (a bare "remote" plus "mine" and "theirs" clones via `make_shared()`). Checks are made against what git itself says. Run:
   `godot --headless --path project -s res://tests/run_tests.gd [-- --online] [-- <suite>]`. Exit code 1 on failure; scratch repos go to the OS temp folder and are kept (path printed) when something fails. CI runs them on Windows, Linux x86_64/arm64 and macOS against each freshly built library. It first opens the project with `-e --quit-after 300` so the extension gets registered; see gotcha about `--import`. **Every bug from real use gets a test here.** The suite has already caught one on its first run: the push "pull first" message never showed for the common unfetched case.
2. **Smoke test of the dock: `tools/smoke/`, in CI on all five platforms** (added 2026-09-26). `python tools/make_playground.py <folder> --smoke` builds a playground (history with a merge and 60 extra commits, staged/unstaged/new/deleted/renamed/binary files, images: a recolored sprite, a new one, one re-saved with the same pixels, an SVG) with a test plugin that, in a headless editor, refreshes twice, folds each section, clicks a file, switches both diff views, checks the image views, stages the shown file, uses Load More, expands a commit and opens its file, survives a refresh, and waits for background line counts; it prints `SMOKE: OK` or what failed. Run it twice (`-e --quit-after 300` to register the extension, then `-e --quit-after 20000`); CI fails without `SMOKE: OK`. **What it's worth, checked by putting old bugs back:** it catches the startup recursion from 2026-09-26 (the editor crashes, no `SMOKE: OK`); it does **not** catch the fold loop (gotcha 37: headless and windowed, Vulkan and OpenGL, single-window or not; that loop needed a particular dock width and content height), nor anything that needs drawing, nor real clicks (gotcha 38). So it's a crash net for code paths, not a UI test.
3. **Scripted UI runs with screenshots.** Not in the repo; ad hoc from the session scratchpad. Copy the addon into a throwaway project and add a test-only `EditorPlugin` (`addons/ui_driver/`) enabled in `project.godot` that finds dock controls (`find_children` on `EditorInterface.get_base_control()`) and presses them (`button.pressed.emit()`, `popup.id_pressed.emit(id)`), then prints results. Run with `--headless -e --path <project> --quit-after <frames>` and **redirect stdout**. Gotchas:
   - PowerShell 5's `Set-Content -Encoding utf8` writes a **BOM**, and Godot then silently ignores `plugin.cfg`. Write files with `[IO.File]::WriteAllText(path, text, (New-Object Text.UTF8Encoding $false))`.
   - `--log-file` did **not** capture plugin `print()` output. Redirect stdout instead.
   - `--quit-after` counts frames, not seconds. Give network tests a generous number (e.g. 6000).
   - Tree **button clicks can't be injected**: Tree re-checks the real OS cursor before accepting a button press. Emit `tree.button_clicked` directly to test the handler.
   - Injected hover works headless but gets overridden in a GUI editor by the real cursor.
   - For screenshots of dialogs, start the editor with `--single-window` (otherwise popups are separate OS windows that `PrintWindow` on the main window misses), and trigger the driver with `get_tree().create_timer(...)`, not a frame count: an unfocused editor only draws ~10 frames a second.
4. **Visual checks.** Screenshot the editor window with Win32 `PrintWindow` (works when occluded, but restore it if minimized), then crop the dock. **Never** simulate the OS mouse or keyboard: it acts on whatever window is on top of the user's desktop.
5. **Demo scenarios** must be designed so the git CLI agrees with them. A demo where "teammate" and "me" edit **adjacent lines** conflicts in git too. That once made pull look broken when it was behaving correctly.

**Open the editor once after every C++ change, before handing back**: the smoke test does exactly that (see 2) (`--headless -e --path <a project with the addon> --quit-after 600`, exit code 0). The backend suite never creates the dock: a one-line infinite recursion in History (2026-09-26) passed all 341 checks and crashed every editor at startup. On Windows, a crashed editor run in Git Bash exits with **127** (stack overflow) or 139 (access violation), often with nothing in the log; the Windows Application event log (Event ID 1000) names the faulting module. A second GUI editor started while another runs may also just exit; close the other one first.

Before handing UI work back, look at a screenshot. Several layout bugs (clipped columns, invisible letters, highlight on half a row) were only visible that way.

---

## godot-cpp / Godot gotchas (learned the hard way)

1. **`String + "literal"` is ambiguous** in godot-cpp (MSVC C2593). Use `vformat("%s…", s)` or `String("…")`. `"literal" + String` is fine.
2. **Plain `"..."` literals become `String` as Latin-1.** Anything non-ASCII (−, ↓, ↑, ·) must go through `String::utf8("…")`. MSVC is built with `/utf-8`, so the bytes in the literal are right; it's the `String(const char*)` constructor that isn't.
3. **No global or static `String` objects.** They're constructed when the DLL loads, before the GDExtension interface exists, and the editor crashes. Use a function that returns the value (see `minus()`).
4. **Tree columns only size to text with `OVERRUN_NO_TRIMMING`.** With the default ellipsis trimming, text doesn't count toward a non-expanding column's minimum width, so letters and numbers silently get clipped away.
5. **Tree custom drawing:** a `CELL_MODE_CUSTOM` callback runs *before* the native text and icon are drawn. Draw on `tree->get_custom_drawing_canvas_item()`: it sits above the hover and selection backgrounds and is clipped to the rows. Drawing on `get_canvas_item()` ends up *under* the selection box.
6. **`SELECT_MULTI` highlights per cell**, not per row. That's why file rows are a single cell. Multi-column rows looked disconnected.
7. **Tree cell buttons shrink the cell's content rect.** Button geometry: `icon + button_pressed stylebox min size`, separated by the `button_margin` constant, right-aligned in the cell. `get_item_area_rect(item, column[, button])` returns the real rects.
8. **Tree with `set_v_scroll_enabled(false)` reports its full content height** as its minimum size. That's what makes "sections exactly as tall as their content" possible.
9. **Double-clicking a non-editable or custom cell emits `item_activated`.**
10. **Hot reload needs `use_hot_reload=yes`** in godot-cpp, or the editor crashes on reload.
11. **Minimum Godot version is genuinely 4.7** (`compatibility_minimum` in the `.gdextension`), not an arbitrary pick. `Tree::get_custom_drawing_canvas_item` (file-row drawing) first shipped in 4.7, `EditorDock` in 4.6, and `FoldableContainer` in 4.5. Supporting older versions would mean rewriting row drawing and testing each version; the decision so far is not to bother. Editor APIs we rely on (all present in 4.7): `EditorDock` (`set_default_slot`, `set_layout_key`, `set_icon_name`), `EditorPlugin::add_dock`, `FoldableContainer::add_title_bar_control`, `Tree::get_custom_drawing_canvas_item`, `EditorToaster::push_toast`.
12. **Not available to extensions:** `TextFile`, `ScriptEditor::open_file`. There is no way to open `.md`/`.txt`/`.cpp` in Godot's script editor from a plugin, so those go to the external editor.
13. **`include_tags` in `.gdextension`** (keeps an editor-only extension out of exports) exists only from **4.8**. On 4.7, users must exclude `addons/godot_git/*` in their export presets, or exports warn and the game logs one harmless error. The key is already in our `.gdextension`; 4.7 ignores it.
14. **Pipes from `OS::execute_with_pipe` never report `eof_reached()`**: it's hard-coded to `false` on Windows and Unix in 4.7.2. Read until `get_error() != OK` instead. Looping on `eof_reached()` spun one core forever and froze pulls from private repos.
15. `reloadable = true` makes Godot drop `~` copies of the library next to it. Fine for development; `tools/package.py` switches it off in release zips so users don't get them in their projects.
16. `EditorInterface` singletons aren't ready in constructors. Do editor-dependent setup in `NOTIFICATION_READY` / `THEME_CHANGED`.
17. **A missing `Variant` converted to `String` is `"<null>"`, not `""`.** Check `get_type()` instead of `String(v).is_empty()`. This bug gave the empty-state row hover buttons.
18. **Trimmed `Label`s have a minimum width of 0.** Two of them in an `HBoxContainer` (or one next to an expanding sibling) can collapse to nothing, which is how the status strip's "just now" and progress step first went missing. The strip uses one wrapping `RichTextLabel` instead. Give it empty `normal` and `focus` styleboxes, or it draws its own dark box. Add text with `add_text`/`push_color`, never BBCode, because branch names and error messages are user data.
19. **Dock width differs between editor runs** (anywhere from ~200 to ~460 px logical at the same scale). Check UI in a narrow dock too; the strip's first version only looked right when wide.
20. **Theme values of sibling controls aren't final at the dock's `NOTIFICATION_THEME_CHANGED`**: the commit box's stylebox margin read as 0 there. Anything that copies another control's theme values should also run on `READY`.
21. **`godot --headless --path <project> --import` crashes (access violation, inside Godot) on exit in 4.7.2** when it's the project's first run and it loads our extension. It doesn't happen without the extension, on later runs, or on a normal first open (`-e`), which is what a teammate cloning a project does. So it isn't user-facing; CI uses `-e --quit-after 300` instead. Not investigated further (no symbols for the official build).
22. **Hot reload can crash the editor inside the unloaded old library** (seen once, in the maintainer's open dev editor, while rebuilding). `callable_mp` connections and deferred calls hold raw function pointers into the old DLL. Dev-only: release zips have `reloadable = false`. If it gets annoying, restart the editor rather than relying on hot reload for big changes. Separately, the editor crashed **on quit after any hot reload**: godot-cpp frees its bindings on engine singletons (`Time`, `OS`, ...) only in its CORE deinit step, which it skips after a reload (Godot re-initializes only from our minimum level, SCENE), and Godot frees those singletons after unloading us. Fixed 2026-09-25 by calling `ClassDB::deinitialize(CORE)` at the end of our SCENE deinit (harmless when the CORE step runs too).
23. **Project Settings saves `project.godot` without any signal**: `ProjectSettings::save()` bypasses `EditorFileSystem`, and `settings_changed` fires *before* the delayed save. The dock watches the file's modified time instead.
24. `OS.execute("cmd", ["/c", "rmdir", "/s", "/q", path])` silently does nothing: Godot quotes each argument separately. Pass cmd one string: `["/c", "rd /s /q \"path\""]`.
25. **`OS.execute` drops empty arguments and mangles embedded quotes** on Windows: `git config credential.helper ""` became a read, and `"$1"` inside an argument lost its quotes. The test suite writes such config lines into `.git/config` directly.
26. **`FileAccess.get_buffer()` on a pipe drops a partial last chunk** when the process exits. Reading `git upload-pack` output that way lost its final `0000`. Read binary pipe output byte by byte (`get_8` until `get_error() != OK`); line-based reads (`get_line`) are fine for newline-terminated text.
27. **`OS::get_process_exit_code()` returns -1 while the process is still running**, and a process's pipes close a moment before it has fully exited. Reading the code right after the pipe closes lost that race on CI's Linux arm64 runner: a `git push` that succeeded was reported as failed. Use `wait_for_exit_code()` (`git_util.h`).
28. **Wrapping `Label`s in a dialog need a minimum width** (`custom_minimum_size.x`). A wrapping label measures its height at its minimum width; at 0 that's one word per line, and the dialog opens as tall as the screen. `make_label` in `git_dock_setup.cpp` takes the width.
29. **libgit2 on Windows finds the global config through `HOMEDRIVE`+`HOMEPATH` as well as `HOME`/`USERPROFILE`.** To start an editor "without a git identity" for a screenshot, override all four (or the real `~/.gitconfig` is found). The tests use `GitRepository.set_config_home` instead.

30. **Godot unloads an extension whose `.gdextension` disappears mid-session** (`EditorFileSystem::_scan_extensions` → `GDExtensionManager::ensure_extensions_loaded`, checked in 4.7.1). It does so whether or not it's `reloadable`, and it happens on every platform, e.g. when switching to a branch without the addon. Two things then pointed into the unmapped library and crashed the editor. First, the dock was `queue_free`d, so it was freed after the unload (now `memdelete`d in `_exit_tree`). Second, godot-cpp's instance bindings on engine objects (EditorFileSystem, theme icons, ...) keep free callbacks into the library, and Godot only clears those for reloadable extensions on *reload*. So at unload, if our `.gdextension` is gone, `register_types.cpp` pins the library (`RTLD_NODELETE` / `GET_MODULE_HANDLE_EX_FLAG_PIN`). When the addon comes back in the same session, the OS returns that same pinned copy, and godot-cpp can't initialize twice (its statics and `library` pointer are stale). So a retired copy loads as an empty extension and warns "restart the editor". The dock asks before switching to a branch without the addon (`_addon_removed_by`). Tested with a headless driver that moves the addon folder away and back.

31. **No `std::filesystem`**: macOS only has it from 10.15, and we target 10.13 (Intel). It builds on Linux and Windows and then fails in CI's macOS job. Use Godot's `String` path helpers and `FileAccess`/`DirAccess`.
32. **Include `windows.h` after godot-cpp's headers** (with `WIN32_LEAN_AND_MEAN` and `NOMINMAX`). Its macros (`CONNECT_DEFERRED`, `min`/`max`, ...) break godot-cpp's class headers, and only CI's Windows jobs notice. Platform-specific code isn't done until a CI run has built it on all five targets.

33. **Environment variables for git go through `ScopedEnvironment`** (`git_util.h`), around starting the process only. Godot can't pass an environment to one process; `OS::set_environment` changes the editor's own, which games started from the editor inherit.

31. **`TreeItem::select()` doesn't emit `multi_selected`** in `SELECT_MULTI` mode (commented out in `Tree::item_selected`, 4.7.2). Only user clicks and keys do. Handy (re-selecting a row after a refresh can't loop back into the handler), but a UI driver has to emit `multi_selected` itself.
32. **`CodeEdit` comes with five gutters of its own** (breakpoints, bookmarks, executing line, line numbers, folding), hidden or not. Gutters you add start after them; the custom draw callback gets the absolute index. The diff panel stores `first_gutter`.
33. **A syntax highlighter only highlights the `TextEdit` it's attached to** (`get_line_syntax_highlighting` reads `text_edit->get_line`), and `set_text_edit` isn't exposed. To wrap one, attach it to a hidden copy of the text (see `GitDiffHighlighter`).
34. **Windows' 260-character path limit bites Godot projects in deep folders**: in the session scratchpad (`...\AppData\Local\Temp\claude\<project>\<session>\scratchpad\...`) Godot couldn't write its shader cache and Python couldn't delete it. Put playground projects in a short path (e.g. `%TEMP%\ggplay`).

35. **A `TextEdit` clips its lines to its whole rect, not to the inside of its stylebox's padding.** A row scrolled half out of view is drawn into the padding, up to the rounded edge. The script editor has this too, but tinted diff rows made it obvious. Fix: let a parent `PanelContainer` draw the stylebox and give the `TextEdit` empty styleboxes, so the clip edge is the padding's inner edge.

36. **A `Tree` with scrolling disabled measures its height before autowrapped cells wrap.** With a wrapping note in History, the tree reported less height than its rows took, and the last commits were cut off (the dock relies on trees reporting their full height, gotcha 8). History's notes are single trimmed lines instead.
37. **Aligning to a folded section's tree looped forever and crashed the editor** (found 2026-09-26, present since at least `v0.1.0`). `_align_header_buttons` measured row geometry of a tree whose `FoldableContainer` was folded: `is_visible()` is still true there (only `is_visible_in_tree()` is false), the measured position was meaningless, the header buttons moved, the header re-sorted, and it aligned again. Millions of layout calls later the editor segfaulted. Folding "Changes" was enough. Use `is_visible_in_tree()` for "is this on screen".

38. **A `Tree` refuses to create or clear items while it handles a mouse press** (`blocked` in `Tree::_gui_input`, 4.7.2): `create_item()` logs "The tree cannot create items during mouse selection events" and returns **null**, and `clear()` does nothing. Everything it emits from there (`item_mouse_selected`, `item_selected`, `multi_selected`, `item_collapsed` from a click on the arrow or row) runs inside that. Expanding a History commit on click used the null item and crashed the editor (maintainer, dev project, 2026-09-26). Build rows from those signals with `call_deferred` (`_fill_commit_later`, `_load_more_commits`). `button_clicked` fires on release, outside it, so hover buttons may refresh directly. **A UI driver can't catch this**: an emitted signal runs outside the blocked section, and injected clicks don't reach the Tree without the real cursor. Test clicks that build rows by hand.

## libgit2 gotchas

1. **`git_error_last()` is never null** since 1.8. "No error" has `klass == GIT_ERROR_NONE`, and `get_last_error()` filters that out.
2. **`git_stash_pop` writes conflict markers into files and then drops the stash** when applying conflicts. The git CLI keeps the stash instead. We never pop a stash whose files overlap the pull.
3. Errors are **thread-local**: read `get_last_error()` on the thread that failed (the network worker does this before `call_deferred`).
4. libgit2 **honors `core.autocrlf`** (`true` on this machine), so checked-out files get CRLF. Compare file contents with `\r` stripped in tests.
5. `git_merge` writes merge state (`MERGE_HEAD`). Always `git_repository_state_cleanup` on every path, including failures.
6. Custom error text: `git_error_set_str` lives in `git2/sys/errors.h`.
7. **`git_reset_default` requires a non-empty pathspec.** "Unstage all" is `git_reset(HEAD, GIT_RESET_MIXED)`, or clearing the index when there are no commits yet.
8. **`sideband_progress` messages arrive torn across packets** ("Counting obje", "cts: 45%"), and one packet can hold several `\r`-separated updates. Buffer them and only show complete lines.
9. **Progress callbacks are the only place to cancel.** Returning `< 0` aborts with `GIT_EUSER`, but a socket blocked mid-read won't call back until data arrives. WinHTTP's own timeouts bound that on Windows. `transfer_progress` also fires for local (`file://`) remotes, which the tests rely on.
10. **A rejected non-fast-forward push has two different messages** in 1.9.7: "cannot push non-fastforwardable reference" (you fetched, and diverged) and "...contains commits that are not present locally" (you haven't fetched). Both return `GIT_ENONFASTFORWARD`; match on the code, never the text.
11. The last fetch time is the mtime of `FETCH_HEAD` in `git_repository_commondir` (not `_path`, which differs in linked worktrees). The git CLI updates it too, so fetches from a terminal count.
12. **Checkouts aren't atomic.** `git_checkout_tree` (and `git_merge`'s checkout) change files one by one and stop at the first they can't change. On Windows that's any file another program has open (Godot importing it, an antivirus scan). They don't undo what they already did, and HEAD doesn't move, so the tree is left half-switched and full of fake changes. Always go through `checkout_all_or_nothing` / `CheckoutGuard` (`git_util.h`): it records the planned and completed paths (the notify callback misses removals; the progress callback catches them), puts them back from HEAD's tree on failure, and retries twice, 500 ms apart, because such locks are usually brief.
13. The libgit2 CMake options live in `tools/libgit2.py`. HTTPS backend per platform: WinHTTP (Windows), SecureTransport (macOS), OpenSSL loaded at runtime (Linux, so no hard libssl dependency). zlib, regex and the http parser are bundled.

---

## Design decisions (and why)

- **Own dock rather than `EditorVCSInterface`.** Godot's built-in VCS panels are fixed and limited (no real history or branch UI). The official godot-git-plugin already fills that role, so an own dock is the only way to make something better. We also don't implement the interface alongside the dock (decided 2026-09-24): it only feeds Godot's own VCS panels, so it would add a second, weaker UI for the same repo. That UI can't show our progress, cancel or refusals. The engine also takes one VCS plugin per project, so it would compete with the official one. The official plugin is short (~1,050 lines) because Godot's `version_control_editor_plugin.cpp` (~1,600 lines) draws its UI, and because its git logic is thin: conflicts are left in the files, progress just gets `print()`ed, and logins are a typed username and password.
- **Editor-only, shipped as `addons/godot_git/`.** It's a dev tool and must not end up in games. The standard addon layout means installing is "unzip into your project".
- **Honesty over cleverness.** Only show actions that can actually be performed. Pull and Push disappear without a remote, Push is disabled with nothing to send, and tooltips say exactly what a button will do. We briefly had one context-sensitive "do the next thing" button; the maintainer preferred **separate Commit / Pull / Push buttons**, labeled with counts ("↓ Pull 2", "↑ Push 1").
- **Features live where you already are, not in menus** (decided 2026-09-26). Each action shows up on the thing it acts on, at the moment it's needed: right-click a commit for commit actions, the branch picker for branch actions, a dialog only when a real decision is needed (e.g. what to do with your changes when switching branches). Sections appear only when they have something in them (like Stashes). The ⋮ menu is for rare, repository-wide actions. The maintainer's counterexample is VS Code's stash: buried two submenus deep, and once made, a stash is invisible, so it's "almost useless". The panel shouldn't lay out every git feature in the open either; things rarely needed in game projects stay out (see "Left to the terminal" under Big features).
- **Native look.** `FoldableContainer` sections, editor theme icons/colors, Tree button styles for header buttons, Title Case. Avoid inventing styles.
- **Calm lists.** Status letter on the left in a fixed column, neutral file names, dimmed folder after the name, action buttons only on hover, line counts as section totals on screen and per file only in the tooltip. Each of these came from an earlier version looking cluttered.
- **The panel does git operations; it doesn't set up accounts or hosting** (decided 2026-09-25). It can initialize a repository, add a remote by URL and set your name and email, because it can do each of those completely. It doesn't create repositories on GitHub/GitLab, run an OAuth flow, store tokens or generate SSH keys: every host differs (we'd do one well and the rest half-way), it would be a security surface inside a game editor plugin, and it would split logins from the git CLI's. Logins come from git's credential helper (Git Credential Manager does the browser sign-in), SSH from the user's own ssh setup. What the panel owes the user is a clear message when a login is missing: on macOS/Linux git often has no helper that can *ask* for one (osxkeychain, libsecret only store), so that message should name Git Credential Manager, `gh auth login`, or signing in once with `git fetch` in a terminal. Done 2026-09-25: when git has no credential helper for the remote (`git config --get-urlmatch credential.helper <url>`), the login error says so and names `gh auth login` and Git Credential Manager (`test_credentials.gd`).
- **Setting up asks only when needed.** Initialize offers the project folder, or the folder above it when that's a sensible place (not a drive root, the home folder, or Desktop/Documents/Downloads/OneDrive, which would sweep up everything else in them), and lists what else that folder holds. Both are found by the panel's upward search, so there's nothing to configure. The first branch follows `init.defaultBranch` like `git init` does (Git for Windows' installer usually sets it to `master`), else `main`. Name and email are asked for at the first commit (and before a pull while you have unpushed commits, since that may merge), saved globally unless "Only for this repository" is ticked; the backend refuses commits without them with a readable message instead of libgit2's "config value 'user.name' was not found".
- **Opening files never surprises.** Godot opens what Godot can (scenes, scripts, resources, which respect Godot's own "use external editor" setting). Everything else goes to the external editor configured in Godot, else VS Code, else a toast explaining how to set one. We never hand files to random OS apps or the file manager.
- **Pull safety.** A pull either completes with your uncommitted work exactly where it was, or refuses and changes nothing. Unrelated uncommitted edits are carried through a merge (internally with a stash that's always restored), which makes pull usable in real Godot projects where the editor constantly rewrites `project.godot` and scenes. Edits to files the pull touches make it refuse up front. A conflicting merge is always fully undone; the panel doesn't resolve conflicts yet. `test_pull_safety.gd` covers all of this.
- **Network on a worker thread** so the editor never freezes on slow remotes or credential prompts.
- **Failed checkouts put everything back, stricter than git** (decided 2026-09-24). When a file is locked, the git CLI still "succeeds" (exit 0, one warning line). It moves HEAD and leaves the locked file with the old branch's content as a fake modification, which a later commit would silently revert. The panel instead retries briefly, then undoes everything and says which file was in use. The maintainer agreed after the two were compared by experiment.
- **No speed traded for binary size** (decided 2026-09-24). Size optimization (`/O1`, `-Os`) saved only 6% on Windows. What's kept are free wins only: dead-code removal on Linux (`--gc-sections`). About 2–2.7 MB per platform is the real cost of bundling a complete git (libgit2 with zlib, regex and HTTP parser) plus godot-cpp. Per-platform downloads are out, because teams commit the addon for every OS.
- **Name**: `godot-git`. The maintainer considers pun names pointless. The official plugin is "Godot Git Plugin", so an Asset Library listing should explain the difference in its description rather than through the name.

---

## Known gaps and risks

### Status and feedback (philosophy point 3)

This used to be the biggest gap. Before, the panel said very little while it worked, and afterwards only showed a toast that disappeared. A pull correctly refused for a conflict looked like "nothing happened", and the frozen-pull bug looked like a hung editor. **The status strip now exists** (see GitDock under Architecture):
- the current operation with real progress ("Fetching origin/main / Receiving objects 12% (54 MiB)") and Cancel;
- a result that stays ("Pulled and merged 2 commits from origin/main · just now", "Committed 4dff129 (2 files)");
- errors and warnings on a tinted background until dismissed;
- "Last fetched 3h ago" when there's nothing else to say.

It has been verified end to end in a real editor (fetch, pull, merge, commit, conflict refusal, canceling a real 60 MB GitHub fetch), but **not yet by the maintainer in daily use**. That's the real test of whether it earns trust. What's left:
- The **warning state** hasn't been looked at on screen (it shares the error's code path). It's used for the automatic-fetch failure and the pull's stash safety net.
- **Auto-fetch against a private repo** in daily use: it must stay silent (no sign-in windows) and show one warning at most. Tested against GitHub with prompts off, not yet over days of real use.
- **Push progress against a real server** hasn't been seen (local pushes finish before the strip can show much).
- Stage/unstage/discard are instant and deliberately don't use the strip, except for errors.

### Unproven or untested

- **The dock on Windows ARM, and in depth on macOS.** Linux x86_64 was checked by the maintainer on 2026-09-25 (looks, dialogs, fetch, pull); push, opening files in VS Code and Show in File Manager weren't explicitly reported there. CI proves all five libraries load in the official Godot 4.7.2 and pass the backend suite, HTTPS included. On macOS, a friend of the maintainer's used the dock on Apple Silicon without problems; nobody has looked closely at fonts, scaling, the file manager or VS Code paths there, or at an Intel Mac. The dock hasn't been seen on Windows ARM at all. macOS: see "macOS signing and Gatekeeper" below.
- **macOS signing and Gatekeeper** (investigated 2026-09-23). The library has no Developer ID signature: the arm64 slice is ad-hoc linker-signed (flags 0x20002, which Apple Silicon requires and the linker adds automatically), and the x86_64 slice is unsigned. Whether it loads depends only on the `com.apple.quarantine` flag:
  - **Asset Library install inside Godot: no flag, loads.** It downloads with `HTTPRequest` and unzips with minizip + `FileAccess` (`editor/asset_library/`). The Godot editor isn't sandboxed and has no `LSFileQuarantineEnabled`, and it has `com.apple.security.cs.disable-library-validation`, so a non-Godot-signed library is allowed. CI's macOS test (curl download, no flag, arm64 runner) is the same condition, and it loads and passes.
  - **git clone of a project with the addon committed: no flag, loads.**
  - **Browser-downloaded zip: flagged, likely blocked** ("Apple cannot check it for malicious software"). Widely reported for GDExtensions, not tested here. The workaround is `xattr -dr com.apple.quarantine addons/godot_git` or System Settings > Privacy & Security > Allow Anyway. It could be verified in CI by setting the flag on the macOS runner (check `spctl --status` first; runners may have Gatekeeper off, which would make the test meaningless).
  - **Consequence:** a Developer ID ($99/year) plus notarization is only needed for a smooth browser-zip install. It is **not** a blocker for an Asset Library listing.
- **SSH remotes** have never been used with a real login (no SSH keys on the dev machine). Since 2026-09-24 fetch and push for SSH remotes go through git (see "SSH remotes" under Architecture), which fixed the earlier findings:
  - the panel ran Windows' own `ssh` while git uses its bundled one;
  - every failure read "could not read refs";
  - ssh could wait on a prompt nobody sees;
  - Cancel waited until ssh gave up.

  `test_ssh.gd` covers routing, ssh's own errors and Cancel with a fake ssh (`core.sshCommand`). A real `ssh` against GitHub without keys fails in 0.5 s with the unknown-server message. Still untested: a real SSH login, fetch and push (CI could run `sshd` on the Linux runner with a throwaway key).
- **Light editor theme** was never looked at. Everything uses theme colors, so it should be fine.
- **RTL layouts**: `_draw_file_row` assumes left-to-right.

### Missing features

Roughly in order of value, after status and feedback. Per philosophy point 1, don't expose any of these half-done; a feature appears in the UI when it fully works.
- **Diff viewer.** First version done for uncommitted files (Big features 1); commits come with History.
- **The git features we'll support, and where each one lives, are planned under Big features** (decided 2026-09-26): History, restore a file from a commit, undo last commit, revert, stash, ignore a file, branch delete/rename, merging branches, conflict resolution, partial staging, LFS locking. Also there: what's deliberately left to the terminal.
- **Managing remotes after the first one** (rename, change URL, remove). Add Remote only appears while there are none; the rest is rare and done in a terminal.
- **UI tests in the repo.** The smoke test (Testing, 2) catches crashes; nothing checks how the dock looks or reacts to real clicks.
- Localization: strings are hardcoded English (Godot uses `TTR`; extensions have no equivalent wired up here).

### Performance considerations
Measured 2026-09-26 (Windows, this machine; median of 5; benchmark scripts were in the session scratchpad, not in the repo):

| | Godot engine repo (86k commits, 14k files, clean) | 2,500 changed files (2,000 modified, 500 staged, 500 new) | one 20,000-line file, 13k lines changed |
|---|---|---|---|
| `get_status` | 76 ms | 8 ms | 3 ms |
| `get_line_stats` unstaged / staged | 73 / 3 ms | **5,180 / 520 ms** | 13 / 0.3 ms |
| `get_commits(51)`, first call | 140 ms | 1 ms | 0.2 ms |
| `get_commit_files` (median of 50 commits) | 6 ms | 7 ms | 5 ms |
| `get_diff` / `get_commit_diff` of one file | 1 / 6 ms | 0.3 / 2 ms | 21 / 11 ms |
| **Dock `refresh()`** (headless editor; how long it blocks the editor) | ~150 ms | **142 ms** (was 6.8 s), counts arrive ~6 s later | 18 ms |
| Diff panel: show / switch to side by side | | 5 / 2 ms | 0.53 / 0.87 s |

- **`get_line_stats` costs 2.6 ms per changed file** (the git CLI's `git diff --numstat`: 0.13 ms). libgit2 loads the content filters for every file without its attribute cache (`diff_file.c`, `git_filter_list_load` without a session), so each file looks up `.gitattributes` from scratch, which is slow on Windows. 2,000 changed files (a Godot version upgrade rewriting every scene) froze the editor for ~7 s on every save. **Now counted on a worker thread** (see "Line counts" under GitDock); the counting itself is as slow as before. Still possible: diff only the paths `get_status` reported (saves the second workdir scan, ~70 ms in the engine repo), or patch libgit2 to pass an attribute session.
- **File icons were the other half second:** `_file_icon` cost 0.26 ms per row (`localize_path` plus theme lookups). Now `_to_res_path` is plain string work when the file is inside the project, and icons are cached per file type (`file_icons`, cleared on theme change). Building 2,000 rows takes ~130 ms.
- Expanding a commit in History counts lines for up to 300 files on the main thread (82 ms for the slowest of the engine repo's last 50 commits). Fine so far.
- Fixed in this pass: per-file LFS attribute checks only run in repositories that use LFS (they cost 0.57 ms per file: 1.3 s of the 7 s); `get_commits` reuses its last result while HEAD and every branch point at the same commits (a millisecond to check; the libgit2 walks cost 140 ms in the engine repo, where `git rev-list` takes 31 ms); the Diff panel skips its hidden highlighter copy for plain text (halved a 13k-line diff to 0.53 s).
- Files over 2 MB are treated as binary on purpose.

### Behavior worth knowing

- Godot itself creates `.uid` files and rewrites `project.godot`, so a fresh project always shows changes. That's correct, but it surprises people.
- Discard on an untracked file **deletes it** (with a confirmation that says so).
- If the incoming commits change a file you have uncommitted changes to, pull refuses and names the files; you commit or discard them first. This is stricter than `git pull --autostash`, on purpose: the old behavior (merge, then leave your edits in a stash the panel can't restore) broke philosophy point 1. Godot rewrites `project.godot` and scenes often, so if a teammate also changed `project.godot`, you'll be asked to commit yours first. Watch for whether that's annoying in practice; the fix would be conflict handling, not stashes.
- Commit runs on the main thread. It's fast in practice; the strip confirms it with the new commit's id.
- While a pull or push runs, the branch picker, ⋮ menu and Commit are disabled: the worker thread is rewriting the repository.
- Cancel stops the network part of an operation (including a login window) and reports "Canceled. Nothing was changed." The local part of a pull (updating files, merging) is quick and runs to the end; Cancel disappears once it starts.

### Bugs found in real use (and fixed)

- **Switching to a branch without the addon crashed the editor** (maintainer, StorageWars on Linux, 2026-09-25): branch `Niller` has no `addons/godot_git`, and the editor segfaulted a second after the switch. See gotcha 30. A second crash that day (restarting on `master`, inside Godot's own glTF reimport, no godot_git frames) wasn't reproduced headless; it may need the real renderer. The dev-only crash on quit after a hot reload is fixed too (gotcha 22).

Worth remembering, because each came from a path the tests didn't cover:
- **A branch switch that failed halfway left the tree half-switched** (maintainer, playground project, 2026-09-24): `boss.png` was in use, so the switch failed after deleting `notes.txt`, the `.uid` files and several `.import` files. HEAD stayed on `main`, so they all showed as deleted changes, and the next pull was refused because of them. Pull's fast-forward and merge had the same flaw. Fixed with `checkout_all_or_nothing`; `test_checkout_safety.gd` reproduces it on Windows by holding a file open with `FileAccess`.
- **Signing in from the panel never worked; only logins already saved by the git CLI did.** GCM refused to show its window when started by Godot (see `credential.interactive=always`), and even a successful sign-in wasn't saved because we never sent `approve`. The maintainer had a login saved from the CLI, so it went unnoticed until they tried the official plugin's sign-in. Verified on GitHub by signing out, signing in through the panel, and fetching again with prompts off. `test_credentials.gd` covers approve/reject with a fake helper and a local HTTP server.
- **Pull from a private repo hung forever, pinning one core.** The login read looped on `eof_reached()`, which Godot pipes never report. Tests had only fetched a public repo, which needs no login. Reproduced with the old build, fixed, and verified against the private repo.
- **"Unstage all" failed** with a libgit2 assertion (empty pathspec). Only single-file unstage had been tested.
- **The "No changes." placeholder acted like a file** (hover highlight, stage/discard buttons). Placeholder rows were real tree rows, plus the `"<null>"` metadata gotcha.
- **Push behind the remote showed libgit2's raw sentence** instead of "Pull first, then push" when you hadn't fetched (the common case): the code matched one of libgit2's two wordings. Found by the in-repo tests on their first run.
- **Pull looked broken in the demo** because the demo's commits genuinely conflicted, and the refusal was only a toast. That's a status/feedback problem as much as a demo problem.

---

## Roadmap

Two tiers: small **stepping stones** that make the base solid, then the **big features**. Finish the stepping stones first; the big features change a lot of code and need the tests and a trusted base underneath them.

### Stepping stones

1. **Real use of the status strip and auto-fetch.** *Waiting on the maintainer.* They use it on StorageWars and report what still feels untrustworthy. The design is only settled after real use.
2. ~~Tests in the repo, run in CI.~~ **Done:** `project/tests/`, run on Windows x86_64/arm64, Linux x86_64/arm64 and macOS in CI. Keep adding a test for every bug from real use. All four test jobs passed on the first CI run (105 checks each).
3. ~~Small gaps.~~ **Done:**
   - Saving inside Godot refreshes the panel. It already did via `filesystem_changed`, and `project.godot` is now watched too.
   - Auto-fetch.
   - Pull refuses up front instead of ever stranding edits in a stash.
4. ~~Run the Linux build on a real machine~~ (**done** 2026-09-25, see Supported targets; it also turned up the branch-switch crash, gotcha 30), **then tag `v0.1.0`**. `v0.1.0` was released on 2026-09-25, before the crash fix, so it has the branch-switch crash on every platform: release `v0.1.1` next (CI green on all five at `5cbbd8d`). Also: check on Windows what the new "switch to a branch without the addon" dialog leads to. The loaded DLL is locked, so the panel's switch probably refuses ("file in use") rather than closing the panel as the dialog says; adjust the wording if so. Also check that a terminal `git checkout` there no longer crashes the editor. The Windows/macOS pinning code has only been compiled in CI.
5. **Asset Library listing** after `v0.1.0`. macOS signing is *not* a blocker (see "macOS signing and Gatekeeper"). The source repo has no binaries, so the listing needs a custom download URL pointing at the release zip (or a binaries branch). Check whether Godot's newer Asset Store or the classic Asset Library is current, and their rules, at that time. The README needs a macOS note for browser-downloaded zips (`xattr -dr com.apple.quarantine addons/godot_git`).
6. Later: a performance pass on `refresh()` for big repos.
7. ~~Name for the dock tab.~~ **Decided 2026-09-24: it stays "Git".** The maintainer tried "Version Control" and found it too long for a small tab (it also clashes with Godot's own Version Control panel). The bottom-panel diff dock is called "Diff" (decided 2026-09-26). The maintainer finds pun names pointless. Keep `set_layout_key("GodotGit")` unchanged if anything is ever renamed, or users lose their saved dock position.

### Next up (agreed 2026-09-24, in this order)

1. ~~Git LFS.~~ **Done** (2026-09-24), see "Git LFS" under Architecture. Still open: a real LFS server with a sign-in, and a project with thousands of LFS files (speed).
2. ~~Commit hooks and signing.~~ **Done** (2026-09-24), see "Hooks and commit signing" under Architecture. GPG signing with a real key and pinentry is untested (the tests sign with SSH keys).
3. ~~SSH fixes.~~ **Done** (2026-09-24), see "SSH remotes" under Architecture.
4. ~~New action layout plus Amend.~~ **Done** (2026-09-24), pulled forward so the maintainer can use it while the rest is built. Checked on screen at normal and narrowest (~190 px) dock widths.

### Big features (in order)

**1. Diff viewer: a bottom-panel dock.** **First version done (2026-09-26)** for uncommitted files: see "GitDiffDock" under Architecture. Checked on screen (headless driver + screenshots) for a multi-hunk script, both views, a scene, staged, untracked, binary, rename and deleted files, following a staged file, and updating after an edit. Not yet seen by the maintainer. Still open from the plan below: images, word-level highlights, commits (feature 2). A second `EditorDock` with default slot `DOCK_SLOT_BOTTOM`, beside Output / Debugger / Animation (decided 2026-09-26; this replaces an earlier plan for a main-screen tab beside 2D / 3D / Script):
- **Split of roles.** The right dock holds the lists (changes, history) and does the daily work (stage, commit, pull, push, status). The bottom panel shows content: the diff of whatever file you click. The two complement each other. One rule everywhere: **click a file in the dock and its diff opens at the bottom**, whether it's unstaged, staged or from an old commit. Double-click still opens the file for editing.
- **Why not a main-screen tab.** The maintainer found it too much space and clunky: it switches you away from your scene/script for every glance at a change. The bottom panel keeps your work visible. It spans the whole editor width, which is what side-by-side needs. When more height is needed (a long diff, merging later), Godot 4.7 has **Expand Bottom Panel** (Shift+F12), and a dock can float as its own window (`DOCK_LAYOUT_FLOATING`; checked in the 4.7.2 source). The maintainer expects that to be enough even for merging.
- **How the official plugin does it** (checked 2026-09-26 in 4.7.2's `editor/version_control/version_control_editor_plugin.cpp`; the plugin only supplies hunks through `EditorVCSInterface::_get_diff`, Godot draws them). The same shape we chose: an `EditorDock` with `DOCK_SLOT_BOTTOM`, layouts horizontal + floating, minimum height 300 px, a title plus a Split/Unified `OptionButton`. The diff is one `RichTextLabel` with a table per hunk (line numbers, `-|`/`+|`, text), colored with `error_color`/`success_color` text, and a centered `@@` header per hunk. Split view pairs removed and added lines itself. Worth doing better: colored *text* instead of faint line backgrounds, no syntax highlighting, no word-level changes, and a commit shows **all its files' diffs at once** in one label (no lazy loading, and a big table in a `RichTextLabel` gets slow). It caches theme colors in `static` locals, so a theme change isn't picked up.
- **Name: "Diff"** (decided 2026-09-26; layout key `GodotGitDiff`). One tab, not one per purpose: everything planned for it (uncommitted changes, a commit's files, merge conflicts) is a diff of one file, and the right dock picks which. "Review" was the alternative, rejected as vague; "Changes" clashes with the dock's section.
- **Rendering.** A read-only `CodeEdit` with the editor's code font; added/removed lines get faint success/error-colored backgrounds (`set_line_background_color`); old and new line numbers in gutters; syntax highlighting where available. Unified and side-by-side views, side-by-side with synced scrolling. No file list of its own (the dock is the file list); a header names the file and where it's from (staged, unstaged, commit `4dff129`).
- **Diffs for Godot's file types** (planned 2026-09-26, in this order; none built yet). Today anything binary says "binary file", and scenes show as noisy text.
  - **Foundation: a file's bytes on either side** (**done** 2026-09-26: `get_file_bytes`, LFS from the local cache; downloading missing LFS versions not yet), for everything below: HEAD, the index, the working tree, or a commit and its first parent (e.g. `get_file_bytes(revision_or_side, path)`). **LFS files** hold a ~130-byte pointer in git; the real bytes come from git-lfs's local cache (`.git/lfs/objects/<oid[0:2]>/<oid[2:4]>/<oid>`), which has every version you've checked out or pulled, so usually no download. A version never fetched is downloaded in the background with `git lfs` (the machinery pull uses), with progress in the panel ("Downloading the old version (2.4 MB)..."), prompts off, and a plain message if it can't.
  - **1. Images** (**done** 2026-09-26, see the Diff panel under Architecture; checked on screen with a sprite, a new image, a re-saved image and an SVG; LFS images only through backend tests so far) (png, jpg, webp, svg, bmp, tga; Godot loads them from bytes): before | after side by side on a checkerboard, each with its size ("512×512 → 1024×1024"), or one side with "added"/"deleted". SVG also keeps its text diff.
  - **2. Scenes and resources as structure** (`.tscn`, `.tres`): the most valuable for every Godot user, because their text diffs are full of `ExtResource("3_k2x8f")`, uid churn and reordered sections. Parse both versions ourselves (a simple, documented text format; no importing) and compare node by node and property by property: "Player › Camera2D: `zoom` (2, 2) → (3, 3)", "Enemy: node added", "Sprite2D: `texture` icon.png → boss.png". A view toggle next to Unified / Side by Side; the raw text stays one click away.
  - **3. Audio** (wav, ogg, mp3; loadable from bytes): before and after with a play button each, length and sample rate; a small waveform later.
  - **4. 3D models, glTF/GLB**: `GLTFDocument.append_from_buffer()` builds a scene from the bytes without importing, so both versions can be shown in two small 3D views with one shared orbit camera, plus a summary of what changed (vertices, triangles, materials, bones, animations). FBX only if it's as clean as glTF: `FBXDocument` (4.3+) should take the same path (`append_from_buffer`); if it needs workarounds (temp files, importer tricks), leave FBX out and say so (maintainer, 2026-09-26). `EditorResourcePreview` can't help: it only makes thumbnails of the current file in the project, not of an old version.
  - **Not planned** (maintainer agreed): fonts (rarely iterated), OBJ (rare in Godot pipelines, no loader from bytes), `.blend` (Godot imports it by running Blender; the panel says so).
  - Huge files keep showing a notice instead of freezing (over 2 MB today).
- **Scope.** Read-only first. Staging single hunks or lines from the diff comes after, and only once it works completely (philosophy point 1).
- **Backend.** A `get_diff(path, staged)` returning hunks and lines with old/new line numbers; libgit2 already builds these patches for the line stats. Computed only for the file that's clicked.

**2. History: expandable commits in the right dock**, like VS Code's Source Control graph (decided 2026-09-26). **First version done (2026-09-26)**, see "History" under Architecture; checked on screen (expanding, a merge, a commit's file in the Diff panel, surviving a refresh, Load More with 65 commits), not yet by the maintainer:
- **Expanding a commit** in History shows its full message, author and date, and the files it changed, as rows that look exactly like the Changes rows (status letter, icon, name, dimmed folder, +/− in the tooltip). **Clicking one of those files opens its diff in the bottom panel**, the same as for uncommitted files. The diffs themselves never go in the dock: it's ~200–460 px wide, too narrow for code.
- **Everything loads on demand**, never all up front:
  - a commit's files are read only when it's expanded (the row gets a placeholder child so the arrow shows, replaced on the Tree's `item_collapsed` signal);
  - a diff is computed only when its file is clicked;
  - the commit list grows with a "Load more" row (or near the end of the scroll) instead of today's fixed 50.
- **Merge commits** show their changes against the first parent (like `git log -p --first-parent`), or a merge would list every file the other branch touched.
- **Backend.** A call returning one commit's changed files (path, status, +/−), plus `get_diff` for a commit's file, plus paging for `get_history`.
- Start without a branch graph; a graph is a separate, harder piece.

**3. Commit actions in History** (right-click a commit or one of its files; all build on feature 2):
- **Restore This Version** (right-click a file under a commit): puts the file back as it was in that commit, as an uncommitted change you can review in the Diff panel and commit or discard. For "I broke this scene yesterday". Probably the most-used of all; refuses (naming the file) if the file has uncommitted changes, rather than overwrite them.
- **Undo Last Commit** (the newest commit, only while it isn't on any remote-tracking branch, like Amend): moves the branch back one commit and leaves its changes staged. Nothing is lost. Hidden or disabled with a reason once pushed, since rewriting pushed history hurts teammates.
- **Revert Commit** (any commit): a new commit that undoes it, safe for pushed history. A revert that would conflict is refused and fully undone until conflict resolution exists.

**4. Manual stash** (decided 2026-09-26). Branch switching stays as it is: like `git switch` and SourceTree, it carries uncommitted changes over when none of the changed files differ between the branches, and otherwise refuses before touching anything (all or nothing, never half-switched). No question on every switch. The maintainer is fine with that; what's missing is a way to set changes aside without committing or discarding them:
- **Stash Changes** on the Changes header and in the ⋮ menu. Includes new (untracked) files, or "set aside" would quietly leave them behind.
- **A Stashes section** in the dock, below Changes, shown only while stashes exist (a stash must never be forgotten, the maintainer's complaint about VS Code). Rows like "`main` · 4 files · 2h ago"; Restore and Delete on each (Delete asks first). Stashes made in a terminal show too. Expanding one to see its files, and each file's diff in the Diff panel, comes with History (the same mechanism).
- **Restore is all or nothing**, like switching and Pull: it refuses up front, naming the files, when your current changes touch the same files, and the stash is only dropped after it applied completely. Never libgit2's `git_stash_pop` (libgit2 gotcha 2).
- Files open in Godot (scenes) get rewritten by stash and restore; check on screen that the editor reloads them properly.
- **Also: the switch refusal should name the files in the way** (today it only says "Your local changes would be overwritten by switching branches. Commit or discard them first."), and mention stashing as a third way out.
- **Rule: never stash as part of another operation** (the maintainer, 2026-09-26). Other clients' "stash, switch, re-apply" (VS Code's Stash & Checkout, SourceTree, GitHub Desktop bringing changes along) can leave you on the new branch with conflict markers in your files, and sometimes without the stash. The panel's promise is the opposite: an operation either works or refuses before touching anything. Switching and Pull already work that way (Pull's internal autostash only runs once it's certain to restore cleanly); stash stays something only the user does, and Restore is all or nothing.
- Considered and dropped: asking "leave or bring my changes" on every switch with changes (GitHub Desktop), and restoring them automatically when you come back. More friction than it saves.

**5. Small, contextual extras:**
- **Ignore...** (right-click a new, untracked file; decided 2026-09-26). One menu item that opens a small dialog, not a submenu, because the choice has consequences a menu can't show:
  - Three choices: **this file** (`/art/boss.psd`), **all files of this type** (`*.psd`), **the folder** (`/art/raw/`, with a dropdown for which parent). No custom-pattern field: the maintainer found it pointless, since a power user opens the file. An **Edit .gitignore** button opens it instead.
  - It shows the effect before you confirm ("Hides 12 files from Changes"), computed by libgit2 against the current untracked files with a temporary in-memory rule (`git_ignore_add_rule`, then `git_ignore_clear_internal_rules`), so it's git's real matching, not ours.
  - Godot's companion files (`.import`, `.uid`) are ignored along with the file, and the dialog says so; otherwise the list doesn't get cleaner. Several selected files are listed, and "this type" covers all their extensions.
  - **Only for untracked files.** `.gitignore` does nothing to a tracked file, so offering it there would be a button that lies. The real action for tracked files is "Stop Tracking" (`git rm --cached`), which deletes the file from teammates' disks when they pull; it needs its own warning and is left out until someone asks.
  - The rule goes into the `.gitignore` next to `project.godot` (the one Initialize Repository writes), or the nearest existing one above it. Afterwards `.gitignore` shows in Changes like any edit: the Diff panel shows the added lines and a discard undoes them, so it needs no undo of its own. The status strip confirms ("Ignored `*.psd` (12 files)").
  - Initialize Repository already writes Godot's own `.gitignore` and `.gitattributes`.
- **Branch delete and rename** in the branch picker (delete refuses unmerged work unless confirmed; never deletes the current branch).

**6. Merging and conflicts:**
- **Conflict resolution.** Let a pull (or merge, or stash restore) stop at conflicts instead of refusing. List conflicted files with "Keep Mine / Take Theirs / Open in Editor", show them in the Diff panel as a three-way view (it's still a diff), and finish the merge when all are resolved, or abort cleanly. This removes the last "use git in a terminal" message.
- **Merge a branch into the current one** (from the branch picker). Only after conflict resolution; before that it would refuse too often to be worth showing.

**7. Later:**
- **Stage part of a file** (hunks, then lines) from the Diff panel, once it works completely.
- **LFS file locking** (`git lfs lock`), for teams where artists share binary scenes and textures. git-lfs supports it; it needs a clear "locked by X" state on file rows.
- Tags, if releases need them.

**Left to the terminal** (decided 2026-09-26): rebase and interactive rebase, cherry-pick, submodules, worktrees, reflog, bisect, blame. They're powerful but rare in game projects, and each needs a lot of UI to do right; half-done would break philosophy point 1. Revisit only if real use asks for one.

## Where we left off (2026-09-24)

A long session: RAII cleanup, sign-in from the panel, Git LFS, the new action layout with Amend, hooks and signing, all-or-nothing checkouts, SSH through git, and a CI race fix. All of "Next up" is done. The notes below are for picking it up again.

**Before anything else**
- **Push what's uncommitted** (SSH through git, Linux `--gc-sections`, docs), then check CI:
  - all five test jobs green; `test_ssh.gd` has only run on Windows so far, and its fake ssh is a `sh` script;
  - the Linux `.so` size: expected about 2 MB instead of 4.2 MB (inferred from the macOS numbers, not measured);
  - signing tested on all five (it was on the last run).
- **Playgrounds**: `tools/make_playground.py <folder>` (added 2026-09-26) builds one with history and every kind of change. It has no remote or LFS files yet; add them when a UI check needs them. Keep playgrounds in a short path (gotcha 34).

**Built but never seen by a human**
- Ticking **Amend** and pressing it in a real editor (headless drivers can't click; backend tests only).
- The **Signing in...** strip text, a **background commit** with a slow hook, and a **background LFS branch switch**, in the GUI. Headless drivers checked the logic, not the looks.
- The sync row at the narrowest dock width **while an operation runs**. Its labels get longer ("Publishing...") and may push the row wider.

**Unverified against the real thing**
- Git LFS against a real server with a sign-in (only local remotes so far), and with thousands of LFS files (speed of one `filter-process` round trip per file).
- GPG signing with a real key and passphrase window (the tests sign with SSH keys).
- An actual SSH login that fetches and pushes. CI could run `sshd` on the Linux runner with a throwaway key.
- Cancel during a real LFS download (local ones finish too fast).
- The dock on macOS beyond a friend's quick look.

**Things I'd think about**
- **What locked `boss.png`?** Unknown: Godot importing it, the thumbnail generator, or antivirus. Background operations (LFS switch, commit with hooks) now let the editor keep scanning while files change. Checkouts survive it now, but pausing or deferring Godot's filesystem scan during our own operations might avoid the lock in the first place (`EditorFileSystem` has no pause API; worth a look).
- **macOS PATH**: Godot started from Finder/Dock gets a minimal PATH (`/usr/bin:/bin:/usr/sbin:/sbin`). `/usr/bin/git` works if the developer tools are installed, but a Homebrew `git-lfs` (`/opt/homebrew/bin`) won't be found, so LFS repos would refuse ("Git LFS isn't installed") even though it is. Unverified; check on a Mac before an LFS user hits it.
- **Two commit paths** (libgit2 without hooks or signing, git with them) can drift apart. If they ever do, "always commit through git" is simpler and costs a process start (~50–100 ms).
- **Hooks not run**: `pre-merge-commit` (we commit merges with `git commit`), and `post-checkout`/`post-merge` after libgit2 checkouts. git-lfs installs `post-checkout`/`post-merge` for file locking, which the panel doesn't support either.
- **LFS limits**: only the root `.gitattributes` is checked, each file is held in memory while filtered, and there's no LFS locking.
- **The test suite keeps growing** (296 checks, a few minutes on Windows). Fine for now; split slow suites if it starts to hurt.
- **Asset Library**: check whether the official plugin is listed there for 4.x before writing our description.

**Next** (2026-09-26): `v0.1.1` is out. The Diff panel and History (Big features 1, 2) have first versions, waiting on the maintainer's use. Next: manual stash (4; it fixes a real problem now) and the commit actions (3). The fix for the fold crash (gotcha 37) is worth a `v0.1.2`. Still open alongside: check the branch-switch dialog on Windows, and the Asset Library listing.

## Advice

General advice for working on this codebase:
- **Verify engine behavior in the engine source** (and for 4.7, in the `4.7.2-stable` tag) before guessing. Most UI bugs here came from Tree internals that only the source explains.
- **Look at the UI** after every visual change. Don't trust that it looks right.
- **Keep the UI honest.** If something can't be done in a state, hide it or disable it with a tooltip that says why. Never show a button that promises more than it does.
- **Test the paths real users hit.** Private repos, logins, "all" variants of actions, empty states, a dirty `project.godot`. Every bug so far lived in one of those.
- **When the maintainer reports a problem, reproduce it before explaining it.** Say clearly what's proven and what's inferred.

My own take on the status work: the strip's job is to always answer "what is the panel doing, and what did it last do?" Guard that when adding features. Every new operation that can fail or take time should report through `_set_status`, never through a toast alone.
