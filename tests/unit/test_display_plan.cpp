/**
 * @file tests/unit/test_display_plan.cpp
 * @brief Packing screens into the frame, and scaling them to fit the encoder's largest frame.
 */
#include "src/display_plan.h"
#include "src/platform/linux/display_manifest.h"

#include <gtest/gtest.h>

namespace {
  std::vector<display_plan::physical_t> one_1440p() {
    return {{"DP-1", 0, 0, 2560, 1440, true}};
  }

  struct encoder_limit {
    explicit encoder_limit(int axis) {
      display_plan::encoder_max_axis = axis;
    }

    ~encoder_limit() {
      display_plan::encoder_max_axis = display_plan::max_axis_px;
    }
  };

  void expect_inside(const display_plan::plan_t &plan, int max_axis) {
    EXPECT_LE(plan.frame_width, max_axis);
    EXPECT_LE(plan.frame_height, max_axis);
    for (const auto *s : plan.enabled()) {
      EXPECT_LE(s->frame_x + s->frame_w, plan.frame_width) << s->id;
      EXPECT_LE(s->frame_y + s->frame_h, plan.frame_height) << s->id;
      EXPECT_EQ(s->frame_w % 2, 0) << s->id;
      EXPECT_EQ(s->frame_h % 2, 0) << s->id;
    }
  }
}  // namespace

TEST(DisplayPlan, FullSizeWhenItFits) {
  const auto plan = display_plan::make(one_1440p(), {}, 3, false);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->frame_width, 5120);
  EXPECT_EQ(plan->frame_height, 2880);
  for (const auto *s : plan->enabled()) {
    EXPECT_EQ(s->frame_w, s->width);
    EXPECT_EQ(s->frame_h, s->height);
  }
}

TEST(DisplayPlan, TwoScreensFitA4096EncoderUnscaled) {
  encoder_limit limit {4096};
  const auto plan = display_plan::make(one_1440p(), {}, 1, false);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->frame_width, 2560);
  EXPECT_EQ(plan->frame_height, 2880);
  EXPECT_EQ(plan->enabled()[1]->frame_w, 2560);
}

TEST(DisplayPlan, FourScreensScaleToFitA4096Encoder) {
  encoder_limit limit {4096};
  const auto plan = display_plan::make(one_1440p(), {}, 3, false);
  ASSERT_TRUE(plan);
  expect_inside(*plan, 4096);
  // Two rows of two at 80%: 2048x1152 each, 4096x2304 in all.
  EXPECT_EQ(plan->frame_width, 4096);
  EXPECT_EQ(plan->frame_height, 2304);
  for (const auto *s : plan->enabled()) {
    EXPECT_EQ(s->width, 2560) << "desktop size stays";
    EXPECT_EQ(s->frame_w, 2048);
    EXPECT_EQ(s->frame_h, 1152);
  }
}

TEST(DisplayPlan, ThreeScreensScaleToFitA4096Encoder) {
  encoder_limit limit {4096};
  const auto plan = display_plan::make(one_1440p(), {}, 2, false);
  ASSERT_TRUE(plan);
  expect_inside(*plan, 4096);
  for (const auto *s : plan->enabled()) {
    EXPECT_LT(s->frame_w, s->width);
    EXPECT_NEAR((double) s->frame_w / s->frame_h, 16.0 / 9.0, 0.01);
  }
  // No screen overlaps another.
  const auto screens = plan->enabled();
  for (std::size_t i = 0; i < screens.size(); ++i) {
    for (std::size_t j = i + 1; j < screens.size(); ++j) {
      const auto *a = screens[i], *b = screens[j];
      const bool apart = a->frame_x + a->frame_w <= b->frame_x || b->frame_x + b->frame_w <= a->frame_x ||
                         a->frame_y + a->frame_h <= b->frame_y || b->frame_y + b->frame_h <= a->frame_y;
      EXPECT_TRUE(apart) << a->id << " overlaps " << b->id;
    }
  }
}

TEST(DisplayPlan, ScaledTileMapsInputToTheWholeMonitor) {
  // A 2560x1440 monitor at desktop 2560,0 drawn as a 2048x1152 tile at frame 2048,0.
  const std::vector<platf::frame_tile_t> tiles {{0, 0, 0, 0, 2048, 1152, 2560, 1440}, {2048, 0, 2560, 0, 2048, 1152, 2560, 1440}};
  const auto [x, y] = platf::frame_to_desktop(tiles, 2048 + 1024, 576);
  EXPECT_FLOAT_EQ(x, 2560 + 1280);
  EXPECT_FLOAT_EQ(y, 720);
  const auto [ux, uy] = platf::frame_to_desktop({{0, 0, 100, 50, 640, 480}}, 10, 20);
  EXPECT_FLOAT_EQ(ux, 110);
  EXPECT_FLOAT_EQ(uy, 70);
}
