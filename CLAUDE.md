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
- History of the last 50 commits; commits not yet on any remote are highlighted with the accent color.
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
| `src/git/git_repository_remote.cpp` | Fetch, pull, push. `pull()` is split into `paths_blocking_pull` / `fast_forward` / `merge_and_commit` / `merge_with_autostash`. |
| `src/git/git_remote_callbacks.{h,cpp}` | libgit2 remote callbacks: logins via `git credential fill/approve/reject`, progress reporting (`RemoteContext`, `report_progress`), cancel. |
| `src/git/git_cli.{h,cpp}` | Steps handed to the git CLI because libgit2 would skip hooks or signing, or handles SSH badly: `commit_needs_git`, `has_hook`, `commit_with_git`, and `run_git_command` (runs git with merged output, live progress lines, Cancel). |
| `src/git/git_lfs.{h,cpp}` | Git LFS: a libgit2 filter for `filter=lfs` backed by `git lfs filter-process`, plus `git lfs fetch`/`push` with progress. |
| `src/git/git_util.{h,cpp}` | Small libgit2 helpers shared by the above (`Owned` pointers like `CommitPtr`, `fail`, `to_error`, `head_branch`, `changed_paths`, `uncommitted_paths`, ...). |
| `src/editor/` | **Editor UI.** |
| `src/editor/git_dock.h` | `GitDock` (EditorDock). Its private methods are grouped by the file that implements them. |
| `src/editor/git_dock.cpp` | Building the dock, `refresh()`, toolbar and action row, local actions (stage, discard, commit, branches). |
| `src/editor/git_dock_lists.cpp` | Staged Changes / Changes / History: row drawing, header alignment, hover buttons, clicks, context menu. |
| `src/editor/git_dock_status.cpp` | The status strip. |
| `src/editor/git_dock_network.cpp` | Fetch / pull / push on a worker thread, auto-fetch. |
| `src/editor/git_dock_setup.cpp` | Setting a repository up: the empty state with Initialize Repository, Add Remote, the name and email dialog. |
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
- `refresh()` rebuilds everything: sync status, branches, status + line stats for both panes, history, action buttons. It runs on:
  - `READY`;
  - `filesystem_changed` (every scene/script/resource save in Godot ends up there, via `ResourceSaver` → `EditorFileSystem::update_file`);
  - `NOTIFICATION_APPLICATION_FOCUS_IN` (changes made outside Godot);
  - a change of `project.godot`'s modified time, checked every second, because Project Settings saves it without notifying anything;
  - after every action.

  There is no other polling.
- Network ops run on a `Thread` (`_network_worker`) and report back with `call_deferred` to `_network_done`. `_finish_network_thread` joins on `EXIT_TREE`. Commit, Amend, the branch picker and the ⋮ menu are disabled while a pull, push or LFS branch switch runs; the worker is rewriting the repo.
- **Auto-fetch** (`_on_auto_fetch_timer`): checks 10 s after opening, then every minute, and fetches when `last_fetched` (FETCH_HEAD's age, so CLI fetches count) is 5+ minutes old. After a failed attempt it also waits 5 minutes. Per-project setting in the ⋮ menu (`EditorSettings` project metadata `godot_git/auto_fetch`, default on). It runs **quietly** (`network_quiet`): no strip, buttons stay enabled, and no sign-in windows (`set_login_prompts_allowed(false)`). Pressing Fetch during it just shows it. Pressing Pull/Push queues the op (`queued_op`), shown as busy with "Waiting for a background fetch...". A failure shows one warning, cleared by the next success. `_shown_network_op()` is what the buttons reflect.
- **Status strip** (`_set_status` / `_update_status` / `_update_status_style`), under the toolbar. Every result and error goes here. It's one wrapping `RichTextLabel` (never trimmed; selectable so errors can be copied), an icon, a Cancel/Dismiss button, and a thin progress bar while busy. Kinds: idle ("Last fetched 3h ago", from `last_fetched` in the sync status), busy, success ("· just now", kept current by a 30 s timer), neutral (canceled), warning and error (tinted background, stay until dismissed or replaced). Results never time out. Toasts are only used when the dock is hidden behind another tab, so an outcome is never missed.
- Progress: the worker's `GitRepository` gets `set_progress_callback(callable_mp(dock, &_network_progress))`. The backend throttles to ~10 updates/s and uses `call_deferred`, so the dock is only touched on the main thread. Cancel calls the static `GitRepository::cancel_network()`, which sets an atomic flag the libgit2 callbacks check (returning `GIT_EUSER`) and kills the child process being waited on (`git credential fill`, `git lfs fetch/push`; tracked with `track_process`). On Windows that's `taskkill /T`, because git's own children (Git Credential Manager's window, git-lfs) outlive their parent otherwise. The op then fails with `ERR_SKIP`.

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

1. **Backend tests: `project/tests/`, in the repo and in CI.** `run_tests.gd` (a `SceneTree` script) runs the suites `test_local.gd`, `test_sync.gd`, `test_pull_safety.gd`, `test_checkout_safety.gd` (Windows only), `test_amend.gd`, `test_hooks.gd` (real hooks, and SSH signing with a throwaway key), `test_credentials.gd`, `test_ssh.gd` (fake ssh), `test_lfs.gd` (skipped without git-lfs; CI installs it), `test_no_git.gd` (points the extension at a git that doesn't exist via `GitRepository.set_git_program`), `test_setup.gd` (init, add remote, name and email, against an empty global config via `GitRepository.set_config_home`) and `test_online.gd` (the last only with `-- --online`). Each suite extends `test_case.gd`, which builds throwaway repos with the real git CLI (a bare "remote" plus "mine" and "theirs" clones via `make_shared()`). Checks are made against what git itself says. Run:
   `godot --headless --path project -s res://tests/run_tests.gd [-- --online] [-- <suite>]`. Exit code 1 on failure; scratch repos go to the OS temp folder and are kept (path printed) when something fails. CI runs them on Windows, Linux x86_64/arm64 and macOS against each freshly built library. It first opens the project with `-e --quit-after 300` so the extension gets registered; see gotcha about `--import`. **Every bug from real use gets a test here.** The suite has already caught one on its first run: the push "pull first" message never showed for the common unfetched case.
2. **UI tests in a real (headless) editor.** Not in the repo yet; these run from the session scratchpad. Copy the addon into a throwaway project and add a test-only `EditorPlugin` (`addons/ui_driver/`) enabled in `project.godot` that finds dock controls (`find_children` on `EditorInterface.get_base_control()`) and presses them (`button.pressed.emit()`, `popup.id_pressed.emit(id)`), then prints results. Run with `--headless -e --path <project> --quit-after <frames>` and **redirect stdout**. Gotchas:
   - PowerShell 5's `Set-Content -Encoding utf8` writes a **BOM**, and Godot then silently ignores `plugin.cfg`. Write files with `[IO.File]::WriteAllText(path, text, (New-Object Text.UTF8Encoding $false))`.
   - `--log-file` did **not** capture plugin `print()` output. Redirect stdout instead.
   - `--quit-after` counts frames, not seconds. Give network tests a generous number (e.g. 6000).
   - Tree **button clicks can't be injected**: Tree re-checks the real OS cursor before accepting a button press. Emit `tree.button_clicked` directly to test the handler.
   - Injected hover works headless but gets overridden in a GUI editor by the real cursor.
   - For screenshots of dialogs, start the editor with `--single-window` (otherwise popups are separate OS windows that `PrintWindow` on the main window misses), and trigger the driver with `get_tree().create_timer(...)`, not a frame count: an unfocused editor only draws ~10 frames a second.
3. **Visual checks.** Screenshot the editor window with Win32 `PrintWindow` (works when occluded, but restore it if minimized), then crop the dock. **Never** simulate the OS mouse or keyboard: it acts on whatever window is on top of the user's desktop.
4. **Demo scenarios** must be designed so the git CLI agrees with them. A demo where "teammate" and "me" edit **adjacent lines** conflicts in git too. That once made pull look broken when it was behaving correctly.

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
22. **Hot reload can crash the editor inside the unloaded old library** (seen once, in the maintainer's open dev editor, while rebuilding). `callable_mp` connections and deferred calls hold raw function pointers into the old DLL. Dev-only: release zips have `reloadable = false`. If it gets annoying, restart the editor rather than relying on hot reload for big changes.
23. **Project Settings saves `project.godot` without any signal**: `ProjectSettings::save()` bypasses `EditorFileSystem`, and `settings_changed` fires *before* the delayed save. The dock watches the file's modified time instead.
24. `OS.execute("cmd", ["/c", "rmdir", "/s", "/q", path])` silently does nothing: Godot quotes each argument separately. Pass cmd one string: `["/c", "rd /s /q \"path\""]`.
25. **`OS.execute` drops empty arguments and mangles embedded quotes** on Windows: `git config credential.helper ""` became a read, and `"$1"` inside an argument lost its quotes. The test suite writes such config lines into `.git/config` directly.
26. **`FileAccess.get_buffer()` on a pipe drops a partial last chunk** when the process exits. Reading `git upload-pack` output that way lost its final `0000`. Read binary pipe output byte by byte (`get_8` until `get_error() != OK`); line-based reads (`get_line`) are fine for newline-terminated text.
27. **`OS::get_process_exit_code()` returns -1 while the process is still running**, and a process's pipes close a moment before it has fully exited. Reading the code right after the pipe closes lost that race on CI's Linux arm64 runner: a `git push` that succeeded was reported as failed. Use `wait_for_exit_code()` (`git_util.h`).
28. **Wrapping `Label`s in a dialog need a minimum width** (`custom_minimum_size.x`). A wrapping label measures its height at its minimum width; at 0 that's one word per line, and the dialog opens as tall as the screen. `make_label` in `git_dock_setup.cpp` takes the width.
29. **libgit2 on Windows finds the global config through `HOMEDRIVE`+`HOMEPATH` as well as `HOME`/`USERPROFILE`.** To start an editor "without a git identity" for a screenshot, override all four (or the real `~/.gitconfig` is found). The tests use `GitRepository.set_config_home` instead.

30. **Godot unloads an extension whose `.gdextension` disappears mid-session** (`EditorFileSystem::_scan_extensions` → `GDExtensionManager::ensure_extensions_loaded`, checked in 4.7.1). It does so whether or not it's `reloadable`, and it happens on every platform, e.g. when switching to a branch without the addon. Two things then pointed into the unmapped library and crashed the editor. First, the dock was `queue_free`d, so it was freed after the unload (now `memdelete`d in `_exit_tree`). Second, godot-cpp's instance bindings on engine objects (EditorFileSystem, theme icons, ...) keep free callbacks into the library, and Godot only clears those for reloadable extensions on *reload*. So at unload, if our `.gdextension` is gone, `register_types.cpp` pins the library (`RTLD_NODELETE` / `GET_MODULE_HANDLE_EX_FLAG_PIN`). When the addon comes back in the same session, the OS returns that same pinned copy, and godot-cpp can't initialize twice (its statics and `library` pointer are stale). So a retired copy loads as an empty extension and warns "restart the editor". The dock asks before switching to a branch without the addon (`_addon_removed_by`). Tested with a headless driver that moves the addon folder away and back.

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
- **Native look.** `FoldableContainer` sections, editor theme icons/colors, Tree button styles for header buttons, Title Case. Avoid inventing styles.
- **Calm lists.** Status letter on the left in a fixed column, neutral file names, dimmed folder after the name, action buttons only on hover, line counts as section totals on screen and per file only in the tooltip. Each of these came from an earlier version looking cluttered.
- **The panel does git operations; it doesn't set up accounts or hosting** (decided 2026-09-25). It can initialize a repository, add a remote by URL and set your name and email, because it can do each of those completely. It doesn't create repositories on GitHub/GitLab, run an OAuth flow, store tokens or generate SSH keys: every host differs (we'd do one well and the rest half-way), it would be a security surface inside a game editor plugin, and it would split logins from the git CLI's. Logins come from git's credential helper (Git Credential Manager does the browser sign-in), SSH from the user's own ssh setup. What the panel owes the user is a clear message when a login is missing: on macOS/Linux git often has no helper that can *ask* for one (osxkeychain, libsecret only store), so that message should name Git Credential Manager, `gh auth login`, or signing in once with `git fetch` in a terminal. Not done yet; check it on the Linux machine.
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
- **Diff viewer.** The biggest gap. Selecting a file should show its diff, probably in a bottom-panel dock or a split. libgit2 patches are already computed for line stats.
- **Conflict resolution.** Today conflicting pulls are refused. A minimal version: let the merge happen, list conflicted files with "take mine / take theirs / open in editor", and commit when resolved.
- Stash UI, branch delete/rename, tags, blame, per-hunk staging.
- **Managing remotes after the first one** (rename, change URL, remove). Add Remote only appears while there are none; the rest is rare and done in a terminal.
- **UI tests in the repo.** The backend has a suite; the dock is only checked with scratch UI drivers and screenshots.
- Localization: strings are hardcoded English (Godot uses `TTR`; extensions have no equivalent wired up here).

### Performance considerations
- `refresh()` does full status + two diffs (line stats) + a 50-commit revwalk + the unpushed walk (capped at 1000) on **every** filesystem change and focus-in. Fine for normal projects; big repos or huge changesets will feel it. Candidates: debounce refreshes, compute line stats lazily or on a thread, skip stats above N files.
- `get_line_stats` generates a patch per changed file. Files over 2 MB are treated as binary on purpose.

### Behavior worth knowing

- Godot itself creates `.uid` files and rewrites `project.godot`, so a fresh project always shows changes. That's correct, but it surprises people.
- Discard on an untracked file **deletes it** (with a confirmation that says so).
- If the incoming commits change a file you have uncommitted changes to, pull refuses and names the files; you commit or discard them first. This is stricter than `git pull --autostash`, on purpose: the old behavior (merge, then leave your edits in a stash the panel can't restore) broke philosophy point 1. Godot rewrites `project.godot` and scenes often, so if a teammate also changed `project.godot`, you'll be asked to commit yours first. Watch for whether that's annoying in practice; the fix would be conflict handling, not stashes.
- Commit runs on the main thread. It's fast in practice; the strip confirms it with the new commit's id.
- While a pull or push runs, the branch picker, ⋮ menu and Commit are disabled: the worker thread is rewriting the repository.
- Cancel stops the network part of an operation (including a login window) and reports "Canceled. Nothing was changed." The local part of a pull (updating files, merging) is quick and runs to the end; Cancel disappears once it starts.

### Bugs found in real use (and fixed)

- **Switching to a branch without the addon crashed the editor** (maintainer, StorageWars on Linux, 2026-09-25): branch `Niller` has no `addons/godot_git`, and the editor segfaulted a second after the switch. See gotcha 30. A second crash that day (restarting on `master`, inside Godot's own glTF reimport, no godot_git frames) wasn't reproduced headless; it may need the real renderer. Still open, dev-only: after a hot reload (`reloadable = true`) the editor crashes on quit (it used to crash right after the reload).

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
4. ~~Run the Linux build on a real machine~~ (**done** 2026-09-25, see Supported targets; it also turned up the branch-switch crash, gotcha 30), **then tag `v0.1.0`**. Before tagging: check on Windows what the new "switch to a branch without the addon" dialog leads to. The loaded DLL is locked, so the panel's switch probably refuses ("file in use") rather than closing the panel as the dialog says; adjust the wording if so. Also check that a terminal `git checkout` there no longer crashes the editor. The Windows/macOS pinning code has only been compiled in CI.
5. **Asset Library listing** after `v0.1.0`. macOS signing is *not* a blocker (see "macOS signing and Gatekeeper"). The source repo has no binaries, so the listing needs a custom download URL pointing at the release zip (or a binaries branch). Check whether Godot's newer Asset Store or the classic Asset Library is current, and their rules, at that time. The README needs a macOS note for browser-downloaded zips (`xattr -dr com.apple.quarantine addons/godot_git`).
6. Later: a performance pass on `refresh()` for big repos.
7. ~~Name for the dock tab.~~ **Decided 2026-09-24: it stays "Git".** The maintainer tried "Version Control" and found it too long for a small tab (it also clashes with Godot's own Version Control panel). The planned main-screen tab (diff/history) still needs its own name, not "Git" too; candidates "Review" / "Diff". The maintainer finds pun names pointless. Keep `set_layout_key("GodotGit")` unchanged if anything is ever renamed, or users lose their saved dock position.

### Next up (agreed 2026-09-24, in this order)

1. ~~Git LFS.~~ **Done** (2026-09-24), see "Git LFS" under Architecture. Still open: a real LFS server with a sign-in, and a project with thousands of LFS files (speed).
2. ~~Commit hooks and signing.~~ **Done** (2026-09-24), see "Hooks and commit signing" under Architecture. GPG signing with a real key and pinentry is untested (the tests sign with SSH keys).
3. ~~SSH fixes.~~ **Done** (2026-09-24), see "SSH remotes" under Architecture.
4. ~~New action layout plus Amend.~~ **Done** (2026-09-24), pulled forward so the maintainer can use it while the rest is built. Checked on screen at normal and narrowest (~190 px) dock widths.

### Big features (in order)

**1. Diff viewer: a "Git" main-screen tab.** Extensions can add a main-screen tab beside 2D / 3D / Script / Game / AssetLib (`EditorPlugin::_has_main_screen`, `_make_visible`, `_get_plugin_name`, `_get_plugin_icon`). That's the home for reviewing:
- **Split of roles.** The right dock stays the place for daily work (stage, commit, pull, push, status). The main tab is for reviewing (diffs now, history next, conflicts later). Selecting a file in the dock opens the Git tab on its diff; double-click still opens the file for editing.
- **Why not elsewhere.** The bottom panel is too short for code, and a floating window gets lost behind the editor. The main area has the full width that side-by-side needs. The cost: opening the tab switches you away from your scene/script, which is fine for a review screen (one click back).
- **Rendering.** A read-only `CodeEdit` with the editor's code font; added/removed lines get faint success/error-colored backgrounds (`set_line_background_color`); old and new line numbers in gutters; syntax highlighting where available. Unified and side-by-side views, side-by-side with synced scrolling. The file list on the left (same rows as the dock), the diff on the right.
- **Godot-specific files.**
  - `.tscn`/`.tres` are text but noisy (ids, uids): plain diffs first, a readable "what changed in this scene" later, maybe.
  - Images get a before/after view instead of "binary file".
  - Huge files show a notice instead of freezing.
- **Scope.** Read-only first. Staging single hunks or lines from the diff comes after, and only once it works completely (philosophy point 1).
- **Backend.** A `get_diff(path, staged)` returning hunks and lines with old/new line numbers; libgit2 already builds these patches for the line stats.

**2. History viewer**, in the same main tab. The commit list; pick a commit to see its full message, author, date, its files, and their diffs (reusing the diff view). Start without a branch graph; a graph is a separate, harder piece.

**3. Conflict resolution.** Let a pull stop at conflicts instead of refusing. List conflicted files with "keep mine / take theirs / open in editor", show them in the diff view, and finish the merge when all are resolved (or abort cleanly). This removes the last "use git in a terminal" message.

**4. Stash and branch management.** A stash list with restore/drop, and branch delete/rename. Tags fit here too.

## Where we left off (2026-09-24)

A long session: RAII cleanup, sign-in from the panel, Git LFS, the new action layout with Amend, hooks and signing, all-or-nothing checkouts, SSH through git, and a CI race fix. All of "Next up" is done. The notes below are for picking it up again.

**Before anything else**
- **Push what's uncommitted** (SSH through git, Linux `--gc-sections`, docs), then check CI:
  - all five test jobs green; `test_ssh.gd` has only run on Windows so far, and its fake ssh is a `sh` script;
  - the Linux `.so` size: expected about 2 MB instead of 4.2 MB (inferred from the macOS numbers, not measured);
  - signing tested on all five (it was on the last run).
- **The playground** (`MyGame`, built by `make_playground.py` in the session scratchpad) is in the broken half-switched state from the checkout bug, and runs an old build. The scratchpad may be gone next time. Rebuilding a playground like it is quick, and worth keeping as `tools/make_playground.py` if UI testing becomes a habit: a teammate remote, LFS art, a feature branch, staged/unstaged/unpushed work.

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
- **Process-wide environment variables** (`GIT_TERMINAL_PROMPT`, `GIT_LFS_FORCE_PROGRESS`) are set on the editor process and so leak into games started from the editor. Harmless as far as known; passing them per command would be cleaner.
- **Hooks not run**: `pre-merge-commit` (we commit merges with `git commit`), and `post-checkout`/`post-merge` after libgit2 checkouts. git-lfs installs `post-checkout`/`post-merge` for file locking, which the panel doesn't support either.
- **LFS limits**: only the root `.gitattributes` is checked, each file is held in memory while filtered, and there's no LFS locking.
- **The test suite keeps growing** (296 checks, a few minutes on Windows). Fine for now; split slow suites if it starts to hurt.
- **Asset Library**: check whether the official plugin is listed there for 4.x before writing our description.

**Next**: `v0.1.0` (check the branch-switch dialog on Windows, then tag), the Asset Library listing, then the diff viewer (Big features 1).

## Advice

General advice for working on this codebase:
- **Verify engine behavior in the engine source** (and for 4.7, in the `4.7.2-stable` tag) before guessing. Most UI bugs here came from Tree internals that only the source explains.
- **Look at the UI** after every visual change. Don't trust that it looks right.
- **Keep the UI honest.** If something can't be done in a state, hide it or disable it with a tooltip that says why. Never show a button that promises more than it does.
- **Test the paths real users hit.** Private repos, logins, "all" variants of actions, empty states, a dirty `project.godot`. Every bug so far lived in one of those.
- **When the maintainer reports a problem, reproduce it before explaining it.** Say clearly what's proven and what's inferred.

My own take on the status work: the strip's job is to always answer "what is the panel doing, and what did it last do?" Guard that when adding features. Every new operation that can fail or take time should report through `_set_status`, never through a toast alone.
