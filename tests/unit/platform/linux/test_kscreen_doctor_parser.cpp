/**
 * @file tests/unit/test_kscreen_doctor_parser.cpp
 * @brief Tests for parsing `kscreen-doctor -j` output into output_descriptor_t entries.
 *
 * The two-monitor sample below is a real capture from `kscreen-doctor -j` on a live KDE
 * Plasma Wayland session (DP-0 primary 2560x1440 + DP-2 secondary 1920x1080 at +2560+0),
 * trimmed to the fields the parser reads.
 */
#include <gtest/gtest.h>

#include "src/platform/linux/kscreen_doctor_parser.h"

namespace {

  const platf::output_descriptor_t *find(const std::vector<platf::output_descriptor_t> &outputs, const std::string &name) {
    for (const auto &out : outputs) {
      if (out.name == name) {
        return &out;
      }
    }
    return nullptr;
  }

}  // namespace

TEST(KscreenDoctorParser, TwoMonitorSampleParsesConnectedEnabledOutputsOnly) {
  const std::string sample = R"({
    "outputs": [
      {"name": "HDMI-0", "connected": false, "enabled": false, "priority": 0,
       "pos": {"x": 0, "y": 0}, "size": {"width": -1, "height": -1}},
      {"name": "DP-0", "connected": true, "enabled": true, "priority": 1,
       "pos": {"x": 0, "y": 0}, "size": {"width": 2560, "height": 1440}},
      {"name": "DP-1", "connected": false, "enabled": false, "priority": 0,
       "pos": {"x": 0, "y": 0}, "size": {"width": -1, "height": -1}},
      {"name": "DP-2", "connected": true, "enabled": true, "priority": 2,
       "pos": {"x": 2560, "y": 0}, "size": {"width": 1920, "height": 1080}}
    ]
  })";

  const auto outputs = platf::parse_kscreen_doctor_outputs(sample);
  ASSERT_EQ(outputs.size(), 2u);

  const auto *dp0 = find(outputs, "DP-0");
  ASSERT_NE(dp0, nullptr);
  EXPECT_EQ(dp0->x, 0);
  EXPECT_EQ(dp0->y, 0);
  EXPECT_EQ(dp0->width, 2560);
  EXPECT_EQ(dp0->height, 1440);
  EXPECT_TRUE(dp0->primary);

  const auto *dp2 = find(outputs, "DP-2");
  ASSERT_NE(dp2, nullptr);
  EXPECT_EQ(dp2->x, 2560);
  EXPECT_EQ(dp2->y, 0);
  EXPECT_EQ(dp2->width, 1920);
  EXPECT_EQ(dp2->height, 1080);
  EXPECT_FALSE(dp2->primary);
}

TEST(KscreenDoctorParser, DisconnectedOutputsAreExcludedButDisabledConnectedOnesAreNot) {
  // Regression test for the manifest lockout bug: a connected-but-disabled monitor (e.g. one
  // dropped out of the current capture selection) must still be reported, just with
  // capturable=false, so it can't silently disappear from the client's known topology.
  const std::string sample = R"({
    "outputs": [
      {"name": "HDMI-0", "connected": false, "enabled": false, "priority": 0,
       "pos": {"x": 0, "y": 0}, "size": {"width": -1, "height": -1}},
      {"name": "DP-1", "connected": true, "enabled": false, "priority": 0,
       "pos": {"x": 0, "y": 0}, "size": {"width": 1920, "height": 1080}}
    ]
  })";

  const auto outputs = platf::parse_kscreen_doctor_outputs(sample);
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].name, "DP-1");
  EXPECT_FALSE(outputs[0].capturable);
  EXPECT_EQ(outputs[0].width, 1920);
  EXPECT_EQ(outputs[0].height, 1080);
}

TEST(KscreenDoctorParser, DisabledOutputWithNoCurrentSizeFallsBackToPreferredMode) {
  // Disabled outputs commonly report size {-1,-1} (no current mode) — this is the realistic
  // shape kscreen-doctor -j actually produces for a disabled-but-connected monitor.
  const std::string sample = R"({
    "outputs": [
      {"name": "DP-2", "connected": true, "enabled": false, "priority": 0,
       "pos": {"x": 0, "y": 0}, "size": {"width": -1, "height": -1},
       "preferredModes": ["454"],
       "modes": [
         {"id": "446", "size": {"width": 2560, "height": 1440}},
         {"id": "454", "size": {"width": 1920, "height": 1080}}
       ]}
    ]
  })";

  const auto outputs = platf::parse_kscreen_doctor_outputs(sample);
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_FALSE(outputs[0].capturable);
  EXPECT_EQ(outputs[0].width, 1920);
  EXPECT_EQ(outputs[0].height, 1080);
}

TEST(KscreenDoctorParser, DisabledOutputWithNoModesAtAllIsExcluded) {
  const std::string sample = R"({
    "outputs": [
      {"name": "DP-3", "connected": true, "enabled": false, "priority": 0,
       "pos": {"x": 0, "y": 0}, "size": {"width": -1, "height": -1},
       "preferredModes": [], "modes": []}
    ]
  })";

  EXPECT_TRUE(platf::parse_kscreen_doctor_outputs(sample).empty());
}

TEST(KscreenDoctorParser, EnabledConnectedButNoValidModeIsExcluded) {
  const std::string sample = R"({
    "outputs": [
      {"name": "DP-9", "connected": true, "enabled": true, "priority": 1,
       "pos": {"x": 0, "y": 0}, "size": {"width": -1, "height": -1}}
    ]
  })";

  const auto outputs = platf::parse_kscreen_doctor_outputs(sample);
  EXPECT_TRUE(outputs.empty());
}

TEST(KscreenDoctorParser, MalformedJsonYieldsEmptyResultInsteadOfThrowing) {
  EXPECT_TRUE(platf::parse_kscreen_doctor_outputs("not json").empty());
  EXPECT_TRUE(platf::parse_kscreen_doctor_outputs("").empty());
  EXPECT_TRUE(platf::parse_kscreen_doctor_outputs("{}").empty());
  EXPECT_TRUE(platf::parse_kscreen_doctor_outputs(R"({"outputs": "not-an-array"})").empty());
}

TEST(KscreenDoctorParser, FractionalScaleAndNegativeOriginArePreserved) {
  const auto outputs = platf::parse_kscreen_doctor_outputs(R"({"outputs":[
    {"name":"DP-1","connected":true,"enabled":true,"priority":1,
     "pos":{"x":-2560,"y":-100},"size":{"width":3840,"height":2160},"scale":1.5}]})");
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].x, -2560);
  EXPECT_EQ(outputs[0].y, -100);
  EXPECT_EQ(outputs[0].width, 3840);
  EXPECT_DOUBLE_EQ(outputs[0].scale, 1.5);
}

TEST(KscreenDoctorParser, BadFieldTypesDoNotHideOtherOutputs) {
  const auto outputs = platf::parse_kscreen_doctor_outputs(R"({"outputs":[
    {"name":"broken","connected":"yes"},
    {"name":"DP-1","connected":true,"enabled":true,"size":{"width":1920,"height":1080},"scale":0}]})");
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].name, "DP-1");
  EXPECT_DOUBLE_EQ(outputs[0].scale, 1.0);
}
