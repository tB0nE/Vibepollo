/** PipeWire cursor metadata owned by the consumer, never by an encoder/producer. */
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <spa/buffer/meta.h>
#include <spa/param/video/format.h>
#include <vector>

namespace pipewire {
  struct cursor_metadata_t {
    static constexpr int max_dimension = 512;
    static constexpr int metadata_size = sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + max_dimension * max_dimension * 4;
    bool visible = false;
    int x = 0, y = 0, hot_x = 0, hot_y = 0, width = 0, height = 0;
    std::uint64_t serial = 0;
    std::vector<std::uint32_t> pixels;  // tightly packed premultiplied BGRA

    bool update(const spa_meta *meta) {
      if (!meta || !meta->data || meta->size < sizeof(spa_meta_cursor)) {
        return false;
      }
      const auto *base = static_cast<const std::uint8_t *>(meta->data);
      spa_meta_cursor c;
      std::memcpy(&c, base, sizeof(c));
      // KWin sends id=0 when the cursor leaves this captured region.
      if (!c.id) {
        visible = false;
        return true;
      }
      x = c.position.x;
      y = c.position.y;
      hot_x = c.hotspot.x;
      hot_y = c.hotspot.y;
      if (c.bitmap_offset) {
        if (c.bitmap_offset < sizeof(c) || c.bitmap_offset > meta->size || meta->size - c.bitmap_offset < sizeof(spa_meta_bitmap)) {
          visible = false;
          return true;
        }
        spa_meta_bitmap b;
        std::memcpy(&b, base + c.bitmap_offset, sizeof(b));
        if (b.format) {
          if (!b.offset) {
            visible = false;
            pixels.clear();
            return true;
          }
          const std::size_t w = b.size.width, h = b.size.height;
          const std::size_t remaining = meta->size - c.bitmap_offset;
          if ((b.format != SPA_VIDEO_FORMAT_BGRA && b.format != SPA_VIDEO_FORMAT_RGBA) || !w || !h || w > max_dimension || h > max_dimension || b.stride < static_cast<int>(w * 4) || b.offset < sizeof(b) || b.offset > remaining || (h - 1) * static_cast<std::size_t>(b.stride) + w * 4 > remaining - b.offset) {
            visible = false;
            return true;
          }
          pixels.resize(w * h);
          for (std::size_t row = 0; row < h; ++row) {
            auto *dst = reinterpret_cast<std::uint8_t *>(pixels.data() + row * w);
            const auto *src = base + c.bitmap_offset + b.offset + row * b.stride;
            for (std::size_t col = 0; col < w; ++col) {
              dst[col * 4] = src[col * 4 + (b.format == SPA_VIDEO_FORMAT_RGBA ? 2 : 0)];
              dst[col * 4 + 1] = src[col * 4 + 1];
              dst[col * 4 + 2] = src[col * 4 + (b.format == SPA_VIDEO_FORMAT_RGBA ? 0 : 2)];
              dst[col * 4 + 3] = src[col * 4 + 3];
            }
          }
          width = w;
          height = h;
          ++serial;
        }
      }
      visible = !pixels.empty();
      return true;
    }

    // Relative to the captured image, not global desktop coordinates. Clamp
    // malformed positions before multiplying hotspots by the requested scale.
    int left(int scale) const {
      return std::clamp(x, -1000000, 1000000) - std::clamp(hot_x, -max_dimension, max_dimension) * scale;
    }

    int top(int scale) const {
      return std::clamp(y, -1000000, 1000000) - std::clamp(hot_y, -max_dimension, max_dimension) * scale;
    }
  };
}  // namespace pipewire
