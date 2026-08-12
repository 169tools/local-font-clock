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
#include <memory>
#include <string>
#include <vector>

/// What to draw, in terms the platform back end can act on.
struct clock_style {
	std::string face;
	std::string style;

	/// Target height of the time row's ink, in pixels. The point size is solved
	/// for rather than given, so that swapping typefaces keeps the visual size.
	double time_ink_height = 44.0;

	/// The date row rides on the time row's height rather than being set
	/// independently, so the proportion between the rows survives a size change.
	double date_ink_height = 18.0;

	/// Letter spacing, as a share of each row's own point size. Negative tightens
	/// and zero leaves the face alone; opening the spacing up is not offered.
	///
	/// Per row rather than one figure for both, and relative rather than in
	/// pixels, because the two rows are set at different sizes: the same pixel
	/// amount would bite twice as hard into the date, and tight small text is
	/// the wrong way round -- small text wants more air, not less.
	double time_tracking_em = 0.0;
	double date_tracking_em = 0.0;

	/// OBS byte order: red in the low byte, as vec4_from_rgba expects. The rule
	/// takes the same color as the text, since it reads as part of the mark.
	std::uint32_t color = 0xffffffff;
};

/// The strings to typeset. Kept apart from the style so the caller can rebuild
/// them every minute without touching anything else.
struct clock_content {
	std::string time;
	std::string date;
};

/// A CPU-side bitmap ready to be handed to the graphics subsystem.
struct rendered_text {
	std::uint32_t width = 0;
	std::uint32_t height = 0;

	/// RGBA, premultiplied alpha, rows running top to bottom.
	std::vector<std::uint8_t> pixels;

	bool valid() const noexcept { return width > 0 && height > 0 && !pixels.empty(); }
};

/// A clock whose typeface is resolved and whose geometry is solved: everything
/// that follows from the style and not from the time.
///
/// Resolving the face, solving the point size against the digit envelope and
/// measuring the widest digit are all comparatively expensive, and none of them
/// change from one minute to the next. Holding on to this means a minute
/// rolling over costs two strings' worth of typesetting and nothing else.
///
/// The back end supplies the subclass; callers only ever see this interface,
/// which is what keeps the platform types out of the rest of the plugin.
class prepared_clock {
public:
	virtual ~prepared_clock() = default;

	prepared_clock(const prepared_clock &) = delete;
	prepared_clock &operator=(const prepared_clock &) = delete;

	/// Canvas size, fixed for the life of the object. It follows the widest
	/// digit rather than the time on screen, so the source does not resize
	/// itself as the minutes go by -- and the texture can be reused.
	virtual std::uint32_t width() const noexcept = 0;
	virtual std::uint32_t height() const noexcept = 0;

	/// Typesets the whole clock -- time, rule and date -- into one bitmap.
	/// Returns an empty result if the strings could not be laid out.
	virtual rendered_text render(const clock_content &content) const = 0;

protected:
	prepared_clock() = default;
};

/// Returns null if the platform could not prepare the clock: no back end for
/// this OS, or a style whose face would not resolve.
std::unique_ptr<prepared_clock> prepare_clock(const clock_style &style);
