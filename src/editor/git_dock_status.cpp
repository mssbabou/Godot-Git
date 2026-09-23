// The status strip: what the panel is doing, or what it last did. See CLAUDE.md, "Status and
// feedback".

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
	Ref<StyleBoxFlat> panel;
	panel.instantiate();
	panel->set_bg_color(Color(tint, tint.a * 0.15));
	panel->set_corner_radius_all(Math::round(3 * scale));
	// Text starts where the commit message's text does.
	panel->set_content_margin(SIDE_LEFT, commit_message->get_theme_stylebox("normal")->get_margin(SIDE_LEFT));
	panel->set_content_margin(SIDE_RIGHT, tint.a > 0 ? Math::round(2 * scale) : 0);
	panel->set_content_margin(SIDE_TOP, Math::round(2 * scale));
	panel->set_content_margin(SIDE_BOTTOM, Math::round(2 * scale));
	status_strip->add_theme_stylebox_override("panel", panel);

	status_icon->set_texture(icon.is_empty() ? Ref<Texture2D>() : get_theme_icon(icon, "EditorIcons"));
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
