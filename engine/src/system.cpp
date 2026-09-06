#include "rewind/core/system.h"

#include <SDL3/SDL.h>

#include <mutex>

namespace rw {

namespace {

struct Pending {
    DialogCallback callback;
    std::string path;
};

std::mutex g_mutex;
std::vector<Pending> g_results;

/// SDL may call this from another thread: only queue the result.
void SDLCALL on_dialog(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* callback = static_cast<DialogCallback*>(userdata);
    std::string path;
    if (filelist && filelist[0]) path = filelist[0];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_results.push_back(Pending{std::move(*callback), std::move(path)});
    }
    delete callback;
}

} // namespace

void pick_folder(DialogCallback on_done, const std::string& start_dir) {
    auto* callback = new DialogCallback(std::move(on_done));
    SDL_ShowOpenFolderDialog(on_dialog, callback, nullptr, start_dir.empty() ? nullptr : start_dir.c_str(), false);
}

void pick_file(DialogCallback on_done, const std::vector<FileFilter>& filters, const std::string& start_dir) {
    auto* callback = new DialogCallback(std::move(on_done));
    std::vector<SDL_DialogFileFilter> sdl_filters;
    for (const FileFilter& f : filters) sdl_filters.push_back(SDL_DialogFileFilter{f.name.c_str(), f.pattern.c_str()});
    SDL_ShowOpenFileDialog(on_dialog, callback, nullptr, sdl_filters.empty() ? nullptr : sdl_filters.data(),
                           static_cast<int>(sdl_filters.size()), start_dir.empty() ? nullptr : start_dir.c_str(), false);
}

void dispatch_dialog_results() {
    std::vector<Pending> ready;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ready.swap(g_results);
    }
    for (Pending& p : ready)
        if (p.callback) p.callback(p.path);
}

bool spawn_process(const std::vector<std::string>& args, std::string* error) {
    if (args.empty()) {
        if (error) *error = "no program given";
        return false;
    }
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Process* process = SDL_CreateProcess(argv.data(), false);
    if (!process) {
        if (error) *error = SDL_GetError();
        return false;
    }
    SDL_DestroyProcess(process); // detaches; the child keeps running
    return true;
}

bool open_url(const std::string& url) { return SDL_OpenURL(url.c_str()); }

} // namespace rw
