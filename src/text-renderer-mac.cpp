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

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <cmath>

namespace {

/* Outlines scale linearly with point size, so one division lands close enough
 * that a single correcting pass settles it. Measuring from a large reference
 * keeps the rounding error small. */
constexpr double reference_point_size = 100.0;

/* Antialiased edges reach a fraction of a pixel past the glyph outline. */
constexpr int bitmap_margin = 2;

CFPtr<CFStringRef> make_cfstring(const std::string &value)
{
	return CFPtr<CFStringRef>(
		CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(value.data()),
					static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false));
}

/// Resolves a family/style pair -- what the OBS font property gives us -- into
/// a font at the requested point size.
CFPtr<CTFontRef> make_font(const text_style &style, double point_size)
{
	CFPtr<CFStringRef> face = make_cfstring(style.face);
	if (!face)
		return {};

	CFPtr<CFMutableDictionaryRef> attributes(CFDictionaryCreateMutable(
		nullptr, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
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

	CFPtr<CFAttributedStringRef> attributed(
		CFAttributedStringCreate(nullptr, string.get(), attributes.get()));
	if (!attributed)
		return {};

	return CFPtr<CTLineRef>(CTLineCreateWithAttributedString(attributed.get()));
}

/// Extent of the drawn ink, relative to the baseline. Deliberately not the
/// typographic bounds: the em box and the line box both include space the glyph
/// does not use, and how much varies per typeface, which is exactly what makes
/// point sizes incomparable.
CGRect ink_bounds(CTLineRef line)
{
	return CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
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
double solve_point_size(const std::string &text, const text_style &style, CGColorRef color, double target_height)
{
	CFPtr<CTFontRef> probe = make_font(style, reference_point_size);
	if (!probe)
		return 0.0;

	CFPtr<CTLineRef> line = make_line(text, probe.get(), color);
	if (!line)
		return 0.0;

	const double reference_height = ink_bounds(line.get()).size.height;
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

	const double actual_height = ink_bounds(corrected_line.get()).size.height;
	if (actual_height <= 0.0)
		return estimate;

	return estimate * target_height / actual_height;
}

} // namespace

rendered_text render_text(const std::string &text, const text_style &style)
{
	if (text.empty() || style.ink_height <= 0.0)
		return {};

	CFPtr<CGColorRef> color = make_color(style.color);
	if (!color)
		return {};

	const double point_size = solve_point_size(text, style, color.get(), style.ink_height);
	if (point_size <= 0.0)
		return {};

	CFPtr<CTFontRef> font = make_font(style, point_size);
	if (!font)
		return {};

	CFPtr<CTLineRef> line = make_line(text, font.get(), color.get());
	if (!line)
		return {};

	const CGRect bounds = ink_bounds(line.get());
	if (bounds.size.width <= 0.0 || bounds.size.height <= 0.0)
		return {};

	rendered_text result;
	result.width = static_cast<std::uint32_t>(std::ceil(bounds.size.width)) + bitmap_margin * 2;
	result.height = static_cast<std::uint32_t>(std::ceil(bounds.size.height)) + bitmap_margin * 2;
	result.pixels.assign(static_cast<std::size_t>(result.width) * result.height * 4, 0);

	CFPtr<CGColorSpaceRef> space(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
	if (!space)
		return {};

	/* PremultipliedLast with 32Big byte order lays the channels out as R, G, B,
	 * A in memory, which is what GS_RGBA expects, and the premultiplication
	 * matches OBS_EFFECT_PREMULTIPLIED_ALPHA. */
	/* C++20 deprecated bitwise ops between distinct enum types, and these two
	 * constants come from different ones, so the combination is spelled out. */
	const CGBitmapInfo bitmap_info = static_cast<CGBitmapInfo>(
		static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
		static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));

	CFPtr<CGContextRef> context(CGBitmapContextCreate(
		result.pixels.data(), result.width, result.height, 8, static_cast<std::size_t>(result.width) * 4,
		space.get(), bitmap_info));
	if (!context)
		return {};

	CGContextSetShouldAntialias(context.get(), true);
	CGContextSetShouldSmoothFonts(context.get(), false);

	/* Core Graphics puts the drawing origin at the bottom left, so the baseline
	 * sits `bitmap_margin` above the bottom edge plus however far the ink
	 * descends. Rows are still stored top to bottom, so the bitmap comes out the
	 * right way up and needs no flip on upload. */
	CGContextSetTextPosition(context.get(), bitmap_margin - bounds.origin.x,
				 bitmap_margin - bounds.origin.y);
	CTLineDraw(line.get(), context.get());

	return result;
}
