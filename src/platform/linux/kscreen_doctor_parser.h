/**
 * @file src/platform/linux/kscreen_doctor_parser.h
 * @brief Pure parsing of `kscreen-doctor -j` output into backend-neutral output descriptors.
 *
 * Ported from Polaris (7f6c2b6); kept separate so tests need not invoke the real
 * kscreen-doctor binary or a live KDE session — mirrors the x11grab.cpp / display_manifest.h
 * split (geometry math kept pure and testable, process/display-server calls kept thin).
 */
#pragma once

// standard includes
#include <string>
#include <vector>

// local includes
#include "display_manifest.h"

namespace platf {

  /**
   * @brief Parse the JSON emitted by `kscreen-doctor -j` into every *connected* output.
   *
   * Includes connected-but-disabled outputs (`capturable = false`, best-effort geometry from
   * their preferred mode) so a manifest built from this list never silently drops a monitor
   * just because it isn't currently active — disconnected outputs are excluded entirely, since
   * there's nothing a client could ever "add" for those.
   *
   * kscreen-doctor doesn't report an explicit "primary" flag; the primary output is the
   * enabled one with `priority == 1` (confirmed against a live two-monitor KDE Plasma session —
   * the primary display's priority is always 1, secondary displays get higher priority
   * numbers). A disabled output's `priority` is treated as meaningless.
   *
   * @param json_text Raw stdout of `kscreen-doctor -j`.
   * @return Connected outputs (enabled or not) as plain descriptors. Malformed/empty input yields an empty vector; malformed output records are skipped
   *         so callers can fall back cleanly.
   */
  std::vector<output_descriptor_t> parse_kscreen_doctor_outputs(const std::string &json_text);

}  // namespace platf
