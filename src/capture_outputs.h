/**
 * @file src/capture_outputs.h
 * @brief The monitors the current session asked to capture (`outputs=` on /launch and /resume).
 */
#pragma once

// standard includes
#include <mutex>
#include <string>

namespace capture_outputs {

  namespace detail {
    inline std::mutex mutex;
    inline std::string outputs;
  }  // namespace detail

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
