/**
 * @file src/platform/linux/wayland_workspace.h
 * @brief Pure KWin workspace-region geometry, shared by capture, manifest and input.
 *
 * Follows Polaris's single-workspace approach, using XDG output logical rectangles
 * directly. KWin stream_region(scale=1) supplies matching pixels without a CPU crop.
 */
#pragma once

#include <cmath>
#include <optional>
#include <set>
#include "display_manifest.h"

namespace platf {
  struct wayland_workspace_t {
    desktop_manifest_t manifest;
    // Signed compositor coordinates for stream_region; input is relative to desktop origin.
    int capture_x = 0, capture_y = 0;
    int input_x = 0, input_y = 0;
  };

  inline std::optional<wayland_workspace_t> make_wayland_workspace(
    const std::vector<output_descriptor_t> &logical_outputs,
    const std::vector<std::string> &selected
  ) {
    // Explicit selection is mandatory. Unknown/disabled names must never capture a fallback.
    if (selected.empty() || logical_outputs.empty()) {
      return std::nullopt;
    }
    std::set<std::string> names;
    bool have_bounds = false;
    int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    for (const auto &out : logical_outputs) {
      if (out.name.empty() || !names.insert(out.name).second ||
          std::abs(static_cast<long long>(out.x)) > 1000000 ||
          std::abs(static_cast<long long>(out.y)) > 1000000 ||
          out.width <= 0 || out.height <= 0 || out.width > 1000000 || out.height > 1000000) {
        return std::nullopt;
      }
      if (!out.capturable) {
        continue;
      }
      if (!have_bounds) {
        min_x = out.x; min_y = out.y; max_x = out.x + out.width; max_y = out.y + out.height;
        have_bounds = true;
      } else {
        min_x = std::min(min_x, out.x); min_y = std::min(min_y, out.y);
        max_x = std::max(max_x, out.x + out.width); max_y = std::max(max_y, out.y + out.height);
      }
    }
    for (const auto &name : selected) {
      if (std::none_of(logical_outputs.begin(), logical_outputs.end(), [&](const auto &out) {
            return out.name == name && out.capturable;
          })) {
        return std::nullopt;
      }
    }
    const auto region = compute_capture_region_from_outputs(logical_outputs, 0, 0, selected);
    // The initial region path deliberately does not promise GPU tile packing.
    if (!have_bounds || !region.contiguous || region.width <= 0 || region.height <= 0 ||
        region.width > k_max_capture_axis_px || region.height > k_max_capture_axis_px ||
        static_cast<long long>(region.width) * region.height > k_max_capture_total_px) {
      return std::nullopt;
    }
    wayland_workspace_t result;
    result.capture_x = region.offset_x;
    result.capture_y = region.offset_y;
    result.input_x = region.offset_x - min_x;
    result.input_y = region.offset_y - min_y;
    result.manifest.frame_width = region.width;
    result.manifest.frame_height = region.height;
    result.manifest.desktop_x = min_x;
    result.manifest.desktop_y = min_y;
    result.manifest.desktop_width = max_x - min_x;
    result.manifest.desktop_height = max_y - min_y;
    result.manifest.monitors = region.monitors;
    return result;
  }

  inline bool wayland_workspace_buffer_matches(const wayland_workspace_t &layout, int width, int height) {
    return width == layout.manifest.frame_width && height == layout.manifest.frame_height;
  }

  // Only automatic/KWin selection may use this path; explicit portal/wlr/KMS stay explicit.
  inline bool use_kwin_workspace(bool wayland, const std::string &backend, const std::string &outputs) {
    return wayland && (backend.empty() || backend == "kwin") && !split_csv(outputs).empty();
  }
}  // namespace platf
