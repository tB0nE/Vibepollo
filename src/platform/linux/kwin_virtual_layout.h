/**
 * @file src/platform/linux/kwin_virtual_layout.h
 * @brief Where a session's KWin virtual monitors go and which outputs it captures.
 *
 * KWin captures one desktop rectangle (stream_region), so the virtual monitors sit in a row
 * at the desktop's right edge, level with the top of the chosen monitors, or in a grid beside
 * and below them when a row would be wider than a stream allows. The session then
 * captures the chosen monitors, the row, and any monitor between them, which keeps the
 * rectangle free of monitors nobody asked for. A client can ask for this layout before
 * launching, before the virtual monitors exist, and get the same frame the launch makes.
 */
#pragma once

#include <algorithm>
#include <climits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "display_manifest.h"
#include "wayland_workspace.h"

namespace platf::kwin_virtual {
  inline constexpr int max_count = 4;
  /// KWin names an output made by stream_virtual_output "Virtual-" plus the name asked for.
  inline constexpr std::string_view requested_prefix = "Nightfall-Virtual-";
  inline constexpr std::string_view output_prefix = "Virtual-Nightfall-Virtual-";

  /// The name to ask KWin for, for virtual monitor `index` (1-based).
  inline std::string requested_name(int index) {
    return std::string {requested_prefix} + std::to_string(index);
  }

  /// The output name KWin gives virtual monitor `index` (1-based).
  inline std::string output_name(int index) {
    return std::string {output_prefix} + std::to_string(index);
  }

  inline bool is_virtual_output(std::string_view name) {
    return name.starts_with(output_prefix);
  }

  struct layout_t {
    /// Every output, physical ones first, with the virtual row at its planned positions.
    std::vector<output_descriptor_t> outputs;
    /// What the session captures, in output order.
    std::vector<std::string> selected;
    wayland_workspace_t workspace;
  };

  /**
   * @brief Plans a session's virtual monitors and selection.
   *
   * @param physical The desktop's outputs in KWin's order, without any virtual monitor of
   *                 ours. The first one marked primary (else the first) is the main monitor.
   * @param chosen The physical monitors asked for by name. Empty means the main monitor.
   * @param count How many virtual monitors, each the size of the main monitor.
   * @param virtual_only Capture the virtual monitors alone; the first becomes the primary.
   * @param error Why there is no layout, when there isn't.
   */
  inline std::optional<layout_t> plan(
    std::vector<output_descriptor_t> physical,
    std::vector<std::string> chosen,
    int count,
    bool virtual_only,
    std::string &error
  ) {
    std::erase_if(physical, [](const auto &out) {
      return out.is_virtual || is_virtual_output(out.name);
    });
    const auto main_it = std::find_if(physical.begin(), physical.end(), [](const auto &out) {
      return out.primary && out.capturable;
    });
    const auto first_capturable = std::find_if(physical.begin(), physical.end(), [](const auto &out) {
      return out.capturable;
    });
    const auto main = main_it != physical.end() ? main_it : first_capturable;
    if (main == physical.end()) {
      error = "No monitor to size the virtual monitors from";
      return std::nullopt;
    }
    count = std::clamp(count, 0, max_count);
    if (virtual_only && count == 0) {
      error = "Virtual monitors only, but none were asked for";
      return std::nullopt;
    }

    std::erase_if(chosen, [&](const auto &name) {
      return std::none_of(physical.begin(), physical.end(), [&](const auto &out) {
        return out.capturable && out.name == name;
      });
    });
    if (virtual_only) {
      chosen.clear();
    } else if (chosen.empty()) {
      chosen.push_back(main->name);
    }

    int right = main->x + main->width;
    for (const auto &out : physical) {
      right = std::max(right, out.x + out.width);
    }
    const auto is_chosen = [&](const std::string &name) {
      return std::find(chosen.begin(), chosen.end(), name) != chosen.end();
    };
    int left = INT_MAX, top = INT_MAX;
    for (const auto &out : physical) {
      if (is_chosen(out.name)) {
        left = std::min(left, out.x);
        top = std::min(top, out.y);
      }
    }
    if (chosen.empty()) {
      left = right;
      top = main->y;
    }

    std::vector<output_descriptor_t> base = physical;
    if (virtual_only) {
      for (auto &out : base) {
        out.primary = false;
      }
    }
    const auto make_virtual = [&](int index, int x, int y) {
      output_descriptor_t out;
      out.name = output_name(index);
      out.x = x;
      out.y = y;
      out.width = main->width;
      out.height = main->height;
      out.primary = virtual_only && index == 1;
      out.is_virtual = true;
      return out;
    };

    // Captures the chosen monitors and the virtual ones, plus every monitor inside their
    // rectangle until none is left out; nothing when that can't be one stream.
    const auto finish = [&](std::vector<output_descriptor_t> outputs) -> std::optional<layout_t> {
      std::vector<std::string> wanted = chosen;
      for (int i = 1; i <= count; ++i) {
        wanted.push_back(output_name(i));
      }
      for (bool grew = true; grew;) {
        grew = false;
        const auto region = compute_capture_region_from_outputs(outputs, 0, 0, wanted);
        for (const auto &out : outputs) {
          if (!out.capturable || std::find(wanted.begin(), wanted.end(), out.name) != wanted.end()) {
            continue;
          }
          const bool overlap_x = out.x < region.offset_x + region.width && out.x + out.width > region.offset_x;
          const bool overlap_y = out.y < region.offset_y + region.height && out.y + out.height > region.offset_y;
          if (overlap_x && overlap_y) {
            wanted.push_back(out.name);
            grew = true;
          }
        }
      }
      layout_t result;
      result.outputs = std::move(outputs);
      for (const auto &out : result.outputs) {
        if (std::find(wanted.begin(), wanted.end(), out.name) != wanted.end()) {
          result.selected.push_back(out.name);
        }
      }
      auto workspace = make_wayland_workspace(result.outputs, result.selected);
      if (!workspace) {
        return std::nullopt;
      }
      result.workspace = std::move(*workspace);
      return result;
    };

    // One row at the desktop's right edge when it fits, as Windows uses one row when it can.
    {
      auto outputs = base;
      for (int i = 1; i <= count; ++i) {
        outputs.push_back(make_virtual(i, right + (i - 1) * main->width, top));
      }
      if (auto layout = finish(std::move(outputs))) {
        return layout;
      }
    }

    // Otherwise a grid of main-monitor-sized cells from the chosen monitors' top-left corner,
    // filled row by row and never over another monitor. Of the grid widths that fit a stream,
    // the one with the smallest captured rectangle wins (fewer rows on a tie).
    std::optional<layout_t> best;
    long long best_area = 0;
    for (int cols = 1; cols <= 2 * max_count; ++cols) {
      auto outputs = base;
      const auto free = [&](int x, int y) {
        return std::none_of(outputs.begin(), outputs.end(), [&](const auto &out) {
          return x < out.x + out.width && x + main->width > out.x && y < out.y + out.height && y + main->height > out.y;
        });
      };
      int placed = 0;
      for (int cell = 0; placed < count && cell < cols * (max_count + 1); ++cell) {
        const int x = left + (cell % cols) * main->width;
        const int y = top + (cell / cols) * main->height;
        if (free(x, y)) {
          outputs.push_back(make_virtual(++placed, x, y));
        }
      }
      if (placed < count) {
        continue;
      }
      auto layout = finish(std::move(outputs));
      if (!layout) {
        continue;
      }
      const long long area = static_cast<long long>(layout->workspace.manifest.frame_width) * layout->workspace.manifest.frame_height;
      if (!best || area < best_area ||
          (area == best_area && layout->workspace.manifest.frame_height < best->workspace.manifest.frame_height)) {
        best_area = area;
        best = std::move(layout);
      }
    }
    if (best) {
      return best;
    }
    error = "The chosen monitors and virtual monitors don't fit in one stream";
    return std::nullopt;
  }

  /// Comma-separated names, as outputs= and capture_outputs take them.
  inline std::string join(const std::vector<std::string> &names) {
    std::string csv;
    for (const auto &name : names) {
      if (!csv.empty()) {
        csv += ',';
      }
      csv += name;
    }
    return csv;
  }

  /**
   * @brief Marks virtual outputs and fixes the primary for a selection of only virtual ones.
   *
   * KWin reports our virtual monitors as ordinary outputs. When none of the captured outputs is
   * physical, the first captured virtual monitor stands in as the primary.
   */
  inline void mark_virtual(std::vector<output_descriptor_t> &outputs, const std::vector<std::string> &selected) {
    bool any_physical = false;
    for (auto &out : outputs) {
      out.is_virtual = out.is_virtual || is_virtual_output(out.name);
      const bool chosen = std::find(selected.begin(), selected.end(), out.name) != selected.end();
      any_physical = any_physical || (chosen && !out.is_virtual);
    }
    if (any_physical || selected.empty()) {
      return;
    }
    bool primary_set = false;
    for (auto &out : outputs) {
      const bool chosen = std::find(selected.begin(), selected.end(), out.name) != selected.end();
      out.primary = !primary_set && chosen && out.is_virtual;
      primary_set = primary_set || out.primary;
    }
  }
}  // namespace platf::kwin_virtual
