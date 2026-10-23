/**
 * @file src/platform/frame_tile.h
 * @brief Where one monitor sits in a multi-monitor frame packed into rows.
 */
#pragma once

namespace platf {
  /**
   * @brief One monitor's place in a multi-monitor frame whose monitors were packed into rows
   *        instead of cropped from the desktop as one rectangle.
   *
   * Absolute input inside the tile maps back to the desktop at desktop_x/desktop_y.
   */
  struct frame_tile_t {
    int frame_x, frame_y;
    int desktop_x, desktop_y;
    int width, height;
  };
}  // namespace platf
