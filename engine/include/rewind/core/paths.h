#pragma once

#include <string>

namespace rw {

/// Directory of the running executable, with a trailing separator.
const std::string& executable_dir();

/// Where to read an asset from. An absolute path or a relative path that
/// exists from the current directory is returned unchanged; otherwise the
/// same relative path next to the executable is tried. Games can therefore
/// be started from anywhere and still find their shaders and textures.
std::string resolve_asset_path(const std::string& path);

} // namespace rw
