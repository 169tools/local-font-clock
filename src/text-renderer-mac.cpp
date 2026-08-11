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
#include <memory>
#include <string>
#include <utility>

namespace {

/* Outlines scale linearly with point size, so one division lands close enough
 * that a single correcting pass settles it. Measuring from a large reference
 * keeps the rounding error small. */
constexpr double reference_point_size = 100.0;

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

/// Vertical envelope of every digit, at the size `font` was built for.
///
/// Sizing and placement both work from this rather than from a reference string
/// such as `0:00`. Any reference string assumes the digits it happens to hold
/// are the tallest in the face -- true of text faces, not of display or
/// handwritten ones. Where the assumption fails, the requested ink height lands
/// on a digit that is not the tallest, so the clock draws larger than it was
/// asked for and the surplus eats into the top margin. The envelope encloses
/// every reading, so the size the user sets is the height of the tallest digit,
/// the margins hold, and nothing shifts as the time changes.
///
/// Digits only: the date's slash and its weekday capitals rise past them in
/// most faces, and including those would shrink the numbers to compensate.
ink_span digit_envelope(CTFontRef font, CGColorRef color)
{
	ink_span envelope;
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

double envelope_height(const clock_style &style, CGColorRef color, double point_size)
{
	CFPtr<CTFontRef> font = make_font(style, point_size);
	if (!font)
		return 0.0;

	return digit_envelope(font.get(), color).height();
}

/// Finds the point size whose digit envelope is `target_height` tall.
double solve_point_size(const clock_style &style, CGColorRef color, double target_height)
{
	const double reference_height = envelope_height(style, color, reference_point_size);
	if (reference_height <= 0.0)
		return 0.0;

	const double estimate = reference_point_size * target_height / reference_height;

	/* Hinting makes the relationship slightly non-linear, so measure once more
	 * at the estimate and correct by the same ratio. */
	const double actual_height = envelope_height(style, color, estimate);
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

/// Draws a line centered on the rule. The measurement is taken here rather than
/// carried in the frame because it is the one figure that changes with the
/// time: the string's own ink width and left bearing.
void draw_centered(CGContextRef context, CTLineRef line, const clock_frame &frame, double baseline_y)
{
	const double origin_x = center_origin_x(frame, measure(line));

	/* Layout works top-down; Core Graphics draws bottom-up. */
	CGContextSetTextPosition(context, origin_x, frame.height - baseline_y);
	CTLineDraw(line, context);
}

class mac_clock final : public prepared_clock {
public:
	mac_clock(CFPtr<CTFontRef> time_font, CFPtr<CTFontRef> date_font, CFPtr<CGColorRef> color,
		  const clock_frame &frame)
		: time_font_(std::move(time_font)),
		  date_font_(std::move(date_font)),
		  color_(std::move(color)),
		  frame_(frame)
	{
	}

	std::uint32_t width() const noexcept override { return frame_.width; }
	std::uint32_t height() const noexcept override { return frame_.height; }

	rendered_text render(const clock_content &content) const override;

private:
	CFPtr<CTFontRef> time_font_;
	CFPtr<CTFontRef> date_font_;
	CFPtr<CGColorRef> color_;
	clock_frame frame_;
};

rendered_text mac_clock::render(const clock_content &content) const
{
	if (content.time.empty())
		return {};

	CFPtr<CTLineRef> time_line = make_line(content.time, time_font_.get(), color_.get());
	CFPtr<CTLineRef> date_line = make_line(content.date, date_font_.get(), color_.get());
	if (!time_line || !date_line)
		return {};

	rendered_text result;
	result.width = frame_.width;
	result.height = frame_.height;
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

	/* The date's slash and capitals ride past the digits' top, which is the
	 * intended trade: the numbers are the visual subject, so they are what the
	 * margins are measured against. */
	draw_centered(context.get(), time_line.get(), frame_, frame_.time_baseline_y);
	draw_centered(context.get(), date_line.get(), frame_, frame_.date_baseline_y);

	/* The rule takes the text color, so it disappears against a background for
	 * the same reasons the text does, and stays legible for the same fixes. */
	CGContextSetFillColorWithColor(context.get(), color_.get());
	CGContextFillRect(context.get(), CGRectMake(frame_.rule_x, frame_.height - frame_.rule_y - frame_.rule_height,
						    frame_.rule_width, frame_.rule_height));

	return result;
}

} // namespace

std::unique_ptr<prepared_clock> prepare_clock(const clock_style &style)
{
	if (style.time_ink_height <= 0.0 || style.date_ink_height <= 0.0)
		return nullptr;

	CFPtr<CGColorRef> color = make_color(style.color);
	if (!color)
		return nullptr;

	const double time_size = solve_point_size(style, color.get(), style.time_ink_height);
	const double date_size = solve_point_size(style, color.get(), style.date_ink_height);
	if (time_size <= 0.0 || date_size <= 0.0)
		return nullptr;

	CFPtr<CTFontRef> time_font = make_font(style, time_size);
	CFPtr<CTFontRef> date_font = make_font(style, date_size);
	if (!time_font || !date_font)
		return nullptr;

	clock_measurements measurements;
	measurements.time = digit_envelope(time_font.get(), color.get());
	measurements.date = digit_envelope(date_font.get(), color.get());
	measurements.rule_reference_width = rule_reference_width(time_font.get(), color.get());
	if (measurements.rule_reference_width <= 0.0)
		return nullptr;

	const clock_frame frame = solve_frame(measurements);
	if (frame.width == 0 || frame.height == 0)
		return nullptr;

	return std::make_unique<mac_clock>(std::move(time_font), std::move(date_font), std::move(color), frame);
}
