// History: 50 commits at a time; expanding a commit shows its details and files (built deferred,
// never inside the Tree's mouse handling: gotcha 41).

#include "editor/git_dock.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/text_line.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/core/math.hpp>

#include "editor/avatars.h"
#include "editor/git_diff_dock.h"
#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

namespace {

// "Markus Steen" -> "MS", "mssbabou" -> "M".
String initials(const String &p_name) {
	const PackedStringArray words = p_name.strip_edges().split(" ", false);
	if (words.is_empty()) {
		return "?";
	}
	String result = words[0].left(1).to_upper();
	if (words.size() > 1) {
		result += words[words.size() - 1].left(1).to_upper();
	}
	return result;
}

// One color per person, the same every time: a hue from their email (their name without one).
Color person_color(const Dictionary &p_commit) {
	String key = String(p_commit.get("email", String())).to_lower();
	if (key.is_empty()) {
		key = p_commit.get("author", String());
	}
	return Color::from_hsv((key.hash() % 360) / 360.0, 0.45, 0.62);
}

} // namespace

// History: the latest commits, each expandable to its details and changed files (loaded when
// first expanded). Rebuilt only when the commits changed, so expanded commits, the selection and
// the scroll position survive the refresh that every save triggers.
void GitDock::_fill_history() {
	Array commits = repo->get_commits(history_limit + 1, history_path, history_query);
	const bool more = commits.size() > history_limit;
	if (more) {
		commits.resize(history_limit);
	}
	// For Amend and Undo Last Commit, whatever History shows. "unpushed" means on no
	// remote-tracking branch; without remotes it's never set.
	const Dictionary last = repo->get_commit("HEAD");
	has_commits = !last.is_empty();
	last_commit_id = last.get("id", String());
	last_commit_message = last.get("message", String());
	last_commit_pushed = has_commits && bool(sync_status.get("has_remotes", false)) && !bool(last.get("unpushed", false));

	// Pictures are asked about on the remote History's links would open (see _web_commit_url).
	String remote = String(sync_status.get("upstream", String())).get_slice("/", 0);
	const PackedStringArray remotes = repo->get_remotes();
	if (remote.is_empty() || !remotes.has(remote)) {
		remote = remotes.has("origin") || remotes.is_empty() ? String("origin") : remotes[0];
	}
	avatars->set_repository(repo->get_workdir(), remotes.is_empty() ? String() : repo->get_remote_url(remote));
	avatars->set_enabled(EditorInterface::get_singleton()->get_editor_settings()->get_setting(AVATARS_SETTING));

	TreeItem *root = history_tree->get_root();
	if (root && commits == history_shown && more == history_more) {
		// Same commits: only the ages ("5m") move on, which the rows draw as they go.
		history_tree->queue_redraw();
		return;
	}
	history_shown = commits;
	history_more = more;

	history_tree->clear();
	root = history_tree->create_item();
	history_tree->set_visible(!commits.is_empty());
	history_empty->get_parent_control()->set_visible(commits.is_empty());
	if (!history_path.is_empty()) {
		history_empty->set_text(history_query.is_empty() ? vformat("No commits changed %s.", history_path.get_file()) : vformat("No commits that changed %s match \"%s\".", history_path.get_file(), history_query));
	} else if (!history_query.is_empty()) {
		history_empty->set_text(vformat("No commits match \"%s\".", history_query));
	} else {
		history_empty->set_text("No commits yet.");
	}
	if (commits.is_empty()) {
		return;
	}

	const Color accent = get_theme_color("accent_color", "Editor");
	// Two lines (summary, then who and when) next to the author's avatar.
	const Ref<Font> font = history_tree->get_theme_font("font");
	const int font_size = history_tree->get_theme_font_size("font_size");
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const int row_height = Math::round(font->get_height(font_size + Math::round(scale)) + font->get_height(font_size) + 10 * scale);

	for (int i = 0; i < commits.size(); i++) {
		const Dictionary commit = commits[i];
		const bool unpushed = commit["unpushed"];

		TreeItem *item = history_tree->create_item(root);
		item->set_meta("git_row", "commit");
		item->set_metadata(0, commit);
		item->set_cell_mode(0, TreeItem::CELL_MODE_CUSTOM);
		item->set_custom_draw_callback(0, callable_mp(this, &GitDock::_draw_commit_row));
		item->set_custom_minimum_height(row_height);
		// The summary as the cell's text, invisible (drawn by _draw_commit_row), for type-to-search.
		item->set_text(0, commit["summary"]);
		item->set_custom_color(0, Color(0, 0, 0, 0));
		String tooltip = vformat(String::utf8("%s\n\n%s · %s · %s"), commit["message"], commit["id"], commit["author"], local_date_time(commit["time"]));
		if (unpushed) {
			tooltip += "\nNot pushed yet.";
		}
		if (bool(commit.get("merge", false))) {
			tooltip += "\nA merge: its files and diffs show what it brought into this branch.";
		}
		item->set_tooltip_text(0, tooltip);
		item->set_tooltip_text(1, tooltip);

		// Collapsed with a placeholder child, so it shows the arrow; the details load on expand.
		TreeItem *placeholder = history_tree->create_item(item);
		placeholder->set_meta("git_row", "placeholder");
		placeholder->set_selectable(0, false);
		placeholder->set_selectable(1, false);
		// Read before collapsing: collapsing emits item_collapsed, whose handler forgets the
		// commit was expanded (so Show Commit, and expanded commits across a rebuild, stayed shut).
		const bool expanded = history_expanded.has(commit["hash"]);
		item->set_collapsed(true);
		if (expanded) {
			item->set_collapsed(false); // Loads it (item_collapsed).
		}
	}

	if (more) {
		TreeItem *item = history_tree->create_item(root);
		item->set_meta("git_row", "more");
		item->set_text(0, "Load More Commits");
		item->set_custom_color(0, accent);
		item->set_tooltip_text(0, "Show 50 more commits.");
		item->set_selectable(1, false);
	}
}

// Fills an expanded commit with the files it changed. Who and when are on its row; the whole
// message, its parents and the branches that have it go in the row's tooltip.
void GitDock::_fill_commit(TreeItem *p_item) {
	while (p_item->get_first_child()) {
		memdelete(p_item->get_first_child());
	}
	const Dictionary commit = p_item->get_metadata(0);
	const String hash = commit["hash"];
	const Color dim = _dim_color();

	// Notes are information, not buttons. The Tree highlights every row under the mouse, but draws
	// a row's own background above that highlight, so the section's color covers it.
	const Ref<StyleBoxFlat> section = history_pane->get_theme_stylebox("panel");
	const Color background = section.is_valid() ? section->get_bg_color() : Color(0, 0, 0, 0);
	auto add_note = [&](const String &p_text) {
		TreeItem *note = history_tree->create_item(p_item);
		note->set_meta("git_row", "note");
		note->set_text(0, p_text);
		note->set_custom_color(0, dim);
		for (int column = 0; column < 2; column++) {
			note->set_selectable(column, false);
			if (background.a > 0) {
				note->set_custom_bg_color(column, background);
			}
		}
	};

	if (!p_item->has_meta("git_details")) {
		const Dictionary details = repo->get_commit_details(hash);
		const Array parents = details.get("parents", Array());
		const PackedStringArray branches = details.get("branches", PackedStringArray());
		PackedStringArray parent_lines;
		for (int i = 0; i < parents.size(); i++) {
			const Dictionary parent = parents[i];
			parent_lines.push_back(vformat("%s %s", parent["hash"], parent["summary"]));
		}
		String extra;
		if (!parent_lines.is_empty()) {
			extra += vformat("\n\n%s:\n%s", parent_lines.size() == 1 ? "Parent" : "Parents", String("\n").join(parent_lines));
		}
		if (!branches.is_empty()) {
			extra += vformat("\n\nOn %s.", join_list(branches, 20));
		}
		p_item->set_meta("git_details", true);
		for (int column = 0; column < 2; column++) {
			p_item->set_tooltip_text(column, p_item->get_tooltip_text(column) + extra);
		}
	}

	if (!commit_files.has(hash)) {
		commit_files[hash] = repo->get_commit_files(hash);
	}
	const Array files = commit_files[hash];
	if (files.is_empty()) {
		add_note("No file changes.");
	}
	// 500 rows is enough for any real commit; beyond that the tree would only get slow.
	const int left_out = _add_commit_file_rows(history_tree, p_item, files, hash, 500);
	if (left_out > 0) {
		add_note(vformat("...and %d more files, not listed.", left_out));
	}
}

// A commit row: the author's avatar (initials in their color), the summary, and under it who and
// how long ago, plus "not pushed" (and a ring around the avatar) for commits on no remote branch.
void GitDock::_draw_commit_row(TreeItem *p_item, const Rect2 &p_rect) {
	Tree *tree = p_item->get_tree();
	const Dictionary commit = p_item->get_metadata(0);
	const Ref<Font> font = tree->get_theme_font("font");
	const int font_size = tree->get_theme_font_size("font_size");
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const RID canvas = tree->get_custom_drawing_canvas_item();
	RenderingServer *rs = RenderingServer::get_singleton();
	const bool unpushed = commit.get("unpushed", false);
	const Color accent = get_theme_color("accent_color", "Editor");

	const float diameter = MIN(Math::round(28 * scale), p_rect.size.y - 8 * scale);
	const Vector2 center(p_rect.position.x + 3 * scale + diameter / 2, p_rect.position.y + p_rect.size.y / 2);
	// Their GitHub picture once it's here (see GitAvatars), else their initials in their color.
	const Ref<Texture2D> picture = avatars->get_avatar(commit);
	if (picture.is_valid()) {
		picture->draw_rect(canvas, Rect2(center - Vector2(diameter, diameter) / 2, Vector2(diameter, diameter)), false);
	} else {
		rs->canvas_item_add_circle(canvas, center, diameter / 2, person_color(commit), true);
		const String letters = initials(commit.get("author", String()));
		const int letter_size = Math::round(diameter * 0.42);
		const float letters_width = font->get_string_size(letters, HORIZONTAL_ALIGNMENT_LEFT, -1, letter_size).x;
		font->draw_string(canvas, Vector2(center.x - letters_width / 2, center.y - font->get_height(letter_size) / 2 + font->get_ascent(letter_size)), letters, HORIZONTAL_ALIGNMENT_LEFT, -1, letter_size, Color(1, 1, 1, 0.95));
	}
	if (unpushed) {
		PackedVector2Array ring;
		const float radius = diameter / 2 + 2.5 * scale;
		for (int i = 0; i <= 48; i++) {
			ring.push_back(center + Vector2(radius, 0).rotated(Math::TAU * i / 48));
		}
		rs->canvas_item_add_polyline(canvas, ring, PackedColorArray({ accent }), 1.5 * scale, true);
	}

	const float x = center.x + diameter / 2 + 8 * scale;
	// Up to the row's end: the second column (a commit's files put their letters there) is empty here.
	const float width = p_rect.get_end().x + tree->get_column_width(1) - x - 4 * scale;
	if (width < font_size) {
		return;
	}
	// The summary a pixel larger than the line under it (the maintainer's ask). The two lines are
	// placed by their baselines and centered on the avatar as they look (the summary's capitals to
	// the name's baseline), not by their line boxes, whose empty space above and below differs.
	const int summary_size = font_size + Math::round(scale);
	const float summary_cap = font->get_ascent(summary_size) * 0.72f; // Roughly the capitals' height.
	const float name_cap = font->get_ascent(font_size) * 0.72f;
	const float gap = 9 * scale; // From the summary's baseline to the top of the name's capitals.
	const float summary_baseline = Math::round(center.y - (summary_cap + gap + name_cap) / 2 + summary_cap);
	const float name_baseline = Math::round(summary_baseline + gap + name_cap);
	const float top = summary_baseline - font->get_ascent(summary_size); // TextLine draws from its top.
	const float line_height = name_baseline - font->get_ascent(font_size) - top; // Where the second line starts.
	// Draws one line trimmed to p_width; returns how wide it came out.
	auto draw_line = [&](const String &p_text, float p_x, float p_y, float p_width, const Color &p_color, int p_size = 0) -> float {
		Ref<TextLine> line;
		line.instantiate();
		line->add_string(p_text, font, p_size > 0 ? p_size : font_size);
		line->set_width(p_width);
		line->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		line->draw(canvas, Vector2(p_x, p_y), p_color);
		return MIN(line->get_size().x, p_width);
	};
	draw_line(commit.get("summary", String()), x, top, width, tree->get_theme_color(p_item->is_selected(0) ? "font_selected_color" : "font_color"), summary_size);
	const String who = vformat(String::utf8("%s · %s"), commit.get("author", String()), relative_time(commit.get("time", 0)));
	if (!unpushed) {
		draw_line(who, x, top + line_height, width, _dim_color());
		return;
	}
	// "not pushed" in the accent color after it, never trimmed away: the name gives way first.
	const String suffix = String::utf8(" · not pushed");
	const float suffix_width = font->get_string_size(suffix, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
	const float who_width = draw_line(who, x, top + line_height, MAX(float(font_size), width - suffix_width), _dim_color());
	draw_line(suffix, x + who_width, top + line_height, MAX(0.0f, width - who_width), accent);
}

void GitDock::_on_avatars_changed() {
	history_tree->queue_redraw();
}

// A commit's or stash's files under p_parent, with companions (player.gd.uid) on their file's row
// like the change lists (see _fill_file_pane). Returns how many files didn't fit in p_max_rows.
int GitDock::_add_commit_file_rows(Tree *p_tree, TreeItem *p_parent, const Array &p_files, const String &p_hash, int p_max_rows) {
	PackedStringArray paths;
	for (int i = 0; i < p_files.size(); i++) {
		paths.push_back(Dictionary(p_files[i])["path"]);
	}
	const Dictionary companions = grouped_companions(paths);
	int rows = 0;
	for (int i = 0; i < p_files.size(); i++) {
		if (is_grouped(companions, paths[i])) {
			continue;
		}
		if (rows == p_max_rows) {
			return p_files.size() - i;
		}
		TreeItem *item = _add_commit_file_row(p_tree, p_parent, p_files[i], p_hash);
		if (companions.has(paths[i])) {
			const PackedStringArray with = companions[paths[i]];
			item->set_meta("git_companions", with);
			item->set_tooltip_text(0, vformat("%s\nWith %s, which Godot keeps next to it.", item->get_tooltip_text(0), String(", ").join(with)));
		}
		rows++;
	}
	return 0;
}

// A row for one file of a commit or stash (p_hash; see GitRepository::get_commit_files), drawn
// like the change lists' rows, with the status letter in the second (ages') column.
TreeItem *GitDock::_add_commit_file_row(Tree *p_tree, TreeItem *p_parent, const Dictionary &p_file, const String &p_hash) {
	const String path = p_file["path"];
	const String state = p_file["status"];
	TreeItem *item = p_tree->create_item(p_parent);
	item->set_meta("git_row", "file");
	item->set_meta("git_path", path);
	item->set_meta("git_state", state);
	item->set_meta("git_hash", p_hash);
	item->set_meta("git_icon", _file_icon(path));
	item->set_cell_mode(0, TreeItem::CELL_MODE_CUSTOM);
	item->set_custom_draw_callback(0, callable_mp(this, &GitDock::_draw_file_row));
	item->set_text(0, path.get_file());
	item->set_custom_color(0, Color(0, 0, 0, 0));
	const int added = p_file["added"];
	const int removed = p_file["removed"];
	String what = status_name(state);
	if (String(p_file["old_path"]) != path) {
		what += vformat(" from %s", p_file["old_path"]);
	}
	if (added > 0 || removed > 0) {
		what += vformat(String::utf8(" · +%d %s%d"), added, minus(), removed);
	}
	const String note = companion_note(path, state);
	item->set_tooltip_text(0, note.is_empty() ? vformat("%s\n%s", path, what) : vformat("%s\n%s\n%s", path, what, note));
	item->set_selectable(1, false);
	// The status letter in the ages' column, so it lines up with the letters of the lists
	// above (at the right edge) instead of stopping short of this column.
	item->set_text(1, status_letter(state));
	item->set_custom_color(1, _status_color(state));
	item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
	item->set_tooltip_text(1, what);
	return item;
}

void GitDock::_on_history_item_collapsed(TreeItem *p_item) {
	if (row_kind(p_item) != "commit") {
		return;
	}
	const String hash = Dictionary(p_item->get_metadata(0))["hash"];
	if (p_item->is_collapsed()) {
		history_expanded.erase(hash);
		return;
	}
	history_expanded[hash] = true;
	// Not now: a click (on the row or its arrow) expands it while the Tree is handling that click,
	// and then the Tree refuses to create rows (create_item() returns null) and crashed us.
	callable_mp(this, &GitDock::_fill_commit_later).call_deferred(p_item->get_instance_id());
}

void GitDock::_fill_commit_later(uint64_t p_item) {
	TreeItem *item = Object::cast_to<TreeItem>(ObjectDB::get_instance(ObjectID(p_item)));
	TreeItem *first = item ? item->get_first_child() : nullptr;
	if (!item || item->is_collapsed() || !first || row_kind(first) != "placeholder") {
		return; // Gone (rebuilt), collapsed again, or already filled.
	}
	_fill_commit(item);
	_select_diff_row();
}

void GitDock::_load_more_commits() {
	history_limit += 50;
	_fill_history();
}

// Selecting a commit's file (by mouse or keyboard) shows its change in the Diff panel.
void GitDock::_on_history_item_selected() {
	TreeItem *item = history_tree->get_selected();
	if (row_kind(item) != "file") {
		return;
	}
	staged_pane.tree->deselect_all();
	changes_pane.tree->deselect_all();
	stashes_tree->deselect_all();
	_show_commit_diff(item->get_meta("git_hash"), item->get_meta("git_path"), false);
}

// History's filters: a search field (opened from the magnifier in its header) and, while History
// shows one file's commits, a bar naming the file (icon and name, like a filter chip; "History of
// ..." got cut off in a narrow dock) with a button back to all commits. Both look
// through the whole history, not just the commits already listed.
void GitDock::_build_history_filters() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	Control *body = history_tree->get_parent_control();

	history_search_button = memnew(Button);
	history_search_button->set_tooltip_text("Search commits by message, author or hash.");
	history_search_button->set_toggle_mode(true);
	history_search_button->set_v_size_flags(SIZE_SHRINK_CENTER);
	history_search_button->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	history_search_button->connect("pressed", callable_mp(this, &GitDock::_on_history_search_pressed));
	history_pane->add_title_bar_control(history_search_button);

	MarginContainer *search_margin = memnew(MarginContainer);
	search_margin->add_theme_constant_override("margin_bottom", Math::round(4 * scale));
	search_margin->hide();
	history_search_row = search_margin;
	history_search = memnew(LineEdit);
	history_search->set_placeholder("Search commits");
	history_search->set_clear_button_enabled(true);
	history_search->connect("text_changed", callable_mp(this, &GitDock::_on_history_search_changed));
	history_search->connect("text_submitted", callable_mp(this, &GitDock::_apply_history_search).unbind(1));
	history_search->connect("gui_input", callable_mp(this, &GitDock::_on_history_search_input));
	search_margin->add_child(history_search);
	body->add_child(search_margin);
	body->move_child(search_margin, 0);

	HBoxContainer *file_bar = memnew(HBoxContainer);
	file_bar->hide();
	history_file_bar = file_bar;
	history_file_icon = memnew(TextureRect);
	history_file_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	file_bar->add_child(history_file_icon);
	history_file_label = memnew(Label);
	history_file_label->set_h_size_flags(SIZE_EXPAND_FILL);
	history_file_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	history_file_label->set_mouse_filter(MOUSE_FILTER_PASS); // For its tooltip (the full path).
	file_bar->add_child(history_file_label);
	Button *all = memnew(Button);
	all->set_flat(true);
	all->set_tooltip_text("Show every commit again.");
	history_file_close = all;
	all->connect("pressed", callable_mp(this, &GitDock::_set_history_filter).bind(String(), String()));
	file_bar->add_child(all);
	body->add_child(file_bar);
	body->move_child(file_bar, 0);

	history_search_timer = memnew(Timer);
	history_search_timer->set_one_shot(true);
	history_search_timer->set_wait_time(0.4);
	history_search_timer->connect("timeout", callable_mp(this, &GitDock::_apply_history_search));
	add_child(history_search_timer);
}

void GitDock::_on_history_search_pressed() {
	if (history_search_row->is_visible()) {
		// Closing the search ends it.
		history_search_row->hide();
		history_search->clear();
		_set_history_filter(history_path, String());
		return;
	}
	history_pane->set_folded(false);
	history_search_row->show();
	history_search_button->set_pressed_no_signal(true);
	history_search->grab_focus();
}

void GitDock::_on_history_search_changed(const String &p_text) {
	history_search_timer->start();
}

void GitDock::_on_history_search_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && key->get_keycode() == KEY_ESCAPE) {
		history_search->accept_event();
		_on_history_search_pressed(); // Closes it.
	}
}

void GitDock::_apply_history_search() {
	history_search_timer->stop();
	if (history_search->get_text().strip_edges() != history_query) {
		_set_history_filter(history_path, history_search->get_text().strip_edges());
	}
}

// Shows p_path's commits (or all with ""), matching p_query (or all with "").
void GitDock::_set_history_filter(const String &p_path, const String &p_query) {
	history_path = p_path;
	history_query = p_query;
	history_limit = 50;
	history_file_bar->set_visible(!p_path.is_empty());
	history_file_label->set_text(p_path.get_file());
	history_file_icon->set_texture(p_path.is_empty() ? Ref<Texture2D>() : _file_icon(p_path));
	history_file_label->set_tooltip_text(vformat("History shows the commits that changed %s.", p_path));
	history_search_button->set_pressed_no_signal(history_search_row->is_visible());
	_fill_history();
}

// The Git dock up front, History unfolded and scrolled into view.
void GitDock::_scroll_to_history() {
	make_visible();
	history_pane->set_folded(false);
	for (Node *parent = history_pane->get_parent(); parent; parent = parent->get_parent()) {
		if (ScrollContainer *scroll = Object::cast_to<ScrollContainer>(parent)) {
			callable_mp(scroll, &ScrollContainer::ensure_control_visible).call_deferred(history_pane);
			break;
		}
	}
}

// The repository path of a res:// path, or "" outside the repository.
String GitDock::get_repo_path(const String &p_res_path) const {
	if (repo.is_null() || !repo->is_open()) {
		return String();
	}
	const String workdir = repo->get_workdir().simplify_path().trim_suffix("/") + "/";
	const String absolute = ProjectSettings::get_singleton()->globalize_path(p_res_path).simplify_path();
	return absolute.begins_with(workdir) ? absolute.substr(workdir.length()) : String();
}

// History narrowed to one file's commits (from the lists' and the FileSystem dock's right-click
// menus, and the script editor's).
void GitDock::show_file_history(const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}
	history_search->clear();
	history_search_row->hide();
	_set_history_filter(p_path, String());
	_scroll_to_history();
}

// One commit in History, expanded: found by searching for its hash.
void GitDock::show_commit(const String &p_hash) {
	history_expanded[p_hash] = true;
	history_search_row->show();
	history_search->set_text(p_hash.left(10));
	_set_history_filter(String(), p_hash.left(10));
	_scroll_to_history();
	TreeItem *root = history_tree->get_root();
	for (TreeItem *item = root ? root->get_first_child() : nullptr; item; item = item->get_next()) {
		if (row_kind(item) == "commit" && String(Dictionary(item->get_metadata(0))["hash"]) == p_hash) {
			item->select(0);
			break;
		}
	}
}

// Show Commit for This Line, from the script editor: p_text is the editor's text (unsaved edits
// included), p_line 0-based.
void GitDock::show_line_commit(const String &p_path, const String &p_text, int p_line) {
	const Dictionary commit = repo->get_line_commit(p_path, p_text, p_line);
	if (commit.is_empty()) {
		_set_status(STATUS_NEUTRAL, vformat("Line %d of %s isn't committed yet: it's new or changed since the last commit.", p_line + 1, p_path.get_file()));
		return;
	}
	show_commit(commit["hash"]);
}

// Undo Last Commit: its changes go back to Staged Changes, and its message into the box (unless
// you're typing another one) so committing again is one click.
void GitDock::_undo_last_commit() {
	const Dictionary last = repo->get_commit("HEAD");
	const Error err = repo->undo_last_commit();
	_report(err, "Undo commit");
	if (err != OK) {
		return;
	}
	if (commit_message->get_text().strip_edges().is_empty() && !amend_check->is_pressed()) {
		commit_message->set_text(last.get("message", String()));
	}
	refresh();
	_set_status(STATUS_SUCCESS, vformat("Undid \"%s\": its changes are staged again", last.get("summary", String())));
}

void GitDock::_confirm_revert(const String &p_hash, const String &p_summary) {
	pending_revert = p_hash;
	pending_revert_summary = p_summary;
	revert_confirm->set_text(vformat("Revert \"%s\"?\n\nThis adds a new commit that undoes its changes. The commit itself stays in the history.", p_summary));
	revert_confirm->popup_centered();
}

// Like a pull, a revert rewrites files: unsaved scenes and scripts are offered to be saved first,
// and it runs in the background (a hook may run on its commit).
void GitDock::_on_revert_confirmed() {
	if (pending_revert.is_empty()) {
		return;
	}
	if (_ask_to_save("Revert", callable_mp(this, &GitDock::_on_revert_confirmed))) {
		return;
	}
	_start_network(NETWORK_REVERT);
}

// Restore This Version / Restore Version Before This Commit: the file as it was, as an uncommitted
// change. p_what finishes the status line ("as it was in abc1234").
void GitDock::_restore_version(const String &p_revision, const String &p_path, const String &p_what) {
	_remember_open_scenes();
	const Error err = repo->restore_file_version(p_revision, p_path);
	_report(err, "Restore");
	if (err != OK) {
		return;
	}
	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	refresh();
	_set_status(STATUS_SUCCESS, vformat("Restored %s %s", p_path.get_file(), p_what));
	_reload_changed_scenes();
}

// Create Branch Here: a branch at p_hash, staying on the current branch.
void GitDock::_show_branch_here(const String &p_hash) {
	branch_here = p_hash;
	branch_dialog->set_title("Create Branch Here");
	branch_dialog_label->set_text(vformat("Create a branch at commit %s. You stay on %s.", p_hash.left(7), repo->get_current_branch()));
	branch_name_edit->clear();
	branch_dialog->popup_centered(Vector2i(360, 0) * EditorInterface::get_singleton()->get_editor_scale());
	branch_name_edit->grab_focus();
}
