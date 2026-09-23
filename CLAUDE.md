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
- ⟳ Fetch, and a ⋮ menu (stage/unstage/discard all, new branch, refresh, open repo folder).
- Commit message box (Ctrl+Enter commits) with **Commit**, **↓ Pull N** and **↑ Push N / Publish** buttons in one row.
- "Staged Changes" and "Changes" sections (Godot `FoldableContainer`s) with file counts and **+/− line totals** in the header, discard-all / stage-all / unstage-all header buttons, and per-file hover buttons (stage / unstage / discard). Both sections always show; when empty they show a plain dim label ("Nothing staged." / "No changes.").
- Right-click menus on files (open, stage/unstage, discard, show in FileSystem / file manager, copy paths) and commits (copy hash / message).
- History of the last 50 commits; commits not yet on any remote are highlighted with the accent color.
- Double-click opens a file: in Godot if it's a scene/script/resource, otherwise in the external editor from Godot's settings, or VS Code.
- Pull fast-forwards or creates a merge commit; uncommitted changes to other files stay put. It's refused up front (naming the files) if the new commits touch files you have uncommitted changes to, and conflicting merges are refused and fully undone. It never leaves anything in a stash.
- Auto-fetch every few minutes, quietly, never opening a sign-in window (toggle in the ⋮ menu).

Supported targets: Windows x86_64/arm64, Linux x86_64/arm64, macOS universal. All five build in CI. The backend test suite (including HTTPS fetches from GitHub) passes on Windows x86_64, Linux x86_64/arm64 and macOS in CI (first confirmed 2026-09-23). The dock UI has only been seen on **Windows x86_64**. (Windows arm64 has a test job since the next push; unconfirmed until it runs.) It's in real use on the maintainer's StorageWars project, a private GitHub repo over HTTPS.

Minimum OS versions of the built libraries (check with `pyelftools`/`macholib` on a CI zip):
- **macOS 10.13 (Intel) / 11.0 (Apple Silicon)**, matching Godot 4.7. Set via `macos_deployment_target` in `SConstruct` and passed to libgit2's CMake. Without it the library requires the CI runner's macOS version (the first CI build required macOS 26).
- **Linux: glibc 2.34+** (Ubuntu 22.04+, Debian 12+, Fedora 35+), because CI builds on Ubuntu 22.04. Older distros would need a build in an older container. Runtime deps are only libc/libm/libstdc++; OpenSSL is loaded at runtime.

---

## Repository layout

| Path | What |
|---|---|
| `src/git/` | **Backend**, no editor dependencies. All git logic lives here. |
| `src/git/git_repository.h` | `GitRepository` (RefCounted, exposed to GDScript): the libgit2 wrapper's whole API. |
| `src/git/git_repository.cpp` | Opening, reading state (status, line stats, branches, history), local changes (stage, discard, commit, checkout). |
| `src/git/git_repository_remote.cpp` | Fetch, pull, push. `pull()` is split into `paths_blocking_pull` / `fast_forward` / `merge_and_commit` / `merge_with_autostash`. |
| `src/git/git_remote_callbacks.{h,cpp}` | libgit2 remote callbacks: logins via `git credential fill`, progress reporting (`RemoteContext`, `report_progress`), cancel. |
| `src/git/git_util.{h,cpp}` | Small libgit2 helpers shared by the above (`fail`, `to_error`, `head_branch`, `changed_paths`, `uncommitted_paths`, ...). |
| `src/editor/` | **Editor UI.** |
| `src/editor/git_dock.h` | `GitDock` (EditorDock). Its private methods are grouped by the file that implements them. |
| `src/editor/git_dock.cpp` | Building the dock, `refresh()`, toolbar and action row, local actions (stage, discard, commit, branches). |
| `src/editor/git_dock_lists.cpp` | Staged Changes / Changes / History: row drawing, header alignment, hover buttons, clicks, context menu. |
| `src/editor/git_dock_status.cpp` | The status strip. |
| `src/editor/git_dock_network.cpp` | Fetch / pull / push on a worker thread, auto-fetch. |
| `src/editor/file_opener.{h,cpp}` | `open_file()`: in Godot if it can edit the file, else the configured external editor or VS Code. |
| `src/editor/ui_text.{h,cpp}` | Wording helpers: `plural`, `time_ago`, `status_letter`, ... |
| `src/editor/git_editor_plugin.{h,cpp}` | `GitEditorPlugin`: adds/removes the dock. |
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
- **Which repository.** The dock always opens the repository containing `res://`. The maintainer's estimate: ~80% of the time the Godot project sits at the root of its git repo, so this is a safe default. Being able to pick a different repository (or find one elsewhere) is useful but not paramount. See Missing features.
- Errors: methods return Godot `Error`. The human-readable reason is `GitRepository::get_last_error()` (libgit2's thread-local last error). Custom messages are set with the file-local `fail()` helper (`git_error_set_str`). `get_notice()` carries a warning from an operation that *succeeded* (only the pull's stash safety net, which shouldn't trigger anymore). `get_pull_result()` says how many commits the last pull brought in and whether it merged. A canceled network op returns `ERR_SKIP`.
- Network (fetch / pull / push) uses `RemoteContext` callbacks:
  - **HTTPS credentials** come from git's own credential helper by running `git credential fill` (with `GIT_TERMINAL_PROMPT=0`). On this machine that's Git Credential Manager. If git isn't installed, HTTPS auth can't work.
  - **SSH** uses libgit2's `USE_SSH=exec` backend, i.e. the system `ssh` client (keys, agent, `~/.ssh/config`).
  - The credential callback refuses a second attempt, so a rejected login fails instead of looping.
  - `set_login_prompts_allowed(false)` (used for background fetches) runs `git -c credential.interactive=never credential fill`: Git Credential Manager then only uses saved logins. Other common helpers (osxkeychain, libsecret, store) never prompt anyway. A configured `askpass` program could still pop up; not handled.
- `pull()`:
  1. fetches the upstream's remote;
  2. **refuses up front** if the incoming commits (merge base → theirs) touch any path with uncommitted changes (staged, unstaged, or an untracked file the pull would add). The message names the files. Nothing is touched, and no stash is made;
  3. up to date → nothing; fast-forward → safe checkout + move the ref;
  4. diverged → autostash the (by now guaranteed unrelated) tracked changes, `git_merge`, then either create the merge commit or, on conflicts, `git_reset --hard` + `state_cleanup` (nothing changes), and restore the stash. It applies cleanly because of step 2; the old "keep it in the stash if files overlap" logic remains only as a safety net.
- `push()` pushes to the upstream, or **publishes** (pushes to `origin`, or the only remote, under the same name, then sets upstream) when there is none. Non-fast-forward (`GIT_ENONFASTFORWARD`, or a server "fetch first" rejection) is reported as "pull first".
- `get_line_stats(staged)` diffs HEAD↔index or index↔workdir (untracked content included) and returns `{path: Vector2i(added, removed)}`; binary or >2 MB files are `(-1,-1)`.

### GitDock (UI)

- Layout, top to bottom: toolbar (branch `OptionButton`, fetch, ⋮ `MenuButton`) → commit `TextEdit` → action row (Commit, Pull, Push) → `ScrollContainer` holding three `FoldableContainer`s (Staged Changes, Changes, History).
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
- Network ops run on a `Thread` (`_network_worker`) and report back with `call_deferred` to `_network_done`. `_finish_network_thread` joins on `EXIT_TREE`. Commit, the branch picker and the ⋮ menu are disabled while a pull or push runs; the worker is rewriting the repo.
- **Auto-fetch** (`_on_auto_fetch_timer`): checks 10 s after opening, then every minute, and fetches when `last_fetched` (FETCH_HEAD's age, so CLI fetches count) is 5+ minutes old. After a failed attempt it also waits 5 minutes. Per-project setting in the ⋮ menu (`EditorSettings` project metadata `godot_git/auto_fetch`, default on). It runs **quietly** (`network_quiet`): no strip, buttons stay enabled, and no sign-in windows (`set_login_prompts_allowed(false)`). Pressing Fetch during it just shows it. Pressing Pull/Push queues the op (`queued_op`), shown as busy with "Waiting for a background fetch...". A failure shows one warning, cleared by the next success. `_shown_network_op()` is what the buttons reflect.
- **Status strip** (`_set_status` / `_update_status` / `_update_status_style`), under the toolbar. Every result and error goes here. It's one wrapping `RichTextLabel` (never trimmed; selectable so errors can be copied), an icon, a Cancel/Dismiss button, and a thin progress bar while busy. Kinds: idle ("Last fetched 3h ago", from `last_fetched` in the sync status), busy, success ("· just now", kept current by a 30 s timer), neutral (canceled), warning and error (tinted background, stay until dismissed or replaced). Results never time out. Toasts are only used when the dock is hidden behind another tab, so an outcome is never missed.
- Progress: the worker's `GitRepository` gets `set_progress_callback(callable_mp(dock, &_network_progress))`. The backend throttles to ~10 updates/s and uses `call_deferred`, so the dock is only touched on the main thread. Cancel calls the static `GitRepository::cancel_network()`, which sets an atomic flag the libgit2 callbacks check (returning `GIT_EUSER`) and kills a pending `git credential fill`. The op then fails with `ERR_SKIP`.

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
  - Libgit2 cleanup is manual (`git_*_free`): prefer small functions with one exit path, or early returns that free what they took, over one long function. That's what made the old `pull()` hard to follow.
- **Formatting**: run clang-format (the repo's `.clang-format`, from godot-cpp) on changed C++ files. On this machine it's at `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-format.exe`.
- **Commits**: the maintainer commits and pushes themselves. Don't commit, create repos, or push unless explicitly asked. Leaving work staged has been the norm.

---

## Testing

1. **Backend tests: `project/tests/`, in the repo and in CI.** `run_tests.gd` (a `SceneTree` script) runs the suites `test_local.gd`, `test_sync.gd`, `test_pull_safety.gd` and `test_online.gd` (the last only with `-- --online`). Each suite extends `test_case.gd`, which builds throwaway repos with the real git CLI (a bare "remote" plus "mine" and "theirs" clones via `make_shared()`). Checks are made against what git itself says. Run:
   `godot --headless --path project -s res://tests/run_tests.gd [-- --online] [-- <suite>]`. Exit code 1 on failure; scratch repos go to the OS temp folder and are kept (path printed) when something fails. CI runs them on Windows, Linux x86_64/arm64 and macOS against each freshly built library. It first opens the project with `-e --quit-after 300` so the extension gets registered; see gotcha about `--import`. **Every bug from real use gets a test here.** The suite has already caught one on its first run: the push "pull first" message never showed for the common unfetched case.
2. **UI tests in a real (headless) editor.** Not in the repo yet; these run from the session scratchpad. Copy the addon into a throwaway project and add a test-only `EditorPlugin` (`addons/ui_driver/`) enabled in `project.godot` that finds dock controls (`find_children` on `EditorInterface.get_base_control()`) and presses them (`button.pressed.emit()`, `popup.id_pressed.emit(id)`), then prints results. Run with `--headless -e --path <project> --quit-after <frames>` and **redirect stdout**. Gotchas:
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
12. The libgit2 CMake options live in `tools/libgit2.py`. HTTPS backend per platform: WinHTTP (Windows), SecureTransport (macOS), OpenSSL loaded at runtime (Linux, so no hard libssl dependency). zlib, regex and the http parser are bundled.

---

## Design decisions (and why)

- **Own dock rather than `EditorVCSInterface`.** Godot's built-in VCS panels are fixed and limited (no real history or branch UI). The official godot-git-plugin already fills that role, so an own dock is the only way to make something better.
- **Editor-only, shipped as `addons/godot_git/`.** It's a dev tool and must not end up in games. The standard addon layout means installing is "unzip into your project".
- **Honesty over cleverness.** Only show actions that can actually be performed. Pull and Push disappear without a remote, Push is disabled with nothing to send, and tooltips say exactly what a button will do. We briefly had one context-sensitive "do the next thing" button; the maintainer preferred **separate Commit / Pull / Push buttons**, labeled with counts ("↓ Pull 2", "↑ Push 1").
- **Native look.** `FoldableContainer` sections, editor theme icons/colors, Tree button styles for header buttons, Title Case. Avoid inventing styles.
- **Calm lists.** Status letter on the left in a fixed column, neutral file names, dimmed folder after the name, action buttons only on hover, line counts only as section totals. Each of these came from an earlier version looking cluttered.
- **Opening files never surprises.** Godot opens what Godot can (scenes, scripts, resources, which respect Godot's own "use external editor" setting). Everything else goes to the external editor configured in Godot, else VS Code, else a toast explaining how to set one. We never hand files to random OS apps or the file manager.
- **Pull safety.** A pull either completes with your uncommitted work exactly where it was, or refuses and changes nothing. Unrelated uncommitted edits are carried through a merge (internally with a stash that's always restored), which makes pull usable in real Godot projects where the editor constantly rewrites `project.godot` and scenes. Edits to files the pull touches make it refuse up front. A conflicting merge is always fully undone; the panel doesn't resolve conflicts yet. `test_pull_safety.gd` covers all of this.
- **Network on a worker thread** so the editor never freezes on slow remotes or credential prompts.
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

- **The dock on Linux and macOS, and anything on Windows ARM.** CI proves the Linux (x86_64, arm64) and macOS libraries load in the official Godot 4.7.2 and pass the backend suite, HTTPS included. Nobody has looked at the dock UI there (fonts, scaling, file manager and VS Code paths). Windows arm64 got a test job after the first CI run; check it passed. macOS builds aren't signed; loading worked in CI (downloaded with curl, so no quarantine flag), but a browser-downloaded zip will likely hit Gatekeeper.
- **Push over HTTPS/SSH to a real server.** Authenticated HTTPS *fetch/pull* from a private GitHub repo works (verified on StorageWars through Git Credential Manager). Push uses the same credential path but hasn't been exercised. SSH is untested.
- **Light editor theme** was never looked at. Everything uses theme colors, so it should be fine.
- **RTL layouts**: `_draw_file_row` assumes left-to-right.

### Missing features

Roughly in order of value, after status and feedback. Per philosophy point 1, don't expose any of these half-done; a feature appears in the UI when it fully works.
- **Diff viewer.** The biggest gap. Selecting a file should show its diff, probably in a bottom-panel dock or a split. libgit2 patches are already computed for line stats.
- **Conflict resolution.** Today conflicting pulls are refused. A minimal version: let the merge happen, list conflicted files with "take mine / take theirs / open in editor", and commit when resolved.
- Amend last commit, stash UI, branch delete/rename, tags, blame, per-hunk staging.
- **Choosing the repository.** Today it's always the one containing the project (searching up from `res://`), which covers the common case: a project at its repo's root (~80% per the maintainer), and a project in a subfolder of a repo. Not covered:
  - a project that isn't in a repo at all (the dock just says so: offer "Initialize repository" or "Choose folder...");
  - a project whose repo is somewhere unexpected;
  - wanting a nested repo or submodule instead.

  A per-project override (stored in `EditorSettings` project metadata) plus a picker would do. Nice to have, not a priority.
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

Worth remembering, because each came from a path the tests didn't cover:
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
4. **Run the Mac and Linux builds on real machines, then tag `v0.1.0`.** CI now runs the backend tests on those platforms (a real check that the libraries load and work), but the dock UI still hasn't been seen on them. A stable base to come back to.
5. Later: a performance pass on `refresh()` for big repos, and an Asset Library listing once macOS signing is sorted (needs an Apple Developer account; otherwise Gatekeeper blocks the library).

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

**4. Stash and branch management.** A stash list with restore/drop, and branch delete/rename. Amend last commit and tags fit here too.

## Advice

General advice for working on this codebase:
- **Verify engine behavior in the engine source** (and for 4.7, in the `4.7.2-stable` tag) before guessing. Most UI bugs here came from Tree internals that only the source explains.
- **Look at the UI** after every visual change. Don't trust that it looks right.
- **Keep the UI honest.** If something can't be done in a state, hide it or disable it with a tooltip that says why. Never show a button that promises more than it does.
- **Test the paths real users hit.** Private repos, logins, "all" variants of actions, empty states, a dirty `project.godot`. Every bug so far lived in one of those.
- **When the maintainer reports a problem, reproduce it before explaining it.** Say clearly what's proven and what's inferred.

My own take on the status work: the strip's job is to always answer "what is the panel doing, and what did it last do?" Guard that when adding features. Every new operation that can fail or take time should report through `_set_status`, never through a toast alone.
