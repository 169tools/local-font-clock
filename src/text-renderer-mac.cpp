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
#include <vector>

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

/// Everything a row is set with. Bundled because tracking has to reach the
/// measurements as well as the drawing -- the rule follows the time's ink width,
/// so tightening the digits has to narrow the rule with them -- and every one of
/// those paths goes through make_line. Passing it alongside the font is what
/// stops one of them being forgotten.
///
/// A view, not an owner: the refs belong to the prepared clock.
struct row_style {
	CTFontRef font = nullptr;
	CGColorRef color = nullptr;

	/// Share of the point size, so the two rows tighten in proportion.
	double tracking_em = 0.0;
};

CFPtr<CTLineRef> make_line(const std::string &text, const row_style &row)
{
	CFPtr<CFStringRef> string = make_cfstring(text);
	if (!string)
		return {};

	/* Core Text wants points; the setting is relative to the row's own size.
	 *
	 * kCTTrackingAttributeName rather than kCTKernAttributeName: zero is this
	 * one's "leave it alone", where a zero kern means "no kerning at all" and
	 * would throw away the pairs the face itself specifies. It also counts the
	 * space it adds after the last glyph as trailing whitespace, which keeps it
	 * out of the ink bounds the layout centers on. */
	const double tracking_points = row.tracking_em * CTFontGetSize(row.font);
	CFPtr<CFNumberRef> tracking(CFNumberCreate(nullptr, kCFNumberDoubleType, &tracking_points));
	if (!tracking)
		return {};

	const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTTrackingAttributeName};
	const void *values[] = {row.font, row.color, tracking.get()};

	CFPtr<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
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

/// Black at the fixed opacity, independent of the text colour.
CFPtr<CGColorRef> make_shadow_color()
{
	CFPtr<CGColorSpaceRef> space(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
	if (!space)
		return {};

	const CGFloat components[] = {0.0, 0.0, 0.0, static_cast<CGFloat>(shadow_opacity)};
	return CFPtr<CGColorRef>(CGColorCreate(space.get(), components));
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
ink_span digit_envelope(const row_style &row)
{
	ink_span envelope;
	bool seen = false;

	for (char digit = '0'; digit <= '9'; ++digit) {
		CFPtr<CTLineRef> line = make_line(std::string(1, digit), row);
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

double envelope_height(const clock_style &style, CGColorRef color, double point_size, double tracking_em)
{
	CFPtr<CTFontRef> font = make_font(style, point_size);
	if (!font)
		return 0.0;

	return digit_envelope({font.get(), color, tracking_em}).height();
}

/// Finds the point size whose digit envelope is `target_height` tall.
///
/// Tracking is carried through for consistency but cannot move the answer: the
/// envelope is measured a digit at a time, and the space tracking adds falls
/// after the glyph, outside its ink. That is the behaviour to want -- the clock
/// keeps the height it was set to while the spacing is adjusted, instead of
/// growing and shrinking under the slider.
double solve_point_size(const clock_style &style, CGColorRef color, double target_height, double tracking_em)
{
	const double reference_height = envelope_height(style, color, reference_point_size, tracking_em);
	if (reference_height <= 0.0)
		return 0.0;

	const double estimate = reference_point_size * target_height / reference_height;

	/* Hinting makes the relationship slightly non-linear, so measure once more
	 * at the estimate and correct by the same ratio. */
	const double actual_height = envelope_height(style, color, estimate, tracking_em);
	if (actual_height <= 0.0)
		return estimate;

	return estimate * target_height / actual_height;
}

/// Width the rule is pinned to: the widest digit, repeated across the time
/// format. Measured per digit rather than assuming zero is widest, because it
/// is not in every face -- and if it is not, 12:34 would overhang a rule sized
/// from 00:00.
///
/// Measured with the row's tracking applied, so tightening the digits pulls the
/// rule in with them rather than leaving it hanging past the ink it is meant to
/// be the width of.
double rule_reference_width(const row_style &row)
{
	std::string widest = "0";
	double widest_width = 0.0;

	for (char digit = '0'; digit <= '9'; ++digit) {
		const std::string candidate(1, digit);
		CFPtr<CTLineRef> line = make_line(candidate, row);
		if (!line)
			continue;

		const double width = measure(line.get()).width;
		if (width > widest_width) {
			widest_width = width;
			widest = candidate;
		}
	}

	const std::string reference = widest + widest + ":" + widest + widest;
	CFPtr<CTLineRef> line = make_line(reference, row);
	return line ? measure(line.get()).width : 0.0;
}

/// Draws a line centered on the rule, with any colon lifted by `colon_rise`.
///
/// The glyphs are placed one at a time rather than handed to CTLineDraw,
/// because one of them has to move and the rest must not. Core Text has already
/// laid the whole string out -- kerning, tracking, the lot -- and this only
/// reads back where it decided each glyph goes and adds to one of them.
///
/// Setting the colon as its own line and positioning it by hand would be the
/// obvious alternative and is worse: it throws away the kerning across both
/// joins, so the digits would sit differently from the string the layout was
/// measured against.
///
/// The measurement is taken here rather than carried in the frame because it is
/// the one figure that changes with the time: the string's ink width and left
/// bearing.
void draw_centered(CGContextRef context, CTLineRef line, const std::string &text, const clock_frame &frame,
		   double baseline_y, double colon_rise)
{
	const double origin_x = center_origin_x(frame, measure(line));

	/* Layout works top-down; Core Graphics draws bottom-up. */
	const double origin_y = frame.height - baseline_y;

	CFArrayRef runs = CTLineGetGlyphRuns(line);
	const CFIndex run_count = runs ? CFArrayGetCount(runs) : 0;

	for (CFIndex r = 0; r < run_count; ++r) {
		CTRunRef run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, r));
		const CFIndex count = CTRunGetGlyphCount(run);
		if (count <= 0)
			continue;

		/* A run carries the font it was resolved with, which is not always the
		 * one asked for: Core Text falls back per glyph when a face is missing
		 * one. Drawing with the run's own font is what keeps that working. */
		CFDictionaryRef attributes = CTRunGetAttributes(run);
		CTFontRef run_font =
			attributes ? static_cast<CTFontRef>(CFDictionaryGetValue(attributes, kCTFontAttributeName))
				   : nullptr;
		if (!run_font)
			continue;

		const CFRange all = CFRangeMake(0, count);
		std::vector<CGGlyph> glyphs(static_cast<std::size_t>(count));
		std::vector<CGPoint> positions(static_cast<std::size_t>(count));
		std::vector<CFIndex> indices(static_cast<std::size_t>(count));

		CTRunGetGlyphs(run, all, glyphs.data());
		CTRunGetPositions(run, all, positions.data());
		CTRunGetStringIndices(run, all, indices.data());

		for (CFIndex i = 0; i < count; ++i) {
			auto &position = positions[static_cast<std::size_t>(i)];
			position.x += origin_x;
			position.y += origin_y;

			/* Unlike the shadow, this is not negated. The setting is stated
			 * the way it looks -- positive is up -- and up is where positive y
			 * already points in this context.
			 *
			 * The index is into the string Core Text was given. Everything the
			 * clock draws is ASCII, so it also indexes the std::string. */
			const std::size_t at = static_cast<std::size_t>(indices[static_cast<std::size_t>(i)]);
			if (at < text.size() && text[at] == ':')
				position.y += colon_rise;
		}

		CTFontDrawGlyphs(run_font, glyphs.data(), positions.data(), static_cast<std::size_t>(count), context);
	}
}

class mac_clock final : public prepared_clock {
public:
	mac_clock(CFPtr<CTFontRef> time_font, CFPtr<CTFontRef> date_font, CFPtr<CGColorRef> color,
		  double time_tracking_em, double date_tracking_em, bool shadow, double colon_offset,
		  const clock_frame &frame)
		: time_font_(std::move(time_font)),
		  date_font_(std::move(date_font)),
		  color_(std::move(color)),
		  time_tracking_em_(time_tracking_em),
		  date_tracking_em_(date_tracking_em),
		  shadow_(shadow),
		  colon_offset_(colon_offset),
		  frame_(frame)
	{
	}

	std::uint32_t width() const noexcept override { return frame_.width; }
	std::uint32_t height() const noexcept override { return frame_.height; }

	rendered_text render(const clock_content &content) const override;

private:
	/// The same values the frame was solved against. Rebuilt per row rather than
	/// stored as row_style, since that holds borrowed refs and these outlive any
	/// one call.
	row_style time_row() const noexcept { return {time_font_.get(), color_.get(), time_tracking_em_}; }
	row_style date_row() const noexcept { return {date_font_.get(), color_.get(), date_tracking_em_}; }

	CFPtr<CTFontRef> time_font_;
	CFPtr<CTFontRef> date_font_;
	CFPtr<CGColorRef> color_;
	double time_tracking_em_ = 0.0;
	double date_tracking_em_ = 0.0;
	bool shadow_ = true;
	double colon_offset_ = 0.0;
	clock_frame frame_;
};

rendered_text mac_clock::render(const clock_content &content) const
{
	if (content.time.empty())
		return {};

	CFPtr<CTLineRef> time_line = make_line(content.time, time_row());
	CFPtr<CTLineRef> date_line = make_line(content.date, date_row());
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

	/* Set once and left on: every fill that follows casts it, which is what the
	 * rule needs too.
	 *
	 * The vertical offset is negated. The frame gives it as it should look --
	 * down and to the right -- and this context draws bottom-up, so down is
	 * negative here, the same inversion the baselines go through below. */
	CFPtr<CGColorRef> shadow_color;
	if (shadow_) {
		shadow_color = make_shadow_color();
		if (!shadow_color)
			return {};

		CGContextSetShadowWithColor(context.get(), CGSizeMake(frame_.shadow_offset, -frame_.shadow_offset),
					    frame_.shadow_blur, shadow_color.get());
	}

	/* CTFontDrawGlyphs takes its colour from the context, where CTLineDraw took
	 * it from the string's attributes. Same colour either way. */
	CGContextSetFillColorWithColor(context.get(), color_.get());
	CGContextSetTextMatrix(context.get(), CGAffineTransformIdentity);

	/* The date's slash and capitals ride past the digits' top, which is the
	 * intended trade: the numbers are the visual subject, so they are what the
	 * margins are measured against.
	 *
	 * The date goes through the same path even though it holds no colon: one
	 * way of drawing a row is easier to be sure of than two. */
	const double colon_rise = colon_offset_ * frame_.scale;
	draw_centered(context.get(), time_line.get(), content.time, frame_, frame_.time_baseline_y, colon_rise);
	draw_centered(context.get(), date_line.get(), content.date, frame_, frame_.date_baseline_y, colon_rise);

	/* The rule takes the text color, so it disappears against a background for
	 * the same reasons the text does, and stays legible for the same fixes --
	 * the shadow included. */
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

	const double time_size = solve_point_size(style, color.get(), style.time_ink_height, style.time_tracking_em);
	const double date_size = solve_point_size(style, color.get(), style.date_ink_height, style.date_tracking_em);
	if (time_size <= 0.0 || date_size <= 0.0)
		return nullptr;

	CFPtr<CTFontRef> time_font = make_font(style, time_size);
	CFPtr<CTFontRef> date_font = make_font(style, date_size);
	if (!time_font || !date_font)
		return nullptr;

	const row_style time_row = {time_font.get(), color.get(), style.time_tracking_em};
	const row_style date_row = {date_font.get(), color.get(), style.date_tracking_em};

	clock_measurements measurements;
	measurements.time = digit_envelope(time_row);
	measurements.date = digit_envelope(date_row);
	measurements.rule_reference_width = rule_reference_width(time_row);
	if (measurements.rule_reference_width <= 0.0)
		return nullptr;

	const clock_frame frame = solve_frame(measurements);
	if (frame.width == 0 || frame.height == 0)
		return nullptr;

	return std::make_unique<mac_clock>(std::move(time_font), std::move(date_font), std::move(color),
					   style.time_tracking_em, style.date_tracking_em, style.shadow,
					   style.colon_offset, frame);
}

double suggest_colon_offset(const clock_style &style)
{
	if (style.time_ink_height <= 0.0)
		return 0.0;

	CFPtr<CGColorRef> color = make_color(style.color);
	if (!color)
		return 0.0;

	const double point_size = solve_point_size(style, color.get(), style.time_ink_height, style.time_tracking_em);
	if (point_size <= 0.0)
		return 0.0;

	CFPtr<CTFontRef> font = make_font(style, point_size);
	if (!font)
		return 0.0;

	const row_style row = {font.get(), color.get(), style.time_tracking_em};

	const ink_span digits = digit_envelope(row);
	if (digits.height() <= 0.0)
		return 0.0;

	CFPtr<CTLineRef> colon = make_line(":", row);
	if (!colon)
		return 0.0;

	const ink_extents colon_ink = measure(colon.get());

	/* Centres, not heights, so the terms subtract. A height is how far ink
	 * spreads and adds up; this is the midpoint of a span measured from the
	 * baseline, and the two figures point in opposite directions from it.
	 *
	 * A colon usually sits with its lower dot on the baseline and its upper one
	 * around the x-height, which puts its centre well below the digits'. That is
	 * why the answer comes out positive for most faces. */
	const double digit_center = (digits.ascent - digits.descent) / 2.0;
	const double colon_center = (colon_ink.ascent - colon_ink.descent) / 2.0;

	return (digit_center - colon_center) / digits.height() - colon_optical_correction;
}
