#pragma once
#include <chrono>
#include <optional>
#include <string>
#include "display_manifest.h"
#include "src/display_plan.h"

namespace platf {
  // Returns no manifest when the active backend cannot honor the reported selection.
  std::optional<desktop_manifest_t> kwin_query_desktop_manifest(const std::string &outputs);
  std::optional<std::string> kwin_workspace_selection_error(const std::string &outputs);

  bool kwin_available();

  /// Whether this KWin can make virtual monitors (zkde_screencast v2 or later).
  bool kwin_virtual_outputs_supported();

  /**
   * @brief Makes the session's virtual monitors and places them (see kwin_virtual_layout.h).
   * @param outputs The outputs= list, or capture_outputs::virtual_only_token.
   * @param count How many virtual monitors; 0 removes any this host made.
   * @param refresh_millihz The stream's frame rate. KWin makes virtual monitors at 60 Hz, and a
   *                        capture of virtual monitors alone runs at their rate, so they are set
   *                        to the stream's rate.
   * @return The outputs the session captures, or nothing with `error` set.
   */
  std::optional<std::string> kwin_prepare_virtual_outputs(const std::string &outputs, int count, int refresh_millihz, std::string &error);

  /**
   * @brief The session's screen plan (display_plan.h) from KWin's outputs.
   * @param outputs The outputs= list, or capture_outputs::virtual_only_token.
   */
  std::optional<display_plan::plan_t> kwin_make_plan(const std::string &outputs, int virtual_count);

  /// The running session's packed layout, when it has several screens.
  std::optional<desktop_manifest_t> kwin_session_manifest();

  /// The manifest a launch with these options would get, before its virtual monitors exist.
  std::optional<desktop_manifest_t> kwin_plan_manifest(const std::string &outputs, int count, bool virtual_only);

  void kwin_release_virtual_outputs();
  /// Removes the virtual monitors after `delay` unless a launch or resume cancels it first.
  void kwin_schedule_virtual_outputs_release(std::chrono::seconds delay);
  void kwin_cancel_virtual_outputs_release();
}  // namespace platf
