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

#include <utility>

/// Releases a COM interface on the way out, the way CFPtr does for Core
/// Foundation. Same reasoning: the back end has a dozen early returns, and a
/// cleanup ladder that has to grow with each of them is the kind of thing that
/// leaks on the path nobody exercised.
///
/// Deliberately not wil::com_ptr or ATL's CComPtr -- neither is a dependency
/// this plugin has, and the whole of what is needed is below.
template<typename T> class ComPtr {
public:
	ComPtr() = default;
	explicit ComPtr(T *raw) noexcept : ref_(raw) {}

	ComPtr(const ComPtr &) = delete;
	ComPtr &operator=(const ComPtr &) = delete;

	ComPtr(ComPtr &&other) noexcept : ref_(other.release()) {}
	ComPtr &operator=(ComPtr &&other) noexcept
	{
		if (this != &other)
			reset(other.release());
		return *this;
	}

	~ComPtr() { reset(); }

	T *get() const noexcept { return ref_; }
	T *operator->() const noexcept { return ref_; }
	explicit operator bool() const noexcept { return ref_ != nullptr; }

	/// For the out-parameter form every COM factory uses. Asserts nothing was
	/// already held, since overwriting would drop a reference.
	T **put() noexcept
	{
		reset();
		return &ref_;
	}

	/// Same, for the calls that hand back void** rather than a typed pointer.
	void **put_void() noexcept { return reinterpret_cast<void **>(put()); }

	T *release() noexcept { return std::exchange(ref_, nullptr); }

	void reset(T *raw = nullptr) noexcept
	{
		if (ref_)
			ref_->Release();
		ref_ = raw;
	}

private:
	T *ref_ = nullptr;
};
