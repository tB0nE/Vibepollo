#include <gtest/gtest.h>
#include <src/platform/linux/kwin_virtual_layout.h>

namespace {
  using platf::output_descriptor_t;
  namespace kv = platf::kwin_virtual;

  // Main 2560x1440 on the left, a 4K monitor (logical 2560x1440 at 1.5) to its right.
  const std::vector<output_descriptor_t> two_side_by_side {
    {"DP-1", 0, 0, 2560, 1440, true, true},
    {"DP-3", 2560, 0, 2560, 1440, false, true, 1.5},
  };

  const platf::monitor_manifest_entry_t *find(const kv::layout_t &layout, const std::string &label) {
    for (const auto &m : layout.workspace.manifest.monitors) {
      if (m.label == label) {
        return &m;
      }
    }
    return nullptr;
  }
}  // namespace

TEST(KwinVirtualLayout, NamesMatchWhatKwinCallsTheOutputs) {
  EXPECT_EQ(kv::requested_name(1), "Nightfall-Virtual-1");
  EXPECT_EQ(kv::output_name(2), "Virtual-Nightfall-Virtual-2");
  EXPECT_TRUE(kv::is_virtual_output("Virtual-Nightfall-Virtual-3"));
  EXPECT_FALSE(kv::is_virtual_output("Virtual-0"));
  EXPECT_FALSE(kv::is_virtual_output("DP-1"));
}

TEST(KwinVirtualLayout, RowSitsAtTheDesktopsRightEdgeSizedLikeTheMainMonitor) {
  std::string error;
  const auto layout = kv::plan({{"main", 0, 0, 1920, 1080, true, true}}, {}, 2, false, error);
  ASSERT_TRUE(layout) << error;
  ASSERT_EQ(layout->outputs.size(), 3u);
  EXPECT_EQ(layout->outputs[1].name, "Virtual-Nightfall-Virtual-1");
  EXPECT_EQ(layout->outputs[1].x, 1920);
  EXPECT_EQ(layout->outputs[2].x, 3840);
  EXPECT_EQ(layout->outputs[2].y, 0);
  EXPECT_EQ(layout->outputs[2].width, 1920);
  EXPECT_EQ(layout->outputs[2].height, 1080);
  EXPECT_EQ(layout->selected, (std::vector<std::string> {"main", "Virtual-Nightfall-Virtual-1", "Virtual-Nightfall-Virtual-2"}));
  EXPECT_EQ(layout->workspace.manifest.frame_width, 5760);
  EXPECT_EQ(layout->workspace.manifest.frame_height, 1080);
  const auto *v2 = find(*layout, "Virtual-Nightfall-Virtual-2");
  ASSERT_NE(v2, nullptr);
  EXPECT_TRUE(v2->is_virtual);
  EXPECT_TRUE(v2->enabled);
  EXPECT_EQ(v2->frame_x, 3840);
  EXPECT_FALSE(find(*layout, "main")->is_virtual);
  EXPECT_TRUE(find(*layout, "main")->is_primary);
}

TEST(KwinVirtualLayout, MonitorsBetweenTheChoiceAndTheRowAreCaptured) {
  std::string error;
  const auto layout = kv::plan(two_side_by_side, {"DP-1"}, 1, false, error);
  ASSERT_TRUE(layout) << error;
  EXPECT_EQ(layout->selected, (std::vector<std::string> {"DP-1", "DP-3", "Virtual-Nightfall-Virtual-1"}));
  EXPECT_EQ(layout->workspace.manifest.frame_width, 7680);
  EXPECT_TRUE(find(*layout, "DP-3")->enabled);
}

TEST(KwinVirtualLayout, VirtualOnlyCapturesTheRowAloneWithTheFirstAsPrimary) {
  std::string error;
  const auto layout = kv::plan(two_side_by_side, {"DP-1"}, 2, true, error);
  ASSERT_TRUE(layout) << error;
  EXPECT_EQ(layout->selected, (std::vector<std::string> {"Virtual-Nightfall-Virtual-1", "Virtual-Nightfall-Virtual-2"}));
  EXPECT_EQ(layout->workspace.manifest.frame_width, 5120);
  EXPECT_EQ(layout->workspace.manifest.frame_height, 1440);
  EXPECT_EQ(layout->workspace.capture_x, 5120);
  int primaries = 0;
  for (const auto &m : layout->workspace.manifest.monitors) {
    primaries += m.enabled && m.is_primary;
  }
  EXPECT_EQ(primaries, 1);
  EXPECT_TRUE(find(*layout, "Virtual-Nightfall-Virtual-1")->is_primary);
  EXPECT_FALSE(find(*layout, "DP-1")->enabled);
}

TEST(KwinVirtualLayout, RowIsLevelWithTheTopOfTheChosenMonitors) {
  std::string error;
  const std::vector<output_descriptor_t> lowered {
    {"main", 0, 0, 2560, 1440, true, true},
    {"side", 2560, 593, 1920, 1080, false, true},
  };
  const auto layout = kv::plan(lowered, {"side"}, 1, false, error);
  ASSERT_TRUE(layout) << error;
  EXPECT_EQ(layout->outputs.back().y, 593);
  EXPECT_EQ(layout->outputs.back().x, 4480);
  // Sized like the main monitor, so the row is taller than "side".
  EXPECT_EQ(layout->workspace.manifest.frame_height, 1440);
}

TEST(KwinVirtualLayout, ExistingVirtualMonitorsAreReplannedNotCountedAsPhysical) {
  std::string error;
  auto outputs = two_side_by_side;
  outputs.push_back({"Virtual-Nightfall-Virtual-1", 5120, 0, 2560, 1440, false, true});
  const auto layout = kv::plan(outputs, {}, 1, false, error);
  ASSERT_TRUE(layout) << error;
  ASSERT_EQ(layout->outputs.size(), 3u);
  EXPECT_EQ(layout->outputs.back().x, 5120);
}

TEST(KwinVirtualLayout, UnknownNamesFallBackToTheMainMonitor) {
  std::string error;
  const auto layout = kv::plan(two_side_by_side, {"HDMI-9"}, 1, false, error);
  ASSERT_TRUE(layout) << error;
  EXPECT_EQ(layout->selected.front(), "DP-1");
}

TEST(KwinVirtualLayout, RefusesWhatCantBeOneStream) {
  std::string error;
  EXPECT_FALSE(kv::plan({}, {}, 1, false, error));
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_FALSE(kv::plan(two_side_by_side, {}, 0, true, error));
  EXPECT_FALSE(error.empty());
  error.clear();
  // Four 4K-sized virtual monitors beside a 4K monitor exceed 8192 pixels across.
  EXPECT_FALSE(kv::plan({{"4k", 0, 0, 3840, 2160, true, true}}, {}, 4, false, error));
  EXPECT_FALSE(error.empty());
}

TEST(KwinVirtualLayout, MarkVirtualPromotesTheFirstVirtualWhenNoPhysicalIsCaptured) {
  std::vector<output_descriptor_t> outputs {
    {"DP-1", 0, 0, 2560, 1440, true, true},
    {"Virtual-Nightfall-Virtual-1", 2560, 0, 2560, 1440, false, true},
    {"Virtual-Nightfall-Virtual-2", 5120, 0, 2560, 1440, false, true},
  };
  kv::mark_virtual(outputs, {"Virtual-Nightfall-Virtual-1", "Virtual-Nightfall-Virtual-2"});
  EXPECT_FALSE(outputs[0].primary);
  EXPECT_TRUE(outputs[1].primary);
  EXPECT_FALSE(outputs[2].primary);
  EXPECT_TRUE(outputs[2].is_virtual);
  EXPECT_FALSE(outputs[0].is_virtual);

  std::vector<output_descriptor_t> mixed = {outputs[0], outputs[1]};
  mixed[0].primary = true;
  mixed[1].primary = false;
  kv::mark_virtual(mixed, {"DP-1", "Virtual-Nightfall-Virtual-1"});
  EXPECT_TRUE(mixed[0].primary);
  EXPECT_FALSE(mixed[1].primary);
}

TEST(KwinVirtualLayout, TooWideForOneRowBecomesAGridBesideAndBelow) {
  std::string error;
  // 2560 + 3 x 2560 = 10240 pixels across, more than a stream allows.
  const auto layout = kv::plan({{"DP-1", 0, 0, 2560, 1440, true, true}}, {}, 3, false, error);
  ASSERT_TRUE(layout) << error;
  EXPECT_EQ(layout->workspace.manifest.frame_width, 5120);
  EXPECT_EQ(layout->workspace.manifest.frame_height, 2880);
  EXPECT_EQ(layout->outputs[1].x, 2560);
  EXPECT_EQ(layout->outputs[1].y, 0);
  EXPECT_EQ(layout->outputs[2].x, 0);
  EXPECT_EQ(layout->outputs[2].y, 1440);
  EXPECT_EQ(layout->outputs[3].x, 2560);
  EXPECT_EQ(layout->outputs[3].y, 1440);
  EXPECT_EQ(layout->selected.size(), 4u);
}

TEST(KwinVirtualLayout, GridNeverCoversAnotherMonitor) {
  std::string error;
  const std::vector<output_descriptor_t> with_lower {
    {"DP-1", 0, 0, 2560, 1440, true, true},
    {"below", 0, 1440, 1920, 1080, false, true},
  };
  const auto layout = kv::plan(with_lower, {"DP-1"}, 3, false, error);
  ASSERT_TRUE(layout) << error;
  for (const auto &v : layout->outputs) {
    if (!v.is_virtual) {
      continue;
    }
    for (const auto &p : with_lower) {
      const bool overlap = v.x < p.x + p.width && v.x + v.width > p.x && v.y < p.y + p.height && v.y + v.height > p.y;
      EXPECT_FALSE(overlap) << v.name << " covers " << p.name;
    }
  }
  EXPECT_LE(layout->workspace.manifest.frame_width, 8192);
}

TEST(KwinVirtualLayout, VirtualOnlyFourMakesATwoByTwoGrid) {
  std::string error;
  const auto layout = kv::plan({{"DP-1", 0, 0, 2560, 1440, true, true}}, {}, 4, true, error);
  ASSERT_TRUE(layout) << error;
  EXPECT_EQ(layout->workspace.manifest.frame_width, 5120);
  EXPECT_EQ(layout->workspace.manifest.frame_height, 2880);
  EXPECT_FALSE(find(*layout, "DP-1")->enabled);
}
