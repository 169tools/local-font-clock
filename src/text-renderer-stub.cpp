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

/* Placeholder for platforms whose back end is not written yet. Windows gets a
 * DirectWrite implementation; keeping the symbol defined means the rest of the
 * plugin still compiles and links there in the meantime. Returning null is the
 * documented way to say the platform cannot prepare a clock, so callers already
 * handle it. */
std::unique_ptr<prepared_clock> prepare_clock(const clock_style &)
{
	return nullptr;
}

/* Zero leaves the colon where the face put it, which is the right answer to
 * give when the face cannot be measured at all. */
double suggest_colon_offset(const clock_style &)
{
	return 0.0;
}
