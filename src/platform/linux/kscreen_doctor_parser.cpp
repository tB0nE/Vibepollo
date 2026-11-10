/**
 * @file src/platform/linux/kscreen_doctor_parser.cpp
 * @brief Implementation of kscreen-doctor -j output parsing. See header for design notes.
 */
#include "kscreen_doctor_parser.h"

#include <cmath>

#include <nlohmann/json.hpp>

namespace platf {

  std::vector<output_descriptor_t> parse_kscreen_doctor_outputs(const std::string &json_text) {
    std::vector<output_descriptor_t> result;

    nlohmann::json doc;
    try {
      doc = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::parse_error &) {
      return result;
    }

    if (!doc.is_object() || !doc.contains("outputs") || !doc["outputs"].is_array()) {
      return result;
    }

    for (const auto &out : doc["outputs"]) {
      if (!out.is_object()) {
        continue;
      }
      try {
        // Connected-but-disabled outputs are included (capturable=false) so a monitor that
        // dropped out of the current capture selection still shows up in a manifest built from
        // this list, instead of vanishing entirely. This matches the X11 manifest behavior.
        const bool connected = out.value("connected", false);
        if (!connected) {
          continue;
        }
        const bool enabled = out.value("enabled", false);

        output_descriptor_t desc;
        desc.name = out.value("name", std::string {});
        desc.capturable = enabled;

        const auto pos = out.value("pos", nlohmann::json::object());
        desc.x = pos.value("x", 0);
        desc.y = pos.value("y", 0);

        const auto size = out.value("size", nlohmann::json::object());
        desc.width = size.value("width", 0);
        desc.height = size.value("height", 0);
        desc.scale = out.value("scale", 1.0);
        if (!std::isfinite(desc.scale) || desc.scale <= 0.0) {
          desc.scale = 1.0;
        }

        if (!enabled && (desc.width <= 0 || desc.height <= 0)) {
          // Disabled outputs often report size {-1,-1} (no current mode). Best-effort fall back
          // to the preferred mode's size so the manifest can still report a plausible
          // resolution; position is meaningless without an active mode, so it's left at the
          // pos value above (typically {0,0}) rather than guessed.
          const auto preferred_ids = out.value("preferredModes", nlohmann::json::array());
          if (!preferred_ids.empty() && preferred_ids[0].is_string()) {
            const auto preferred_id = preferred_ids[0].get<std::string>();
            for (const auto &mode : out.value("modes", nlohmann::json::array())) {
              if (mode.value("id", std::string {}) == preferred_id) {
                const auto mode_size = mode.value("size", nlohmann::json::object());
                desc.width = mode_size.value("width", 0);
                desc.height = mode_size.value("height", 0);
                break;
              }
            }
          }
        }

        // kscreen-doctor doesn't expose an explicit "primary" flag; priority 1 is the
        // primary output in a connected+enabled set (see header for how this was confirmed).
        // A disabled output's stale priority is meaningless, so only enabled ones can be primary.
        desc.primary = enabled && out.value("priority", 0) == 1;

        if (desc.width <= 0 || desc.height <= 0) {
          // No usable geometry at all (not even a preferred mode) — skip rather than report a
          // zero-size tile. Still genuinely rare: only outputs with literally no modes at all.
          continue;
        }

        if (!desc.name.empty()) {
          result.push_back(std::move(desc));
        }
      } catch (const nlohmann::json::exception &) {
        // One malformed output must not hide the other usable displays.
        continue;
      }
    }

    return result;
  }

}  // namespace platf
