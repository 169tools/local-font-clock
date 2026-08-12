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

/// DirectWrite back end, answering the same two questions the Core Text one
/// does: how big is the ink, and put it here.
///
/// The geometry is not repeated -- solve_frame in layout.hpp does that for both
/// platforms. What differs is that DirectWrite has no equivalent of
/// CTLineGetBoundsWithOptions(kCTLineBoundsUseGlyphPathBounds): there is no call
/// that hands back the ink box of a laid-out string. It has to be assembled from
/// per-glyph design metrics, which is done in glyph_ink below.

#include "text-renderer.hpp"

#include "com-ptr.hpp"
#include "layout.hpp"

#include <windows.h>

#include <dwrite_1.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr double reference_point_size = 100.0;

/// The clock's strings are ASCII throughout -- digits, colon, slash, space and
/// three uppercase letters -- so widening is a cast rather than a conversion.
std::wstring widen(const std::string &value)
{
	return std::wstring(value.begin(), value.end());
}

ComPtr<IDWriteFactory> make_factory()
{
	ComPtr<IDWriteFactory> factory;
	if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
				       reinterpret_cast<IUnknown **>(factory.put()))))
		return {};
	return factory;
}

/// Resolves a family plus a style name -- what the picker hands over, since
/// QFontDatabase names faces that way -- into a font face.
///
/// The style is matched by name against the family's faces rather than being
/// mapped onto a weight/stretch/style triple. A name is what the picker showed
/// the user, and faces called "Book" or "Roman" have no place in that triple.
ComPtr<IDWriteFontFace> make_font_face(IDWriteFactory *factory, const clock_style &style)
{
	ComPtr<IDWriteFontCollection> collection;
	if (FAILED(factory->GetSystemFontCollection(collection.put(), FALSE)) || !collection)
		return {};

	const std::wstring family_name = widen(style.face);

	UINT32 index = 0;
	BOOL exists = FALSE;
	if (FAILED(collection->FindFamilyName(family_name.c_str(), &index, &exists)) || !exists)
		return {};

	ComPtr<IDWriteFontFamily> family;
	if (FAILED(collection->GetFontFamily(index, family.put())) || !family)
		return {};

	ComPtr<IDWriteFont> chosen;

	if (!style.style.empty()) {
		const std::wstring wanted = widen(style.style);
		const UINT32 count = family->GetFontCount();

		for (UINT32 i = 0; i < count && !chosen; ++i) {
			ComPtr<IDWriteFont> candidate;
			if (FAILED(family->GetFont(i, candidate.put())) || !candidate)
				continue;

			ComPtr<IDWriteLocalizedStrings> names;
			if (FAILED(candidate->GetFaceNames(names.put())) || !names)
				continue;

			const UINT32 name_count = names->GetCount();
			for (UINT32 n = 0; n < name_count; ++n) {
				UINT32 length = 0;
				if (FAILED(names->GetStringLength(n, &length)))
					continue;

				std::wstring name(length + 1, L'\0');
				if (FAILED(names->GetString(n, name.data(), length + 1)))
					continue;
				name.resize(length);

				if (name == wanted) {
					chosen = std::move(candidate);
					break;
				}
			}
		}
	}

	/* No style given, or none of the faces answered to it. Fall back to the
	 * family's regular the way Core Text does when the style attribute misses. */
	if (!chosen &&
	    FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
						DWRITE_FONT_STYLE_NORMAL, chosen.put())))
		return {};

	if (!chosen)
		return {};

	ComPtr<IDWriteFontFace> face;
	if (FAILED(chosen->CreateFontFace(face.put())) || !face)
		return {};

	return face;
}

/// A resolved face at a size, plus the setting that has to reach every
/// measurement. The Core Text side calls the same thing row_style, for the same
/// reason: the rule follows the time's ink width, so tracking has to be applied
/// wherever a width is taken.
struct row_style {
	IDWriteFontFace *face = nullptr;
	double em_size = 0.0;
	double tracking_em = 0.0;

	double tracking_pixels() const noexcept { return tracking_em * em_size; }
};

UINT16 glyph_for(IDWriteFontFace *face, char ascii)
{
	const UINT32 codepoint = static_cast<UINT32>(static_cast<unsigned char>(ascii));
	UINT16 glyph = 0;
	if (FAILED(face->GetGlyphIndices(&codepoint, 1, &glyph)))
		return 0;
	return glyph;
}

/// Ink box of a single glyph, in pixels, relative to the baseline at y = 0.
///
/// This is the call the whole design rests on, and the one place the two back
/// ends differ in kind rather than in spelling. Core Text hands back the ink box
/// of a laid-out line; DirectWrite hands back design-unit side bearings per
/// glyph, and the box has to be reassembled:
///
///     ascent  = verticalOriginY - topSideBearing
///     descent = advanceHeight - bottomSideBearing - verticalOriginY
///
/// verticalOriginY is measured up from the baseline, and the two side bearings
/// are the gaps between the black box and the vertical advance box. Both figures
/// stay signed, as ink_span requires: a glyph sitting entirely above the
/// baseline gives a negative descent, and solve_frame depends on that.
///
/// Design units are converted with the face's own designUnitsPerEm rather than
/// an assumed 1000 or 2048; both are common and neither is guaranteed.
ink_extents glyph_ink(const row_style &row, UINT16 glyph)
{
	DWRITE_FONT_METRICS font_metrics = {};
	row.face->GetMetrics(&font_metrics);
	if (font_metrics.designUnitsPerEm == 0)
		return {};

	DWRITE_GLYPH_METRICS metrics = {};
	if (FAILED(row.face->GetDesignGlyphMetrics(&glyph, 1, &metrics, FALSE)))
		return {};

	const double scale = row.em_size / static_cast<double>(font_metrics.designUnitsPerEm);

	ink_extents extents;
	extents.left = static_cast<double>(metrics.leftSideBearing) * scale;
	extents.width = static_cast<double>(static_cast<INT32>(metrics.advanceWidth) - metrics.leftSideBearing -
					    metrics.rightSideBearing) *
			scale;
	extents.ascent = static_cast<double>(metrics.verticalOriginY - metrics.topSideBearing) * scale;
	extents.descent = static_cast<double>(static_cast<INT32>(metrics.advanceHeight) - metrics.bottomSideBearing -
					      metrics.verticalOriginY) *
			  scale;
	return extents;
}

/// Vertical envelope of every digit. Same contract, and the same reasoning, as
/// the Core Text side: a reference string assumes zero is the tallest digit,
/// which is true of text faces and not of display ones.
ink_span digit_envelope(const row_style &row)
{
	ink_span envelope;
	bool seen = false;

	for (char digit = '0'; digit <= '9'; ++digit) {
		const UINT16 glyph = glyph_for(row.face, digit);
		if (glyph == 0)
			continue;

		const ink_extents extents = glyph_ink(row, glyph);
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

double envelope_height(IDWriteFactory *factory, const clock_style &style, double em_size, double tracking_em)
{
	ComPtr<IDWriteFontFace> face = make_font_face(factory, style);
	if (!face)
		return 0.0;

	return digit_envelope({face.get(), em_size, tracking_em}).height();
}

/// Finds the em size whose digit envelope is `target_height` tall.
///
/// Outlines scale linearly with the em size here in a way they do not under
/// Core Text's hinting -- the metrics are design units multiplied by a ratio --
/// so a single division would do. The correcting pass is kept anyway, since it
/// costs one more measurement and makes the two back ends answer the same way if
/// a face ever reports something non-linear.
double solve_em_size(IDWriteFactory *factory, const clock_style &style, double target_height, double tracking_em)
{
	const double reference_height = envelope_height(factory, style, reference_point_size, tracking_em);
	if (reference_height <= 0.0)
		return 0.0;

	const double estimate = reference_point_size * target_height / reference_height;

	const double actual_height = envelope_height(factory, style, estimate, tracking_em);
	if (actual_height <= 0.0)
		return estimate;

	return estimate * target_height / actual_height;
}

/// Width the rule is pinned to: the widest digit repeated across the time
/// format, measured with the row's tracking so the rule tightens with the
/// digits.
double rule_reference_width(const row_style &row)
{
	UINT16 widest_glyph = 0;
	double widest = 0.0;

	for (char digit = '0'; digit <= '9'; ++digit) {
		const UINT16 glyph = glyph_for(row.face, digit);
		if (glyph == 0)
			continue;

		const double width = glyph_ink(row, glyph).width;
		if (width > widest) {
			widest = width;
			widest_glyph = glyph;
		}
	}

	if (widest_glyph == 0)
		return 0.0;

	const UINT16 colon = glyph_for(row.face, ':');
	if (colon == 0)
		return 0.0;

	/* Four of the widest digit and a colon, laid out the way the string would
	 * be: advances plus tracking between clusters, and the ink box trimmed by
	 * the side bearings at each end. Tracking after the last cluster is trailing
	 * space, so it is left out -- the Core Text side gets that for free from
	 * kCTTrackingAttributeName, and here it is the four in the loop below. */
	DWRITE_FONT_METRICS font_metrics = {};
	row.face->GetMetrics(&font_metrics);
	if (font_metrics.designUnitsPerEm == 0)
		return 0.0;

	const double scale = row.em_size / static_cast<double>(font_metrics.designUnitsPerEm);
	const UINT16 glyphs[] = {widest_glyph, widest_glyph, colon, widest_glyph, widest_glyph};

	DWRITE_GLYPH_METRICS metrics[5] = {};
	if (FAILED(row.face->GetDesignGlyphMetrics(glyphs, 5, metrics, FALSE)))
		return 0.0;

	double pen = 0.0;
	double ink_left = 0.0;
	double ink_right = 0.0;
	for (int i = 0; i < 5; ++i) {
		const double left = pen + static_cast<double>(metrics[i].leftSideBearing) * scale;
		const double right = pen + static_cast<double>(static_cast<INT32>(metrics[i].advanceWidth) -
							       metrics[i].rightSideBearing) *
						   scale;
		if (i == 0) {
			ink_left = left;
			ink_right = right;
		} else {
			ink_left = std::min(ink_left, left);
			ink_right = std::max(ink_right, right);
		}

		pen += static_cast<double>(metrics[i].advanceWidth) * scale;
		if (i < 4)
			pen += row.tracking_pixels();
	}

	return ink_right - ink_left;
}

/// One glyph, placed. The clock's strings are short enough that laying them out
/// by advance is the whole of the shaping needed: every character is its own
/// cluster, none of them form ligatures, and the digits a clock draws are the
/// tabular ones a face spaces evenly by design.
struct placed_glyph {
	UINT16 glyph = 0;
	double x = 0.0;
	bool is_colon = false;
};

struct placed_line {
	std::vector<placed_glyph> glyphs;
	double ink_left = 0.0;
	double ink_right = 0.0;

	double width() const noexcept { return ink_right - ink_left; }
};

placed_line place(const row_style &row, const std::string &text)
{
	placed_line line;
	if (text.empty())
		return line;

	DWRITE_FONT_METRICS font_metrics = {};
	row.face->GetMetrics(&font_metrics);
	if (font_metrics.designUnitsPerEm == 0)
		return line;

	const double scale = row.em_size / static_cast<double>(font_metrics.designUnitsPerEm);

	double pen = 0.0;
	bool seen = false;

	for (std::size_t i = 0; i < text.size(); ++i) {
		const UINT16 glyph = glyph_for(row.face, text[i]);

		DWRITE_GLYPH_METRICS metrics = {};
		if (FAILED(row.face->GetDesignGlyphMetrics(&glyph, 1, &metrics, FALSE)))
			continue;

		const double advance = static_cast<double>(metrics.advanceWidth) * scale;
		const double ink_width =
			static_cast<double>(static_cast<INT32>(metrics.advanceWidth) - metrics.leftSideBearing -
					    metrics.rightSideBearing) *
			scale;

		/* A space has no ink, so it moves the pen without widening the box. */
		if (ink_width > 0.0) {
			const double left = pen + static_cast<double>(metrics.leftSideBearing) * scale;
			const double right = left + ink_width;
			if (!seen) {
				line.ink_left = left;
				line.ink_right = right;
				seen = true;
			} else {
				line.ink_left = std::min(line.ink_left, left);
				line.ink_right = std::max(line.ink_right, right);
			}

			line.glyphs.push_back({glyph, pen, text[i] == ':'});
		}

		pen += advance;
		if (i + 1 < text.size())
			pen += row.tracking_pixels();
	}

	return line;
}

/// 8-bit coverage, which is all the compositing here needs: the text is one
/// colour and the shadow is the same coverage blurred.
struct coverage {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> alpha;

	std::uint8_t &at(std::uint32_t x, std::uint32_t y) noexcept
	{
		return alpha[static_cast<std::size_t>(y) * width + x];
	}
	std::uint8_t at(std::uint32_t x, std::uint32_t y) const noexcept
	{
		return alpha[static_cast<std::size_t>(y) * width + x];
	}
};

/// Rasterises a run into `into`, at a baseline given in top-down pixels.
///
/// IDWriteGlyphRunAnalysis is used rather than Direct2D: it hands back an alpha
/// texture and nothing else, which is exactly what is wanted here, and it keeps
/// the plugin off the D2D and WIC dependency chain for a bitmap it is going to
/// composite itself anyway.
bool rasterise(IDWriteFactory *factory, const row_style &row, const placed_line &line, double origin_x,
	       double baseline_y, double colon_rise, coverage &into)
{
	if (line.glyphs.empty())
		return true;

	std::vector<UINT16> indices;
	std::vector<FLOAT> advances;
	std::vector<DWRITE_GLYPH_OFFSET> offsets;
	indices.reserve(line.glyphs.size());
	advances.reserve(line.glyphs.size());
	offsets.reserve(line.glyphs.size());

	for (const placed_glyph &glyph : line.glyphs) {
		indices.push_back(glyph.glyph);

		/* Every glyph is placed absolutely through its offset, so the advances
		 * are zeroed rather than accumulated. That is what lets one glyph move
		 * without disturbing the rest -- the same reason the Core Text side
		 * draws glyph by glyph instead of handing over a whole line. */
		advances.push_back(0.0f);

		/* ascenderOffset is positive upwards, which is the direction the colon
		 * setting is already stated in, so it goes straight through. */
		offsets.push_back({static_cast<FLOAT>(origin_x + glyph.x),
				   static_cast<FLOAT>(glyph.is_colon ? colon_rise : 0.0)});
	}

	DWRITE_GLYPH_RUN run = {};
	run.fontFace = row.face;
	run.fontEmSize = static_cast<FLOAT>(row.em_size);
	run.glyphCount = static_cast<UINT32>(indices.size());
	run.glyphIndices = indices.data();
	run.glyphAdvances = advances.data();
	run.glyphOffsets = offsets.data();
	run.isSideways = FALSE;
	run.bidiLevel = 0;

	ComPtr<IDWriteGlyphRunAnalysis> analysis;
	if (FAILED(factory->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL,
						   DWRITE_MEASURING_MODE_NATURAL, 0.0f,
						   static_cast<FLOAT>(baseline_y), analysis.put())) ||
	    !analysis)
		return false;

	RECT bounds = {};
	if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds)))
		return false;

	const LONG texture_width = bounds.right - bounds.left;
	const LONG texture_height = bounds.bottom - bounds.top;
	if (texture_width <= 0 || texture_height <= 0)
		return true;

	/* CLEARTYPE_3x1 is the only texture type that comes back antialiased;
	 * ALIASED_1x1 is one bit per pixel dressed as a byte. The three subpixel
	 * coverages are averaged back into one, since the clock is composited over
	 * whatever the scene puts behind it and subpixel positioning would be wrong
	 * against an unknown background. */
	std::vector<BYTE> texture(static_cast<std::size_t>(texture_width) * texture_height * 3);
	if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, texture.data(),
						static_cast<UINT32>(texture.size()))))
		return false;

	for (LONG y = 0; y < texture_height; ++y) {
		const LONG target_y = bounds.top + y;
		if (target_y < 0 || target_y >= static_cast<LONG>(into.height))
			continue;

		for (LONG x = 0; x < texture_width; ++x) {
			const LONG target_x = bounds.left + x;
			if (target_x < 0 || target_x >= static_cast<LONG>(into.width))
				continue;

			const std::size_t i = (static_cast<std::size_t>(y) * texture_width + x) * 3;
			const int average = (texture[i] + texture[i + 1] + texture[i + 2]) / 3;

			std::uint8_t &existing = into.at(static_cast<std::uint32_t>(target_x),
							 static_cast<std::uint32_t>(target_y));
			existing = static_cast<std::uint8_t>(std::max<int>(existing, average));
		}
	}

	return true;
}

void fill_rect(coverage &into, double x, double y, double width, double height)
{
	const long x0 = std::max<long>(0, static_cast<long>(std::floor(x)));
	const long y0 = std::max<long>(0, static_cast<long>(std::floor(y)));
	const long x1 = std::min<long>(into.width, static_cast<long>(std::ceil(x + width)));
	const long y1 = std::min<long>(into.height, static_cast<long>(std::ceil(y + height)));

	for (long row = y0; row < y1; ++row)
		for (long column = x0; column < x1; ++column)
			into.at(static_cast<std::uint32_t>(column), static_cast<std::uint32_t>(row)) = 255;
}

/// Three box passes, which is close enough to a Gaussian for a drop shadow and
/// is what most compositors do. The radius is derived from the blur the frame
/// asks for on the same terms Core Graphics reads it: blur is about twice the
/// standard deviation.
void blur(coverage &image, double blur_radius)
{
	const double sigma = blur_radius / 2.0;
	if (sigma <= 0.0 || image.width == 0 || image.height == 0)
		return;

	const int radius = std::max(1, static_cast<int>(std::round(sigma * 3.0 / 2.0)));
	std::vector<std::uint8_t> scratch(image.alpha.size());

	auto pass = [&](const std::vector<std::uint8_t> &from, std::vector<std::uint8_t> &to, bool horizontal) {
		const std::uint32_t outer = horizontal ? image.height : image.width;
		const std::uint32_t inner = horizontal ? image.width : image.height;

		for (std::uint32_t o = 0; o < outer; ++o) {
			for (std::uint32_t i = 0; i < inner; ++i) {
				int total = 0;
				int count = 0;
				for (int k = -radius; k <= radius; ++k) {
					const long at = static_cast<long>(i) + k;
					if (at < 0 || at >= static_cast<long>(inner))
						continue;

					const std::size_t index =
						horizontal ? static_cast<std::size_t>(o) * image.width + at
							   : static_cast<std::size_t>(at) * image.width + o;
					total += from[index];
					++count;
				}

				const std::size_t index = horizontal ? static_cast<std::size_t>(o) * image.width + i
								     : static_cast<std::size_t>(i) * image.width + o;
				to[index] = static_cast<std::uint8_t>(count ? total / count : 0);
			}
		}
	};

	for (int i = 0; i < 3; ++i) {
		pass(image.alpha, scratch, true);
		pass(scratch, image.alpha, false);
	}
}

class win_clock final : public prepared_clock {
public:
	win_clock(ComPtr<IDWriteFactory> factory, ComPtr<IDWriteFontFace> face, double time_em, double date_em,
		  const clock_style &style, const clock_frame &frame)
		: factory_(std::move(factory)),
		  face_(std::move(face)),
		  time_em_(time_em),
		  date_em_(date_em),
		  style_(style),
		  frame_(frame)
	{
	}

	std::uint32_t width() const noexcept override { return frame_.width; }
	std::uint32_t height() const noexcept override { return frame_.height; }

	rendered_text render(const clock_content &content) const override;

private:
	row_style time_row() const noexcept { return {face_.get(), time_em_, style_.time_tracking_em}; }
	row_style date_row() const noexcept { return {face_.get(), date_em_, style_.date_tracking_em}; }

	ComPtr<IDWriteFactory> factory_;
	ComPtr<IDWriteFontFace> face_;
	double time_em_ = 0.0;
	double date_em_ = 0.0;
	clock_style style_;
	clock_frame frame_;
};

rendered_text win_clock::render(const clock_content &content) const
{
	if (content.time.empty())
		return {};

	coverage ink;
	ink.width = frame_.width;
	ink.height = frame_.height;
	ink.alpha.assign(static_cast<std::size_t>(ink.width) * ink.height, 0);

	const row_style time = time_row();
	const row_style date = date_row();

	const placed_line time_line = place(time, content.time);
	const placed_line date_line = place(date, content.date);

	const double colon_rise = style_.colon_offset * frame_.scale;

	const ink_extents time_extents = {time_line.width(), 0.0, 0.0, time_line.ink_left};
	const ink_extents date_extents = {date_line.width(), 0.0, 0.0, date_line.ink_left};

	if (!rasterise(factory_.get(), time, time_line, center_origin_x(frame_, time_extents), frame_.time_baseline_y,
		       colon_rise, ink))
		return {};
	if (!rasterise(factory_.get(), date, date_line, center_origin_x(frame_, date_extents), frame_.date_baseline_y,
		       0.0, ink))
		return {};

	fill_rect(ink, frame_.rule_x, frame_.rule_y, frame_.rule_width, frame_.rule_height);

	rendered_text result;
	result.width = frame_.width;
	result.height = frame_.height;
	result.pixels.assign(static_cast<std::size_t>(result.width) * result.height * 4, 0);

	const double red = static_cast<double>(style_.color & 0xff) / 255.0;
	const double green = static_cast<double>((style_.color >> 8) & 0xff) / 255.0;
	const double blue = static_cast<double>((style_.color >> 16) & 0xff) / 255.0;
	const double opacity = static_cast<double>((style_.color >> 24) & 0xff) / 255.0;

	/* The shadow is the same coverage, blurred and shifted. Core Graphics does
	 * this behind CGContextSetShadowWithColor; here it is spelled out, which is
	 * also why it is composited first -- the shadow goes under the mark. */
	if (style_.shadow) {
		coverage shade = ink;
		blur(shade, frame_.shadow_blur);

		const long shift = static_cast<long>(std::round(frame_.shadow_offset));
		for (std::uint32_t y = 0; y < result.height; ++y) {
			for (std::uint32_t x = 0; x < result.width; ++x) {
				const long from_x = static_cast<long>(x) - shift;
				const long from_y = static_cast<long>(y) - shift;
				if (from_x < 0 || from_y < 0 || from_x >= static_cast<long>(shade.width) ||
				    from_y >= static_cast<long>(shade.height))
					continue;

				const double alpha =
					shade.at(static_cast<std::uint32_t>(from_x),
						 static_cast<std::uint32_t>(from_y)) /
					255.0 * shadow_opacity;
				if (alpha <= 0.0)
					continue;

				/* Black, premultiplied, so only the alpha channel carries. */
				const std::size_t i = (static_cast<std::size_t>(y) * result.width + x) * 4;
				result.pixels[i + 3] = static_cast<std::uint8_t>(std::round(alpha * 255.0));
			}
		}
	}

	for (std::uint32_t y = 0; y < result.height; ++y) {
		for (std::uint32_t x = 0; x < result.width; ++x) {
			const double alpha = ink.at(x, y) / 255.0 * opacity;
			if (alpha <= 0.0)
				continue;

			const std::size_t i = (static_cast<std::size_t>(y) * result.width + x) * 4;
			const double behind = result.pixels[i + 3] / 255.0;
			const double combined = alpha + behind * (1.0 - alpha);

			/* Premultiplied, matching GS_RGBA under
			 * OBS_EFFECT_PREMULTIPLIED_ALPHA. The shadow under the mark is
			 * black, so it contributes nothing to the colour channels. */
			result.pixels[i + 0] = static_cast<std::uint8_t>(std::round(red * alpha * 255.0));
			result.pixels[i + 1] = static_cast<std::uint8_t>(std::round(green * alpha * 255.0));
			result.pixels[i + 2] = static_cast<std::uint8_t>(std::round(blue * alpha * 255.0));
			result.pixels[i + 3] = static_cast<std::uint8_t>(std::round(combined * 255.0));
		}
	}

	return result;
}

} // namespace

std::unique_ptr<prepared_clock> prepare_clock(const clock_style &style)
{
	if (style.time_ink_height <= 0.0 || style.date_ink_height <= 0.0)
		return nullptr;

	ComPtr<IDWriteFactory> factory = make_factory();
	if (!factory)
		return nullptr;

	const double time_em = solve_em_size(factory.get(), style, style.time_ink_height, style.time_tracking_em);
	const double date_em = solve_em_size(factory.get(), style, style.date_ink_height, style.date_tracking_em);
	if (time_em <= 0.0 || date_em <= 0.0)
		return nullptr;

	ComPtr<IDWriteFontFace> face = make_font_face(factory.get(), style);
	if (!face)
		return nullptr;

	const row_style time_row = {face.get(), time_em, style.time_tracking_em};
	const row_style date_row = {face.get(), date_em, style.date_tracking_em};

	clock_measurements measurements;
	measurements.time = digit_envelope(time_row);
	measurements.date = digit_envelope(date_row);
	measurements.rule_reference_width = rule_reference_width(time_row);
	if (measurements.rule_reference_width <= 0.0)
		return nullptr;

	const clock_frame frame = solve_frame(measurements);
	if (frame.width == 0 || frame.height == 0)
		return nullptr;

	return std::make_unique<win_clock>(std::move(factory), std::move(face), time_em, date_em, style, frame);
}

double suggest_colon_offset(const clock_style &style)
{
	if (style.time_ink_height <= 0.0)
		return 0.0;

	ComPtr<IDWriteFactory> factory = make_factory();
	if (!factory)
		return 0.0;

	const double em_size = solve_em_size(factory.get(), style, style.time_ink_height, style.time_tracking_em);
	if (em_size <= 0.0)
		return 0.0;

	ComPtr<IDWriteFontFace> face = make_font_face(factory.get(), style);
	if (!face)
		return 0.0;

	const row_style row = {face.get(), em_size, style.time_tracking_em};

	const ink_span digits = digit_envelope(row);
	if (digits.height() <= 0.0)
		return 0.0;

	const UINT16 colon = glyph_for(row.face, ':');
	if (colon == 0)
		return 0.0;

	const ink_extents colon_ink = glyph_ink(row, colon);

	/* Centres, not heights, so the terms subtract -- the same arithmetic as the
	 * Core Text side, on measurements that got here a different way. */
	const double digit_center = (digits.ascent - digits.descent) / 2.0;
	const double colon_center = (colon_ink.ascent - colon_ink.descent) / 2.0;

	return (digit_center - colon_center) / digits.height() - colon_optical_correction;
}
