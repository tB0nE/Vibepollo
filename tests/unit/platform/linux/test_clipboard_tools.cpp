#include "src/platform/linux/clipboard_tools.h"
#include <gtest/gtest.h>

TEST(ClipboardTools, UnicodeInputIsDataNotShellCode) {
  const std::string text="Unicode: é\n$(not-a-command); literal clipboard text";
  auto result=clipboard_tools::run({"/bin/cat"},text,true);
  ASSERT_TRUE(result);EXPECT_EQ(*result,text);
}
TEST(ClipboardTools, OversizedOutputAndMissingCommandFail) {
  EXPECT_FALSE(clipboard_tools::run({"/usr/bin/head","-c","70000","/dev/zero"},"",true));
  EXPECT_FALSE(clipboard_tools::run({"/nonexistent/nightfall-clipboard-test"},"",true));
}
TEST(ClipboardTools, DeadlineKillsUnresponsiveHelper) {
  const auto start=std::chrono::steady_clock::now();
  EXPECT_FALSE(clipboard_tools::run({"/bin/sleep","20"},"",true));
  EXPECT_LT(std::chrono::steady_clock::now()-start,std::chrono::seconds(6));
}
