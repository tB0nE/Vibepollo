#pragma once
#include <charconv>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
namespace virtual_display_modes {
using modes_t = std::vector<std::pair<int, int>>;
// Empty dimensions inherit the physical primary. Request scoped, never global.
inline std::optional<modes_t> parse(std::string_view text, int count) {
  modes_t modes;
  if (text.empty()) return modes;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto item = text.substr(0, comma);
    const auto x = item.find('x');
    if (x == std::string_view::npos) return std::nullopt;
    int w = 0, h = 0;
    auto a = std::from_chars(item.data(), item.data()+x, w);
    auto b = std::from_chars(item.data()+x+1, item.data()+item.size(), h);
    if (a.ec != std::errc{} || b.ec != std::errc{} || a.ptr != item.data()+x || b.ptr != item.data()+item.size()) return std::nullopt;
    if ((w != 0 || h != 0) && (w < 320 || h < 240 || w > 8192 || h > 8192 || w%2 || h%2 || 1LL*w*h > 35389440)) return std::nullopt;
    modes.emplace_back(w,h);
    if (modes.size() > 64) return std::nullopt;
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma+1);
    if (text.empty()) return std::nullopt;
  }
  if (static_cast<int>(modes.size()) != count) return std::nullopt;
  return modes;
}
inline std::pair<int,int> size(const modes_t &modes, int index, int w, int h) {
  if (index >= 0 && index < static_cast<int>(modes.size()) && modes[index].first > 0) return modes[index];
  return {w,h};
}
}
