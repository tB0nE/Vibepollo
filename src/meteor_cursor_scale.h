#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace meteor_cursor {
inline int read_scale(const std::string &path) {
  int scale = 1;
  std::ifstream file(path);
  if (!(file >> scale) || scale < 1 || scale > 4) return 1;
  file >> std::ws;
  return file.eof() ? scale : 1;
}
inline int current_scale() {
  static const std::string path = [] { auto p = std::getenv("METEOR_CURSOR_SCALE_FILE"); return p ? std::string(p) : std::string(); }();
  if (path.empty()) return 1;
  static std::once_flag announced;
  std::call_once(announced, [&] { std::ofstream(path + ".supported") << "x11-cursor-scale-v1\n"; });
  thread_local int cached = 1;
  thread_local auto next = std::chrono::steady_clock::time_point::min();
  const auto now = std::chrono::steady_clock::now();
  if (now >= next) { cached = read_scale(path); next = now + std::chrono::milliseconds(100); }
  return cached;
}

// XFixes pixels are unsigned long, but their low 32 bits are premultiplied ARGB.
// Scaling the hotspot along with the bitmap keeps the click point stationary.
template<class Pixel>
inline void blend(std::uint8_t *data, int width, int height, int row_pitch,
                  const Pixel *source, int source_w, int source_h,
                  int pointer_x, int pointer_y, int hot_x, int hot_y, int scale) {
  if (!data || !source || source_w <= 0 || source_h <= 0 || scale < 1 || scale > 4) return;
  const int left = pointer_x - hot_x * scale;
  const int top = pointer_y - hot_y * scale;
  const int right = std::min(width, left + source_w * scale);
  const int bottom = std::min(height, top + source_h * scale);
  for (int y = std::max(0, top); y < bottom; ++y) {
    auto *row = reinterpret_cast<std::uint32_t *>(data + y * row_pitch);
    const auto *src = source + ((y - top) / scale) * source_w;
    for (int x = std::max(0, left); x < right; ++x) {
      const auto pixel = static_cast<std::uint32_t>(src[(x - left) / scale]);
      const unsigned alpha = pixel >> 24;
      if (alpha == 255) row[x] = pixel;
      else if (alpha) {
        auto *out = reinterpret_cast<std::uint8_t *>(&row[x]);
        for (int channel = 0; channel < 3; ++channel)
          out[channel] = ((pixel >> (channel * 8)) & 255) + (out[channel] * (255 - alpha) + 127) / 255;
      }
    }
  }
}
}
