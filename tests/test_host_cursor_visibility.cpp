#include "../src/host_cursor_visibility.h"
#include <cassert>
#include <thread>
std::atomic_bool display_cursor {true};
int main() {
    assert(host_cursor_visibility::visible());
    host_cursor_visibility::set_visible(false);
    assert(!host_cursor_visibility::visible());
    display_cursor = !display_cursor; // Existing keyboard shortcut.
    assert(host_cursor_visibility::visible());
    std::thread api([]{for (int i=0;i<10000;++i) host_cursor_visibility::set_visible(i%2);});
    for(int i=0;i<10000;++i) { bool capture_cursor=host_cursor_visibility::visible(); (void)capture_cursor; }
    api.join();
    host_cursor_visibility::set_visible(true);
    assert(host_cursor_visibility::visible());
}
