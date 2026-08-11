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

#include "text-renderer.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <cstdint>
#include <string>

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

/* The colon can be nudged -2px..+10px against a 44px time row -- asymmetric,
 * because the correction is almost always upwards. Expressed as a share of the
 * ink height so the range keeps meaning the same thing at any size. */
constexpr double colon_offset_min_percent = -2.0 / default_time_ink_height * 100.0;
constexpr double colon_offset_max_percent = 10.0 / default_time_ink_height * 100.0;

/* Tightening only; opening the tracking up is not useful here. */
constexpr double tracking_min_em = -0.1;

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
	std::uint32_t font_flags = 0;

	double time_ink_height = default_time_ink_height;
	double date_ink_height = default_time_ink_height * date_height_ratio;

	/* Byte order is OBS's own: red in the low byte, as vec4_from_rgba expects. */
	std::uint32_t color = 0xffffffff;
	bool shadow = true;
	double colon_offset_percent = 0.0;
	double tracking_em = 0.0;

	gs_texture_t *texture = nullptr;
};

/* Rasterises the clock and uploads it. Must hold the graphics context. */
void rebuild_texture(clock_source *context)
{
	if (context->texture) {
		gs_texture_destroy(context->texture);
		context->texture = nullptr;
	}

	clock_style style;
	style.face = context->font_face;
	style.style = context->font_style;
	style.time_ink_height = context->time_ink_height;
	style.date_ink_height = context->date_ink_height;
	style.color = context->color;

	/* Fixed strings until the clock starts telling the time. */
	clock_content content;
	content.time = "12:34";
	content.date = "7/29 WED";

	const rendered_text bitmap = render_clock(content, style);
	if (!bitmap.valid()) {
		context->width = placeholder_width;
		context->height = placeholder_height;
		return;
	}

	const std::uint8_t *rows = bitmap.pixels.data();
	context->texture = gs_texture_create(bitmap.width, bitmap.height, GS_RGBA, 1, &rows, 0);
	context->width = bitmap.width;
	context->height = bitmap.height;
}

const char *clock_source_get_name(void *)
{
	return obs_module_text("ClockSource");
}

void clock_source_update(void *data, obs_data_t *settings)
{
	auto *context = static_cast<clock_source *>(data);

	obs_data_t *font = obs_data_get_obj(settings, "font");
	context->font_face = obs_data_get_string(font, "face");
	context->font_style = obs_data_get_string(font, "style");
	context->font_flags = static_cast<std::uint32_t>(obs_data_get_int(font, "flags"));
	context->time_ink_height = static_cast<double>(obs_data_get_int(font, "size"));
	obs_data_release(font);

	context->date_ink_height = context->time_ink_height * date_height_ratio;
	context->color = static_cast<std::uint32_t>(obs_data_get_int(settings, "color"));
	context->shadow = obs_data_get_bool(settings, "shadow");
	context->colon_offset_percent = obs_data_get_double(settings, "colon_offset");
	context->tracking_em = obs_data_get_double(settings, "tracking");

	/* Rebuilt here rather than in video_render so the source reports a real
	 * size straight away; a zero-sized source can be culled before it ever gets
	 * a chance to draw. update() runs off the graphics thread, hence the
	 * explicit context. */
	obs_enter_graphics();
	rebuild_texture(context);
	obs_leave_graphics();

	/* Debug level: a slider drag fires update() on every step, and the OBS log
	 * is something users are routinely asked to paste into bug reports. Run OBS
	 * with --verbose to see these. */
	obs_log(LOG_DEBUG,
		"settings: face='%s' style='%s' flags=%u ink=%.1f/%.1f color=%08x shadow=%d colon=%+.2f%% tracking=%.3fem",
		context->font_face.c_str(), context->font_style.c_str(), context->font_flags, context->time_ink_height,
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
	obs_data_t *font = obs_data_create();
	obs_data_set_default_string(font, "face", default_font_face);
	obs_data_set_default_string(font, "style", "");
	obs_data_set_default_int(font, "size", default_time_ink_height);
	obs_data_set_default_int(font, "flags", 0);
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);

	obs_data_set_default_int(settings, "color", 0xffffffff);
	obs_data_set_default_bool(settings, "shadow", true);
	obs_data_set_default_double(settings, "colon_offset", 0.0);
	obs_data_set_default_double(settings, "tracking", 0.0);
}

obs_properties_t *clock_source_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_font(props, "font", obs_module_text("Font"));
	obs_properties_add_color(props, "color", obs_module_text("Color"));
	obs_properties_add_bool(props, "shadow", obs_module_text("Shadow"));
	obs_properties_add_float_slider(props, "colon_offset", obs_module_text("ColonOffset"), colon_offset_min_percent,
					colon_offset_max_percent, 0.1);
	obs_properties_add_float_slider(props, "tracking", obs_module_text("Tracking"), tracking_min_em, 0.0, 0.005);

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
		.video_render = clock_source_render,
	};

	obs_register_source(&info);
}
