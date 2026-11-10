/**
 * @file src/platform/linux/mutter_workspace.h
 * @brief GNOME (Mutter) capture, plans and virtual monitors (muttergrab.cpp).
 */
#pragma once
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "display_manifest.h"
#include "src/display_plan.h"
#include "src/platform/common.h"

namespace platf {
  /// Whether Mutter's ScreenCast interface is on the session bus (GNOME on Wayland).
  bool mutter_available();
  bool mutter_virtual_outputs_supported();
  std::vector<std::string> mutter_display_names();
  std::shared_ptr<display_t> mutter_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  /// The session's screen plan from Mutter's monitors (see kwin_make_plan()).
  std::optional<display_plan::plan_t> mutter_make_plan(const std::string &outputs, int virtual_count);
  /// Makes and places the session's virtual monitors (see kwin_prepare_virtual_outputs()).
  std::optional<std::string> mutter_prepare_virtual_outputs(const std::string &outputs, int count, int refresh_millihz, std::string &error);
  std::optional<desktop_manifest_t> mutter_plan_manifest(const std::string &outputs, int count, bool virtual_only);
  std::optional<desktop_manifest_t> mutter_session_manifest();
  void mutter_release_virtual_outputs();
  void mutter_schedule_virtual_outputs_release(std::chrono::seconds delay);
  void mutter_cancel_virtual_outputs_release();
}  // namespace platf
