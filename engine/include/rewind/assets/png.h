#pragma once

#include "rewind/core/types.h"

#include <string>
#include <vector>

namespace rw {

/// Encode RGBA8 pixels as an uncompressed (stored deflate) PNG. Small and
/// dependency-free; meant for templates, tests and screenshots, not for
/// shipping big images.
bool encode_png(int width, int height, const u8* rgba, std::vector<u8>& out);
bool write_png(const std::string& path, int width, int height, const u8* rgba, std::string* error = nullptr);

} // namespace rw
