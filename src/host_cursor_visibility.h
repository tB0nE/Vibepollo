#pragma once
#include <atomic>

extern std::atomic_bool display_cursor;
// The API and keyboard shortcut share one atomic request. Capture backends
// receive a thread-local bool pointer, refreshed between captured frames.
namespace host_cursor_visibility {
inline bool visible() { return display_cursor.load(std::memory_order_relaxed); }
inline void set_visible(bool visible) { display_cursor.store(visible, std::memory_order_relaxed); }
}
