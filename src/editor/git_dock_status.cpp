// The status strip: what the panel is doing, or what it last did. Every result and error stays
// here until the next one, so an outcome is never missed.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "editor/ui_text.h"

using namespace godot_git;

void GitDock::_build_status_strip(Control *p_parent) {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();

	// Status strip: one line that always answers "what is the panel doing, and what did it last
	// do?", with a thin progress bar under it while something runs.
	status_strip = memnew(PanelContainer);
	status_strip->hide();
	p_parent->add_child(status_strip);

	VBoxContainer *status_vb = memnew(VBoxContainer);
	status_vb->add_theme_constant_override("separation", Math::round(3 * scale));
	status_strip->add_child(status_vb);

	HBoxContainer *status_hb = memnew(HBoxContainer);
	status_vb->add_child(status_hb);

	status_icon = memnew(TextureRect);
	status_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	status_icon->set_v_size_flags(SIZE_SHRINK_BEGIN);
	status_hb->add_child(status_icon);

	// Wraps rather than trims: a result or an error is only useful if all of it can be read.
	// Selectable, so an error can be copied.
	status_label = memnew(RichTextLabel);
	status_label->set_h_size_flags(SIZE_EXPAND_FILL);
	status_label->set_fit_content(true);
	status_label->set_scroll_active(false);
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status_label->set_selection_enabled(true);
	status_label->set_context_menu_enabled(true);
	status_hb->add_child(status_label);

	status_log_button = memnew(Button);
	status_log_button->set_flat(true);
	status_log_button->set_v_size_flags(SIZE_SHRINK_BEGIN);
	status_log_button->set_tooltip_text("Earlier results: what the panel did this session.");
	status_log_button->hide();
	status_log_button->connect("pressed", callable_mp(this, &GitDock::_show_status_log));
	status_hb->add_child(status_log_button);

	status_log_popup = memnew(PopupPanel);
	add_child(status_log_popup);
	status_log_label = memnew(RichTextLabel);
	status_log_label->set_fit_content(true);
	status_log_label->set_scroll_active(true);
	status_log_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status_log_label->set_selection_enabled(true);
	status_log_label->set_context_menu_enabled(true);
	status_log_popup->add_child(status_log_label);

	status_button = memnew(Button);
	status_button->set_flat(true);
	status_button->set_v_size_flags(SIZE_SHRINK_BEGIN);
	status_button->connect("pressed", callable_mp(this, &GitDock::_on_status_button));
	status_hb->add_child(status_button);

	status_progress = memnew(ProgressBar);
	status_progress->set_show_percentage(false);
	status_progress->set_max(1.0);
	status_progress->set_step(0.0);
	status_progress->set_custom_minimum_size(Vector2(0, Math::round(3 * scale)));
	status_vb->add_child(status_progress);

	status_timer = memnew(Timer);
	status_timer->set_wait_time(30.0);
	status_timer->set_autostart(true);
	status_timer->connect("timeout", callable_mp(this, &GitDock::_update_status));
	add_child(status_timer);
}

void GitDock::_report(Error p_err, const String &p_action) {
	if (p_err == OK) {
		return;
	}
	String message = GitRepository::get_last_error();
	if (message.is_empty()) {
		message = UtilityFunctions::error_string(p_err);
	}
	_set_status(STATUS_ERROR, vformat("%s failed. %s", p_action, message));
}

// Replaces what the strip shows. Results stay until the next one; there are no timeouts.
// If the dock is hidden behind another tab, a toast says it too, so nothing goes unnoticed.
void GitDock::_set_status(StatusKind p_kind, const String &p_text) {
	// The result being replaced goes into the log, so nothing that was shown is lost.
	if (status_kind != STATUS_BUSY && status_kind != STATUS_IDLE && !status_text.is_empty()) {
		Dictionary entry;
		entry["kind"] = (int)status_kind;
		entry["text"] = status_text;
		entry["time"] = status_time;
		status_log.push_back(entry);
		if (status_log.size() > 50) {
			status_log.pop_front();
		}
	}
	status_kind = p_kind;
	status_text = p_text;
	status_step = String();
	status_time = (int64_t)Time::get_singleton()->get_unix_time_from_system();
	status_cancellable = false;
	_update_status_style();
	_update_status();

	if (p_kind != STATUS_BUSY && p_kind != STATUS_IDLE && !is_visible_in_tree()) {
		const EditorToaster::Severity severity = p_kind == STATUS_ERROR ? EditorToaster::SEVERITY_ERROR : (p_kind == STATUS_WARNING ? EditorToaster::SEVERITY_WARNING : EditorToaster::SEVERITY_INFO);
		EditorInterface::get_singleton()->get_editor_toaster()->push_toast("Git: " + p_text, severity);
	}
}

void GitDock::_update_status() {
	if (!repo.is_valid() || !repo->is_open()) {
		return;
	}
	String text = status_text;
	String detail; // Dim, after the text: the current step, or "5m ago".
	String tooltip;

	switch (status_kind) {
		case STATUS_IDLE: {
			// Nothing to report yet: say how fresh the Pull/Push counts are.
			if (!sync_status.get("has_remotes", false)) {
				status_strip->hide();
				return;
			}
			const int64_t fetched = sync_status.get("last_fetched", 0);
			text = fetched > 0 ? vformat("Last fetched %s", time_ago(fetched)) : String("Not fetched yet");
			tooltip = "Pull and Push counts are as of the last fetch. Fetch to check the remote for new commits.";
		} break;
		case STATUS_BUSY: {
			detail = status_step;
		} break;
		case STATUS_SUCCESS:
		case STATUS_NEUTRAL: {
			detail = time_ago(status_time);
		} break;
		case STATUS_WARNING:
		case STATUS_ERROR: {
		} break;
	}

	// Plain text through add_text, never BBCode: branch names and error messages are user data.
	const bool dim = status_kind == STATUS_IDLE || status_kind == STATUS_NEUTRAL;
	status_label->clear();
	if (dim) {
		status_label->push_color(_dim_color());
	}
	status_label->add_text(text);
	if (dim) {
		status_label->pop();
	}
	if (!detail.is_empty()) {
		// While busy the step gets its own line, so the strip doesn't change height as the
		// numbers tick. "· 5m ago" stays in one piece (non-breaking spaces) at the end of the text.
		status_label->push_color(_dim_color());
		if (status_kind == STATUS_BUSY) {
			status_label->add_text("\n" + detail);
		} else {
			status_label->add_text(String::utf8("  · ") + detail.replace(" ", String::utf8(" ")));
		}
		status_label->pop();
	}
	status_label->set_tooltip_text(tooltip);
	status_progress->set_visible(status_kind == STATUS_BUSY);

	const bool dismissable = status_kind == STATUS_WARNING || status_kind == STATUS_ERROR;
	status_button->set_visible(dismissable || (status_kind == STATUS_BUSY && status_cancellable));
	status_log_button->set_visible(!status_log.is_empty() && status_kind != STATUS_BUSY);
	status_button->set_tooltip_text(status_kind == STATUS_BUSY ? String("Cancel") : String("Dismiss"));
	status_strip->show();
}

// Icon and colors per kind. Separate from _update_status, which runs on every progress tick.
void GitDock::_update_status_style() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	Color tint(0, 0, 0, 0);
	String icon;
	switch (status_kind) {
		case STATUS_SUCCESS: {
			icon = "StatusSuccess";
		} break;
		case STATUS_WARNING: {
			icon = "StatusWarning";
			tint = get_theme_color("warning_color", "Editor");
		} break;
		case STATUS_ERROR: {
			icon = "StatusError";
			tint = get_theme_color("error_color", "Editor");
		} break;
		default:
			break;
	}

	// Problems sit on a faint tint of their color, so they read as "needs your attention" and
	// not as just another line of text. Everything else is flush with the rest of the dock.
	const Ref<StyleBoxFlat> panel = _tinted_panel(tint);
	panel->set_content_margin(SIDE_RIGHT, tint.a > 0 ? Math::round(2 * scale) : 0);
	panel->set_content_margin(SIDE_TOP, Math::round(2 * scale));
	panel->set_content_margin(SIDE_BOTTOM, Math::round(2 * scale));
	status_strip->add_theme_stylebox_override("panel", panel);

	status_icon->set_texture(icon.is_empty() ? Ref<Texture2D>() : get_theme_icon(icon, "EditorIcons"));
	status_log_button->set_button_icon(get_theme_icon("History", "EditorIcons"));
	status_icon->set_visible(!icon.is_empty());
	// As tall as one line of text, so the icon is centered on the first line when the text wraps.
	const Ref<Font> font = status_label->get_theme_font("normal_font");
	status_icon->set_custom_minimum_size(Vector2(0, font->get_height(status_label->get_theme_font_size("normal_font_size"))));

	status_progress->set_indeterminate(true);
	status_progress->set_value(0);
}

void GitDock::_on_status_button() {
	if (status_kind == STATUS_BUSY) {
		GitRepository::cancel_network();
		status_step = "Canceling...";
		status_cancellable = false;
		_update_status();
	} else {
		_set_status(STATUS_IDLE, String());
	}
}

// The earlier results, newest first, each with its icon and how long ago.
void GitDock::_show_status_log() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const int icon_size = Math::round(16 * scale);
	status_log_label->clear();
	for (int i = status_log.size() - 1; i >= 0; i--) {
		const Dictionary entry = status_log[i];
		const int kind = entry["kind"];
		const char *icon = kind == STATUS_SUCCESS ? "StatusSuccess" : (kind == STATUS_WARNING ? "StatusWarning" : (kind == STATUS_ERROR ? "StatusError" : nullptr));
		if (icon) {
			status_log_label->add_image(get_theme_icon(icon, "EditorIcons"), icon_size, icon_size);
			status_log_label->add_text(" ");
		}
		if (kind == STATUS_NEUTRAL) {
			status_log_label->push_color(_dim_color());
		}
		status_log_label->add_text(entry["text"]); // Plain text: messages are user data.
		if (kind == STATUS_NEUTRAL) {
			status_log_label->pop();
		}
		status_log_label->push_color(_dim_color());
		status_log_label->add_text(String::utf8("  · ") + time_ago(entry["time"]).replace(" ", String::utf8(" ")));
		status_log_label->pop();
		if (i > 0) {
			status_log_label->add_text("\n\n");
		}
	}
	const float width = MAX(status_strip->get_size().x, 300 * scale);
	status_log_label->set_custom_minimum_size(Vector2(width, 0));
	status_log_popup->reset_size();
	const Vector2 at = status_strip->get_screen_position() + Vector2(0, status_strip->get_size().y);
	status_log_popup->popup(Rect2i(at, Vector2(width, MIN(status_log_label->get_content_height() + 16 * scale, 400 * scale))));
}

// The strip's and the operation banner's background: a faint rounded tint of p_tint (transparent
// for none), with the text starting where the commit message's text does.
Ref<StyleBoxFlat> GitDock::_tinted_panel(const Color &p_tint) const {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	Ref<StyleBoxFlat> panel;
	panel.instantiate();
	panel->set_bg_color(Color(p_tint, p_tint.a * 0.15));
	panel->set_corner_radius_all(Math::round(3 * scale));
	panel->set_content_margin(SIDE_LEFT, commit_message->get_theme_stylebox("normal")->get_margin(SIDE_LEFT));
	return panel;
}

// The operation banner: shown while the repository is in the middle of a merge, rebase,
// cherry-pick, revert, `git am` or bisect (usually one a terminal stopped at conflicts). Without
// it the panel would show the conflicted files as ordinary changes, and a plain commit would
// quietly drop the merge; Commit, Pull and switching branches refuse meanwhile.
void GitDock::_build_operation_banner(Control *p_parent) {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	operation_banner = memnew(PanelContainer);
	operation_banner->hide();
	p_parent->add_child(operation_banner);

	VBoxContainer *vb = memnew(VBoxContainer);
	operation_banner->add_child(vb);

	HBoxContainer *text_hb = memnew(HBoxContainer);
	vb->add_child(text_hb);
	TextureRect *icon = memnew(TextureRect);
	icon->set_name("Icon");
	icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	icon->set_v_size_flags(SIZE_SHRINK_BEGIN);
	text_hb->add_child(icon);
	operation_label = memnew(Label);
	operation_label->set_h_size_flags(SIZE_EXPAND_FILL);
	operation_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	// A wrapping label needs a minimum width, even in a hidden dock (gotcha 45).
	operation_label->set_custom_minimum_size(Vector2(160 * scale, 0));
	text_hb->add_child(operation_label);

	HBoxContainer *buttons = memnew(HBoxContainer);
	buttons->set_alignment(BoxContainer::ALIGNMENT_END);
	vb->add_child(buttons);
	operation_abort = memnew(Button);
	operation_abort->connect("pressed", callable_mp(this, &GitDock::_on_operation_abort));
	buttons->add_child(operation_abort);
	operation_continue = memnew(Button);
	operation_continue->connect("pressed", callable_mp(this, &GitDock::_start_network).bind(NETWORK_CONTINUE));
	buttons->add_child(operation_continue);

	// Title and button are set when it opens (they name the operation).
	abort_confirm = _make_confirm(String(), String(), callable_mp(this, &GitDock::_start_network).bind(NETWORK_ABORT));
}

bool GitDock::_in_operation() const {
	return !String(operation.get("kind", String())).is_empty();
}

// "merge", "rebase", "cherry-pick", "revert", "git am", "bisect".
String GitDock::_operation_name() const {
	const String kind = operation.get("kind", String());
	if (kind == "pull") {
		return "merge"; // Your edits merged with a pull (see GitRepository::pull): a merge to you.
	}
	if (kind == "stash") {
		return "stash restore"; // A stash restored into conflicts (see GitRepository::restore_stash).
	}
	return kind == "apply" ? String("git am") : kind;
}

void GitDock::_update_operation_banner() {
	operation_banner->set_visible(_in_operation());
	if (!_in_operation()) {
		return;
	}
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const String raw_kind = operation["kind"];
	// The panel's own operations: no git needed to finish or abort them.
	const bool own = raw_kind == "pull" || raw_kind == "stash";
	const String kind = raw_kind == "pull" ? String("merge") : raw_kind;
	const String name = _operation_name();
	const String subject = operation.get("subject", String());
	const PackedStringArray conflicts = operation.get("conflicts", PackedStringArray());

	String text;
	if (kind == "apply") {
		text = "Applying patches with git am is in progress.";
	} else if (kind == "bisect") {
		text = "A git bisect is in progress: the files are those of the commit it's testing.";
	} else if (subject.is_empty()) {
		text = vformat("A %s is in progress.", name);
	} else if (kind == "merge") {
		text = vformat("A merge is in progress: %s.", subject);
	} else if (kind == "stash") {
		text = vformat("Restoring the stash of %s.", subject);
	} else {
		text = vformat("A %s of %s is in progress.", name, subject);
	}
	if (!conflicts.is_empty()) {
		text += vformat(" %s: resolve %s under Conflicts.", plural(conflicts.size(), "file has conflicts", "files have conflicts"), conflicts.size() == 1 ? "it" : "them");
	} else if (kind == "merge") {
		text += " No conflicts left: finish the merge when you're ready.";
	} else if (kind == "stash") {
		text += " No conflicts left: finish to remove the stash.";
	} else if (kind != "bisect") {
		text += " No conflicts left: continue to finish it.";
	}
	operation_label->set_text(text);
	operation_label->set_tooltip_text(conflicts.is_empty() ? String() : vformat("Conflicted (marked ! in Changes):\n%s", String("\n").join(conflicts)));

	const bool busy = _shown_network_op() != NETWORK_NONE;
	const String needs_git = git_missing && !own ? vformat("Finishing or aborting the %s needs git, which isn't installed (or isn't on the PATH). Install it from git-scm.com.", name) : String();
	operation_abort->set_text(kind == "bisect" ? String("End Bisect") : (kind == "apply" ? String("Abort") : (kind == "stash" ? String("Abort Restore") : vformat("Abort %s", name.capitalize().replace(" ", "-")))));
	operation_abort->set_disabled(busy || !needs_git.is_empty());
	if (kind == "stash") {
		operation_abort->set_tooltip_text("Undo the restore: the stash's files go back to how they were, and the stash is kept.");
	} else {
		operation_abort->set_tooltip_text(!needs_git.is_empty() ? needs_git : (kind == "bisect" ? String("End the bisect: go back to the branch you started it on.") : vformat("Undo the %s: the branch and files go back to how they were before it started.", name)));
	}

	operation_continue->set_visible(kind != "bisect");
	operation_continue->set_text(kind == "merge" ? String("Finish Merge") : (kind == "stash" ? String("Finish Restore") : String("Continue")));
	operation_continue->set_disabled(busy || !conflicts.is_empty() || !needs_git.is_empty());
	if (!needs_git.is_empty()) {
		operation_continue->set_tooltip_text(needs_git);
	} else if (!conflicts.is_empty()) {
		operation_continue->set_tooltip_text(vformat("Resolve the conflicts first: %s still %s them.", plural(conflicts.size(), "file", "files"), conflicts.size() == 1 ? "has" : "have"));
	} else {
		if (kind == "stash") {
			operation_continue->set_tooltip_text("Finish the restore: the stash is removed, its changes stay as uncommitted changes.");
		} else if (raw_kind == "pull") {
			operation_continue->set_tooltip_text("Finish the merge: your resolved changes stay uncommitted, as before the pull.");
		} else {
			operation_continue->set_tooltip_text(kind == "merge" ? String("Finish the merge: a merge commit with git's prepared message.") : vformat("Let the %s go on: it may stop at conflicts again.", name));
		}
	}

	// Warning-tinted like the strip's warnings: it needs attention, but nothing failed.
	const Color tint = get_theme_color("warning_color", "Editor");
	const Ref<StyleBoxFlat> panel = _tinted_panel(tint);
	for (const Side side : { SIDE_TOP, SIDE_RIGHT, SIDE_BOTTOM }) {
		panel->set_content_margin(side, Math::round(4 * scale));
	}
	operation_banner->add_theme_stylebox_override("panel", panel);
	Object::cast_to<TextureRect>(operation_banner->find_child("Icon", true, false))->set_texture(get_theme_icon("StatusWarning", "EditorIcons"));
}

void GitDock::_on_operation_abort() {
	const String name = _operation_name();
	abort_confirm->set_title(operation_abort->get_text());
	abort_confirm->set_ok_button_text(operation_abort->get_text());
	abort_confirm->set_text(String(operation["kind"]) == "bisect"
					? String("End the bisect and go back to the branch you started it on?")
					: vformat("Abort the %s? Everything it changed is undone, including conflicts you've already resolved, and the branch goes back to where it was before it started.", name));
	abort_confirm->popup_centered();
}
