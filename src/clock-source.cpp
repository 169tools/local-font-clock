/*
Local Font Clock
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

#include <obs-module.h>

#include <cstdint>

namespace {

/* Stand-in until the measured layout decides the real extents. */
constexpr std::uint32_t placeholder_width = 320;
constexpr std::uint32_t placeholder_height = 180;
constexpr std::uint32_t placeholder_color = 0xff3c7dd9; /* ARGB */

struct clock_source {
	obs_source_t *source = nullptr;
	std::uint32_t width = placeholder_width;
	std::uint32_t height = placeholder_height;
};

const char *clock_source_get_name(void *)
{
	return obs_module_text("ClockSource");
}

void *clock_source_create(obs_data_t *, obs_source_t *source)
{
	auto *context = new clock_source();
	context->source = source;
	return context;
}

void clock_source_destroy(void *data)
{
	delete static_cast<clock_source *>(data);
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

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	gs_effect_set_color(color, placeholder_color);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(nullptr, 0, context->width, context->height);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

} // namespace

void register_clock_source()
{
	obs_source_info info = {
		.id = "local_font_clock",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
		.get_name = clock_source_get_name,
		.create = clock_source_create,
		.destroy = clock_source_destroy,
		.get_width = clock_source_get_width,
		.get_height = clock_source_get_height,
		.video_render = clock_source_render,
	};

	obs_register_source(&info);
}
