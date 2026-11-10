#include <gtest/gtest.h>
#include <chrono>
#include <optional>
#include <src/startup_ping_wait.h>

using namespace std::chrono_literals;
namespace {
  struct Clock {
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<Clock, duration>;
    inline static time_point time {};
    static time_point now() { return time; }
  };
}

TEST(StartupPingWait, IdleDisconnectDoesNotWaitTenSeconds) {
  Clock::time = {};
  auto receive = [&](auto interval) {
    EXPECT_LE(interval, 50ms);
    Clock::time += interval;
    return std::optional<int>();
  };
  auto result = stream::startup_ping::wait<Clock>(10s,
    [&] { return Clock::now().time_since_epoch() >= 100ms; }, receive, [](int) { return true; });
  EXPECT_EQ(result, stream::startup_ping::result_e::cancelled);
  EXPECT_EQ(Clock::now().time_since_epoch(), 100ms);
}

TEST(StartupPingWait, NormalPingReturnsImmediately) {
  Clock::time = {};
  int accepted = 0;
  auto result = stream::startup_ping::wait<Clock>(10s, [] { return false; },
    [](auto) { return std::optional<int>(42); }, [&](int value) { accepted = value; return true; });
  EXPECT_EQ(result, stream::startup_ping::result_e::received);
  EXPECT_EQ(accepted, 42);
  EXPECT_EQ(Clock::now().time_since_epoch(), 0ms);
}

TEST(StartupPingWait, NoiseDoesNotExtendTheOriginalDeadline) {
  Clock::time = {};
  auto result = stream::startup_ping::wait<Clock>(125ms, [] { return false; },
    [](auto interval) { Clock::time += interval; return std::optional<int>(1); }, [](int) { return false; });
  EXPECT_EQ(result, stream::startup_ping::result_e::timed_out);
  EXPECT_EQ(Clock::now().time_since_epoch(), 125ms);
}

TEST(StartupPingWait, StoppedSessionNeverConsumesALatePing) {
  Clock::time = {};
  bool stopped = false;
  bool accepted = false;
  auto result = stream::startup_ping::wait<Clock>(10s, [&] { return stopped; },
    [&](auto) { stopped = true; return std::optional<int>(1); }, [&](int) { accepted = true; return true; });
  EXPECT_EQ(result, stream::startup_ping::result_e::cancelled);
  EXPECT_FALSE(accepted);
}

TEST(StartupPingWait, AlreadyStoppedDoesNotReadTheQueue) {
  Clock::time = {};
  bool read = false;
  auto result = stream::startup_ping::wait<Clock>(10s, [] { return true; },
    [&](auto) { read = true; return std::optional<int>(); }, [](int) { return true; });
  EXPECT_EQ(result, stream::startup_ping::result_e::cancelled);
  EXPECT_FALSE(read);
}

TEST(StartupPingWait, StartingWorkersMustNotCancelAudioOrVideo) {
  enum class State { STOPPED, STOPPING, STARTING, RUNNING };
  EXPECT_FALSE(stream::startup_ping::session_stopped(State::STARTING));
  EXPECT_FALSE(stream::startup_ping::session_stopped(State::RUNNING));
  EXPECT_TRUE(stream::startup_ping::session_stopped(State::STOPPING));
  EXPECT_TRUE(stream::startup_ping::session_stopped(State::STOPPED));
  Clock::time = {};
  auto state = State::STARTING;
  auto result = stream::startup_ping::wait<Clock>(10s,
    [&] { return stream::startup_ping::session_stopped(state); },
    [&](auto) { state = State::RUNNING; return std::optional<int>(42); },
    [](int value) { return value == 42; });
  EXPECT_EQ(result, stream::startup_ping::result_e::received);
}
