// The Diff panel's image view: an image's old and new version side by side, each with its size,
// or why there's none (new, deleted, not downloaded from Git LFS).

#include "editor/git_diff_dock.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/math.hpp>

#include "editor/ui_text.h"

using namespace godot_git;

namespace {

// The image formats Godot can read from bytes, by extension.
const char *IMAGE_EXTENSIONS[] = { "png", "jpg", "jpeg", "webp", "bmp", "tga", "svg", "exr", "dds" };

// r_size is the image's own size; an SVG is rendered larger than that, so it stays sharp when
// it's shown bigger (it's a vector image).
Ref<Image> decode_image(const PackedByteArray &p_bytes, const String &p_extension, Size2i &r_size) {
	Ref<Image> image;
	image.instantiate();
	Error err = ERR_FILE_UNRECOGNIZED;
	if (p_extension == "png") {
		err = image->load_png_from_buffer(p_bytes);
	} else if (p_extension == "jpg" || p_extension == "jpeg") {
		err = image->load_jpg_from_buffer(p_bytes);
	} else if (p_extension == "webp") {
		err = image->load_webp_from_buffer(p_bytes);
	} else if (p_extension == "bmp") {
		err = image->load_bmp_from_buffer(p_bytes);
	} else if (p_extension == "tga") {
		err = image->load_tga_from_buffer(p_bytes);
	} else if (p_extension == "svg") {
		err = image->load_svg_from_buffer(p_bytes);
		r_size = err == OK ? image->get_size() : Size2i();
		const int largest = MAX(r_size.x, r_size.y);
		if (err == OK && largest > 0 && largest < 1024) {
			err = image->load_svg_from_buffer(p_bytes, 1024.0f / largest);
		}
		return err == OK && !image->is_empty() ? image : Ref<Image>();
	} else if (p_extension == "exr") {
		err = image->load_exr_from_buffer(p_bytes);
	} else if (p_extension == "dds") {
		err = image->load_dds_from_buffer(p_bytes);
	}
	r_size = err == OK ? image->get_size() : Size2i();
	return err == OK && !image->is_empty() ? image : Ref<Image>();
}

String file_size_text(int64_t p_bytes) {
	if (p_bytes < 1024) {
		return vformat("%d bytes", p_bytes);
	}
	if (p_bytes < 1024 * 1024) {
		return vformat("%d KB", (p_bytes + 512) / 1024);
	}
	return vformat("%.1f MB", p_bytes / (1024.0 * 1024.0));
}

} // namespace

bool GitDiffDock::is_image_path(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	for (const char *image_extension : IMAGE_EXTENSIONS) {
		if (extension == image_extension) {
			return true;
		}
	}
	return false;
}

void GitDiffDock::_make_image_side(int p_index, Control *p_parent) {
	ImageSide &side = image_sides[p_index];
	VBoxContainer *column = memnew(VBoxContainer);
	column->set_h_size_flags(SIZE_EXPAND_FILL);
	p_parent->add_child(column);

	side.caption = memnew(Label);
	side.caption->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	column->add_child(side.caption);

	// The frame lays its children over each other: the picture, or the note.
	side.frame = memnew(PanelContainer);
	side.frame->set_v_size_flags(SIZE_EXPAND_FILL);
	column->add_child(side.frame);
	side.picture = memnew(Control);
	side.picture->connect("draw", callable_mp(this, &GitDiffDock::_draw_image_side).bind(p_index));
	side.frame->add_child(side.picture);
	side.note = memnew(Label);
	side.note->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	side.note->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	side.note->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	side.frame->add_child(side.note);
}

// The picture as large as fits, centered, on a checkerboard of exactly its size (so transparent
// parts show, and where the image ends is clear).
void GitDiffDock::_draw_image_side(int p_index) {
	const ImageSide &side = image_sides[p_index];
	if (side.texture.is_null()) {
		return;
	}
	const Size2 area = side.picture->get_size();
	const Size2 size = side.texture->get_size();
	const float scale = MIN(area.x / size.x, area.y / size.y);
	const Size2 shown = (size * scale).floor();
	const Rect2 rect(((area - shown) / 2).floor(), shown);
	side.picture->draw_texture_rect(theme.checkerboard, rect, true);
	side.picture->draw_texture_rect(side.texture, rect, false);
}

// Before | after for an image: each side's picture with its size, or why there's none.
void GitDiffDock::_show_images() {
	const String extension = String(diff.get("path", String())).get_extension().to_lower();
	Ref<Image> images[2];
	Size2i sizes[2];
	for (int i = 0; i < 2; i++) {
		ImageSide &side = image_sides[i];
		const Dictionary version = diff.get(i == 0 ? "image_old" : "image_new", Dictionary());
		// A conflict's sides are mine and theirs, named after their branches.
		const bool conflict = diff.has("conflict");
		const String label = diff.get(i == 0 ? "mine_label" : "theirs_label", String());
		String caption = conflict ? (i == 0 ? String("Mine") : String("Theirs")) : (i == 0 ? String("Before") : String("After"));
		if (conflict && !label.is_empty()) {
			caption += vformat(String::utf8(" · %s"), label);
		}
		String note;
		if (_current_view() != VIEW_IMAGE) {
			// Not shown; drop the textures.
		} else if (!bool(version.get("exists", false))) {
			note = conflict ? String("Deleted on this side.") : (i == 0 ? String("Not in the old version: this is a new file.") : String("Not in the new version: the file is deleted."));
		} else if (String(version.get("lfs", String())) == "missing") {
			note = "Stored with Git LFS, and this version hasn't been downloaded.";
		} else {
			const PackedByteArray bytes = version["bytes"];
			images[i] = decode_image(bytes, extension, sizes[i]);
			if (images[i].is_valid()) {
				caption += vformat(String::utf8(" · %d×%d · %s"), sizes[i].x, sizes[i].y, file_size_text(bytes.size()));
			} else {
				note = "Couldn't read this image.";
			}
		}
		side.caption->set_text(caption);
		side.note->set_text(note);
		side.note->set_visible(!note.is_empty());
		side.texture = images[i].is_valid() ? Ref<Texture2D>(ImageTexture::create_from_image(images[i])) : Ref<Texture2D>();
		// Small images are mostly pixel art: scaled up without blurring. SVGs are rendered large.
		const bool small = images[i].is_valid() && extension != "svg" && MAX(sizes[i].x, sizes[i].y) <= 256;
		side.picture->set_texture_filter(small ? TEXTURE_FILTER_NEAREST : TEXTURE_FILTER_LINEAR);
		side.picture->queue_redraw();
	}
	// Re-saved or re-compressed without a visible change: say so, or it looks like a missed edit.
	if (images[0].is_valid() && images[1].is_valid() && images[0]->get_size() == images[1]->get_size() && images[0]->get_format() == images[1]->get_format() && images[0]->get_data() == images[1]->get_data()) {
		image_sides[1].caption->set_text(image_sides[1].caption->get_text() + String::utf8(" · same pixels"));
		image_sides[1].caption->set_tooltip_text("Pixel for pixel the same image; only the file changed (saved again, or compressed differently).");
	} else {
		image_sides[1].caption->set_tooltip_text(String());
	}
}
