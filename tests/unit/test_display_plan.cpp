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

TEST(DisplayPlan, CustomVirtualModesValidateAndKeepIndependentSizes) {
  using namespace virtual_display_modes;
  EXPECT_FALSE(parse("2561x1440", 1));
  EXPECT_FALSE(parse("8192x8192", 1));
  EXPECT_FALSE(parse("2560x1440,", 1));
  EXPECT_FALSE(parse("2560x1440", 2));
  auto modes = parse("0x0,1920x1080", 2);
  ASSERT_TRUE(modes);
  auto result = display_plan::make({{"DP-1",0,0,2560,1440,true}}, {"DP-1"}, 2, false, *modes);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->screens[1].width,2560);
  EXPECT_EQ(result->screens[2].width,1920);
  EXPECT_EQ(result->screens[2].height,1080);
}

TEST(DisplayPlan, ClientH264BudgetPreservesTwo1440pTiles) {
  client_packing::budget_t b; b.active=true;b.axis=4032;b.pixels=16257024;b.strict=true;
  client_packing::scope scope(b);
  auto p=display_plan::make(one_1440p(),{},1,false);
  ASSERT_TRUE(p);EXPECT_EQ(p->frame_width,2560);EXPECT_EQ(p->frame_height,2880);
  EXPECT_TRUE(client_packing::request().fits);EXPECT_TRUE(p->profiled);
  EXPECT_FALSE(display_plan::make(one_1440p(),{},2,false));
}
TEST(DisplayPlan, ClientBudgetScopeAndEncoderLimit) {
  client_packing::budget_t b;b.active=true;b.axis=8192;b.pixels=21233664;b.strict=true;
  {client_packing::scope scope(b);
    EXPECT_TRUE(display_plan::make(one_1440p(),{},2,false));
    encoder_limit limit{4096};
    EXPECT_FALSE(display_plan::make(one_1440p(),{},2,false));
  }
  EXPECT_FALSE(client_packing::request().active);
}
TEST(DisplayPlan, PhysicalProfilePreservesDesktopAndCustomVirtualMode) {
  client_packing::budget_t b;b.active=true;b.axis=8192;b.pixels=21233664;
  b.physical_w=2560;b.physical_h=1440;b.physical_modes["m0"]={1920,1080};
  client_packing::scope scope(b);
  auto p=display_plan::make({{"DP-1",0,0,3840,2160,true}}, {},1,false,{{2560,1440}});
  ASSERT_TRUE(p);EXPECT_EQ(p->screens[0].width,3840);EXPECT_EQ(p->screens[0].frame_w,1920);
  EXPECT_EQ(p->screens[1].frame_w,2560);
}
TEST(DisplayPlan, Four1080pTilesFitH264Grid) {
  client_packing::budget_t b;b.active=true;b.axis=4032;b.pixels=16257024;b.strict=true;
  client_packing::scope scope(b);
  auto p=display_plan::make({{"DP-1",0,0,1920,1080,true}}, {},3,false);
  ASSERT_TRUE(p);EXPECT_EQ(p->frame_width,3840);EXPECT_EQ(p->frame_height,2160);
}
