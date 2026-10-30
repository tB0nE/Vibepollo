#pragma once

#include <algorithm>
#include <chrono>

namespace stream::startup_ping {
  enum class result_e { received, cancelled, timed_out };

  template<class State>
  bool session_stopped(State state) {
    return state == State::STOPPING || state == State::STOPPED;
  }

  // Startup only: bound idle waits so disconnect cannot hold the next launch
  // behind a full network timeout. The network deadline never extends on noise.
  template<class Clock = std::chrono::steady_clock, class Cancelled, class Receive, class Accept>
  result_e wait(std::chrono::milliseconds timeout, Cancelled cancelled, Receive receive, Accept accept) {
    const auto deadline = Clock::now() + timeout;
    while (true) {
      if (cancelled()) {
        return result_e::cancelled;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
      if (remaining <= std::chrono::milliseconds::zero()) {
        return result_e::timed_out;
      }
      auto message = receive(std::min(remaining, std::chrono::milliseconds(50)));
      if (cancelled()) {
        return result_e::cancelled;
      }
      if (message && accept(*message)) {
        return result_e::received;
      }
    }
  }
}
