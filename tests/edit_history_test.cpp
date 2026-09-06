// Headless checks for EditHistory: streak merging, undo/redo cursor, redo tail drop, cap.

#include <rewind/debug/edit_history.h>

#include <cstdio>
#include <vector>

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

std::vector<u8> state(u8 v) { return {v}; }

void test_streaks_and_cursor() {
    EditHistory h;
    h.frame(false, state(0));
    CHECK(!h.can_undo() && !h.can_redo());
    // A drag: three edited frames in a row make one entry (before = 0, after = 3).
    h.frame(true, state(1));
    h.frame(true, state(2));
    h.frame(true, state(3));
    CHECK(!h.can_undo()); // still in the streak
    h.frame(false, state(3));
    CHECK(h.can_undo() && h.size() == 1);
    CHECK(*h.undo() == state(0));
    CHECK(!h.can_undo() && h.can_redo());
    CHECK(*h.redo() == state(3));
    CHECK(!h.can_redo());

    // Second edit, then undo twice, redo once.
    h.frame(false, state(3));
    h.frame(true, state(4));
    h.frame(false, state(4));
    CHECK(h.size() == 2);
    CHECK(*h.undo() == state(3));
    CHECK(*h.undo() == state(0));
    CHECK(h.undo() == nullptr);
    CHECK(*h.redo() == state(3));
    CHECK(h.position() == 1);

    // Editing after an undo drops the redo tail.
    h.frame(false, state(3));
    h.frame(true, state(9));
    h.frame(false, state(9));
    CHECK(h.size() == 2 && !h.can_redo());
    CHECK(*h.undo() == state(3));
    CHECK(*h.redo() == state(9));

    h.clear();
    CHECK(h.size() == 0 && !h.can_undo());
}

void test_cap() {
    EditHistory h(3);
    h.frame(false, state(0));
    for (u8 i = 1; i <= 5; ++i) {
        h.frame(true, state(i));
        h.frame(false, state(i));
    }
    CHECK(h.size() == 3);
    CHECK(*h.undo() == state(4));
    CHECK(*h.undo() == state(3));
    CHECK(*h.undo() == state(2));
    CHECK(h.undo() == nullptr);
}

} // namespace

int main() {
    test_streaks_and_cursor();
    test_cap();
    if (failures == 0) std::printf("edit_history_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
