// The Diff panel's audio view: a sound's old and new version side by side, each with its length
// (and for WAV its sample rate and channels), a waveform, and a button to play it, or why there's
// none (new, deleted, not downloaded from Git LFS).

#include "editor/git_diff_dock.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/audio_stream_ogg_vorbis.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/math.hpp>

#include "editor/ui_text.h"

using namespace godot_git;

namespace {

constexpr int WAVEFORM_POINTS = 600;
// Longer sounds get no waveform: decoding them on the main thread would stall the editor.
constexpr double WAVEFORM_MAX_SECONDS = 300;

Ref<AudioStream> decode_audio(const PackedByteArray &p_bytes, const String &p_extension) {
	if (p_extension == "wav") {
		// Uncompressed, so the samples can be compared ("same samples").
		Dictionary options;
		options["compress/mode"] = 0;
		return AudioStreamWAV::load_from_buffer(p_bytes, options);
	}
	if (p_extension == "ogg") {
		return AudioStreamOggVorbis::load_from_buffer(p_bytes);
	}
	if (p_extension == "mp3") {
		return AudioStreamMP3::load_from_buffer(p_bytes);
	}
	return Ref<AudioStream>();
}

// The loudest sample in each of WAVEFORM_POINTS slices, 0 to 1, by playing the sound into memory.
PackedFloat32Array waveform(const Ref<AudioStream> &p_stream) {
	PackedFloat32Array peaks;
	const double length = p_stream->get_length();
	if (length <= 0 || length > WAVEFORM_MAX_SECONDS) {
		return peaks;
	}
	Ref<AudioStreamPlayback> playback = p_stream->instantiate_playback();
	if (playback.is_null()) {
		return peaks;
	}
	const int64_t total = (int64_t)(length * AudioServer::get_singleton()->get_mix_rate());
	if (total <= 0) {
		return peaks;
	}
	peaks.resize(WAVEFORM_POINTS);
	peaks.fill(0);
	playback->start(0);
	int64_t done = 0;
	while (done < total && playback->is_playing()) {
		const int chunk = (int)MIN((int64_t)4096, total - done);
		const PackedVector2Array frames = playback->mix_audio(1.0, chunk);
		if (frames.is_empty()) {
			break;
		}
		for (int i = 0; i < frames.size(); i++) {
			const int slot = (int)MIN((int64_t)WAVEFORM_POINTS - 1, (done + i) * WAVEFORM_POINTS / total);
			const float level = MAX(Math::abs(frames[i].x), Math::abs(frames[i].y));
			peaks.set(slot, MAX(peaks[slot], MIN(level, 1.0f)));
		}
		done += frames.size();
	}
	playback->stop();
	return peaks;
}

String length_text(double p_seconds) {
	if (p_seconds < 60) {
		return vformat("%.1f s", p_seconds);
	}
	return vformat("%d:%02d", (int)p_seconds / 60, (int)p_seconds % 60);
}

String audio_size_text(int64_t p_bytes) {
	if (p_bytes < 1024) {
		return vformat("%d bytes", p_bytes);
	}
	if (p_bytes < 1024 * 1024) {
		return vformat("%d KB", (p_bytes + 512) / 1024);
	}
	return vformat("%.1f MB", p_bytes / (1024.0 * 1024.0));
}

} // namespace

bool GitDiffDock::is_audio_path(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	return extension == "wav" || extension == "ogg" || extension == "mp3";
}

void GitDiffDock::_make_audio_side(int p_index, Control *p_parent) {
	AudioSide &side = audio_sides[p_index];
	VBoxContainer *column = memnew(VBoxContainer);
	column->set_h_size_flags(SIZE_EXPAND_FILL);
	p_parent->add_child(column);

	HBoxContainer *top = memnew(HBoxContainer);
	column->add_child(top);
	side.play = memnew(Button);
	side.play->set_flat(true);
	side.play->connect("pressed", callable_mp(this, &GitDiffDock::_on_audio_play).bind(p_index));
	top->add_child(side.play);
	side.caption = memnew(Label);
	side.caption->set_h_size_flags(SIZE_EXPAND_FILL);
	side.caption->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	top->add_child(side.caption);

	side.frame = memnew(PanelContainer);
	side.frame->set_v_size_flags(SIZE_EXPAND_FILL);
	column->add_child(side.frame);
	side.wave = memnew(Control);
	side.wave->connect("draw", callable_mp(this, &GitDiffDock::_draw_audio_side).bind(p_index));
	side.frame->add_child(side.wave);
	side.note = memnew(Label);
	side.note->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	side.note->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	side.note->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	side.frame->add_child(side.note);
}

// The waveform as bars around the middle line, and where playback is while it plays.
void GitDiffDock::_draw_audio_side(int p_index) {
	const AudioSide &side = audio_sides[p_index];
	const Size2 area = side.wave->get_size();
	if (side.peaks.is_empty() || area.x <= 0) {
		return;
	}
	const float middle = area.y / 2;
	const Color color = p_index == 0 ? theme.removed : theme.added;
	// Both sides on one time scale, so a longer or shorter sound shows as one.
	double longest = 0;
	for (const AudioSide &other : audio_sides) {
		longest = MAX(longest, other.stream.is_valid() ? other.stream->get_length() : 0.0);
	}
	const float width = longest > 0 ? (float)(area.x * side.stream->get_length() / longest) : area.x;
	const float step = width / side.peaks.size();
	for (int i = 0; i < side.peaks.size(); i++) {
		const float half = MAX(0.5f, side.peaks[i] * (middle - 2 * theme.scale));
		side.wave->draw_rect(Rect2(i * step, middle - half, MAX(1.0f, step), half * 2), color * Color(1, 1, 1, 0.8));
	}
	side.wave->draw_line(Vector2(0, middle), Vector2(width, middle), theme.dim * Color(1, 1, 1, 0.5));
	if (audio_playing == p_index && audio_player->is_playing() && side.stream->get_length() > 0) {
		const float x = (float)(audio_player->get_playback_position() / side.stream->get_length()) * width;
		side.wave->draw_line(Vector2(x, 0), Vector2(x, area.y), theme.dim, 2 * theme.scale);
	}
}

// Before | after for a sound: each side's length and waveform, or why there's none.
void GitDiffDock::_show_audio() {
	_stop_audio();
	const String extension = String(diff.get("path", String())).get_extension().to_lower();
	const bool shown = diff.has("audio_new");
	for (int i = 0; i < 2; i++) {
		AudioSide &side = audio_sides[i];
		const Dictionary version = diff.get(i == 0 ? "audio_old" : "audio_new", Dictionary());
		const bool conflict = diff.has("conflict");
		const String label = diff.get(i == 0 ? "mine_label" : "theirs_label", String());
		String caption = conflict ? (i == 0 ? String("Mine") : String("Theirs")) : (i == 0 ? String("Before") : String("After"));
		if (conflict && !label.is_empty()) {
			caption += vformat(String::utf8(" · %s"), label);
		}
		String note;
		side.stream = Ref<AudioStream>();
		side.peaks = PackedFloat32Array();
		if (!shown) {
			// Not shown; drop the sounds.
		} else if (!bool(version.get("exists", false))) {
			note = conflict ? String("Deleted on this side.") : (i == 0 ? String("Not in the old version: this is a new file.") : String("Not in the new version: the file is deleted."));
		} else if (String(version.get("lfs", String())) == "missing") {
			note = "Stored with Git LFS, and this version hasn't been downloaded.";
		} else {
			const PackedByteArray bytes = version["bytes"];
			side.stream = decode_audio(bytes, extension);
			if (side.stream.is_valid() && side.stream->get_length() > 0) {
				caption += vformat(String::utf8(" · %s"), length_text(side.stream->get_length()));
				const Ref<AudioStreamWAV> wav = side.stream;
				if (wav.is_valid()) {
					caption += vformat(String::utf8(" · %.1f kHz · %s"), wav->get_mix_rate() / 1000.0, wav->is_stereo() ? String("stereo") : String("mono"));
				}
				caption += vformat(String::utf8(" · %s"), audio_size_text(bytes.size()));
				side.peaks = waveform(side.stream);
				if (side.peaks.is_empty()) {
					note = vformat("Too long to draw (over %d minutes); it still plays.", (int)WAVEFORM_MAX_SECONDS / 60);
				}
			} else {
				side.stream = Ref<AudioStream>();
				note = "Couldn't read this sound.";
			}
		}
		side.caption->set_text(caption);
		side.note->set_text(note);
		side.note->set_visible(!note.is_empty());
		side.play->set_visible(side.stream.is_valid());
		side.play->set_tooltip_text(vformat("Play the %s version", i == 0 ? (conflict ? "mine" : "old") : (conflict ? "theirs" : "new")));
		side.wave->queue_redraw();
	}
	// Saved again without a change you could hear: say so, or it looks like a missed edit.
	const Ref<AudioStreamWAV> old_wav = audio_sides[0].stream;
	const Ref<AudioStreamWAV> new_wav = audio_sides[1].stream;
	if (old_wav.is_valid() && new_wav.is_valid() && old_wav->get_mix_rate() == new_wav->get_mix_rate() && old_wav->is_stereo() == new_wav->is_stereo() && old_wav->get_data() == new_wav->get_data()) {
		audio_sides[1].caption->set_text(audio_sides[1].caption->get_text() + String::utf8(" · same samples"));
		audio_sides[1].caption->set_tooltip_text("Sample for sample the same sound; only the file changed (saved again, or its metadata).");
	} else {
		audio_sides[1].caption->set_tooltip_text(String());
	}
	_update_audio_buttons();
}

// One sound at a time: playing a side stops the other; pressing a playing side stops it.
void GitDiffDock::_on_audio_play(int p_index) {
	const bool was_playing = audio_playing == p_index && audio_player->is_playing();
	_stop_audio();
	if (was_playing || audio_sides[p_index].stream.is_null()) {
		return;
	}
	audio_player->set_stream(audio_sides[p_index].stream);
	audio_player->play();
	audio_playing = p_index;
	set_process(true);
	_update_audio_buttons();
}

void GitDiffDock::_stop_audio() {
	if (audio_player && audio_player->is_playing()) {
		audio_player->stop();
	}
	if (audio_playing >= 0) {
		audio_sides[audio_playing].wave->queue_redraw();
	}
	audio_playing = -1;
	set_process(false);
	_update_audio_buttons();
}

void GitDiffDock::_update_audio_buttons() {
	for (int i = 0; i < 2; i++) {
		const bool playing = audio_playing == i;
		audio_sides[i].play->set_button_icon(get_theme_icon(playing ? "Stop" : "Play", "EditorIcons"));
	}
}

// While a sound plays: move the line on its waveform, and notice when it's done.
void GitDiffDock::_process_audio() {
	if (audio_playing < 0) {
		set_process(false);
		return;
	}
	if (!audio_player->is_playing()) {
		_stop_audio();
		return;
	}
	audio_sides[audio_playing].wave->queue_redraw();
}
