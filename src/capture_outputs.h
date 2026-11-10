/**
 * @file src/capture_outputs.h
 * @brief The monitors the current session asked to capture (`outputs=` on /launch and /resume).
 */
#pragma once

// standard includes
#include <mutex>
#include <string>
#include <string_view>

namespace capture_outputs {

  /// Stands in for the output list when only the session's virtual displays are captured.
  inline constexpr std::string_view virtual_only_token = "@virtual";

  namespace detail {
    inline std::mutex mutex;
    inline std::string outputs;
  }  // namespace detail

  // Meteor starts on the primary monitor; explicit session/config choices win.
  template<class Outputs>
  std::string initial_selection(const std::string &requested, const std::string &configured,
                                bool managed_primary, const Outputs &outputs) {
    if (!requested.empty() || !configured.empty() || !managed_primary) return requested;
    for (const auto &output : outputs)
      if (output.primary && output.capturable) return output.name;
    for (const auto &output : outputs)
      if (output.capturable) return output.name;
    return requested;
  }

  /**
   * @brief Sets the comma-separated output names for the next capture, e.g. "DP-0,DP-2".
   *        Empty restores the configured single display.
   */
  inline void set(std::string outputs) {
    std::lock_guard lock {detail::mutex};
    detail::outputs = std::move(outputs);
  }

  inline std::string get() {
    std::lock_guard lock {detail::mutex};
    return detail::outputs;
  }

}  // namespace capture_outputs
