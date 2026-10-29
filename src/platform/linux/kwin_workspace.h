#pragma once
#include <optional>
#include <string>
#include "display_manifest.h"

namespace platf {
  // Returns no manifest when the active backend cannot honor the reported selection.
  std::optional<desktop_manifest_t> kwin_query_desktop_manifest(const std::string &outputs);
  std::optional<std::string> kwin_workspace_selection_error(const std::string &outputs);
}
