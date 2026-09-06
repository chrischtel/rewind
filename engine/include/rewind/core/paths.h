#pragma once

#include "rewind/core/types.h"

#include <string>
#include <vector>

namespace rw {

class Pack;

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

/// Mount a pack (project/pack.h). Packs are searched before the roots, most
/// recently mounted first: "<path>.cooked" then "<path>". Returns false
/// when the file is not a pack.
bool add_asset_pack(const std::string& file, std::string* error = nullptr);
void clear_asset_packs();
usize asset_pack_count();

/// Read an asset by name or by an already resolved path: packs first, then
/// the filesystem through resolve_asset_path(). The one call every loader
/// uses, so a mounted pack is transparent to games.
bool read_asset_bytes(const std::string& path, std::vector<unsigned char>& out);
bool read_asset_text(const std::string& path, std::string& out);
bool asset_exists(const std::string& path);

/// Names in mounted packs starting with `prefix` (e.g. "scenes/prefabs/").
std::vector<std::string> packed_asset_names(const std::string& prefix);

} // namespace rw
