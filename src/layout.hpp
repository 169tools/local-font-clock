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

#include <cmath>
#include <cstdint>

/// Placement is worked out from measured ink rather than from font metrics.
/// Ascent, descent, side bearings and line height all vary per typeface, so
/// laying out against them gives a different result for every font -- which is
/// the opposite of what this clock is for. Everything below is expressed in
/// terms of where the ink actually lands.
///
/// This lives apart from the platform back ends so the arithmetic is written
/// once; a back end supplies measurements and does the drawing.

/// Extent of drawn ink, relative to the text origin (baseline at y = 0).
///
/// Both vertical figures are signed, and a back end must pass them through as
/// it finds them. `ascent` is the ink's top edge measured upwards from the
/// baseline; `descent` is its bottom edge measured downwards. Either can come
/// out negative -- text sitting entirely above the baseline gives a negative
/// descent -- and `solve_layout` depends on that, since a gap is measured from
/// where the ink stops rather than from the baseline. Clamping to zero would
/// silently change the gaps and the canvas height, and would do so on one
/// platform and not the other.
struct ink_extents {
	double width = 0.0;
	double ascent = 0.0;  ///< ink top, measured up from the baseline. May be negative.
	double descent = 0.0; ///< ink bottom, measured down from the baseline. May be negative.
	double left = 0.0;    ///< ink's left edge, relative to the origin

	double height() const noexcept { return ascent + descent; }
};

struct clock_measurements {
	/// Vertical figures are the envelope of every digit, not the extents of the
	/// string currently showing. Placing rows by what is on screen would make the
	/// clock hop as the time changed, and placing them by a single reference
	/// digit assumes zero is the tallest -- true of text faces, not of display or
	/// handwritten ones, where the clock then eats into its own margin whenever
	/// the time contains a taller digit. The envelope is fixed and encloses every
	/// reading, so the margins hold and nothing moves.
	///
	/// Horizontal figures still come from the string being drawn, since that is
	/// what has to be centred.
	ink_extents time;
	ink_extents date;

	/// Ink height the point size was solved for. The ratios below scale off this
	/// rather than off the envelope: a face with one tall outlier of a digit
	/// should not end up with every gap inflated to match.
	double scale_height = 0.0;

	/// Ink width of the widest digit repeated across the time format. The rule
	/// is pinned to this rather than to the current time, so it does not twitch
	/// when the clock goes from 9:59 to 10:00.
	double rule_reference_width = 0.0;
};

/// Positions in pixels, y measured downwards from the top of the canvas.
struct clock_layout {
	std::uint32_t width = 0;
	std::uint32_t height = 0;

	double time_origin_x = 0.0;
	double time_baseline_y = 0.0;

	double date_origin_x = 0.0;
	double date_baseline_y = 0.0;

	double rule_x = 0.0;
	double rule_y = 0.0;
	double rule_width = 0.0;
	double rule_height = 0.0;
};

/// Every distance below is a share of the time row's ink height, not a pixel
/// count. Pixels only meant something while that height was pinned: once the
/// user can set it, a 13px gap is invisible on a tall clock and overwhelming on
/// a short one. The figures are the original pixel values over the height they
/// were tuned against.
inline constexpr double reference_ink_height = 44.0;

/// Gaps are clearance, not centre-to-centre: they are measured from the rule's
/// edges, so the ink-to-ink distance across the rule is 13 + 2 + 8.
inline constexpr double rule_thickness_ratio = 2.0 / reference_ink_height;
inline constexpr double time_to_rule_gap_ratio = 13.0 / reference_ink_height;
inline constexpr double rule_to_date_gap_ratio = 8.0 / reference_ink_height;

/// The rule reaches past the time's ink on both sides.
inline constexpr double rule_overhang_ratio = 2.0 / reference_ink_height;

/// Top and bottom are measured from the ink; the sides are measured from the
/// rule, because the rule is the outermost thing drawn. The values differ
/// because equal ones do not look equal -- the rule is wide, which makes the
/// side margins read as larger than they are.
inline constexpr double margin_vertical_ratio = 24.0 / reference_ink_height;
inline constexpr double margin_horizontal_ratio = 20.0 / reference_ink_height;

inline clock_layout solve_layout(const clock_measurements &measurements)
{
	clock_layout layout;

	const double scale = measurements.scale_height;
	const double rule_thickness = rule_thickness_ratio * scale;
	const double time_to_rule_gap = time_to_rule_gap_ratio * scale;
	const double rule_to_date_gap = rule_to_date_gap_ratio * scale;
	const double rule_overhang = rule_overhang_ratio * scale;
	const double margin_vertical = margin_vertical_ratio * scale;
	const double margin_horizontal = margin_horizontal_ratio * scale;

	layout.rule_width = measurements.rule_reference_width + rule_overhang * 2.0;
	layout.rule_height = rule_thickness;
	layout.rule_x = margin_horizontal;

	layout.time_baseline_y = margin_vertical + measurements.time.ascent;
	layout.rule_y = layout.time_baseline_y + measurements.time.descent + time_to_rule_gap;

	const double date_ink_top = layout.rule_y + rule_thickness + rule_to_date_gap;
	layout.date_baseline_y = date_ink_top + measurements.date.ascent;

	const double canvas_width = layout.rule_width + margin_horizontal * 2.0;
	const double canvas_height = layout.date_baseline_y + measurements.date.descent + margin_vertical;

	layout.width = static_cast<std::uint32_t>(std::ceil(canvas_width));
	layout.height = static_cast<std::uint32_t>(std::ceil(canvas_height));

	/* Both rows centre on the rule, not on each other: the rule is the fixed
	 * element, and letting it absorb the time's changing width is the reason
	 * dropping the leading zero does not shift the layout. */
	const double centre_x = layout.rule_x + layout.rule_width / 2.0;
	layout.time_origin_x = centre_x - measurements.time.width / 2.0 - measurements.time.left;
	layout.date_origin_x = centre_x - measurements.date.width / 2.0 - measurements.date.left;

	return layout;
}
