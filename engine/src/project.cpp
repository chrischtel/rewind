#include "rewind/project/project.h"

#include "rewind/assets/png.h"
#include "rewind/core/log.h"
#include "rewind/core/paths.h"
#include "rewind/sim/text_format.h"

#include <cmath>
#include <vector>

namespace rw {

namespace fs = std::filesystem;

bool is_project_dir(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "project.json", ec);
}

bool load_project(const fs::path& dir, Project& out, std::string* error) {
    out = Project{};
    out.root = fs::absolute(dir);
    if (!load_json_file(out.file().string(), out, error)) return false;
    return true;
}

bool save_project(const Project& project, const AppConfig* app_defaults, std::string* error) {
    Project copy = project; // reflect() is non-const
    JsonValue doc = to_json_value(copy);
    if (app_defaults) {
        AppConfig base = *app_defaults;
        const JsonValue diff = json_diff(to_json_value(base), to_json_value(copy.app));
        for (auto& [key, value] : doc.members)
            if (key == "app") value = diff;
    }
    return write_text_file(project.file().string(), write_json(doc), error);
}

bool apply_project_app(const Project& project, AppConfig& config, std::string* error) {
    std::string text;
    if (!read_text_file(project.file().string(), text, error)) return false;
    JsonValue doc;
    std::string err;
    if (!parse_json(text, doc, err)) {
        if (error) *error = err;
        return false;
    }
    if (const JsonValue* app = doc.find("app")) apply_json(*app, config);
    return true;
}

bool create_project(const fs::path& dir, const std::string& name, Project& out, std::string* error) {
    std::error_code ec;
    if (is_project_dir(dir)) {
        if (error) *error = "there is already a project in " + dir.string();
        return false;
    }
    for (const fs::path& sub : {fs::path("assets"), fs::path("scenes"), fs::path("shaders"), fs::path("build") / "cooked"}) {
        fs::create_directories(dir / sub, ec);
        if (ec) {
            if (error) *error = "could not create " + (dir / sub).string() + ": " + ec.message();
            return false;
        }
    }

    out = Project{};
    out.root = fs::absolute(dir);
    out.name = name;
    out.app.title = name;
    const AppConfig defaults;
    if (!save_project(out, &defaults, error)) return false;

    // A starter sprite: a 16x16 coin, so `cook` has work to do right away.
    std::vector<u8> rgba(16 * 16 * 4, 0);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float dx = static_cast<float>(x) - 7.5f, dy = static_cast<float>(y) - 7.5f;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > 7.5f) continue;
            u8* p = &rgba[(static_cast<usize>(y) * 16 + x) * 4];
            const bool rim = d > 6.0f;
            const bool shine = x >= 5 && x <= 6 && y >= 4 && y <= 6;
            p[0] = rim ? 180 : (shine ? 255 : 240);
            p[1] = rim ? 130 : (shine ? 250 : 200);
            p[2] = rim ? 30 : (shine ? 200 : 60);
            p[3] = 255;
        }
    }
    const fs::path coin = dir / "assets" / "coin.png";
    if (!write_png(coin.string(), 16, 16, rgba.data(), error)) return false;
    return true;
}

void use_project_paths(const Project& project) {
    clear_asset_roots();
    add_asset_root(project.cooked_dir().string(), true);
    add_asset_root(project.asset_dir().string(), false);
    RW_LOG_INFO("project", "%s: assets in %s, cooked in %s", project.name.c_str(), project.asset_dir().string().c_str(),
                project.cooked_dir().string().c_str());
}

} // namespace rw
