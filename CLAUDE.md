# CLAUDE.md

Guidance for Claude (and humans) working on **godot-git**: a Git panel for the Godot 4.7 editor, written as a C++ GDExtension on top of [godot-cpp](https://github.com/godotengine/godot-cpp) and [libgit2](https://libgit2.org/).

Repository: https://github.com/mssbabou/Godot-Git (public, default branch `master`).

The first half is reference (what, where, how to build and test, conventions). The second half is the hard-won stuff: engine and library gotchas, design decisions with their reasons, known gaps, and where I'd take it next.

---

## What it is

An editor-only plugin that adds a **Git** dock (default slot: right side, next to the Inspector). It is *not* an `EditorVCSInterface` backend for Godot's built-in VCS panels; it draws its own UI.

What the dock does today:

- Branch picker: local branches, remote-tracking branches (picking `origin/x` creates and checks out a tracking branch), "New Branch…".
- ⟳ Fetch, and a ⋮ menu (stage/unstage/discard all, new branch, refresh, open repo folder).
- Commit message box (Ctrl+Enter commits) with **Commit**, **↓ Pull N** and **↑ Push N / Publish** buttons in one row.
- "Staged Changes" and "Changes" sections (Godot `FoldableContainer`s) with file counts and **+/− line totals** in the header, discard-all / stage-all / unstage-all header buttons, and per-file hover buttons (stage / unstage / discard).
- Right-click menus on files (open, stage/unstage, discard, show in FileSystem / file manager, copy paths) and commits (copy hash / message).
- History of the last 50 commits; commits not yet on any remote are highlighted with the accent color.
- Double-click opens a file: in Godot if it's a scene/script/resource, otherwise in the external editor from Godot's settings, or VS Code.
- Pull fast-forwards or creates a merge commit, with autostash. Conflicting merges are refused and fully undone.

Supported targets: Windows x86_64/arm64, Linux x86_64/arm64, macOS universal. Only **Windows x86_64** has actually been built and run; the rest are set up in CI but unproven (see "Known gaps").

---

## Repository layout

| Path | What |
|---|---|
| `src/git_repository.{h,cpp}` | `GitRepository` (RefCounted, exposed to GDScript): thin libgit2 wrapper. All git logic lives here. |
| `src/git_dock.{h,cpp}` | `GitDock` (EditorDock): the whole UI. Largest file. |
| `src/git_editor_plugin.{h,cpp}` | `GitEditorPlugin`: adds/removes the dock. |
| `src/register_types.cpp` | Extension entry `godot_git_library_init`. SCENE level: `git_libgit2_init()` + `GitRepository`. EDITOR level: `GitDock`/`GitEditorPlugin` (internal classes) + `EditorPlugins::add_by_type`. |
| `project/` | Dev/test Godot project. Has no main scene on purpose. |
| `project/addons/godot_git/` | **The shipped addon.** `godot_git.gdextension` + `bin/<platform>/` (build output, git-ignored). |
| `thirdparty/godot-cpp` | Submodule, pinned to `10.0.0-stable`. Targets `api_version=4.7`. |
| `thirdparty/libgit2` | Submodule, pinned to `v1.9.7`. |
| `tools/libgit2.py` | Configures + builds libgit2 as a static lib with CMake, per platform, and links it into the SCons env. |
| `tools/package.py` | Zips the addon (+ LICENSE + third-party licenses) into `dist/godot_git-<version>.zip`. `--require-all` fails if any platform library is missing. |
| `SConstruct` | Build entry. Default target `editor`, hot reload on, `SCONS_CACHE` support, `package` alias. |
| `.github/workflows/build.yml` | CI: builds 5 targets in parallel, packages one zip, attaches it to published releases. |

Build by-products that land in the tree (all git-ignored): `src/*.obj`, `src/gen/`, `build/libgit2/<platform>.<arch>/`, `dist/`, `project/.godot/`, `project/addons/godot_git/bin/`.

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
- Errors: methods return Godot `Error`. The human-readable reason is `GitRepository::get_last_error()` (libgit2's thread-local last error). Custom messages are set with the file-local `fail()` helper (`git_error_set_str`). `get_notice()` carries a warning from an operation that *succeeded* (currently only "pulled, but your edits stayed in a stash").
- Network (fetch / pull / push) uses `RemoteContext` callbacks:
  - **HTTPS credentials** come from git's own credential helper by running `git credential fill` (with `GIT_TERMINAL_PROMPT=0`). On this machine that's Git Credential Manager. If git isn't installed, HTTPS auth can't work.
  - **SSH** uses libgit2's `USE_SSH=exec` backend, i.e. the system `ssh` client (keys, agent, `~/.ssh/config`).
  - The credential callback refuses a second attempt, so a rejected login fails instead of looping.
- `pull()`:
  1. fetches the upstream's remote;
  2. up to date → nothing; fast-forward → safe checkout + move the ref;
  3. diverged → **autostash** tracked changes (`git_stash_save`), `git_merge`, then either create the merge commit or, on conflicts, `git_reset --hard` + `state_cleanup` (nothing changes);
  4. restore the stash, but **only if** the pull didn't touch any file in the stash (see libgit2 gotchas). Otherwise the stash is left alone and `notice` explains which files.
- `push()` pushes to the upstream, or **publishes** (pushes to `origin`, or the only remote, under the same name, then sets upstream) when there is none. Non-fast-forward is reported as "pull first".
- `get_line_stats(staged)` diffs HEAD↔index or index↔workdir (untracked content included) and returns `{path: Vector2i(added, removed)}`; binary or >2 MB files are `(-1,-1)`.

### GitDock (UI)

- Layout, top to bottom: toolbar (branch `OptionButton`, fetch, ⋮ `MenuButton`) → commit `TextEdit` → action row (Commit, Pull, Push) → `ScrollContainer` holding three `FoldableContainer`s (Staged Changes, Changes, History).
- The file trees have **scrolling disabled** so they report their full height, and the outer `ScrollContainer` scrolls everything as one list. Sections are exactly as tall as their content.
- File rows are **one cell** (`CELL_MODE_CUSTOM`). `_draw_file_row` paints the status letter, icon, name and dimmed folder. The cell's text is set to the file name with a transparent color, so row height, type-to-search and accessibility still work.
- Hover buttons: `_set_hovered` adds stage/unstage/discard buttons to the row under the mouse and clears them from the previous one (tracked by instance id, reset on every refresh).
- Header "all" buttons use the Tree's own button styleboxes and spacing, and `_align_header_buttons` shifts them (via a `MarginContainer`'s right margin) so they line up **exactly** with the row buttons at any scale.
- `refresh()` rebuilds everything: sync status, branches, status + line stats for both panes, history, action buttons. It runs on `READY`, `filesystem_changed`, `NOTIFICATION_APPLICATION_FOCUS_IN`, and after every action.
- Network ops run on a `Thread` (`_network_worker`) and report back with `call_deferred` to `_network_done`. `_finish_network_thread` joins on `EXIT_TREE`. Commit is disabled while a pull or push runs; the worker is rewriting the repo.
- User-facing errors and results are `EditorToaster` toasts prefixed `Git:`.

---

## Conventions

- Code style follows godot-cpp/Godot: tabs, `p_` params, `r_` out-params, `_private_method`, `snake_case`. `.clang-format` is copied from godot-cpp.
- UI strings use Godot's Title Case for buttons and menus ("New Branch...", "Discard All Changes..."), full sentences for tooltips and toasts.
- Use editor theme colors/icons (`get_theme_color("success_color", "Editor")`, `get_theme_icon("Reload", "EditorIcons")`); never hardcode colors. Scale pixel sizes by `EditorInterface::get_editor_scale()`.
- Comments explain *why* (non-obvious engine or library behavior), not what.
- **Commits**: the maintainer commits and pushes themselves. Don't commit, create repos, or push unless explicitly asked. Leaving work staged has been the norm.

---

## Testing

There is **no test suite in the repo yet**. Everything so far was verified with throwaway scripts in the session scratchpad. The techniques are worth keeping (and the scripts worth turning into a `tests/` folder):

1. **Backend tests (headless, fast, reliable).** A GDScript extending `SceneTree`, run with
   `Godot.exe --headless --path project -s path/to/test.gd`. It builds throwaway repos with the git CLI (`OS.execute("git", ...)`): a bare "remote", a "mine" clone and a "teammate" clone. Then it drives `GitRepository` and asserts with the git CLI. Covered so far: stage/unstage/discard/commit, fast-forward, clean merge, conflicting merge (refused, HEAD unchanged, no `MERGE_HEAD`), push rejected when behind, publish, remote-branch checkout, line stats, real HTTPS fetch from GitHub, and autostash (unrelated edits restored, overlapping edits kept in the stash, staged edits stay staged).
2. **UI tests in a real (headless) editor.** Copy the addon into a throwaway project and add a test-only `EditorPlugin` (`addons/ui_driver/`) enabled in `project.godot` that finds dock controls (`find_children` on `EditorInterface.get_base_control()`) and presses them (`button.pressed.emit()`, `popup.id_pressed.emit(id)`), then prints results. Run with `--headless -e --path <project> --quit-after <frames>` and **redirect stdout**. Gotchas:
   - PowerShell 5's `Set-Content -Encoding utf8` writes a **BOM**, and Godot then silently ignores `plugin.cfg`. Write files with `[IO.File]::WriteAllText(path, text, (New-Object Text.UTF8Encoding $false))`.
   - `--log-file` did **not** capture plugin `print()` output. Redirect stdout instead.
   - `--quit-after` counts frames, not seconds. Give network tests a generous number (e.g. 6000).
   - Tree **button clicks can't be injected**: Tree re-checks the real OS cursor before accepting a button press. Emit `tree.button_clicked` directly to test the handler.
   - Injected hover works headless but gets overridden in a GUI editor by the real cursor.
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
11. **Editor APIs we rely on (all present in 4.7):** `EditorDock` (`set_default_slot`, `set_layout_key`, `set_icon_name`), `EditorPlugin::add_dock`, `FoldableContainer::add_title_bar_control`, `Tree::get_custom_drawing_canvas_item`, `EditorToaster::push_toast`.
12. **Not available to extensions:** `TextFile`, `ScriptEditor::open_file`. There is no way to open `.md`/`.txt`/`.cpp` in Godot's script editor from a plugin, so those go to the external editor.
13. **`include_tags` in `.gdextension`** (keeps an editor-only extension out of exports) exists only from **4.8**. On 4.7, users must exclude `addons/godot_git/*` in their export presets, or exports warn and the game logs one harmless error. The key is already in our `.gdextension`; 4.7 ignores it.
14. `EditorInterface` singletons aren't ready in constructors. Do editor-dependent setup in `NOTIFICATION_READY` / `THEME_CHANGED`.

## libgit2 gotchas

1. **`git_error_last()` is never null** since 1.8. "No error" has `klass == GIT_ERROR_NONE`, and `get_last_error()` filters that out.
2. **`git_stash_pop` writes conflict markers into files and then drops the stash** when applying conflicts. The git CLI keeps the stash instead. We never pop a stash whose files overlap the pull.
3. Errors are **thread-local**: read `get_last_error()` on the thread that failed (the network worker does this before `call_deferred`).
4. libgit2 **honors `core.autocrlf`** (`true` on this machine), so checked-out files get CRLF. Compare file contents with `\r` stripped in tests.
5. `git_merge` writes merge state (`MERGE_HEAD`). Always `git_repository_state_cleanup` on every path, including failures.
6. Custom error text: `git_error_set_str` lives in `git2/sys/errors.h`.
7. The libgit2 CMake options live in `tools/libgit2.py`. HTTPS backend per platform: WinHTTP (Windows), SecureTransport (macOS), OpenSSL loaded at runtime (Linux, so no hard libssl dependency). zlib, regex and the http parser are bundled.

---

## Design decisions (and why)

- **Own dock rather than `EditorVCSInterface`.** Godot's built-in VCS panels are fixed and limited (no real history or branch UI). The official godot-git-plugin already fills that role, so an own dock is the only way to make something better.
- **Editor-only, shipped as `addons/godot_git/`.** It's a dev tool and must not end up in games. The standard addon layout means installing is "unzip into your project".
- **Honesty over cleverness.** Only show actions that can actually be performed. Pull and Push disappear without a remote, Push is disabled with nothing to send, and tooltips say exactly what a button will do. We briefly had one context-sensitive "do the next thing" button; the maintainer preferred **separate Commit / Pull / Push buttons**, labeled with counts ("↓ Pull 2", "↑ Push 1").
- **Native look.** `FoldableContainer` sections, editor theme icons/colors, Tree button styles for header buttons, Title Case. Avoid inventing styles.
- **Calm lists.** Status letter on the left in a fixed column, neutral file names, dimmed folder after the name, action buttons only on hover, line counts only as section totals. Each of these came from an earlier version looking cluttered.
- **Opening files never surprises.** Godot opens what Godot can (scenes, scripts, resources, which respect Godot's own "use external editor" setting). Everything else goes to the external editor configured in Godot, else VS Code, else a toast explaining how to set one. We never hand files to random OS apps or the file manager.
- **Pull safety.** Autostash makes pull usable in real Godot projects (the editor constantly rewrites `project.godot` and scenes). A conflicting merge is always fully undone; the panel doesn't resolve conflicts yet.
- **Network on a worker thread** so the editor never freezes on slow remotes or credential prompts.
- **Name**: `godot-git`. The maintainer considers pun names pointless. The official plugin is "Godot Git Plugin", so an Asset Library listing should explain the difference in its description rather than through the name.

---

## Known gaps and risks

Unproven or untested:
- **CI has never run** (the first push went to `master` while the workflow still listened on `main`). Linux, macOS and Windows ARM builds have never been compiled. Expect fixes in `tools/libgit2.py`, e.g. link libraries or macOS deployment target mismatches, on the first run.
- **Push over HTTPS/SSH to a real server** is untested. HTTPS *fetch* from GitHub works. The credential-helper path has only been reasoned about, not exercised against a login.
- **Light editor theme** was never looked at. Everything uses theme colors, so it should be fine.
- **RTL layouts**: `_draw_file_row` assumes left-to-right.

Missing features (roughly in order of value):
- **Diff viewer.** The biggest gap. Selecting a file should show its diff, probably in a bottom-panel dock or a split. libgit2 patches are already computed for line stats.
- **Conflict resolution.** Today conflicting pulls are refused. A minimal version: let the merge happen, list conflicted files with "take mine / take theirs / open in editor", and commit when resolved.
- **Auto-fetch** on an interval. The ↓ count is only as fresh as the last fetch.
- Amend last commit, stash UI, branch delete/rename, tags, blame, per-hunk staging.
- **Tests in the repo** (see Testing), run in CI with a headless Godot.
- Localization: strings are hardcoded English (Godot uses `TTR`; extensions have no equivalent wired up here).

Performance considerations:
- `refresh()` does full status + two diffs (line stats) + a 50-commit revwalk + the unpushed walk (capped at 1000) on **every** filesystem change and focus-in. Fine for normal projects; big repos or huge changesets will feel it. Candidates: debounce refreshes, compute line stats lazily or on a thread, skip stats above N files.
- `get_line_stats` generates a patch per changed file. Files over 2 MB are treated as binary on purpose.

Behavior worth knowing:
- Godot itself creates `.uid` files and rewrites `project.godot`, so a fresh project always shows changes. That's correct, but it surprises people.
- Discard on an untracked file **deletes it** (with a confirmation that says so).
- If a pull changes a file you have uncommitted edits to, your edits stay in `git stash` and the toast says so. The panel has no stash UI yet, so the user needs a terminal (`git stash pop`).
- The empty-section placeholder ("No changes.") starts at the letter column, not aligned with file names.

---

## Where I'd take it next

1. **Get CI green.** The repo is pushed; CI triggers on pushes to `master` (it originally said `main`, so the first push didn't run it). Everything else is guesswork until all five platforms build. Then ship a `v0.1.0` release and try it on at least one Mac and one Linux machine.
2. **Diff view.** It makes the panel useful for *reviewing* changes, not just staging them. Keep it native: a bottom-panel `EditorDock` with a unified/split toggle, reusing editor code fonts and success/error colors.
3. **Move the scratch test scripts into `tests/`** (backend GDScript suite + UI driver) and run them in CI. The backend suite is quick and has caught real bugs (a staged-count bug, the pull/autostash issues).
4. **Conflict handling**, minimal and honest (see above). This is the feature that makes Pull feel safe instead of just refusing.
5. **Performance pass** on `refresh()` once someone uses it on a big project.
6. **Asset Library** listing with screenshots, once macOS signing is sorted (needs an Apple Developer account; otherwise Gatekeeper blocks the library).

General advice for working on this codebase:
- **Verify engine behavior in the engine source** (and for 4.7, in the `4.7.2-stable` tag) before guessing. Most UI bugs here came from Tree internals that only the source explains.
- **Look at the UI** after every visual change. Don't trust that it looks right.
- **Keep the UI honest.** If something can't be done in a state, hide it or disable it with a tooltip that says why. Never show a button that promises more than it does.
