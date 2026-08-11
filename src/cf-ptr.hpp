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

#include <CoreFoundation/CoreFoundation.h>

#include <utility>

/// Owns a CoreFoundation reference and releases it on destruction.
///
/// Core Text hands back a great many CFTypeRefs across a single render, each
/// needing a matching CFRelease on every exit path. Doing that by hand is how
/// leaks and double releases happen, so ownership is expressed in the type
/// instead: construct from a Create/Copy call, and never call CFRelease again.
///
/// Only move-constructible, matching CoreFoundation's Create Rule -- the
/// reference is owned once, by one holder.
template<typename T> class CFPtr {
public:
	CFPtr() = default;

	explicit CFPtr(T ref) noexcept : ref_(ref) {}

	CFPtr(const CFPtr &) = delete;
	CFPtr &operator=(const CFPtr &) = delete;

	CFPtr(CFPtr &&other) noexcept : ref_(other.release()) {}

	CFPtr &operator=(CFPtr &&other) noexcept
	{
		if (this != &other)
			reset(other.release());
		return *this;
	}

	~CFPtr() { reset(); }

	T get() const noexcept { return ref_; }

	explicit operator bool() const noexcept { return ref_ != nullptr; }

	/// Gives up ownership without releasing.
	T release() noexcept { return std::exchange(ref_, nullptr); }

	void reset(T ref = nullptr) noexcept
	{
		if (ref_)
			CFRelease(ref_);
		ref_ = ref;
	}

private:
	T ref_ = nullptr;
};
