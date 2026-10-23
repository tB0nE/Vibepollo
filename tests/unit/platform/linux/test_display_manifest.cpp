/**
 * @file tests/unit/platform/linux/test_display_manifest.cpp
 * @brief Tests for multi-monitor capture geometry and the desktop manifest.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/display_manifest.h>

namespace {
  platf::output_descriptor_t output(std::string name, int x, int y, int w, int h, bool primary = false, bool capturable = true) {
    platf::output_descriptor_t out;
    out.name = std::move(name);
    out.x = x;
    out.y = y;
    out.width = w;
    out.height = h;
    out.primary = primary;
    out.capturable = capturable;
    return out;
  }

  const platf::monitor_manifest_entry_t &monitor(const platf::capture_region_t &region, const std::string &label) {
    for (const auto &m : region.monitors) {
      if (m.label == label) {
        return m;
      }
    }
    ADD_FAILURE() << label << " is not in the region";
    static const platf::monitor_manifest_entry_t none {};
    return none;
  }

  std::tuple<int, int, int, int> frame_rect(const platf::monitor_manifest_entry_t &m) {
    return {m.frame_x, m.frame_y, m.frame_w, m.frame_h};
  }

  // The four monitors of the PC this was developed on.
  const std::vector<platf::output_descriptor_t> four_monitors {
    output("DP-0", 0, 0, 2560, 1440, true),
    output("DP-2", 2560, 0, 1920, 1080),
    output("DP-4", 4480, 0, 1920, 1080),
    output("HDMI-0", 6400, 0, 1920, 1080),
  };
}  // namespace

TEST(DisplayManifest, EveryMonitorWhenNoneAreNamed) {
  const auto region = platf::compute_capture_region_from_outputs(four_monitors, 8320, 1440, {});

  EXPECT_EQ(std::tuple(region.offset_x, region.offset_y, region.width, region.height), std::tuple(0, 0, 8320, 1440));
  EXPECT_TRUE(region.contiguous);
  EXPECT_EQ(frame_rect(monitor(region, "HDMI-0")), std::tuple(6400, 0, 1920, 1080));
  EXPECT_TRUE(monitor(region, "DP-0").is_primary);
}

TEST(DisplayManifest, ASubsetIsCroppedToItsBoundingBox) {
  const auto region = platf::compute_capture_region_from_outputs(four_monitors, 8320, 1440, {"DP-2", "DP-4"});

  EXPECT_EQ(std::tuple(region.offset_x, region.offset_y, region.width, region.height), std::tuple(2560, 0, 3840, 1080));
  EXPECT_TRUE(region.contiguous);
  EXPECT_FALSE(monitor(region, "DP-0").enabled);
  EXPECT_EQ(frame_rect(monitor(region, "DP-0")), std::tuple(0, 0, 0, 0));
  EXPECT_EQ(frame_rect(monitor(region, "DP-2")), std::tuple(0, 0, 1920, 1080));
  EXPECT_EQ(frame_rect(monitor(region, "DP-4")), std::tuple(1920, 0, 1920, 1080));
  EXPECT_EQ(monitor(region, "DP-4").desktop_x, 4480);
}

TEST(DisplayManifest, ASkippedMonitorInBetweenIsNotContiguous) {
  const auto region = platf::compute_capture_region_from_outputs(four_monitors, 8320, 1440, {"DP-0", "DP-2", "HDMI-0"});

  EXPECT_FALSE(region.contiguous);
}

TEST(DisplayManifest, MonitorsOfDifferentHeightsAreStillContiguous) {
  const auto region = platf::compute_capture_region_from_outputs(four_monitors, 8320, 1440, {"DP-0", "DP-2"});

  EXPECT_TRUE(region.contiguous);
  EXPECT_EQ(std::tuple(region.width, region.height), std::tuple(4480, 1440));
}

TEST(DisplayManifest, AnInactiveMonitorIsListedButNeverCaptured) {
  const std::vector<platf::output_descriptor_t> outputs {
    output("DP-0", 0, 0, 2560, 1440, true),
    output("DP-2", 0, 0, 1920, 1080, false, false),
  };
  const auto region = platf::compute_capture_region_from_outputs(outputs, 2560, 1440, {"DP-0", "DP-2"});

  ASSERT_EQ(region.monitors.size(), 2u);
  EXPECT_FALSE(monitor(region, "DP-2").enabled);
  EXPECT_FALSE(monitor(region, "DP-2").capturable);
  EXPECT_TRUE(region.contiguous);
  EXPECT_EQ(std::tuple(region.width, region.height), std::tuple(2560, 1440));
}

TEST(DisplayManifest, UnknownNamesFallBackToTheWholeDesktop) {
  const auto region = platf::compute_capture_region_from_outputs(four_monitors, 8320, 1440, {"DP-9"});

  EXPECT_EQ(std::tuple(region.offset_x, region.width, region.height), std::tuple(0, 8320, 1440));
  for (const auto &m : region.monitors) {
    EXPECT_FALSE(m.enabled) << m.label;
  }
}

TEST(DisplayManifestPacking, AFrameThatFitsIsNotPacked) {
  const auto plain = platf::compute_capture_region_from_outputs(four_monitors, 8320, 1440, {"DP-0", "DP-2", "DP-4"});
  const auto packed = platf::compute_packed_capture_region_from_outputs(four_monitors, 8320, 1440, {"DP-0", "DP-2", "DP-4"});

  EXPECT_FALSE(packed.tiled);
  EXPECT_EQ(std::tuple(packed.width, packed.height), std::tuple(plain.width, plain.height));
  EXPECT_TRUE(platf::frame_tiles_of(packed).empty());
}

TEST(DisplayManifestPacking, FourMonitorsTooWideBecomeTwoRows) {
  const auto region = platf::compute_packed_capture_region_from_outputs(four_monitors, 8320, 1440, {});

  EXPECT_TRUE(region.tiled);
  EXPECT_EQ(std::tuple(region.width, region.height), std::tuple(4480, 2520));
  EXPECT_EQ(frame_rect(monitor(region, "DP-0")), std::tuple(0, 0, 2560, 1440));
  EXPECT_EQ(frame_rect(monitor(region, "DP-2")), std::tuple(2560, 0, 1920, 1080));
  EXPECT_EQ(frame_rect(monitor(region, "DP-4")), std::tuple(0, 1440, 1920, 1080));
  EXPECT_EQ(frame_rect(monitor(region, "HDMI-0")), std::tuple(1920, 1440, 1920, 1080));
  EXPECT_EQ(monitor(region, "HDMI-0").desktop_x, 6400);

  const auto tiles = platf::frame_tiles_of(region);
  ASSERT_EQ(tiles.size(), 4u);
  EXPECT_EQ(std::tuple(tiles[3].frame_x, tiles[3].frame_y, tiles[3].desktop_x, tiles[3].desktop_y), std::tuple(1920, 1440, 6400, 0));
}

TEST(DisplayManifestPacking, OneMonitorOverTheLimitIsLeftAlone) {
  const std::vector<platf::output_descriptor_t> outputs {output("DP-0", 0, 0, 10240, 1440, true)};
  const auto region = platf::compute_packed_capture_region_from_outputs(outputs, 10240, 1440, {});

  EXPECT_FALSE(region.tiled);
  EXPECT_EQ(region.width, 10240);
}

TEST(DisplayManifestPacking, NoLimitMeansNoPacking) {
  const auto region = platf::compute_packed_capture_region_from_outputs(four_monitors, 8320, 1440, {}, 0, 0);

  EXPECT_FALSE(region.tiled);
  EXPECT_EQ(region.width, 8320);
}

TEST(DisplayManifestInput, PointsInATileMapToItsMonitor) {
  const auto tiles = platf::frame_tiles_of(platf::compute_packed_capture_region_from_outputs(four_monitors, 8320, 1440, {}));

  EXPECT_EQ(platf::frame_to_desktop(tiles, 100, 100), std::pair(100.0f, 100.0f));
  EXPECT_EQ(platf::frame_to_desktop(tiles, 2000, 1500), std::pair(6400.0f + 80, 60.0f));
  EXPECT_EQ(platf::frame_to_desktop(tiles, 10, 2000), std::pair(4480.0f + 10, 560.0f));
}

TEST(DisplayManifestInput, PointsInAGapGoToTheNearestMonitor) {
  const auto tiles = platf::frame_tiles_of(platf::compute_packed_capture_region_from_outputs(four_monitors, 8320, 1440, {}));

  // Below DP-2, which is shorter than DP-0, and above HDMI-0's tile.
  const auto [x, y] = platf::frame_to_desktop(tiles, 3000, 1200);
  EXPECT_EQ(x, 2560.0f + 440);
  EXPECT_EQ(y, 1079.0f);
}

TEST(DisplayManifestInput, WithoutTilesPointsAreUnchanged) {
  EXPECT_EQ(platf::frame_to_desktop({}, 12.5f, 7), std::pair(12.5f, 7.0f));
}

TEST(DisplayManifest, SplitsOutputLists) {
  EXPECT_EQ(platf::split_csv(" DP-0, HDMI-1 ,,"), (std::vector<std::string> {"DP-0", "HDMI-1"}));
  EXPECT_TRUE(platf::split_csv("").empty());
  EXPECT_TRUE(platf::split_csv(" , ").empty());
}
