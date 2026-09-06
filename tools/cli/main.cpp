// rewind: the command line for projects.
//
//   rewind new <dir> [name]     create a project with the standard layout
//   rewind info <project>       list assets, ids and cook state
//   rewind cook <project>       cook every changed asset (--force for all)
//   rewind run <project>        run the project's game module in the runner
//   rewind edit <project>       open the project in the editor

#include <rewind/core/log.h>
#include <rewind/core/paths.h>
#include <rewind/project/asset_db.h>
#include <rewind/project/project.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  rewind new <dir> [name]\n"
                 "  rewind info <project dir>\n"
                 "  rewind cook <project dir> [--force]\n"
                 "  rewind run <project dir> [game args...]\n"
                 "  rewind edit <project dir> [game args...]\n");
    return 2;
}

bool open(const std::string& dir, rw::Project& project) {
    std::string error;
    if (!rw::load_project(dir, project, &error)) {
        std::fprintf(stderr, "rewind: %s\n", error.c_str());
        return false;
    }
    return true;
}

int cmd_new(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string dir = argv[2];
    const std::string name = argc > 3 ? argv[3] : fs::path(dir).filename().string();
    rw::Project project;
    std::string error;
    if (!rw::create_project(dir, name, project, &error)) {
        std::fprintf(stderr, "rewind: %s\n", error.c_str());
        return 1;
    }
    std::printf("created project '%s' in %s\n", name.c_str(), project.root.string().c_str());
    std::printf("  project.json, assets/, scenes/, shaders/, build/cooked/\n");
    std::printf("  set \"module\" in project.json to your game DLL, then: rewind cook %s\n", dir.c_str());
    return 0;
}

int cmd_info(int argc, char** argv) {
    if (argc < 3) return usage();
    rw::Project project;
    if (!open(argv[2], project)) return 1;
    rw::AssetDatabase db(project);
    db.scan();
    std::printf("project %s (%s)\n", project.name.c_str(), project.root.string().c_str());
    std::printf("module  %s\n", project.module.empty() ? "(none)" : project.module.c_str());
    std::printf("%-40s %-9s %-16s %s\n", "asset", "type", "id", "state");
    for (const rw::AssetRecord& r : db.records()) {
        const char* state = r.cooked.empty() ? "-" : (r.cooked_hash == 0 ? "not cooked" : (r.stale() ? "stale" : "cooked"));
        std::printf("%-40s %-9s %-16s %s\n", r.path.c_str(), r.meta.type.c_str(), r.meta.id.c_str(), state);
    }
    std::printf("%zu assets\n", db.records().size());
    return 0;
}

int cmd_cook(int argc, char** argv) {
    if (argc < 3) return usage();
    rw::Project project;
    if (!open(argv[2], project)) return 1;
    bool force = false;
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "--force") force = true;
    rw::AssetDatabase db(project);
    db.scan();
    const rw::CookReport report = db.cook(force, [](const std::string& path) { std::printf("  %s\n", path.c_str()); });
    for (const std::string& m : report.messages)
        if (m.rfind("FAILED", 0) == 0) std::printf("%s\n", m.c_str());
    std::printf("%u assets: %u cooked, %u up to date, %u failed\n", report.scanned, report.cooked, report.skipped,
                report.failed);
    return report.failed ? 1 : 0;
}

int cmd_launch(int argc, char** argv, const char* tool) {
    if (argc < 3) return usage();
    rw::Project project;
    if (!open(argv[2], project)) return 1;
    if (project.module.empty()) {
        std::fprintf(stderr, "rewind: project.json has no \"module\"; set it to the game DLL first\n");
        return 1;
    }
    std::string command = "\"" + rw::executable_dir() + tool + "\" \"" + project.root.string() + "\"";
    for (int i = 3; i < argc; ++i) command += std::string(" \"") + argv[i] + "\"";
#if defined(_WIN32)
    command = "\"" + command + "\"";
#endif
    return std::system(command.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    rw::log_set_echo(true);
    if (cmd == "new") return cmd_new(argc, argv);
    if (cmd == "info") return cmd_info(argc, argv);
    if (cmd == "cook") return cmd_cook(argc, argv);
    if (cmd == "run") return cmd_launch(argc, argv, "rewind_run");
    if (cmd == "edit") return cmd_launch(argc, argv, "rewind_editor");
    return usage();
}
