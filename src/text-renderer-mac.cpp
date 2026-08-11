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

#include "text-renderer.hpp"

#include "cf-ptr.hpp"
#include "layout.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <string>

namespace {

/* Outlines scale linearly with point size, so one division lands close enough
 * that a single correcting pass settles it. Measuring from a large reference
 * keeps the rounding error small. */
constexpr double reference_point_size = 100.0;

/* Sizes are solved against these rather than against the text on screen, so
 * that the digits stay put no matter what the clock currently reads. The date's
 * reference leaves out the slash, which is taller than a digit in most faces
 * and would otherwise shrink the numbers to compensate. */
constexpr const char *time_reference_text = "0:00";
constexpr const char *date_reference_text = "1230";

CFPtr<CFStringRef> make_cfstring(const std::string &value)
{
	return CFPtr<CFStringRef>(CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(value.data()),
							  static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8,
							  false));
}

/// Resolves a family/style pair -- what the OBS font property gives us -- into
/// a font at the requested point size.
CFPtr<CTFontRef> make_font(const clock_style &style, double point_size)
{
	CFPtr<CFStringRef> face = make_cfstring(style.face);
	if (!face)
		return {};

	CFPtr<CFMutableDictionaryRef> attributes(CFDictionaryCreateMutable(nullptr, 2, &kCFTypeDictionaryKeyCallBacks,
									   &kCFTypeDictionaryValueCallBacks));
	if (!attributes)
		return {};

	CFDictionarySetValue(attributes.get(), kCTFontFamilyNameAttribute, face.get());

	CFPtr<CFStringRef> style_name;
	if (!style.style.empty()) {
		style_name = make_cfstring(style.style);
		if (style_name)
			CFDictionarySetValue(attributes.get(), kCTFontStyleNameAttribute, style_name.get());
	}

	CFPtr<CTFontDescriptorRef> descriptor(CTFontDescriptorCreateWithAttributes(attributes.get()));
	if (!descriptor)
		return {};

	return CFPtr<CTFontRef>(CTFontCreateWithFontDescriptor(descriptor.get(), point_size, nullptr));
}

CFPtr<CTLineRef> make_line(const std::string &text, CTFontRef font, CGColorRef color)
{
	CFPtr<CFStringRef> string = make_cfstring(text);
	if (!string)
		return {};

	const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
	const void *values[] = {font, color};

	CFPtr<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
							     &kCFTypeDictionaryValueCallBacks));
	if (!attributes)
		return {};

	CFPtr<CFAttributedStringRef> attributed(CFAttributedStringCreate(nullptr, string.get(), attributes.get()));
	if (!attributed)
		return {};

	return CFPtr<CTLineRef>(CTLineCreateWithAttributedString(attributed.get()));
}

/// Extent of the drawn ink, relative to the baseline. Deliberately not the
/// typographic bounds: the em box and the line box both include space the glyph
/// does not use, and how much varies per typeface, which is exactly what makes
/// point sizes incomparable.
ink_extents measure(CTLineRef line)
{
	const CGRect bounds = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);

	ink_extents extents;
	extents.width = bounds.size.width;
	extents.left = bounds.origin.x;
	extents.ascent = bounds.origin.y + bounds.size.height;
	extents.descent = -bounds.origin.y;
	return extents;
}

CFPtr<CGColorRef> make_color(std::uint32_t rgba)
{
	CFPtr<CGColorSpaceRef> space(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
	if (!space)
		return {};

	const CGFloat components[] = {
		static_cast<CGFloat>(rgba & 0xff) / 255.0,
		static_cast<CGFloat>((rgba >> 8) & 0xff) / 255.0,
		static_cast<CGFloat>((rgba >> 16) & 0xff) / 255.0,
		static_cast<CGFloat>((rgba >> 24) & 0xff) / 255.0,
	};

	return CFPtr<CGColorRef>(CGColorCreate(space.get(), components));
}

/// Finds the point size whose ink is `target_height` tall.
double solve_point_size(const std::string &text, const clock_style &style, CGColorRef color, double target_height)
{
	CFPtr<CTFontRef> probe = make_font(style, reference_point_size);
	if (!probe)
		return 0.0;

	CFPtr<CTLineRef> line = make_line(text, probe.get(), color);
	if (!line)
		return 0.0;

	const double reference_height = measure(line.get()).height();
	if (reference_height <= 0.0)
		return 0.0;

	const double estimate = reference_point_size * target_height / reference_height;

	/* Hinting makes the relationship slightly non-linear, so measure once more
	 * at the estimate and correct by the same ratio. */
	CFPtr<CTFontRef> corrected = make_font(style, estimate);
	if (!corrected)
		return estimate;

	CFPtr<CTLineRef> corrected_line = make_line(text, corrected.get(), color);
	if (!corrected_line)
		return estimate;

	const double actual_height = measure(corrected_line.get()).height();
	if (actual_height <= 0.0)
		return estimate;

	return estimate * target_height / actual_height;
}

/// Width the rule is pinned to: the widest digit, repeated across the time
/// format. Measured per digit rather than assuming zero is widest, because it
/// is not in every face -- and if it is not, 12:34 would overhang a rule sized
/// from 00:00.
double rule_reference_width(CTFontRef font, CGColorRef color)
{
	std::string widest = "0";
	double widest_width = 0.0;

	for (char digit = '0'; digit <= '9'; ++digit) {
		const std::string candidate(1, digit);
		CFPtr<CTLineRef> line = make_line(candidate, font, color);
		if (!line)
			continue;

		const double width = measure(line.get()).width;
		if (width > widest_width) {
			widest_width = width;
			widest = candidate;
		}
	}

	const std::string reference = widest + widest + ":" + widest + widest;
	CFPtr<CTLineRef> line = make_line(reference, font, color);
	return line ? measure(line.get()).width : 0.0;
}

/// Vertical envelope of every digit. Rows are placed against this rather than
/// against the string on screen, so the layout does not shift as the time
/// changes, and rather than against a single reference digit, so a face whose
/// zero is short does not let taller digits push past the margin.
ink_extents digit_envelope(CTFontRef font, CGColorRef color)
{
	ink_extents envelope;
	bool seen = false;

	for (char digit = '0'; digit <= '9'; ++digit) {
		CFPtr<CTLineRef> line = make_line(std::string(1, digit), font, color);
		if (!line)
			continue;

		const ink_extents extents = measure(line.get());
		if (!seen) {
			envelope.ascent = extents.ascent;
			envelope.descent = extents.descent;
			seen = true;
			continue;
		}

		envelope.ascent = std::max(envelope.ascent, extents.ascent);
		envelope.descent = std::max(envelope.descent, extents.descent);
	}

	return envelope;
}

void draw_line_at(CGContextRef context, CTLineRef line, double origin_x, double baseline_y, double canvas_height)
{
	/* Layout works top-down; Core Graphics draws bottom-up. */
	CGContextSetTextPosition(context, origin_x, canvas_height - baseline_y);
	CTLineDraw(line, context);
}

} // namespace

rendered_text render_clock(const clock_content &content, const clock_style &style)
{
	if (content.time.empty() || style.time_ink_height <= 0.0 || style.date_ink_height <= 0.0)
		return {};

	CFPtr<CGColorRef> color = make_color(style.color);
	if (!color)
		return {};

	const double time_size = solve_point_size(time_reference_text, style, color.get(), style.time_ink_height);
	const double date_size = solve_point_size(date_reference_text, style, color.get(), style.date_ink_height);
	if (time_size <= 0.0 || date_size <= 0.0)
		return {};

	CFPtr<CTFontRef> time_font = make_font(style, time_size);
	CFPtr<CTFontRef> date_font = make_font(style, date_size);
	if (!time_font || !date_font)
		return {};

	CFPtr<CTLineRef> time_line = make_line(content.time, time_font.get(), color.get());
	CFPtr<CTLineRef> date_line = make_line(content.date, date_font.get(), color.get());
	if (!time_line || !date_line)
		return {};

	/* The size is solved against 0:00, so that is what the spacing ratios scale
	 * off. Placement is a separate question and uses the digit envelope. */
	CFPtr<CTLineRef> time_reference = make_line(time_reference_text, time_font.get(), color.get());
	if (!time_reference)
		return {};

	clock_measurements measurements;
	measurements.scale_height = measure(time_reference.get()).height();

	/* Widths come from the strings being drawn -- those are what gets centred --
	 * while the vertical figures come from the envelope. The date's slash and
	 * capitals still ride past the digits' top, which is the intended trade: the
	 * numbers are the visual subject, so they are what stays put. */
	measurements.time = measure(time_line.get());
	measurements.date = measure(date_line.get());

	const ink_extents time_envelope = digit_envelope(time_font.get(), color.get());
	const ink_extents date_envelope = digit_envelope(date_font.get(), color.get());
	measurements.time.ascent = time_envelope.ascent;
	measurements.time.descent = time_envelope.descent;
	measurements.date.ascent = date_envelope.ascent;
	measurements.date.descent = date_envelope.descent;
	measurements.rule_reference_width = rule_reference_width(time_font.get(), color.get());
	if (measurements.rule_reference_width <= 0.0)
		return {};

	const clock_layout layout = solve_layout(measurements);
	if (layout.width == 0 || layout.height == 0)
		return {};

	rendered_text result;
	result.width = layout.width;
	result.height = layout.height;
	result.pixels.assign(static_cast<std::size_t>(result.width) * result.height * 4, 0);

	CFPtr<CGColorSpaceRef> space(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
	if (!space)
		return {};

	/* PremultipliedLast with 32Big byte order lays the channels out as R, G, B,
	 * A in memory, which is what GS_RGBA expects, and the premultiplication
	 * matches OBS_EFFECT_PREMULTIPLIED_ALPHA.
	 *
	 * C++20 deprecated bitwise ops between distinct enum types, and these two
	 * constants come from different ones, so the combination is spelled out. */
	const CGBitmapInfo bitmap_info =
		static_cast<CGBitmapInfo>(static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
					  static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));

	CFPtr<CGContextRef> context(CGBitmapContextCreate(result.pixels.data(), result.width, result.height, 8,
							  static_cast<std::size_t>(result.width) * 4, space.get(),
							  bitmap_info));
	if (!context)
		return {};

	CGContextSetShouldAntialias(context.get(), true);
	CGContextSetShouldSmoothFonts(context.get(), false);

	draw_line_at(context.get(), time_line.get(), layout.time_origin_x, layout.time_baseline_y, layout.height);
	draw_line_at(context.get(), date_line.get(), layout.date_origin_x, layout.date_baseline_y, layout.height);

	/* The rule takes the text colour, so it disappears against a background for
	 * the same reasons the text does, and stays legible for the same fixes. */
	CGContextSetFillColorWithColor(context.get(), color.get());
	CGContextFillRect(context.get(), CGRectMake(layout.rule_x, layout.height - layout.rule_y - layout.rule_height,
						    layout.rule_width, layout.rule_height));

	return result;
}
