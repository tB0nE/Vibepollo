/**
 * @file src/platform/linux/display_manifest.h
 * @brief Pure geometry for multi-monitor capture and the desktop manifest Nightfall reads.
 *
 * A session can ask for several monitors with `outputs=DP-0,DP-2` on /launch or /resume. The
 * selected monitors become one frame: a crop of their bounding box when it fits the client's
 * decoder, or the monitors packed into rows when it doesn't. GET /polaris/v1/display/manifest
 * tells the client where each monitor is in that frame. Ported from Nightfall's Polaris fork.
 *
 * No X11 types here, so the tests can use it directly.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

// local includes
#include "src/platform/frame_tile.h"

namespace platf {

  /**
   * @brief Nightfall's HEVC decoder limits: 8192 px per axis and about 35.4 M pixels in all.
   *
   * A selection whose bounding box is wider or taller is packed into rows.
   */
  inline constexpr int k_max_capture_axis_px = 8192;
  inline constexpr long long k_max_capture_total_px = 35389440;

  /**
   * @brief A connected output in desktop coordinates.
   */
  struct output_descriptor_t {
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    bool primary = false;
    /// False for a connected output that isn't showing anything (no CRTC): it is listed in
    /// the manifest but can't be captured.
    bool capturable = true;
    /// Wayland mode size divided by scale gives compositor-logical size; X11 stays 1.0.
    double scale = 1.0;
  };

  /**
   * @brief One monitor in the manifest.
   */
  struct monitor_manifest_entry_t {
    std::string id;  ///< "m<n>", in enumeration order.
    std::string label;  ///< The output's name, e.g. "DP-0". Clients send these back as outputs=.
    bool enabled = false;  ///< Part of the captured frame.
    bool capturable = true;
    bool is_primary = false;
    int frame_x = 0, frame_y = 0, frame_w = 0, frame_h = 0;  ///< Where it is in the frame; zero when not enabled.
    int desktop_x = 0, desktop_y = 0, desktop_w = 0, desktop_h = 0;  ///< Where it is on the desktop.
  };

  struct capture_region_t {
    int offset_x = 0, offset_y = 0, width = 0, height = 0;
    std::vector<monitor_manifest_entry_t> monitors;
    /// False when an output that wasn't asked for lies inside the bounding box of those that
    /// were, so a crop would show it too.
    bool contiguous = true;
    /// True when the monitors were packed into rows; each is then captured on its own and
    /// copied to its frame rect.
    bool tiled = false;
  };

  struct desktop_manifest_t {
    int frame_width = 0, frame_height = 0;
    int desktop_x = 0, desktop_y = 0, desktop_width = 0, desktop_height = 0;
    std::vector<monitor_manifest_entry_t> monitors;
    bool contiguous = true;
    bool tiled = false;
  };

  /**
   * @brief The bounding box of the outputs named in `only` (every capturable output when
   *        empty), with each monitor's frame and desktop rect.
   *
   * When nothing matches, the region is the whole desktop (`env_width` x `env_height`).
   */
  inline capture_region_t compute_capture_region_from_outputs(
    const std::vector<output_descriptor_t> &outputs,
    int env_width,
    int env_height,
    const std::vector<std::string> &only
  ) {
    capture_region_t region;
    const auto selected = [&](const std::string &name) {
      return only.empty() || std::find(only.begin(), only.end(), name) != only.end();
    };

    int index = 0;
    bool have_bounds = false;
    int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    for (const auto &out : outputs) {
      monitor_manifest_entry_t entry;
      entry.id = "m" + std::to_string(index++);
      entry.label = out.name.empty() ? entry.id : out.name;
      entry.is_primary = out.primary;
      entry.capturable = out.capturable;
      entry.desktop_x = out.x;
      entry.desktop_y = out.y;
      entry.desktop_w = out.width;
      entry.desktop_h = out.height;
      entry.enabled = out.capturable && selected(entry.label);

      if (entry.enabled) {
        if (!have_bounds) {
          min_x = out.x;
          min_y = out.y;
          max_x = out.x + out.width;
          max_y = out.y + out.height;
          have_bounds = true;
        } else {
          min_x = std::min(min_x, out.x);
          min_y = std::min(min_y, out.y);
          max_x = std::max(max_x, out.x + out.width);
          max_y = std::max(max_y, out.y + out.height);
        }
      }
      region.monitors.push_back(std::move(entry));
    }

    if (!have_bounds) {
      region.width = env_width;
      region.height = env_height;
      return region;
    }

    region.offset_x = min_x;
    region.offset_y = min_y;
    region.width = max_x - min_x;
    region.height = max_y - min_y;
    for (auto &m : region.monitors) {
      if (m.enabled) {
        m.frame_x = m.desktop_x - min_x;
        m.frame_y = m.desktop_y - min_y;
        m.frame_w = m.desktop_w;
        m.frame_h = m.desktop_h;
      }
    }

    if (!only.empty()) {
      for (const auto &out : outputs) {
        if (!out.capturable || selected(out.name)) {
          continue;
        }
        const bool overlap_x = out.x < max_x && out.x + out.width > min_x;
        const bool overlap_y = out.y < max_y && out.y + out.height > min_y;
        if (overlap_x && overlap_y) {
          region.contiguous = false;
          break;
        }
      }
    }
    return region;
  }

  /**
   * @brief Like compute_capture_region_from_outputs(), but packs the selected monitors into
   *        rows when their bounding box exceeds `max_axis_px` or `max_total_px`.
   *
   * Monitors keep their order and the row count grows until the frame fits. A region that
   * already fits is returned unchanged, so one or two monitors are never packed.
   */
  inline capture_region_t compute_packed_capture_region_from_outputs(
    const std::vector<output_descriptor_t> &outputs,
    int env_width,
    int env_height,
    const std::vector<std::string> &only,
    int max_axis_px = k_max_capture_axis_px,
    long long max_total_px = k_max_capture_total_px
  ) {
    auto region = compute_capture_region_from_outputs(outputs, env_width, env_height, only);
    const auto fits = [&](int w, int h) {
      return (max_axis_px <= 0 || (w <= max_axis_px && h <= max_axis_px)) &&
             (max_total_px <= 0 || static_cast<long long>(w) * h <= max_total_px);
    };
    if (max_axis_px <= 0 || fits(region.width, region.height)) {
      return region;
    }

    std::vector<monitor_manifest_entry_t *> enabled;
    for (auto &m : region.monitors) {
      if (m.enabled) {
        enabled.push_back(&m);
      }
    }
    if (enabled.size() <= 1) {
      return region;
    }

    const auto canvas = [&](std::size_t cols) {
      int w = 0, h = 0;
      for (std::size_t start = 0; start < enabled.size(); start += cols) {
        int row_w = 0, row_h = 0;
        for (std::size_t i = start; i < std::min(start + cols, enabled.size()); ++i) {
          row_w += enabled[i]->desktop_w;
          row_h = std::max(row_h, enabled[i]->desktop_h);
        }
        w = std::max(w, row_w);
        h += row_h;
      }
      return std::pair {w, h};
    };

    // The first row count that fits, else the one with the smallest longest side.
    std::size_t best_cols = enabled.size();
    auto best = std::pair {region.width, region.height};
    for (std::size_t rows = 2; rows <= enabled.size(); ++rows) {
      const auto cols = (enabled.size() + rows - 1) / rows;
      const auto size = canvas(cols);
      if (fits(size.first, size.second)) {
        best_cols = cols;
        best = size;
        break;
      }
      if (std::max(size.first, size.second) < std::max(best.first, best.second)) {
        best_cols = cols;
        best = size;
      }
    }

    int y = 0;
    for (std::size_t start = 0; start < enabled.size(); start += best_cols) {
      int x = 0, row_h = 0;
      for (std::size_t i = start; i < std::min(start + best_cols, enabled.size()); ++i) {
        enabled[i]->frame_x = x;
        enabled[i]->frame_y = y;
        x += enabled[i]->desktop_w;
        row_h = std::max(row_h, enabled[i]->desktop_h);
      }
      y += row_h;
    }

    region.width = best.first;
    region.height = best.second;
    region.tiled = true;
    return region;
  }

  /**
   * @brief The frame tiles of a packed region, for capture and input; empty when not packed.
   */
  inline std::vector<frame_tile_t> frame_tiles_of(const capture_region_t &region) {
    std::vector<frame_tile_t> tiles;
    if (!region.tiled) {
      return tiles;
    }
    for (const auto &m : region.monitors) {
      if (m.enabled) {
        tiles.push_back({m.frame_x, m.frame_y, m.desktop_x, m.desktop_y, m.frame_w, m.frame_h});
      }
    }
    return tiles;
  }

  /**
   * @brief Maps a point in a packed frame to the desktop: the tile it falls in, or the
   *        nearest tile for a point between tiles. Returns the point unchanged without tiles.
   */
  inline std::pair<float, float> frame_to_desktop(const std::vector<frame_tile_t> &tiles, float x, float y) {
    if (tiles.empty()) {
      return {x, y};
    }
    const frame_tile_t *nearest = nullptr;
    float nearest_distance = 0;
    for (const auto &tile : tiles) {
      const float dx = std::max({(float) tile.frame_x - x, 0.0f, x - (float) (tile.frame_x + tile.width - 1)});
      const float dy = std::max({(float) tile.frame_y - y, 0.0f, y - (float) (tile.frame_y + tile.height - 1)});
      const float distance = dx * dx + dy * dy;
      if (!nearest || distance < nearest_distance) {
        nearest = &tile;
        nearest_distance = distance;
      }
    }
    const float local_x = std::clamp(x - nearest->frame_x, 0.0f, (float) (nearest->width - 1));
    const float local_y = std::clamp(y - nearest->frame_y, 0.0f, (float) (nearest->height - 1));
    return {nearest->desktop_x + local_x, nearest->desktop_y + local_y};
  }

  /**
   * @brief Splits "DP-0, HDMI-1" into trimmed, non-empty names.
   */
  inline std::vector<std::string> split_csv(const std::string &csv) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= csv.size()) {
      const auto comma = csv.find(',', start);
      auto first = start;
      auto last = comma == std::string::npos ? csv.size() : comma;
      while (first < last && std::isspace(static_cast<unsigned char>(csv[first]))) {
        ++first;
      }
      while (last > first && std::isspace(static_cast<unsigned char>(csv[last - 1]))) {
        --last;
      }
      if (last > first) {
        result.emplace_back(csv.substr(first, last - first));
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
    return result;
  }

}  // namespace platf
