#pragma once

// Small bridges to the operating system for tools: native file and folder
// dialogs, launching a process, opening a URL. Dialog results arrive on
// the main thread from dispatch_dialog_results(), which a tool calls once
// per frame; the callbacks never run from another thread.

#include <functional>
#include <string>
#include <vector>

namespace rw {

/// Receives the chosen path, or an empty string when cancelled.
using DialogCallback = std::function<void(const std::string& path)>;

struct FileFilter {
    std::string name;    ///< "Game DLL"
    std::string pattern; ///< "dll;so;dylib" (extensions without dots, ';' separated) or "*"
};

void pick_folder(DialogCallback on_done, const std::string& start_dir = {});
void pick_file(DialogCallback on_done, const std::vector<FileFilter>& filters = {}, const std::string& start_dir = {});

/// Deliver finished dialogs to their callbacks. Call every frame.
void dispatch_dialog_results();

/// Start a program detached from this one. `args[0]` is the executable.
bool spawn_process(const std::vector<std::string>& args, std::string* error = nullptr);

/// Open a URL or file in the system's default handler.
bool open_url(const std::string& url);

} // namespace rw
