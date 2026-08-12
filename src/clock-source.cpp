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

#include "clock-source.hpp"

#include "font-dialog.hpp"
#include "text-renderer.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <utility>

namespace {

/* The ink height of the time row, in pixels, is what every other dimension is
 * derived from. The font dialog's size field is read as this height rather than
 * as a point size: solving for a measured height is what keeps the clock the
 * same visual size when the typeface changes, since the ratio of ink to em
 * varies wildly between faces. */
constexpr int default_time_ink_height = 44;

/* The date row was a fixed 18px against a fixed 44px time row. Held as a ratio
 * now that the time row can move. */
constexpr double date_height_ratio = 18.0 / 44.0;

/* A share of the time row's ink height, so the range keeps meaning the same
 * thing at any size, and asymmetric because the correction is almost always
 * upwards: a colon usually sits low, with its lower dot on the baseline.
 *
 * Across fifteen faces measured here the suggestion ran from +1.7% to +17.5%,
 * so this leaves room to disagree with it in either direction without letting
 * the slider reach anywhere silly. */
constexpr double colon_offset_min_percent = -5.0;
constexpr double colon_offset_max_percent = 25.0;

/* Mostly tightening, but not only: the spec allows the negative side alone
 * (SPEC 6.2), on the reasoning that opening a clock up is not something anyone
 * wants. That holds until the face is not one of the ones it was written
 * against -- a condensed or a heavy display cut can set its digits close enough
 * that they read as one number, and there is nothing else in the plugin that
 * separates them.
 *
 * The range is deliberately lopsided. Tightening is the adjustment people
 * reach for and wants room; widening is a nudge, and a clock opened up much
 * further stops reading as a clock. */
constexpr double tracking_min_em = -0.1;
constexpr double tracking_max_em = 0.02;

/* The date follows the time's tracking at four fifths of it rather than being
 * set on its own. Both rows want to tighten together, but the date is set much
 * smaller, and small text wants more air than large text, not the same amount.
 * Holding it in em already accounts for the size difference; this is the
 * further easing on top. */
constexpr double date_tracking_ratio = 0.8;

/* Ink height of the time row, which everything else is derived from. The lower
 * bound is where the date row stops being legible; the upper is well past any
 * sensible overlay, and scaling in the scene covers the rest. */
constexpr double size_min = 12.0;
constexpr double size_max = 300.0;

/* Stand-in until the measured layout decides the real extents. */
constexpr std::uint32_t placeholder_width = 320;
constexpr std::uint32_t placeholder_height = 180;

#ifdef _WIN32
constexpr const char *default_font_face = "Arial";
#elif defined(__APPLE__)
constexpr const char *default_font_face = "Helvetica";
#else
constexpr const char *default_font_face = "Sans Serif";
#endif

struct clock_source {
	obs_source_t *source = nullptr;
	std::uint32_t width = placeholder_width;
	std::uint32_t height = placeholder_height;

	std::string font_face;
	std::string font_style;

	double time_ink_height = default_time_ink_height;
	double date_ink_height = default_time_ink_height * date_height_ratio;

	/* Byte order is OBS's own: red in the low byte, as vec4_from_rgba expects. */
	std::uint32_t color = 0xffffffff;
	bool shadow = true;
	double colon_offset_percent = 0.0;
	double tracking_em = 0.0;

	/* Holds the resolved typeface and the solved geometry, so that redrawing
	 * costs only the typesetting of two strings. Null when the platform has no
	 * back end or the style would not resolve. */
	std::unique_ptr<prepared_clock> clock;

	/* What is currently drawn, and the second it was worked out for. Kept so a
	 * tick can tell whether anything has actually changed. */
	clock_content content;
	std::time_t last_read = 0;

	gs_texture_t *texture = nullptr;
};

/* Local time throughout: an overlay shows the streamer's own clock, and
 * localtime already accounts for the timezone and for daylight saving coming
 * and going, which is not arithmetic worth repeating here. */
clock_content read_clock(std::time_t now)
{
	std::tm local = {};
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	static const char *const weekdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

	/* No leading zeroes, except on the minute, where two digits are how the
	 * time is written rather than padding. */
	char time_text[8];
	char date_text[16];
	std::snprintf(time_text, sizeof(time_text), "%d:%02d", local.tm_hour, local.tm_min);
	std::snprintf(date_text, sizeof(date_text), "%d/%d %s", local.tm_mon + 1, local.tm_mday,
		      weekdays[local.tm_wday]);

	clock_content content;
	content.time = time_text;
	content.date = date_text;
	return content;
}

/* Brings the stored reading up to date. Returns whether it changed, which is
 * what decides if anything has to be drawn. */
bool refresh_content(clock_source *context)
{
	const std::time_t now = std::time(nullptr);
	if (now == context->last_read)
		return false;
	context->last_read = now;

	clock_content content = read_clock(now);
	if (content.time == context->content.time && content.date == context->content.date)
		return false;

	context->content = std::move(content);
	return true;
}

/* Rasterises the current reading and uploads it. Must hold the graphics
 * context. */
void redraw_texture(clock_source *context)
{
	const rendered_text bitmap = context->clock ? context->clock->render(context->content) : rendered_text{};
	if (!bitmap.valid()) {
		if (context->texture) {
			gs_texture_destroy(context->texture);
			context->texture = nullptr;
		}
		context->width = placeholder_width;
		context->height = placeholder_height;
		return;
	}

	const std::uint8_t *rows = bitmap.pixels.data();

	/* The canvas is sized from the widest digit rather than from the time on
	 * screen, so a minute rolling over lands in a texture of the same size and
	 * only the contents need replacing. GS_DYNAMIC is what makes the texture
	 * writable after creation; this is how OBS's own text source does it. */
	if (context->texture && context->width == bitmap.width && context->height == bitmap.height) {
		gs_texture_set_image(context->texture, rows, bitmap.width * 4, false);
		return;
	}

	if (context->texture)
		gs_texture_destroy(context->texture);

	context->texture = gs_texture_create(bitmap.width, bitmap.height, GS_RGBA, 1, &rows, GS_DYNAMIC);
	context->width = bitmap.width;
	context->height = bitmap.height;
}

clock_style style_of(const clock_source *context)
{
	clock_style style;
	style.face = context->font_face;
	style.style = context->font_style;
	style.time_ink_height = context->time_ink_height;
	style.date_ink_height = context->date_ink_height;
	style.time_tracking_em = context->tracking_em;
	style.date_tracking_em = context->tracking_em * date_tracking_ratio;
	style.color = context->color;
	style.shadow = context->shadow;
	style.colon_offset = context->colon_offset_percent / 100.0;
	return style;
}

/* Re-resolves the typeface and geometry, then redraws. Only needed when the
 * style changes -- the time changing does not. Must hold the graphics context. */
void rebuild_clock(clock_source *context)
{
	context->clock = prepare_clock(style_of(context));
	redraw_texture(context);
}

/* Where this face wants its colon, as the slider reads it.
 *
 * Only the face and style matter, so the rest of the settings can be whatever
 * the context is currently holding. */
double suggested_colon_offset_percent(const std::string &face, const std::string &style)
{
	clock_style spec;
	spec.face = face;
	spec.style = style;
	return suggest_colon_offset(spec) * 100.0;
}

const char *clock_source_get_name(void *)
{
	return obs_module_text("ClockSource");
}

/* Called once a frame, for every source, whether or not it is on screen -- so a
 * clock that has been hidden for an hour is already correct when it comes back.
 * Nearly every call returns without doing anything: the reading is only worked
 * out when the second has moved on, and only drawn when the minute has. */
void clock_source_video_tick(void *data, float)
{
	auto *context = static_cast<clock_source *>(data);

	if (!refresh_content(context))
		return;

	/* This runs on the graphics thread, but not inside its context: libobs
	 * closes gs_enter_context() before it ticks sources. */
	obs_enter_graphics();
	redraw_texture(context);
	obs_leave_graphics();
}

void clock_source_update(void *data, obs_data_t *settings)
{
	auto *context = static_cast<clock_source *>(data);

	context->font_face = obs_data_get_string(settings, "font_face");
	context->font_style = obs_data_get_string(settings, "font_style");
	context->time_ink_height = obs_data_get_double(settings, "size");

	/* Derived, and written back so the read-only field in the properties window
	 * has something to show. */
	const std::string shown = context->font_style.empty() ? context->font_face
							      : context->font_face + " " + context->font_style;
	obs_data_set_string(settings, "font_display", shown.c_str());

	context->date_ink_height = context->time_ink_height * date_height_ratio;
	context->color = static_cast<std::uint32_t>(obs_data_get_int(settings, "color"));
	context->shadow = obs_data_get_bool(settings, "shadow");

	/* The colon starts where the face wants it rather than at zero, which is why
	 * there is no button to put it there: the useful value is already in the
	 * slider, and what is left for the user to do is disagree with it.
	 *
	 * It cannot be a plain default, since it is a different number for every
	 * face. So it is written in as a real value the first time, and left alone
	 * afterwards -- obs_data_has_user_value is what separates "never set" from
	 * "set, and happens to equal the suggestion". */
	if (!obs_data_has_user_value(settings, "colon_offset"))
		obs_data_set_double(settings, "colon_offset",
				    suggested_colon_offset_percent(context->font_face, context->font_style));

	context->colon_offset_percent = obs_data_get_double(settings, "colon_offset");
	context->tracking_em = obs_data_get_double(settings, "tracking");

	/* A style change does not wait for the clock to tick, and the very first
	 * update has nothing to draw until this has run once. */
	refresh_content(context);

	/* Rebuilt here rather than in video_render so the source reports a real
	 * size straight away; a zero-sized source can be culled before it ever gets
	 * a chance to draw.
	 *
	 * libobs defers update() for video sources: obs_source_update() only bumps a
	 * counter, and obs_source_video_tick() makes the call, just before it calls
	 * video_tick. So the two run on the same thread, one after the other, and
	 * neither holds the graphics context -- which is why both enter it. The one
	 * caller that does not come through libobs is create(), below, which runs on
	 * whichever thread built the source. */
	obs_enter_graphics();
	rebuild_clock(context);
	obs_leave_graphics();

	/* Debug level: a slider drag fires update() on every step, and the OBS log
	 * is something users are routinely asked to paste into bug reports. Run OBS
	 * with --verbose to see these. */
	obs_log(LOG_DEBUG,
		"settings: face='%s' style='%s' ink=%.1f/%.1f color=%08x shadow=%d colon=%+.2f%% tracking=%.3fem",
		context->font_face.c_str(), context->font_style.c_str(), context->time_ink_height,
		context->date_ink_height, context->color, context->shadow ? 1 : 0, context->colon_offset_percent,
		context->tracking_em);
}

void *clock_source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *context = new clock_source();
	context->source = source;
	clock_source_update(context, settings);
	return context;
}

void clock_source_destroy(void *data)
{
	auto *context = static_cast<clock_source *>(data);

	if (context->texture) {
		obs_enter_graphics();
		gs_texture_destroy(context->texture);
		obs_leave_graphics();
	}

	delete context;
}

void clock_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "font_face", default_font_face);
	obs_data_set_default_string(settings, "font_style", "Bold");
	obs_data_set_default_double(settings, "size", default_time_ink_height);
	obs_data_set_default_int(settings, "color", 0xffffffff);
	obs_data_set_default_bool(settings, "shadow", true);
	obs_data_set_default_double(settings, "colon_offset", 0.0);
	obs_data_set_default_double(settings, "tracking", 0.0);
}

/// Opens the picker and writes the result back. Returning true has OBS rebuild
/// the properties, which is what refreshes the name shown above the button.
bool clock_source_choose_font(obs_properties_t *, obs_property_t *, void *data)
{
	auto *context = static_cast<clock_source *>(data);

	std::string face = context->font_face;
	std::string style = context->font_style;
	if (!choose_font(face, style))
		return false;

	const bool family_changed = face != context->font_face;

	obs_data_t *settings = obs_source_get_settings(context->source);
	obs_data_set_string(settings, "font_face", face.c_str());
	obs_data_set_string(settings, "font_style", style.c_str());

	/* Re-suggested rather than carried over: the old figure was measured against
	 * a different face and means nothing here. Anything the user had dialled in
	 * is lost, which is the point -- they were correcting the previous face.
	 *
	 * Re-measured on a weight change too, not just a family change: a bold cut
	 * carries heavier dots set differently against taller digits, so the answer
	 * moves within a family as well as across families. */
	obs_data_set_double(settings, "colon_offset", suggested_colon_offset_percent(face, style));

	/* Letter spacing goes back to the face's own on a change of family. How much
	 * a face wants tightening is a property of that face -- how wide it is drawn
	 * and how much side bearing it carries -- so a figure arrived at by eye on
	 * one family says nothing about the next, and carrying it over leaves the
	 * new family looking wrong for a reason that is not on screen.
	 *
	 * There is nothing to measure here, unlike the colon. The right amount of
	 * tightening is a matter of taste, and zero is what the face's designer
	 * chose, which is the honest place to start from.
	 *
	 * Weights within a family keep it: they are drawn as a set and spaced to
	 * match, so a correction made for one still means something for the next. */
	if (family_changed)
		obs_data_set_double(settings, "tracking", 0.0);

	obs_source_update(context->source, settings);
	obs_data_release(settings);

	return true;
}

obs_properties_t *clock_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "font_display", obs_module_text("Font"), OBS_TEXT_INFO);
	obs_properties_add_button2(props, "choose_font", obs_module_text("ChooseFont"), clock_source_choose_font, data);
	obs_properties_add_float_slider(props, "size", obs_module_text("Size"), size_min, size_max, 1.0);
	obs_properties_add_color(props, "color", obs_module_text("Color"));
	obs_properties_add_bool(props, "shadow", obs_module_text("Shadow"));
	obs_properties_add_float_slider(props, "colon_offset", obs_module_text("ColonOffset"), colon_offset_min_percent,
					colon_offset_max_percent, 0.1);
	obs_properties_add_float_slider(props, "tracking", obs_module_text("Tracking"), tracking_min_em,
					tracking_max_em, 0.005);

	return props;
}

std::uint32_t clock_source_get_width(void *data)
{
	return static_cast<clock_source *>(data)->width;
}

std::uint32_t clock_source_get_height(void *data)
{
	return static_cast<clock_source *>(data)->height;
}

void clock_source_render(void *data, gs_effect_t *)
{
	const auto *context = static_cast<clock_source *>(data);
	if (!context->texture)
		return;

	/* No flip: a bitmap context stores rows top to bottom even though its
	 * drawing origin is bottom left, so the two cancel out. */
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_PREMULTIPLIED_ALPHA);
	while (gs_effect_loop(effect, "Draw"))
		obs_source_draw(context->texture, 0, 0, 0, 0, false);
}

} // namespace

void register_clock_source()
{
	obs_source_info info = {
		.id = "oshi_font_clock",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
		.get_name = clock_source_get_name,
		.create = clock_source_create,
		.destroy = clock_source_destroy,
		.get_width = clock_source_get_width,
		.get_height = clock_source_get_height,
		.get_defaults = clock_source_get_defaults,
		.get_properties = clock_source_get_properties,
		.update = clock_source_update,
		.video_tick = clock_source_video_tick,
		.video_render = clock_source_render,
	};

	obs_register_source(&info);
}
