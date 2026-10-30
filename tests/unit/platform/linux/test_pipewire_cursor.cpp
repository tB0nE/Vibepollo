#include "../../../tests_common.h"

#include <array>
#include <limits>
#include <src/meteor_cursor_scale.h>
#include <src/platform/linux/pipewire_cursor.h>

namespace {
  struct packet_t {
    std::array<std::uint8_t, 256> storage {};
    spa_meta_cursor c {1, 0, {20, 30}, {1, 1}, sizeof(spa_meta_cursor)};
    spa_meta_bitmap b {SPA_VIDEO_FORMAT_RGBA, {2, 2}, 12, sizeof(spa_meta_bitmap)};
    spa_meta meta {SPA_META_Cursor, static_cast<uint32_t>(storage.size()), storage.data()};

    void sync() {
      std::memcpy(storage.data(), &c, sizeof(c));
      std::memcpy(storage.data() + sizeof(c), &b, sizeof(b));
    }

    packet_t() {
      sync();
      const std::uint8_t rgba[] = {128, 0, 0, 128, 0, 255, 0, 255, 99, 99, 99, 99, 0, 0, 255, 255, 255, 255, 255, 255};
      std::memcpy(storage.data() + sizeof(c) + sizeof(b), rgba, sizeof(rgba));
    }
  };
}  // namespace

TEST(PipewireCursor, ConvertsRgbaRowsWithPaddingAndOwnsPixels) {
  packet_t p;
  pipewire::cursor_metadata_t c;
  ASSERT_TRUE(c.update(&p.meta));
  ASSERT_TRUE(c.visible);
  ASSERT_EQ(c.pixels.size(), 4u);
  EXPECT_EQ(c.pixels[0], 0x80800000u);
  EXPECT_EQ(c.pixels[2], 0xff0000ffu);
  p.storage.fill(0);
  EXPECT_EQ(c.pixels[0], 0x80800000u);
}

TEST(PipewireCursor, KeepsBitmapForPositionOnlyPacketsAndRescalesHotspot) {
  packet_t p;
  pipewire::cursor_metadata_t c;
  c.update(&p.meta);
  p.c.bitmap_offset = 0;
  p.c.position = {100, 200};
  p.sync();
  c.update(&p.meta);
  EXPECT_TRUE(c.visible);
  EXPECT_EQ(c.serial, 1u);
  EXPECT_EQ(c.left(4), 96);
  EXPECT_EQ(c.top(4), 196);
}

TEST(PipewireCursor, KwinZeroIdHidesCursorWithoutDiscardingShape) {
  packet_t p;
  pipewire::cursor_metadata_t c;
  c.update(&p.meta);
  p.c.id = 0;
  p.sync();
  c.update(&p.meta);
  EXPECT_FALSE(c.visible);
  p.c.id = 1;
  p.c.bitmap_offset = 0;
  p.sync();
  c.update(&p.meta);
  EXPECT_TRUE(c.visible);
}

TEST(PipewireCursor, HandlesUnknownFormatAndExplicitlyInvisibleBitmap) {
  packet_t p;
  pipewire::cursor_metadata_t c;
  c.update(&p.meta);
  p.b.format = 0;
  p.sync();
  c.update(&p.meta);
  EXPECT_TRUE(c.visible);
  p.b.format = SPA_VIDEO_FORMAT_RGBA;
  p.b.offset = 0;
  p.sync();
  c.update(&p.meta);
  EXPECT_FALSE(c.visible);
  p.c.bitmap_offset = 0;
  p.sync();
  c.update(&p.meta);
  EXPECT_FALSE(c.visible);
}

TEST(PipewireCursor, RejectsTruncatedOffsetsAndInvalidStride) {
  for (int kind = 0; kind < 7; ++kind) {
    packet_t p;
    pipewire::cursor_metadata_t c;
    if (kind == 0) {
      p.meta.size = sizeof(spa_meta_cursor) - 1;
    }
    if (kind == 1) {
      p.c.bitmap_offset = std::numeric_limits<unsigned>::max();
    }
    if (kind == 2) {
      p.b.offset = std::numeric_limits<unsigned>::max();
    }
    if (kind == 3) {
      p.b.size.width = 10000;
    }
    if (kind == 4) {
      p.b.stride = -1;
    }
    if (kind == 5) {
      p.b.stride = 4;
    }
    if (kind == 6) {
      p.meta.size = sizeof(p.c) + sizeof(p.b) + 4;
    }
    p.sync();
    c.update(&p.meta);
    EXPECT_FALSE(c.visible) << kind;
  }
}

TEST(PipewireCursor, ScaledClippedOverlayKeepsHotspotAndPremultipliedEdges) {
  packet_t p;
  pipewire::cursor_metadata_t c;
  c.update(&p.meta);
  c.x = 0;
  c.y = 0;
  std::array<std::uint32_t, 16> screen;
  screen.fill(0xff202020);
  meteor_cursor::blend(reinterpret_cast<std::uint8_t *>(screen.data()), 4, 4, 16, c.pixels.data(), c.width, c.height, c.left(2), c.top(2), 0, 0, 2);
  EXPECT_EQ(screen[0], 0xffffffffu);  // cursor bottom right at pointer origin
  EXPECT_EQ(screen[3], 0xff202020u);  // untouched, beyond the clipped cursor
}

TEST(PipewireCursor, MalformedMetadataFuzzDoesNotReadOutsideProvidedSpan) {
  std::uint32_t rng = 17;
  for (int i = 0; i < 5000; ++i) {
    packet_t p;
    pipewire::cursor_metadata_t c;
    for (auto &b : p.storage) {
      rng = rng * 1664525u + 1013904223u;
      b = rng >> 24;
    }
    p.meta.size = i % p.storage.size();
    c.update(&p.meta);
  }
}
