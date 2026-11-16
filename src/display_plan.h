/**
 * @file src/display_plan.h
 * @brief The session's screen layout: which screens are streamed and where each sits in the frame.
 *
 * One plan is made per launch or resume, before any virtual display exists, and the capture,
 * the display manifest, input and Meteor's depth crop all read it. The layout depends only on
 * the screens' sizes, never on where Windows happens to place them on the desktop, so a client
 * can ask for the plan before launching and request exactly that stream size.
 */
#pragma once

// standard includes
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "virtual_display_modes.h"
#include "client_packing_budget.h"

namespace display_plan {

  /// HEVC's largest picture: 8192 per side, 35,389,440 pixels in all.
  inline constexpr int max_axis_px = 8192;
  inline constexpr long long max_total_px = 35389440;

  /**
   * The largest side the chosen encoder can encode, set after the encoder probe. Older GPUs stop
   * at 4096 (VA-API on AMD Polaris, for one); a plan that won't fit at full size is scaled down
   * evenly until it does.
   */
  inline std::atomic<int> encoder_max_axis {max_axis_px};

  /// A physical monitor attached to the desktop.
  struct physical_t {
    std::string name;  ///< The output's name, e.g. "\\.\DISPLAY1". Clients send these back as outputs=.
    int x = 0, y = 0, width = 0, height = 0;
    bool primary = false;
  };

  struct screen_t {
    std::string id;  ///< "m<n>" for the n-th physical monitor, "v<n>" for virtual display n.
    std::string label;  ///< The physical output's name; "virtual-<n>" for a virtual display.
    bool is_virtual = false;
    int virtual_index = 0;  ///< 1-based; 0 for a physical monitor.
    bool enabled = false;  ///< Part of the frame. Physical monitors left out are still listed.
    bool is_primary = false;  ///< The screen clients and Meteor's depth treat as the main one.
    int width = 0, height = 0;  ///< Its size on the desktop.
    int frame_x = 0, frame_y = 0;  ///< Where it sits in the frame; zero when not enabled.
    int requested_w = 0, requested_h = 0;
    int frame_w = 0, frame_h = 0;  ///< Its size in the frame: width x height unless the plan is scaled.
  };

  struct plan_t {
    int frame_width = 0, frame_height = 0;
    bool profiled = false;
    std::vector<screen_t> screens;

    std::vector<const screen_t *> enabled() const {
      std::vector<const screen_t *> result;
      for (const auto &s : screens) {
        if (s.enabled) {
          result.push_back(&s);
        }
      }
      return result;
    }
  };

  inline bool fits(int w, int h, int max_axis = max_axis_px, long long max_total = max_total_px) {
    return w <= max_axis && h <= max_axis && static_cast<long long>(w) * h <= max_total;
  }

  namespace detail {
    /// The frame size of the screens laid out cols to a row.
    inline std::pair<int, int> canvas(const std::vector<screen_t *> &screens, std::size_t cols) {
      int w = 0, h = 0;
      for (std::size_t start = 0; start < screens.size(); start += cols) {
        int row_w = 0, row_h = 0;
        for (std::size_t i = start; i < std::min(start + cols, screens.size()); ++i) {
          row_w += screens[i]->frame_w;
          row_h = std::max(row_h, screens[i]->frame_h);
        }
        w = std::max(w, row_w);
        h += row_h;
      }
      return {w, h};
    }

    /// Sets each screen's frame position, cols to a row, and returns the frame size.
    inline std::pair<int, int> place(std::vector<screen_t *> &screens, std::size_t cols) {
      int y = 0;
      for (std::size_t start = 0; start < screens.size(); start += cols) {
        int x = 0, row_h = 0;
        for (std::size_t i = start; i < std::min(start + cols, screens.size()); ++i) {
          screens[i]->frame_x = x;
          screens[i]->frame_y = y;
          x += screens[i]->frame_w;
          row_h = std::max(row_h, screens[i]->frame_h);
        }
        y += row_h;
      }
      return canvas(screens, cols);
    }
  }  // namespace detail

  /**
   * @brief Lays the enabled screens out in rows at their frame sizes: the least-padded fitting grid,
   *        breaking equal-area ties by the smallest maximum dimension. Sets each screen's frame position and returns the frame size.
   */
  inline std::pair<int, int> pack(std::vector<screen_t *> &screens, int max_axis = max_axis_px, long long max_total = max_total_px) {
    if (screens.empty()) {
      return {0, 0};
    }
    std::size_t best_cols = screens.size();
    auto best = detail::canvas(screens, best_cols);
    bool found_fit = false;
    for (std::size_t cols = screens.size(); cols > 0; --cols) {
      const auto size = detail::canvas(screens, cols);
      if (fits(size.first, size.second, max_axis, max_total)) {
        const auto area=1LL*size.first*size.second;
        const auto best_area=1LL*best.first*best.second;
        if (!found_fit || area<best_area || (area==best_area && std::max(size.first,size.second)<std::max(best.first,best.second))) {
          best_cols=cols;best=size;
        }
        found_fit=true;
        continue;
      }
      if (!found_fit && std::max(size.first, size.second) < std::max(best.first, best.second)) {
        best_cols = cols;
        best = size;
      }
    }
    return detail::place(screens, best_cols);
  }

  /**
   * @brief Packs the screens at full size if they fit; else scales every screen by the same
   *        factor, in the row layout that keeps them largest, so the frame fits.
   */
  inline std::pair<int, int> pack_to_fit(std::vector<screen_t *> &screens, int max_axis = max_axis_px, long long max_total = max_total_px) {
    for (auto *s : screens) {
      s->frame_w = s->requested_w > 0 ? s->requested_w : s->width;
      s->frame_h = s->requested_h > 0 ? s->requested_h : s->height;
    }
    auto size = pack(screens, max_axis, max_total);
    if (screens.empty() || fits(size.first, size.second, max_axis, max_total)) {
      return size;
    }
    double best_scale = 0.0;
    std::size_t best_cols = screens.size();
    for (std::size_t rows = 1; rows <= screens.size(); ++rows) {
      const auto cols = (screens.size() + rows - 1) / rows;
      const auto [w, h] = detail::canvas(screens, cols);
      if (w <= 0 || h <= 0) {
        continue;
      }
      const double scale = std::min({1.0, (double) max_axis / w, (double) max_axis / h, std::sqrt((double) max_total / ((double) w * h))});
      if (scale > best_scale) {
        best_scale = scale;
        best_cols = cols;
      }
    }
    // Rounded down to even sizes (4:2:0 chroma), so each row stays within the scaled total.
    for (auto *s : screens) {
      s->frame_w = std::max(2, static_cast<int>(std::floor((s->requested_w > 0 ? s->requested_w : s->width) * best_scale)) & ~1);
      s->frame_h = std::max(2, static_cast<int>(std::floor((s->requested_h > 0 ? s->requested_h : s->height) * best_scale)) & ~1);
    }
    return detail::place(screens, best_cols);
  }

  /**
   * @brief Makes the plan for a request.
   *
   * @param physical The physical monitors on the desktop.
   * @param selected The physical monitors asked for by name. Empty means the primary monitor;
   *                 names that match nothing are ignored, and if none match, the primary is used.
   * @param virtual_count How many virtual displays the session has. Each is the primary
   *                      monitor's size, which is how the host creates them.
   * @param virtual_only Stream the virtual displays and no physical monitor.
   * @return The plan, or nothing when there is no screen to stream. Screens that don't fit
   *         the encoder's largest frame at full size are scaled down evenly (frame_w, frame_h).
   */
  inline std::optional<plan_t> make(std::vector<physical_t> physical, const std::vector<std::string> &selected,
                                    int virtual_count, bool virtual_only, const virtual_display_modes::modes_t &modes = {}) {
    // Left to right, then top to bottom, so the frame reads like the desk.
    std::stable_sort(physical.begin(), physical.end(), [](const auto &a, const auto &b) {
      return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    const auto primary = std::find_if(physical.begin(), physical.end(), [](const auto &p) { return p.primary; });
    const physical_t *main_monitor = primary != physical.end() ? &*primary : (physical.empty() ? nullptr : &physical.front());

    plan_t plan;
    bool any_selected = false;
    for (std::size_t i = 0; i < physical.size(); ++i) {
      screen_t s;
      s.id = "m" + std::to_string(i);
      s.label = physical[i].name;
      s.width = physical[i].width;
      s.height = physical[i].height;
      s.enabled = !virtual_only && !selected.empty() &&
                  std::find(selected.begin(), selected.end(), physical[i].name) != selected.end();
      any_selected = any_selected || s.enabled;
      plan.screens.push_back(std::move(s));
    }
    if (!virtual_only && !any_selected && main_monitor) {
      for (auto &s : plan.screens) {
        s.enabled = s.label == main_monitor->name;
      }
    }
    const int vw = main_monitor ? main_monitor->width : 1920;
    const int vh = main_monitor ? main_monitor->height : 1080;
    for (int n = 1; n <= std::clamp(virtual_count, 0, 64); ++n) {
      screen_t s;
      s.id = "v" + std::to_string(n);
      s.label = "virtual-" + std::to_string(n);
      s.is_virtual = true;
      s.virtual_index = n;
      s.enabled = true;
      const auto size = virtual_display_modes::size(modes, n-1, vw, vh);
      s.width = size.first;
      s.height = size.second;
      plan.screens.push_back(std::move(s));
    }

    std::vector<screen_t *> enabled;
    for (auto &s : plan.screens) {
      if (s.enabled) {
        enabled.push_back(&s);
      }
    }
    if (enabled.empty()) {
      return std::nullopt;
    }
    // The primary physical monitor when it's streamed, else the first screen in the frame.
    screen_t *primary_screen = nullptr;
    for (auto *s : enabled) {
      if (!s->is_virtual && main_monitor && s->label == main_monitor->name) {
        primary_screen = s;
      }
    }
    (primary_screen ? primary_screen : enabled.front())->is_primary = true;

    auto &budget=client_packing::request();
    if (budget.active && !budget.valid) return std::nullopt;
    const int max_axis = std::min(std::clamp(encoder_max_axis.load(), 256, max_axis_px),budget.active?budget.axis:max_axis_px);
    const long long max_pixels=budget.active?budget.pixels:max_total_px;
    plan.profiled=budget.active;
    for (auto *screen:enabled) {
      double scale=1.0;
      const bool explicit_virtual=screen->is_virtual && screen->virtual_index<=static_cast<int>(modes.size()) && modes[screen->virtual_index-1].first>0;
      if (budget.active && !explicit_virtual) {
        scale=budget.physical_scale/100.0;
        if (budget.physical_w>0 && budget.physical_h>0)
          scale=std::min({1.0,(double)budget.physical_w/screen->width,(double)budget.physical_h/screen->height});
        if (auto mode=budget.physical_modes.find(screen->id);mode!=budget.physical_modes.end())
          scale=std::min({1.0,(double)mode->second.first/screen->width,(double)mode->second.second/screen->height});
      }
      screen->requested_w=std::max(2,(int)std::floor(screen->width*scale)&~1);
      screen->requested_h=std::max(2,(int)std::floor(screen->height*scale)&~1);
      screen->frame_w=screen->requested_w;screen->frame_h=screen->requested_h;
      budget.requested_sizes[screen->id]={screen->requested_w,screen->requested_h};
    }
    const auto unscaled=pack(enabled,max_axis,max_pixels);
    budget.seen=true;
    budget.fits=fits(unscaled.first,unscaled.second,max_axis,max_pixels);
    if (budget.active && budget.strict && !budget.fits) return std::nullopt;
    std::tie(plan.frame_width, plan.frame_height) = pack_to_fit(enabled,max_axis,max_pixels);
    if (!fits(plan.frame_width, plan.frame_height, max_axis,max_pixels)) {
      return std::nullopt;
    }
    return plan;
  }

  namespace detail {
    inline std::mutex mutex;
    inline std::optional<plan_t> current;
  }  // namespace detail

  /// The plan of the session being started or streamed.
  inline void set(plan_t plan) {
    std::lock_guard lock {detail::mutex};
    detail::current = std::move(plan);
  }

  inline std::optional<plan_t> get() {
    std::lock_guard lock {detail::mutex};
    return detail::current;
  }

}  // namespace display_plan
