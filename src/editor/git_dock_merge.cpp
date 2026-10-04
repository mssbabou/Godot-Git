// Merging a branch into the current one: an icon button next to the branch picker opens "Merge
// into main", a dialog that stays up while you pick the branch and shows what merging it would do
// (worked out without changing anything, see GitRepository::get_merge_preview) before anything
// happens. The merge itself runs on the worker thread (NETWORK_MERGE), like a pull.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

namespace {

// A branch name short enough for a button: "feature/inventory-sys…".
String short_name(const String &p_name) {
	return p_name.length() > 28 ? p_name.left(26) + String::utf8("…") : p_name;
}

} // namespace

void GitDock::_build_merge_button(Control *p_parent) {
	merge_button = memnew(Button);
	merge_button->set_name("MergeButton");
	merge_button->set_flat(true);
	merge_button->connect("pressed", callable_mp(this, &GitDock::_show_merge_dialog));
	p_parent->add_child(merge_button);

	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	merge_dialog = _make_confirm("Merge", "Merge", callable_mp(this, &GitDock::_on_merge_confirmed));
	merge_dialog->set_autowrap(false);
	merge_dialog->get_label()->set_custom_minimum_size(Vector2());
	VBoxContainer *layout = memnew(VBoxContainer);
	layout->set_custom_minimum_size(Vector2(420 * scale, 0));
	merge_dialog->add_child(layout);

	merge_search = memnew(LineEdit);
	merge_search->set_placeholder("Search branches");
	merge_search->set_clear_button_enabled(true);
	merge_search->connect("text_changed", callable_mp(this, &GitDock::_fill_merge_tree).unbind(1));
	merge_search->connect("text_submitted", callable_mp(this, &GitDock::_on_merge_search_submitted));
	merge_search->connect("gui_input", callable_mp(this, &GitDock::_on_merge_search_input));
	layout->add_child(merge_search);

	merge_tree = memnew(Tree);
	merge_tree->set_hide_root(true);
	merge_tree->set_columns(2);
	merge_tree->set_column_expand(0, true);
	merge_tree->set_column_clip_content(0, true);
	merge_tree->set_column_expand(1, false);
	merge_tree->set_h_scroll_enabled(false);
	merge_tree->set_select_mode(Tree::SELECT_ROW);
	merge_tree->set_custom_minimum_size(Vector2(0, 200 * scale));
	merge_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	merge_tree->connect("item_selected", callable_mp(this, &GitDock::_on_merge_row_selected));
	merge_tree->connect("item_activated", callable_mp(this, &GitDock::_on_merge_search_submitted).bind(String()));
	layout->add_child(merge_tree);

	// What merging the selected branch would do: a line, then the details, dimmed. Wrapping labels
	// need a minimum width (gotcha 28).
	MarginContainer *preview = memnew(MarginContainer);
	preview->add_theme_constant_override("margin_top", Math::round(6 * scale));
	layout->add_child(preview);
	VBoxContainer *preview_box = memnew(VBoxContainer);
	preview->add_child(preview_box);
	merge_summary = memnew(Label);
	merge_summary->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	merge_summary->set_custom_minimum_size(Vector2(420 * scale, 0));
	preview_box->add_child(merge_summary);
	merge_detail = memnew(Label);
	merge_detail->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	merge_detail->set_custom_minimum_size(Vector2(420 * scale, 0));
	preview_box->add_child(merge_detail);
}

// Enabled whenever a merge could happen; otherwise the tooltip says why not.
void GitDock::_update_merge_button(bool p_busy, const String &p_in_operation) {
	const String current = sync_status.get("branch", String());
	bool others = false;
	for (int i = 0; i < branch_list.size(); i++) {
		others = others || !bool(Dictionary(branch_list[i])["current"]);
	}
	String reason;
	if (!p_in_operation.is_empty()) {
		reason = p_in_operation;
	} else if (p_busy) {
		reason = "Merge: wait for the current operation to finish.";
	} else if (!has_commits || current.is_empty() || current == "HEAD") {
		reason = has_commits ? String("Merge: switch to a branch first.") : String("Merge: make a first commit first.");
	} else if (!others) {
		reason = "Merge: there are no other branches to merge.";
	}
	merge_button->set_disabled(!reason.is_empty());
	merge_button->set_tooltip_text(reason.is_empty() ? vformat("Merge a branch into %s...", current) : reason);
}

void GitDock::_show_merge_dialog() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const String current = sync_status.get("branch", String());
	merge_dialog->set_title(vformat("Merge into %s", current));
	merge_branches = repo->get_merge_branches();
	merge_selected = String();
	merge_search->clear();
	Ref<StyleBoxEmpty> none;
	none.instantiate();
	merge_tree->add_theme_stylebox_override("focus", none);
	_fill_merge_tree();
	merge_dialog->popup_centered(Vector2i(460, 0) * scale);
	merge_search->grab_focus();
}

// Local branches, then remote branches without a local one, filtered by the search. Branches with
// nothing to merge stay listed, dimmed, so none seems to be missing; they can't be picked. The
// branch picked before stays picked, else the first that can be.
void GitDock::_fill_merge_tree() {
	const String lower = merge_search->get_text().strip_edges().to_lower();
	const String current = sync_status.get("branch", String());
	const Color dim = _dim_color();
	merge_tree->clear();
	TreeItem *root = merge_tree->create_item();
	TreeItem *keep = nullptr;
	TreeItem *first = nullptr;
	bool remote_heading = false;
	for (int i = 0; i < merge_branches.size(); i++) {
		const Dictionary branch = merge_branches[i];
		const String name = branch["name"];
		if (!lower.is_empty() && !name.to_lower().contains(lower)) {
			continue;
		}
		const bool local = branch["local"];
		if (!local && !remote_heading) {
			remote_heading = true;
			TreeItem *heading = merge_tree->create_item(root);
			heading->set_text(0, "Remote branches");
			heading->set_custom_color(0, dim);
			heading->set_selectable(0, false);
			heading->set_selectable(1, false);
		}
		const int commits = branch["commits"];
		const int behind = branch["behind"];
		const int64_t time = branch["time"];
		TreeItem *item = merge_tree->create_item(root);
		item->set_metadata(0, name);
		item->set_text(0, name);
		item->set_icon(0, get_theme_icon("VcsBranches", "EditorIcons"));
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		item->set_text_overrun_behavior(1, TextServer::OVERRUN_NO_TRIMMING); // Gotcha 4.
		item->set_custom_color(1, dim);
		if (commits == 0) {
			item->set_text(1, "nothing to merge");
			item->set_custom_color(0, dim);
			item->set_icon_modulate(0, Color(1, 1, 1, 0.5));
			item->set_selectable(0, false);
			item->set_selectable(1, false);
			item->set_tooltip_text(0, vformat("%s\n%s has every commit on it already.", name, current));
		} else {
			// Against the current branch: ahead is what merging brings in.
			PackedStringArray parts;
			parts.push_back(vformat("%d ahead", commits));
			if (behind > 0) {
				parts.push_back(vformat("%d behind", behind));
			}
			if (time > 0) {
				parts.push_back(relative_time(time));
			}
			item->set_text(1, String::utf8(" · ").join(parts));
			if (!local) {
				item->set_icon_modulate(0, Color(1, 1, 1, 0.5));
			}
			item->set_tooltip_text(0, vformat("%s%s\n%d ahead: %s %s doesn't have, which merging brings in.%s", name, local ? String() : String(" (on the remote)"), commits, plural(commits, "commit", "commits"), current, behind > 0 ? vformat("\n%d behind: %s has %s it doesn't.", behind, current, plural(behind, "commit", "commits")) : String()));
			first = first ? first : item;
			keep = name == merge_selected ? item : keep;
		}
		item->set_tooltip_text(1, item->get_tooltip_text(0));
	}
	if (!root->get_first_child()) {
		TreeItem *none = merge_tree->create_item(root);
		none->set_text(0, "No branch has that name.");
		none->set_custom_color(0, dim);
		none->set_selectable(0, false);
		none->set_selectable(1, false);
	}
	TreeItem *pick = keep ? keep : first;
	if (pick) {
		pick->select(0); // Emits item_selected: the preview follows.
		merge_tree->scroll_to_item(pick);
	} else {
		merge_selected = String();
		_update_merge_preview();
	}
}

void GitDock::_on_merge_row_selected() {
	TreeItem *item = merge_tree->get_selected();
	merge_selected = item ? String(item->get_metadata(0)) : String();
	_update_merge_preview();
}

// "Merge feature into main", for the button.
String GitDock::_merge_branch_label() const {
	return vformat("Merge %s into %s", short_name(merge_selected), short_name(sync_status.get("branch", String())));
}

void GitDock::_update_merge_preview() {
	const String current = sync_status.get("branch", String());
	Button *ok = merge_dialog->get_ok_button();
	merge_preview = merge_selected.is_empty() ? Dictionary() : repo->get_merge_preview(merge_selected);
	const String problem = merge_preview.get("problem", String());
	const int commits = merge_preview.get("commits", 0);
	const int files = merge_preview.get("files", 0);
	const PackedStringArray conflicts = merge_preview.get("conflicts", PackedStringArray());
	const PackedStringArray carried = merge_preview.get("carried", PackedStringArray());
	const int behind_remote = merge_preview.get("behind_remote", 0);

	merge_summary->remove_theme_color_override("font_color");
	merge_detail->add_theme_color_override("font_color", _dim_color());
	String summary;
	PackedStringArray details;
	if (merge_selected.is_empty()) {
		summary = "Pick the branch to merge into " + current + ".";
	} else if (!problem.is_empty()) {
		summary = problem;
		merge_summary->add_theme_color_override("font_color", get_theme_color("error_color", "Editor"));
	} else if (commits == 0) {
		summary = vformat("%s has everything on %s already.", current, merge_selected);
	} else {
		const String size = vformat("%s, %s", plural(commits, "commit", "commits"), plural(files, "file", "files"));
		if (!conflicts.is_empty()) {
			summary = vformat("%s. %s: the merge stops there for you to resolve.", size, plural(conflicts.size(), "conflict", "conflicts"));
			merge_summary->add_theme_color_override("font_color", get_theme_color("warning_color", "Editor"));
			details.push_back(join_list(conflicts, 5));
			details.push_back("Abort Merge puts everything back as it was.");
		} else if (merge_preview.get("fast_forward", false)) {
			summary = vformat("%s. No conflicts: %s moves forward to %s, no merge commit needed.", size, current, merge_selected);
		} else {
			summary = vformat("%s. No conflicts.", size);
		}
		if (!carried.is_empty()) {
			details.push_back(vformat("Your uncommitted edits to %s are merged into the new %s.", join_list(carried, 3), carried.size() == 1 ? "version" : "versions"));
		}
	}
	if (behind_remote > 0 && problem.is_empty()) {
		details.push_back(vformat("%s has %s that %s doesn't; they aren't included.", String(merge_preview.get("remote_branch", String())), plural(behind_remote, "newer commit", "newer commits"), merge_selected));
	}
	merge_summary->set_text(summary);
	merge_detail->set_text(String("\n").join(details));
	merge_detail->set_visible(!details.is_empty());

	ok->set_text(merge_selected.is_empty() ? String("Merge") : _merge_branch_label());
	ok->set_disabled(merge_selected.is_empty() || !problem.is_empty() || commits == 0);
	ok->set_tooltip_text(conflicts.is_empty() ? String() : String("Merge, and stop at the conflicts to resolve them under Conflicts. Abort Merge puts everything back as it was."));
	merge_dialog->reset_size(); // As tall as the preview needs, no taller.
}

void GitDock::_on_merge_confirmed() {
	if (merge_selected.is_empty() || merge_dialog->get_ok_button()->is_disabled()) {
		return;
	}
	network_branch = merge_selected;
	const PackedStringArray conflicts = merge_preview.get("conflicts", PackedStringArray());
	const NetworkOp op = conflicts.is_empty() ? NETWORK_MERGE : NETWORK_MERGE_START;
	// Anything but a fast-forward makes a merge commit, which records your name and email.
	if (!bool(merge_preview.get("fast_forward", false)) && _ask_identity(op)) {
		return;
	}
	_start_network(op);
}

// Enter in the search (or a double-click): merge the selected branch, as the button would.
void GitDock::_on_merge_search_submitted(const String &p_text) {
	if (merge_dialog->get_ok_button()->is_disabled()) {
		return;
	}
	merge_dialog->hide();
	_on_merge_confirmed();
}

// Up and Down in the search move through the branches, like the branch picker.
void GitDock::_on_merge_search_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || (key->get_keycode() != KEY_UP && key->get_keycode() != KEY_DOWN)) {
		return;
	}
	const bool down = key->get_keycode() == KEY_DOWN;
	TreeItem *item = merge_tree->get_selected();
	item = item ? (down ? item->get_next() : item->get_prev()) : merge_tree->get_root()->get_first_child();
	while (item && !item->is_selectable(0)) {
		item = down ? item->get_next() : item->get_prev();
	}
	if (item) {
		item->select(0);
		merge_tree->scroll_to_item(item);
	}
	merge_search->accept_event();
}
