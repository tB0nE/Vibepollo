#include <gtest/gtest.h>
#include <src/platform/linux/wayland_workspace.h>

namespace {
  const std::vector<platf::output_descriptor_t> desktop {
    {"left", -2560, -120, 2560, 1440, false, true},
    {"main", 0, 0, 1920, 1080, true, true},
    {"right", 1920, 0, 1280, 720, false, true}
  };
}

TEST(WaylandWorkspace, NegativeOriginIsOnlyNormalizedForInput) {
  const auto layout = platf::make_wayland_workspace(desktop, {"main"});
  ASSERT_TRUE(layout);
  EXPECT_EQ(layout->capture_x, 0);
  EXPECT_EQ(layout->capture_y, 0);
  EXPECT_EQ(layout->input_x, 2560);
  EXPECT_EQ(layout->input_y, 120);
  EXPECT_EQ(layout->manifest.desktop_x, -2560);
  EXPECT_EQ(layout->manifest.desktop_y, -120);
  EXPECT_EQ(layout->manifest.desktop_width, 5760);
  EXPECT_EQ(layout->manifest.desktop_height, 1440);
  EXPECT_EQ(layout->manifest.frame_width, 1920);
  EXPECT_EQ(layout->manifest.frame_height, 1080);
  ASSERT_EQ(layout->manifest.monitors.size(), 3u);
  EXPECT_FALSE(layout->manifest.monitors[0].enabled);
  EXPECT_TRUE(layout->manifest.monitors[1].enabled);
  EXPECT_EQ(layout->manifest.monitors[1].frame_x, 0);
}

TEST(WaylandWorkspace, MixedDpiUsesCompositorLogicalRectsWithoutAnotherDivision) {
  auto outputs = desktop;
  // 3840x2160 at 1.5 scale is reported by XDG output as 2560x1440.
  outputs[0].scale = 1.5;
  const auto layout = platf::make_wayland_workspace(outputs, {"left", "main"});
  ASSERT_TRUE(layout);
  EXPECT_EQ(layout->manifest.frame_width, 4480);
  EXPECT_EQ(layout->manifest.frame_height, 1440);
  EXPECT_EQ(layout->manifest.monitors[1].frame_x, 2560);
  EXPECT_EQ(layout->manifest.monitors[1].frame_y, 120);
  EXPECT_EQ(layout->capture_x, -2560);
  EXPECT_EQ(layout->capture_y, -120);
  EXPECT_EQ(layout->input_x, 0);
  EXPECT_EQ(layout->input_y, 0);
}

TEST(WaylandWorkspace, InvalidSelectionNeverFallsBackToFirstDisplay) {
  EXPECT_FALSE(platf::make_wayland_workspace(desktop, {}));
  EXPECT_FALSE(platf::make_wayland_workspace(desktop, {"missing"}));
  EXPECT_FALSE(platf::make_wayland_workspace(desktop, {"main", "missing"}));
  EXPECT_FALSE(platf::make_wayland_workspace(desktop, {"left", "right"}));
  auto outputs = desktop;
  outputs[1].capturable = false;
  EXPECT_FALSE(platf::make_wayland_workspace(outputs, {"main"}));
}

TEST(WaylandWorkspace, InactiveMonitorDoesNotChangeInputExtent) {
  auto outputs = desktop;
  outputs.push_back({"inactive", -9000, -9000, 1920, 1080, false, false});
  const auto layout = platf::make_wayland_workspace(outputs, {"main"});
  ASSERT_TRUE(layout);
  EXPECT_EQ(layout->manifest.desktop_width, 5760);
  EXPECT_EQ(layout->input_x, 2560);
  EXPECT_FALSE(layout->manifest.monitors.back().capturable);
}

TEST(WaylandWorkspace, OversizedRegionRejectedUntilGpuPackingIsImplemented) {
  EXPECT_FALSE(platf::make_wayland_workspace({{"wide", 0, 0, 9000, 1080}}, {"wide"}));
  EXPECT_FALSE(platf::make_wayland_workspace({{"huge", 0, 0, 8192, 8192}}, {"huge"}));
}

TEST(WaylandWorkspace, NegotiationMustMatchManifestExactly) {
  const auto layout = platf::make_wayland_workspace(desktop, {"main"});
  ASSERT_TRUE(layout);
  EXPECT_TRUE(platf::wayland_workspace_buffer_matches(*layout, 1920, 1080));
  EXPECT_FALSE(platf::wayland_workspace_buffer_matches(*layout, 2880, 1620));
  EXPECT_FALSE(platf::wayland_workspace_buffer_matches(*layout, 0, 0));
}

TEST(WaylandWorkspace, ExplicitBackendSelectionIsPreserved) {
  EXPECT_TRUE(platf::use_kwin_workspace(true, "", "left, main"));
  EXPECT_TRUE(platf::use_kwin_workspace(true, "kwin", "main"));
  EXPECT_FALSE(platf::use_kwin_workspace(false, "", "main"));
  EXPECT_FALSE(platf::use_kwin_workspace(true, "portal", "main"));
  EXPECT_FALSE(platf::use_kwin_workspace(true, "wlr", "main"));
  EXPECT_FALSE(platf::use_kwin_workspace(true, "kms", "main"));
  EXPECT_FALSE(platf::use_kwin_workspace(true, "", " , "));
}

TEST(WaylandWorkspace, MissingOrDuplicateGeometryCannotProduceManifest) {
  auto outputs = desktop;
  outputs[0].width = 0;
  EXPECT_FALSE(platf::make_wayland_workspace(outputs, {"main"}));
  outputs = desktop;
  outputs[1].name = outputs[0].name;
  EXPECT_FALSE(platf::make_wayland_workspace(outputs, {"left"}));
}

TEST(WaylandWorkspace, EveryMonitorCornerMapsToTheSameDesktopPoint) {
  const auto layout = platf::make_wayland_workspace(desktop, {"left", "main", "right"});
  ASSERT_TRUE(layout);
  for (const auto &monitor : layout->manifest.monitors) {
    for (const int x : {0, monitor.frame_w - 1}) {
      for (const int y : {0, monitor.frame_h - 1}) {
        EXPECT_EQ(layout->input_x + monitor.frame_x + x,
          monitor.desktop_x - layout->manifest.desktop_x + x);
        EXPECT_EQ(layout->input_y + monitor.frame_y + y,
          monitor.desktop_y - layout->manifest.desktop_y + y);
      }
    }
  }
}
