#include "src/meteor_cursor_scale.h"
#include "src/capture_outputs.h"
#include <gtest/gtest.h>
#include <array>
#include <filesystem>

TEST(MeteorCursorScale, ScalesActualImageAndHotspot) {
  const unsigned long source[] = {0xff112233, 0xff445566, 0xff778899, 0xffaabbcc};
  std::array<std::uint32_t, 100> image{};
  meteor_cursor::blend(reinterpret_cast<std::uint8_t *>(image.data()), 10, 10, 40,
      source, 2, 2, 4, 4, 1, 1, 3);
  // Hotspot remains at (4,4), despite enlarging from 2x2 to 6x6.
  EXPECT_EQ(image[1 * 10 + 1], 0xff112233u);
  EXPECT_EQ(image[4 * 10 + 4], 0xffaabbccu);
  EXPECT_EQ(image[6 * 10 + 6], 0xffaabbccu);
  EXPECT_EQ(image[7 * 10 + 7], 0u);
}
TEST(MeteorCursorScale, ClipsNegativeOriginWithoutShiftingCursor) {
  const unsigned long source[] = {0xff112233, 0xff445566, 0xff778899, 0xffaabbcc};
  std::array<std::uint32_t, 6> image{};
  meteor_cursor::blend(reinterpret_cast<std::uint8_t *>(image.data()), 2, 2, 12,
      source, 2, 2, 0, 0, 1, 1, 2);
  EXPECT_EQ(image[0], 0xffaabbccu);
  EXPECT_EQ(image[1], 0xffaabbccu);
  EXPECT_EQ(image[2], 0u); // row-pitch padding untouched
  EXPECT_EQ(image[3], 0xffaabbccu);
  EXPECT_EQ(image[4], 0xffaabbccu);
  EXPECT_EQ(image[5], 0u);
}
TEST(MeteorCursorScale, KeepsPremultipliedAlpha) {
  const unsigned long source[] = {0x80800000, 0x00000000};
  std::array<std::uint32_t, 2> image{0xff000080, 0xff123456};
  meteor_cursor::blend(reinterpret_cast<std::uint8_t *>(image.data()), 2, 1, 8,
      source, 2, 1, 0, 0, 0, 0, 1);
  EXPECT_EQ(image[0], 0xff800040u);
  EXPECT_EQ(image[1], 0xff123456u);
}
TEST(MeteorCursorScale, RejectsInvalidOrMissingControl) {
  const auto path = std::filesystem::temp_directory_path() / "meteor-cursor-control-unit";
  for (const auto text : {"2junk", "0", "5", "-2", ""}) {
    { std::ofstream file(path); file << text; }
    EXPECT_EQ(meteor_cursor::read_scale(path.string()), 1);
  }
  for (int scale = 1; scale <= 4; ++scale) {
    { std::ofstream file(path); file << scale << '\n'; }
    EXPECT_EQ(meteor_cursor::read_scale(path.string()), scale);
  }
  std::filesystem::remove(path);
  EXPECT_EQ(meteor_cursor::read_scale(path.string()), 1);
}

TEST(MeteorCaptureDefaults, PrimaryOnlyDoesNotOverrideExplicitMonitorSelection) {
  struct Output { std::string name; bool primary; bool capturable; };
  const std::array<Output, 2> outputs{{{"HDMI-2", false, true}, {"DP-1", true, true}}};
  EXPECT_EQ(capture_outputs::initial_selection("", "", true, outputs), "DP-1");
  EXPECT_EQ(capture_outputs::initial_selection("DP-1,HDMI-2", "", true, outputs), "DP-1,HDMI-2");
  EXPECT_EQ(capture_outputs::initial_selection("", "1", true, outputs), "");
  EXPECT_EQ(capture_outputs::initial_selection("", "", false, outputs), "");
}
