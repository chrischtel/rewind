#pragma once

#include <string>

namespace rw {

/// Directory of the running executable, with a trailing separator.
const std::string& executable_dir();

/// Register a directory to look for assets in. Roots are searched in the
/// order they were added. A cooked root is checked for "<path>.cooked" (the
/// cook step's output) before the plain path.
void add_asset_root(const std::string& dir, bool cooked);
void clear_asset_roots();

/// Where to read an asset from. Absolute paths and paths that exist from the
/// current directory are returned unchanged; otherwise the asset roots and
/// then the executable's directory are tried. Cooked variants win where they
/// exist, so a game loads "sprites/hero.png" and gets the cooked texture
/// when the project was cooked and the PNG otherwise.
std::string resolve_asset_path(const std::string& path);

} // namespace rw
