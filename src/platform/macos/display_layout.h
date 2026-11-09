/**
 * @file src/platform/macos/display_layout.h
 * @brief Meteor's multi-monitor capture on macOS: the session's displays, its virtual displays
 *        and where each display sits in the frame. Plain C++, so non-Objective-C code can use it.
 *
 * Same design as Windows' display_multi.h: one plan per launch (src/display_plan.h), screens
 * packed into a frame that never changes size while the session runs, a black tile until a
 * screen's display appears. Enabled by Nightfall Meteor with METEOR_MACOS_MULTIMONITOR=1.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "src/display_plan.h"

namespace platf::macos {

  /// One active display on the desktop.
  struct attached_output_t {
    std::string name;  ///< Its name in System Settings, made unique; what clients send back as outputs=.
    std::uint32_t id = 0;  ///< CGDirectDisplayID.
    double x = 0, y = 0, points_w = 0, points_h = 0;  ///< CGDisplayBounds: the desktop, in points.
    int width = 0, height = 0;  ///< The display's mode in pixels, as it is captured.
    bool is_virtual = false;  ///< One this host made for the session.
    bool primary = false;
  };

  /// Whether Meteor asked for multi-monitor capture (METEOR_MACOS_MULTIMONITOR).
  bool multimonitor_enabled();

  /// The active displays: physical ones in desktop order, then this host's virtual ones in the
  /// order they were made. Mirrored displays appear once, as their mirror set's main display.
  std::vector<attached_output_t> attached_outputs();

  /// The plan a launch with `requested` (outputs=, or capture_outputs::virtual_only_token) and
  /// `virtual_count` virtual displays gets.
  std::optional<display_plan::plan_t> make_plan(const std::string &requested, int virtual_count);

  /// The plan of the session being streamed; one made from the desktop as it is otherwise.
  std::optional<display_plan::plan_t> session_plan();

  /// The display each of the plan's screens is captured from, if it's attached.
  std::vector<std::optional<attached_output_t>> place(const display_plan::plan_t &plan, const std::vector<attached_output_t> &outputs);

  /**
   * @brief Makes the session's virtual displays (the primary display's size) and keeps them
   *        until released. 0 removes any this host made.
   * @return False with `error` set when they can't be made.
   */
  bool prepare_virtual_outputs(int count, int fps, std::string &error);
  void release_virtual_outputs();
  /// Removes them after `delay` unless a launch or resume cancels it first.
  void schedule_virtual_outputs_release(std::chrono::seconds delay);
  void cancel_virtual_outputs_release();
  /// Whether this macOS can make virtual displays (the CGVirtualDisplay classes exist).
  bool virtual_outputs_supported();
  /// The CGDirectDisplayIDs of this host's virtual displays, in the order they were made.
  std::vector<std::uint32_t> virtual_output_ids();

  /// One screen's place in the frame and on the desktop, for absolute mouse input.
  struct input_tile_t {
    int frame_x = 0, frame_y = 0, frame_w = 0, frame_h = 0;  ///< In the capture's pixels.
    double x = 0, y = 0, w = 0, h = 0;  ///< The display's bounds on the desktop, in points.
  };

  /// Set while a multi-monitor capture runs; input then maps frame points through it.
  void set_input_layout(const void *owner, std::vector<input_tile_t> tiles);
  /// Clears the layout if `owner` set it last.
  void clear_input_layout(const void *owner);

  /**
   * @brief The desktop point (in points) for a point in the frame, or nothing when no
   *        multi-monitor capture runs. Points between screens go to the nearest one.
   */
  std::optional<std::pair<double, double>> frame_to_desktop(double x, double y);

  /// The union of the captured displays' bounds, or nothing when no multi-monitor capture runs.
  std::optional<std::array<double, 4>> input_bounds();

  /**
   * @brief Development check without a client: makes `virtual_count` virtual displays, captures
   *        the plan for `outputs` once through the multi-display capture, writes the NV12 frame
   *        to `path` and prints its size. Returns 0 on success.
   */
  int capture_probe(const std::string &outputs, int virtual_count, const std::string &path);

}  // namespace platf::macos
