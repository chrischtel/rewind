// Exercises GameModule against a real game DLL: load, poll with no change,
// overwrite the DLL (new timestamp, same code), and check that the module
// swaps builds and carries the simulation state across.
//
//   hot_reload_test <path to bounce_hot.dll>

#include <rewind/core/game_module.h>
#include <rewind/sim/game.h>
#include <rewind/sim/serialize.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace rw;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

u64 fingerprint(Game& game) {
    Writer w;
    game.save(w);
    return hash_bytes(w.bytes());
}

/// Poll until the module reports something other than NoChange, or time out.
GameModule::Reload wait_for_reload(GameModule& module, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        GameModule::Reload r = module.poll();
        if (r.status != GameModule::Reload::Status::NoChange) return r;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: hot_reload_test <game dll>\n");
        return 2;
    }

    // Work on a private copy so the test never fights the build system.
    const fs::path original = argv[1];
    const fs::path work_dir = original.parent_path() / "hot_reload_test_work";
    std::error_code ec;
    fs::create_directories(work_dir, ec);
    const fs::path dll = work_dir / original.filename();
    fs::copy_file(original, dll, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);

    {
        GameModule module(dll);
        const bool loaded = module.load();
        if (!loaded) std::printf("load error: %s\n", module.error().c_str());
        CHECK(loaded);
        CHECK(module.game() != nullptr);
        CHECK(module.generation() == 1);

        // The numbered copy exists, the original is untouched.
        CHECK(fs::exists(work_dir / (original.stem().string() + ".hot1" + original.extension().string())));

        Game& game = *module.game();
        game.init();
        for (Tick t = 0; t < 120; ++t) game.tick(TickContext{t, 1.0f / 60.0f, InputFrame{}});
        const u64 before = fingerprint(game);

        // Nothing changed on disk: polling must be a no-op.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        CHECK(module.poll().status == GameModule::Reload::Status::NoChange);
        CHECK(module.generation() == 1);

        // Simulate a rebuild: rewrite the DLL so its timestamp moves.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        fs::copy_file(original, dll, fs::copy_options::overwrite_existing, ec);
        fs::last_write_time(dll, fs::file_time_type::clock::now(), ec);

        const GameModule::Reload reload = wait_for_reload(module, std::chrono::seconds(5));
        if (reload.status == GameModule::Reload::Status::Failed)
            std::printf("reload error: %s\n", reload.error.c_str());
        CHECK(reload.status == GameModule::Reload::Status::Reloaded);
        CHECK(reload.state_compatible);
        CHECK(module.generation() == 2);
        CHECK(module.game() != nullptr);
        CHECK(module.game() != &game); // a new object from the new build

        // The new build received the old state byte for byte.
        CHECK(fingerprint(*module.game()) == before);

        // Old copy gone, new copy present.
        CHECK(!fs::exists(work_dir / (original.stem().string() + ".hot1" + original.extension().string())));
        CHECK(fs::exists(work_dir / (original.stem().string() + ".hot2" + original.extension().string())));

        // And the new build keeps simulating deterministically.
        Game& next = *module.game();
        for (Tick t = 120; t < 240; ++t) next.tick(TickContext{t, 1.0f / 60.0f, InputFrame{}});
        CHECK(fingerprint(next) != before);
    }

    // Destroying the module removes its working copy.
    CHECK(!fs::exists(work_dir / (original.stem().string() + ".hot2" + original.extension().string())));
    fs::remove_all(work_dir, ec);

    if (failures == 0) {
        std::printf("hot_reload_test: all checks passed\n");
        return 0;
    }
    std::printf("hot_reload_test: %d check(s) failed\n", failures);
    return 1;
}
