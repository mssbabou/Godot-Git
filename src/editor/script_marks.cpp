#include "editor/script_marks.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_scroll_bar.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/syntax_highlighter.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/templates/local_vector.hpp>

#include "editor/git_colors.h"
#include "editor/git_diff_dock.h"
#include "editor/ui_text.h"

using namespace godot_git;

namespace {

const char *GUTTER_NAME = "godot_git_changes";

float editor_scale() {
	return EditorInterface::get_singleton()->get_editor_scale();
}

Color editor_color(const String &p_name) {
	return EditorInterface::get_singleton()->get_editor_theme()->get_color(p_name, "Editor");
}

} // namespace

// GitScriptMarks

GitScriptMarks::GitScriptMarks() {
	// Worked out a moment after typing stops: a diff per keystroke would be cheap too, but the
	// marks flickering under every letter isn't.
	update_timer = memnew(Timer);
	update_timer->set_one_shot(true);
	update_timer->set_wait_time(0.2);
	update_timer->connect("timeout", callable_mp(this, &GitScriptMarks::_on_update_timer));
	add_child(update_timer);
}

void GitScriptMarks::_bind_methods() {
	ADD_SIGNAL(MethodInfo("show_in_diff_requested", PropertyInfo(Variant::STRING, "path")));
	ADD_SIGNAL(MethodInfo("history_requested", PropertyInfo(Variant::STRING, "path")));
	ADD_SIGNAL(MethodInfo("line_commit_requested", PropertyInfo(Variant::STRING, "path"), PropertyInfo(Variant::STRING, "text"), PropertyInfo(Variant::INT, "line")));
	// For the smoke test.
	ClassDB::bind_method(D_METHOD("get_hunks", "code_edit"), &GitScriptMarks::get_hunks);
	ClassDB::bind_method(D_METHOD("get_hunk_at", "code_edit", "line"), &GitScriptMarks::get_hunk_at);
	ClassDB::bind_method(D_METHOD("show_preview", "code_edit", "hunk"), &GitScriptMarks::show_preview);
	ClassDB::bind_method(D_METHOD("revert", "code_edit", "hunk"), &GitScriptMarks::revert);
	ClassDB::bind_method(D_METHOD("show_in_diff", "code_edit"), &GitScriptMarks::show_in_diff);
}

void GitScriptMarks::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
			if (script_editor) {
				// Opening, switching and closing tabs. Closing is reported before the tab goes.
				script_editor->connect("editor_script_changed", callable_mp(this, &GitScriptMarks::sync).unbind(1));
				script_editor->connect("script_close", callable_mp(this, &GitScriptMarks::sync).unbind(1), CONNECT_DEFERRED);
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_uninstall_all();
		} break;
		default:
			break;
	}
}

void GitScriptMarks::set_repository(const Ref<GitRepository> &p_repo, const String &p_head) {
	if (p_repo != repo) {
		_uninstall_all(); // Another repository: every tab starts over.
		repo = p_repo;
	}
	head = p_head;
	sync();
}

void GitScriptMarks::set_enabled(bool p_enabled) {
	if (p_enabled == enabled) {
		return;
	}
	enabled = p_enabled;
	if (!enabled) {
		_uninstall_all();
	}
	sync();
}

String GitScriptMarks::_repo_path(const String &p_res_path) const {
	const String workdir = repo->get_workdir().simplify_path().trim_suffix("/") + "/";
	const String absolute = ProjectSettings::get_singleton()->globalize_path(p_res_path).simplify_path();
	return absolute.begins_with(workdir) ? absolute.substr(workdir.length()) : String();
}

CodeEdit *GitScriptMarks::_code_edit(uint64_t p_id) const {
	return Object::cast_to<CodeEdit>(ObjectDB::get_instance(ObjectID(p_id)));
}

int GitScriptMarks::_gutter(CodeEdit *p_code_edit) const {
	for (int i = 0; i < p_code_edit->get_gutter_count(); i++) {
		if (p_code_edit->get_gutter_name(i) == GUTTER_NAME) {
			return i;
		}
	}
	return -1;
}

void GitScriptMarks::sync() {
	ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
	if (!script_editor) {
		return;
	}
	HashSet<uint64_t> open;
	if (enabled && repo.is_valid() && repo->is_open()) {
		// Godot doesn't say which file a tab shows, but get_open_scripts() lists the script tabs
		// (ScriptTextEditor) in the same order get_open_script_editors() lists all tabs.
		const TypedArray<ScriptEditorBase> editors = script_editor->get_open_script_editors();
		const TypedArray<Script> scripts = script_editor->get_open_scripts();
		int script_index = 0;
		for (int i = 0; i < editors.size(); i++) {
			ScriptEditorBase *editor = Object::cast_to<ScriptEditorBase>(editors[i]);
			if (!editor || editor->get_class() != "ScriptTextEditor") {
				continue;
			}
			const Ref<Script> script = script_index < scripts.size() ? Ref<Script>(scripts[script_index]) : Ref<Script>();
			script_index++;
			CodeEdit *code_edit = Object::cast_to<CodeEdit>(editor->get_base_editor());
			const String res_path = script.is_valid() ? script->get_path() : String();
			if (!code_edit || res_path.is_empty() || res_path.contains("::")) {
				continue; // Built-in scripts live inside a scene.
			}
			const String path = _repo_path(res_path);
			if (path.is_empty()) {
				continue; // Outside the repository.
			}
			const uint64_t id = code_edit->get_instance_id();
			open.insert(id);
			if (!tabs.has(id)) {
				tabs.insert(id, Tab());
				_install(code_edit, tabs[id]);
			}
			Tab &tab = tabs[id];
			if (tab.path != path) {
				tab.path = path; // A new tab, or the script was renamed.
				tab.base_head = String();
			}
			if (tab.base_head != head || head.is_empty()) {
				_update(id);
			}
		}
	}

	LocalVector<uint64_t> gone;
	for (const KeyValue<uint64_t, Tab> &E : tabs) {
		if (!open.has(E.key)) {
			gone.push_back(E.key);
		}
	}
	for (const uint64_t id : gone) {
		if (CodeEdit *code_edit = _code_edit(id)) {
			_uninstall(code_edit, tabs[id]);
		}
		tabs.erase(id);
		changed.erase(id);
	}
}

// The column goes between the line numbers and the fold arrows, right next to the code, where
// other editors put it too. CodeEdit and the script editor find their own gutters by name after
// one is added, so inserting ours doesn't confuse them.
void GitScriptMarks::_install(CodeEdit *p_code_edit, Tab &r_tab) {
	int at = p_code_edit->get_gutter_count();
	for (int i = 0; i < p_code_edit->get_gutter_count(); i++) {
		if (p_code_edit->get_gutter_name(i) == "line_numbers") {
			at = i + 1;
			break;
		}
	}
	const uint64_t id = p_code_edit->get_instance_id();
	p_code_edit->add_gutter(at);
	p_code_edit->set_gutter_name(at, GUTTER_NAME);
	p_code_edit->set_gutter_type(at, TextEdit::GUTTER_TYPE_CUSTOM);
	p_code_edit->set_gutter_width(at, Math::round(9 * editor_scale()));
	p_code_edit->set_gutter_clickable(at, true);
	p_code_edit->set_gutter_custom_draw(at, callable_mp(this, &GitScriptMarks::_draw_mark).bind(id));
	r_tab.on_text_changed = callable_mp(this, &GitScriptMarks::_on_text_changed).bind(id);
	r_tab.on_gutter_clicked = callable_mp(this, &GitScriptMarks::_on_gutter_clicked).bind(id);
	p_code_edit->connect("text_changed", r_tab.on_text_changed);
	p_code_edit->connect("gutter_clicked", r_tab.on_gutter_clicked);
}

void GitScriptMarks::_uninstall(CodeEdit *p_code_edit, Tab &r_tab) {
	const int gutter = _gutter(p_code_edit);
	if (gutter >= 0) {
		p_code_edit->remove_gutter(gutter);
	}
	if (p_code_edit->is_connected("text_changed", r_tab.on_text_changed)) {
		p_code_edit->disconnect("text_changed", r_tab.on_text_changed);
	}
	if (p_code_edit->is_connected("gutter_clicked", r_tab.on_gutter_clicked)) {
		p_code_edit->disconnect("gutter_clicked", r_tab.on_gutter_clicked);
	}
	GitChangePreview *open = Object::cast_to<GitChangePreview>(ObjectDB::get_instance(preview));
	if (open && open->get_parent() == p_code_edit) {
		close_preview();
	}
}

void GitScriptMarks::_uninstall_all() {
	close_preview();
	for (KeyValue<uint64_t, Tab> &E : tabs) {
		if (CodeEdit *code_edit = _code_edit(E.key)) {
			_uninstall(code_edit, E.value);
		}
	}
	tabs.clear();
	changed.clear();
}

void GitScriptMarks::_on_text_changed(uint64_t p_id) {
	changed.insert(p_id);
	update_timer->start();
}

void GitScriptMarks::_on_update_timer() {
	for (const uint64_t id : changed) {
		_update(id);
	}
	changed.clear();
}

// Works out a tab's marks: its text against the committed version (read again after a commit,
// pull or switch moved HEAD).
void GitScriptMarks::_update(uint64_t p_id) {
	CodeEdit *code_edit = _code_edit(p_id);
	Tab *tab = tabs.getptr(p_id);
	if (!code_edit || !tab || repo.is_null() || !repo->is_open()) {
		return;
	}
	if (tab->base_head != head || head.is_empty()) {
		tab->base_head = head;
		const Dictionary committed = head.is_empty() ? Dictionary() : repo->get_file_bytes("HEAD", tab->path);
		const PackedByteArray bytes = committed.get("bytes", PackedByteArray());
		// A new file (everything in it is new, so marks would say nothing), LFS, binary or huge:
		// no marks.
		tab->committed = bool(committed.get("exists", false)) && String(committed.get("lfs", String())).is_empty() && !bytes.has(0) && bytes.size() <= 2 * 1024 * 1024;
		tab->base = tab->committed ? bytes.get_string_from_utf8() : String();
	}

	tab->hunks = tab->committed ? GitRepository::diff_lines(tab->base, code_edit->get_text()) : Array();
	const int lines = code_edit->get_line_count();
	tab->marks.resize(lines);
	tab->marks.fill(MARK_NONE);
	tab->deleted_after.resize(lines);
	tab->deleted_after.fill(0);
	tab->hunk_at.resize(lines);
	tab->hunk_at.fill(-1);
	tab->deleted_at_top = false;
	for (int i = 0; i < tab->hunks.size(); i++) {
		const Dictionary hunk = tab->hunks[i];
		const int new_start = hunk["new_start"];
		const int new_count = hunk["new_count"];
		const int old_count = hunk["old_count"];
		if (new_count == 0) {
			// Deleted lines: a mark between the lines around them, clickable on the line above.
			const int line = MAX(new_start - 1, 0);
			if (line < lines) {
				if (new_start == 0) {
					tab->deleted_at_top = true;
				} else {
					tab->deleted_after.set(line, 1);
				}
				if (tab->hunk_at[line] < 0) {
					tab->hunk_at.set(line, i);
				}
			}
			continue;
		}
		for (int line = new_start - 1; line < MIN(new_start - 1 + new_count, lines); line++) {
			tab->marks.set(line, old_count == 0 ? MARK_ADDED : MARK_CHANGED);
			tab->hunk_at.set(line, i);
		}
	}
	code_edit->queue_redraw();
}

// In the panel's change colors, like the status letters (git_colors.h).
void GitScriptMarks::_draw_mark(int p_line, int p_gutter, const Rect2 &p_rect, uint64_t p_id) {
	const Tab *tab = tabs.getptr(p_id);
	CodeEdit *code_edit = _code_edit(p_id);
	if (!tab || !code_edit || p_line < 0 || p_line >= tab->marks.size()) {
		return;
	}
	const float scale = editor_scale();
	const float bar = MAX(2.0f, Math::round(3 * scale));
	const float x = p_rect.position.x + Math::round((p_rect.size.x - bar) / 2);
	const uint8_t mark = tab->marks[p_line];
	if (mark != MARK_NONE) {
		// Full line height, so the bars of neighboring lines join into one.
		code_edit->draw_rect(Rect2(x, p_rect.position.y, bar, p_rect.size.y), change_color(mark == MARK_ADDED ? CHANGE_ADDED : CHANGE_MODIFIED));
	}
	// Deleted lines: a small wedge pointing at the gap between two lines.
	const auto wedge = [&](float p_y) {
		const float half = MAX(4.0f, Math::round(4.5f * scale));
		PackedVector2Array points;
		points.push_back(Vector2(x, p_y - half));
		points.push_back(Vector2(x + half * 1.4f, p_y));
		points.push_back(Vector2(x, p_y + half));
		code_edit->draw_colored_polygon(points, change_color(CHANGE_REMOVED));
	};
	if (tab->deleted_after[p_line]) {
		wedge(p_rect.position.y + p_rect.size.y);
	}
	if (p_line == 0 && tab->deleted_at_top) {
		wedge(p_rect.position.y + MAX(4.0f, Math::round(4.5f * scale)));
	}
}

void GitScriptMarks::_on_gutter_clicked(int p_line, int p_gutter, uint64_t p_id) {
	CodeEdit *code_edit = _code_edit(p_id);
	if (!code_edit || p_gutter != _gutter(code_edit)) {
		return;
	}
	const int hunk = get_hunk_at(code_edit, p_line);
	if (hunk < 0) {
		return;
	}
	// A second click on the change that's showing closes it.
	GitChangePreview *open = Object::cast_to<GitChangePreview>(ObjectDB::get_instance(preview));
	if (open && open->get_parent() == code_edit && int(open->get_meta("git_hunk", -1)) == hunk) {
		close_preview();
		return;
	}
	show_preview(code_edit, hunk);
}

Array GitScriptMarks::get_hunks(CodeEdit *p_code_edit) const {
	const Tab *tab = p_code_edit ? tabs.getptr(p_code_edit->get_instance_id()) : nullptr;
	return tab ? tab->hunks : Array();
}

int GitScriptMarks::get_hunk_at(CodeEdit *p_code_edit, int p_line) const {
	const Tab *tab = p_code_edit ? tabs.getptr(p_code_edit->get_instance_id()) : nullptr;
	if (!tab || p_line < 0 || p_line >= tab->hunk_at.size()) {
		return -1;
	}
	return tab->hunk_at[p_line];
}

// The script's repository path. From its tab, or (with change marks off, so no tabs are tracked)
// from the script editor, when p_code_edit is its current tab's.
String GitScriptMarks::get_path(CodeEdit *p_code_edit) const {
	const Tab *tab = p_code_edit ? tabs.getptr(p_code_edit->get_instance_id()) : nullptr;
	if (tab) {
		return tab->path;
	}
	ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
	ScriptEditorBase *current = script_editor ? script_editor->get_current_editor() : nullptr;
	if (!p_code_edit || !current || current->get_base_editor() != p_code_edit || repo.is_null() || !repo->is_open()) {
		return String();
	}
	const Ref<Script> script = script_editor->get_current_script();
	const String res_path = script.is_valid() ? script->get_path() : String();
	return res_path.is_empty() || res_path.contains("::") ? String() : _repo_path(res_path);
}

void GitScriptMarks::show_preview(CodeEdit *p_code_edit, int p_hunk) {
	close_preview();
	if (p_hunk < 0 || p_hunk >= get_hunks(p_code_edit).size()) {
		return;
	}
	GitChangePreview *open = memnew(GitChangePreview);
	open->set_meta("git_hunk", p_hunk);
	p_code_edit->add_child(open);
	open->open(this, p_code_edit, p_hunk);
	preview = open->get_instance_id();
}

void GitScriptMarks::close_preview() {
	if (GitChangePreview *open = Object::cast_to<GitChangePreview>(ObjectDB::get_instance(preview))) {
		open->close();
	}
	preview = ObjectID();
}

// Puts the committed version of a change back into the editor, as one edit Ctrl+Z undoes. The
// file on disk isn't touched until you save.
void GitScriptMarks::revert(CodeEdit *p_code_edit, int p_hunk) {
	const Array hunks = get_hunks(p_code_edit);
	if (p_hunk < 0 || p_hunk >= hunks.size()) {
		return;
	}
	close_preview();
	const Dictionary hunk = hunks[p_hunk];
	const int new_start = hunk["new_start"];
	const int new_count = hunk["new_count"];
	const int old_count = hunk["old_count"];
	const String old_text = String("\n").join(PackedStringArray(hunk["old_lines"]));
	const int first = new_start - 1;
	const int lines = p_code_edit->get_line_count();

	p_code_edit->begin_complex_operation();
	if (new_count == 0) {
		// Deleted lines go back below the line they followed (or at the top).
		if (new_start == 0) {
			p_code_edit->insert_text(old_text + String("\n"), 0, 0);
		} else {
			p_code_edit->insert_text(String("\n") + old_text, first, p_code_edit->get_line(first).length());
		}
	} else if (old_count == 0) {
		// Added lines go, with their line breaks.
		const int after = first + new_count;
		if (after < lines) {
			p_code_edit->remove_text(first, 0, after, 0);
		} else if (first > 0) {
			p_code_edit->remove_text(first - 1, p_code_edit->get_line(first - 1).length(), lines - 1, p_code_edit->get_line(lines - 1).length());
		} else {
			p_code_edit->remove_text(0, 0, lines - 1, p_code_edit->get_line(lines - 1).length());
		}
	} else {
		const int last = MIN(first + new_count, lines) - 1;
		p_code_edit->remove_text(first, 0, last, p_code_edit->get_line(last).length());
		p_code_edit->insert_text(old_text, first, 0);
	}
	p_code_edit->end_complex_operation();
	p_code_edit->set_caret_line(CLAMP(first, 0, p_code_edit->get_line_count() - 1));
	p_code_edit->set_caret_column(0);
	// The marks follow right away rather than after the typing pause.
	_update(p_code_edit->get_instance_id());
	changed.erase(p_code_edit->get_instance_id());
}

void GitScriptMarks::show_in_diff(CodeEdit *p_code_edit) {
	const String path = get_path(p_code_edit);
	if (!path.is_empty()) {
		close_preview();
		emit_signal("show_in_diff_requested", path);
	}
}

// GitChangePreview

void GitChangePreview::open(GitScriptMarks *p_marks, CodeEdit *p_code_edit, int p_hunk) {
	marks = p_marks;
	code_edit = p_code_edit;
	hunk = p_hunk;
	const Array hunks = marks->get_hunks(code_edit);
	const Dictionary change = hunks[hunk];
	const int new_start = change["new_start"];
	const int new_count = change["new_count"];
	const int old_count = change["old_count"];
	const PackedStringArray old_lines = change["old_lines"];
	first_line = MAX(new_start - 1, 0);
	last_line = new_count == 0 ? first_line : new_start + new_count - 2;
	const float scale = editor_scale();
	const Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();

	const Color accent = change_color(new_count == 0 ? CHANGE_REMOVED : (old_count == 0 ? CHANGE_ADDED : CHANGE_MODIFIED));
	// Solid, like a tooltip: the code shows through a transparent background otherwise.
	Color background = code_edit->get_theme_color("background_color");
	if (background.a < 0.9) {
		background = editor_color("base_color");
	}
	background.a = 1;
	background = background.get_luminance() < 0.5 ? background.lightened(0.06) : background.darkened(0.04);
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_bg_color(background);
	style->set_border_color(accent);
	style->set_border_width(SIDE_LEFT, Math::round(3 * scale));
	style->set_corner_radius_all(Math::round(4 * scale));
	style->set_content_margin_all(Math::round(6 * scale));
	style->set_content_margin(SIDE_LEFT, Math::round(9 * scale));
	style->set_shadow_color(Color(0, 0, 0, 0.35));
	style->set_shadow_size(Math::round(8 * scale));
	add_theme_stylebox_override("panel", style);
	set_mouse_filter(MOUSE_FILTER_STOP); // Clicks on it are for it, not the code under it.

	VBoxContainer *layout = memnew(VBoxContainer);
	layout->add_theme_constant_override("separation", Math::round(4 * scale));
	add_child(layout);
	HBoxContainer *bar = memnew(HBoxContainer);
	layout->add_child(bar);

	// Icon-only buttons (arrows, close) are flat like Godot's toolbars; the actions are real buttons.
	const auto add_button = [&](const String &p_text, const String &p_icon, const String &p_tooltip, bool p_enabled, const Callable &p_action) {
		Button *button = memnew(Button);
		button->set_text(p_text);
		if (!p_icon.is_empty()) {
			button->set_button_icon(theme->get_icon(p_icon, "EditorIcons"));
		}
		button->set_flat(p_text.is_empty());
		button->set_tooltip_text(p_tooltip);
		button->set_disabled(!p_enabled);
		button->set_focus_mode(FOCUS_NONE);
		button->connect("pressed", p_action);
		bar->add_child(button);
	};

	const auto go_to = [](GitScriptMarks *p_marks, CodeEdit *p_code_edit, int p_hunk) {
		const Dictionary target = p_marks->get_hunks(p_code_edit)[p_hunk];
		p_code_edit->set_caret_line(MAX(int(target["new_start"]) - 1, 0));
		p_code_edit->set_caret_column(0);
		p_code_edit->center_viewport_to_caret();
		p_marks->show_preview(p_code_edit, p_hunk);
	};
	add_button(String(), "ArrowUp", "Previous change", hunk > 0, callable_mp_static(+go_to).bind(marks, code_edit, hunk - 1));
	add_button(String(), "ArrowDown", "Next change", hunk < hunks.size() - 1, callable_mp_static(+go_to).bind(marks, code_edit, hunk + 1));

	String what;
	if (old_count == 0) {
		what = vformat("%s added", plural(new_count, "line", "lines"));
	} else if (new_count == 0) {
		what = vformat("%s deleted", plural(old_count, "line", "lines"));
	} else if (old_count == new_count) {
		what = vformat("%s changed", plural(new_count, "line", "lines"));
	} else {
		what = vformat("%s changed to %d", plural(old_count, "line", "lines"), new_count);
	}
	Label *title = memnew(Label);
	title->set_text(vformat(String::utf8("Change %d of %d · %s"), hunk + 1, hunks.size(), what));
	title->set_h_size_flags(SIZE_EXPAND_FILL);
	title->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	title->set_modulate(Color(1, 1, 1, 0.7));
	title->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	bar->add_child(title);

	const String revert_tooltip = old_count == 0 ? String("Remove the added lines from the editor. Ctrl+Z brings them back; nothing is saved until you save.") : String("Put the committed version of these lines back in the editor. Ctrl+Z undoes it; nothing is saved until you save.");
	add_button("Revert Change", "UndoRedo", revert_tooltip, true, callable_mp(marks, &GitScriptMarks::revert).bind(code_edit, hunk));
	add_button("Show in Diff", "VCSCommit", "Show this file's changes in the Diff panel (as saved on disk).", true, callable_mp(marks, &GitScriptMarks::show_in_diff).bind(code_edit));
	add_button(String(), "Close", "Close (Esc)", true, callable_mp(this, &GitChangePreview::close));

	if (!old_lines.is_empty()) {
		// The lines as they were, in the script editor's font and colors, tinted exactly like
		// removed lines in the Diff panel: its red over the code editor's own background.
		CodeEdit *old_view = memnew(CodeEdit);
		old_view->set_text(String("\n").join(old_lines));
		old_view->set_editable(false);
		old_view->set_highlight_current_line(false);
		old_view->set_syntax_highlighter(GitDiffDock::make_code_highlighter(marks->get_path(code_edit)));
		old_view->add_theme_font_override("font", code_edit->get_theme_font("font"));
		old_view->add_theme_font_size_override("font_size", code_edit->get_theme_font_size("font_size"));
		old_view->add_theme_color_override("font_readonly_color", code_edit->get_theme_color("font_color"));
		// The code's background as the Diff panel has it: the code editor's panel style (its
		// background_color is see-through in the editor theme).
		const Ref<StyleBoxFlat> code_style = code_edit->get_theme_stylebox("normal");
		Color code_background = code_style.is_valid() ? code_style->get_bg_color() : code_edit->get_theme_color("background_color");
		code_background.a = 1;
		Ref<StyleBoxFlat> frame;
		frame.instantiate();
		frame->set_bg_color(code_background.blend(GitDiffDock::row_tint(false)));
		frame->set_corner_radius_all(Math::round(3 * scale));
		frame->set_content_margin_all(Math::round(4 * scale));
		old_view->add_theme_stylebox_override("normal", frame);
		old_view->add_theme_stylebox_override("read_only", frame);
		// A TextEdit paints its background_color over its stylebox; the frame alone is the color.
		old_view->add_theme_color_override("background_color", Color(0, 0, 0, 0));
		Ref<StyleBoxEmpty> none;
		none.instantiate();
		old_view->add_theme_stylebox_override("focus", none);
		const int shown = MIN(old_lines.size(), 12);
		old_view->set_custom_minimum_size(Vector2(0, shown * code_edit->get_line_height() + 8 * scale + old_view->get_h_scroll_bar()->get_combined_minimum_size().y));
		layout->add_child(old_view);
	}

	opened_at = Time::get_singleton()->get_ticks_msec();
	code_edit->connect("text_changed", callable_mp(this, &GitChangePreview::close));
	code_edit->connect("caret_changed", callable_mp(this, &GitChangePreview::_on_caret_changed));
	code_edit->connect("gui_input", callable_mp(this, &GitChangePreview::_on_code_edit_input));
	set_process(true);
	_reposition();
}

void GitChangePreview::close() {
	if (is_queued_for_deletion()) {
		return;
	}
	if (code_edit && ObjectDB::get_instance(code_edit->get_instance_id())) {
		for (const Callable &callable : { callable_mp(this, &GitChangePreview::close), callable_mp(this, &GitChangePreview::_on_caret_changed) }) {
			for (const char *signal : { "text_changed", "caret_changed" }) {
				if (code_edit->is_connected(signal, callable)) {
					code_edit->disconnect(signal, callable);
				}
			}
		}
		if (code_edit->is_connected("gui_input", callable_mp(this, &GitChangePreview::_on_code_edit_input))) {
			code_edit->disconnect("gui_input", callable_mp(this, &GitChangePreview::_on_code_edit_input));
		}
	}
	set_process(false);
	hide();
	queue_free();
}

void GitChangePreview::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		// Scrolling (wheel, minimap, keys) and resizing have no one signal; this is a few lookups.
		_reposition();
	}
}

// Under the change, as wide as the code; above it when there's no room below.
void GitChangePreview::_reposition() {
	if (!code_edit) {
		return;
	}
	const int last = CLAMP(last_line, 0, code_edit->get_line_count() - 1);
	const Vector2i below_line = code_edit->get_pos_at_line_column(last, 0); // Bottom of the line.
	const Vector2i first = code_edit->get_pos_at_line_column(CLAMP(first_line, 0, code_edit->get_line_count() - 1), 0);
	const bool visible = below_line.y >= 0 || first.y >= 0;
	set_visible(visible);
	if (!visible) {
		return;
	}
	const float left = code_edit->get_total_gutter_width();
	const float scroll_bar = code_edit->get_v_scroll_bar()->is_visible() ? code_edit->get_v_scroll_bar()->get_size().x : 0.0f;
	const float minimap = code_edit->is_drawing_minimap() ? code_edit->get_minimap_width() : 0.0f;
	const float width = MAX(code_edit->get_size().x - left - scroll_bar - minimap - 8 * editor_scale(), 200 * editor_scale());
	set_custom_minimum_size(Vector2(width, 0));
	reset_size();
	const float height = get_size().y;
	float y = below_line.y >= 0 ? below_line.y + 2.0f : code_edit->get_size().y;
	if (y + height > code_edit->get_size().y && first.y >= 0) {
		y = MAX(first.y - code_edit->get_line_height() - height - 2.0f, 0.0f);
	}
	set_position(Vector2(left, MIN(y, MAX(code_edit->get_size().y - height, 0.0f))));
}

// The click that opened it (or moved to the next change) may move the caret; anything after that
// means you've moved on.
void GitChangePreview::_on_caret_changed() {
	if (Time::get_singleton()->get_ticks_msec() - opened_at > 150) {
		callable_mp(this, &GitChangePreview::close).call_deferred();
	}
}

void GitChangePreview::_on_code_edit_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && key->get_keycode() == KEY_ESCAPE) {
		code_edit->accept_event();
		callable_mp(this, &GitChangePreview::close).call_deferred();
	}
}

// GitScriptMenu

void GitScriptMenu::set_marks(GitScriptMarks *p_marks) {
	marks = p_marks ? p_marks->get_instance_id() : ObjectID();
}

GitScriptMarks *GitScriptMenu::_get_marks() const {
	return Object::cast_to<GitScriptMarks>(ObjectDB::get_instance(marks));
}

// The menu is given the CodeEdit's node path; its items' callbacks, the CodeEdit itself.
CodeEdit *GitScriptMenu::_code_edit_from(const Variant &p_target) {
	if (p_target.get_type() == Variant::OBJECT) {
		return Object::cast_to<CodeEdit>(p_target.operator Object *());
	}
	const PackedStringArray paths = p_target;
	if (paths.is_empty()) {
		return nullptr;
	}
	SceneTree *tree = EditorInterface::get_singleton()->get_base_control()->get_tree();
	return tree ? Object::cast_to<CodeEdit>(tree->get_root()->get_node_or_null(NodePath(paths[0]))) : nullptr;
}

void GitScriptMenu::_popup_menu(const PackedStringArray &p_paths) {
	GitScriptMarks *script_marks = _get_marks();
	CodeEdit *code_edit = _code_edit_from(p_paths);
	if (!script_marks || !code_edit || script_marks->get_path(code_edit).is_empty()) {
		return; // Not a script in the repository.
	}
	const Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();
	if (script_marks->get_hunk_at(code_edit, code_edit->get_caret_line()) >= 0) {
		add_context_menu_item("Show Change", callable_mp(this, &GitScriptMenu::_show_change));
		add_context_menu_item("Revert Change", callable_mp(this, &GitScriptMenu::_revert), theme->get_icon("UndoRedo", "EditorIcons"));
		add_context_menu_item("Show in Diff", callable_mp(this, &GitScriptMenu::_show_in_diff), theme->get_icon("VCSCommit", "EditorIcons"));
	}
	add_context_menu_item("Show Commit for This Line", callable_mp(this, &GitScriptMenu::_line_commit), theme->get_icon("VCSCommit", "EditorIcons"));
	add_context_menu_item("Show History of This File", callable_mp(this, &GitScriptMenu::_file_history), theme->get_icon("History", "EditorIcons"));
}

void GitScriptMenu::_line_commit(const Variant &p_target) {
	GitScriptMarks *script_marks = _get_marks();
	CodeEdit *code_edit = _code_edit_from(p_target);
	if (script_marks && code_edit) {
		script_marks->emit_signal("line_commit_requested", script_marks->get_path(code_edit), code_edit->get_text(), code_edit->get_caret_line());
	}
}

void GitScriptMenu::_file_history(const Variant &p_target) {
	GitScriptMarks *script_marks = _get_marks();
	CodeEdit *code_edit = _code_edit_from(p_target);
	if (script_marks && code_edit) {
		script_marks->emit_signal("history_requested", script_marks->get_path(code_edit));
	}
}

void GitScriptMenu::_show_change(const Variant &p_target) {
	GitScriptMarks *script_marks = _get_marks();
	CodeEdit *code_edit = _code_edit_from(p_target);
	if (script_marks && code_edit) {
		script_marks->show_preview(code_edit, script_marks->get_hunk_at(code_edit, code_edit->get_caret_line()));
	}
}

void GitScriptMenu::_revert(const Variant &p_target) {
	GitScriptMarks *script_marks = _get_marks();
	CodeEdit *code_edit = _code_edit_from(p_target);
	if (script_marks && code_edit) {
		script_marks->revert(code_edit, script_marks->get_hunk_at(code_edit, code_edit->get_caret_line()));
	}
}

void GitScriptMenu::_show_in_diff(const Variant &p_target) {
	GitScriptMarks *script_marks = _get_marks();
	CodeEdit *code_edit = _code_edit_from(p_target);
	if (script_marks && code_edit) {
		script_marks->show_in_diff(code_edit);
	}
}
