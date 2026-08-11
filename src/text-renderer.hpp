/*
Oshi-Font Clock
Copyright (C) 2026 mizznoff <mizznoff@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// What to draw, in terms the platform back end can act on.
struct text_style {
	std::string face;
	std::string style;

	/// Target height of the drawn ink, in pixels. The point size is solved for
	/// rather than given, so that swapping typefaces keeps the visual size.
	double ink_height = 44.0;

	/// OBS byte order: red in the low byte, as vec4_from_rgba expects.
	std::uint32_t color = 0xffffffff;
};

/// A CPU-side bitmap ready to be handed to the graphics subsystem.
struct rendered_text {
	std::uint32_t width = 0;
	std::uint32_t height = 0;

	/// RGBA, premultiplied alpha, rows running top to bottom.
	std::vector<std::uint8_t> pixels;

	bool valid() const noexcept { return width > 0 && height > 0 && !pixels.empty(); }
};

/// Rasterises `text` in the requested style. Returns an empty result if the
/// text is blank or the platform could not produce a bitmap.
rendered_text render_text(const std::string &text, const text_style &style);
