/**
 * @file src/platform/linux/pipewire_format.h
 * @brief PipeWire video format negotiation shared with focused SPA tests.
 */
#pragma once

#include <cstdint>
#include <limits>
#include <spa/param/video/format.h>
#include <spa/pod/builder.h>

namespace pipewire {
  /// @param fixed_max_framerate When set, ask for exactly this maximum. Mutter sizes a virtual
  ///                            monitor's refresh rate from it (RecordVirtual).
  inline void add_framerate_parameters(spa_pod_builder *builder, bool negotiate_maxframerate, int fixed_max_framerate = 0) {
    const auto variable_rate = SPA_FRACTION(0, 1);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable_rate), 0);
    if (fixed_max_framerate > 0) {
      const auto maximum_rate = SPA_FRACTION(static_cast<uint32_t>(fixed_max_framerate), 1);
      spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_Fraction(&maximum_rate), 0);
    } else if (negotiate_maxframerate) {
      // Prefer an unpaced stream, but accept compositors such as KWin whose
      // maximum-rate range starts at 1 fps. A zero-only range has no overlap
      // with those producers and rejects every otherwise compatible format.
      const auto maximum_rate = SPA_FRACTION(std::numeric_limits<int32_t>::max(), 1);
      spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_maxFramerate,
                          SPA_POD_CHOICE_RANGE_Fraction(&variable_rate, &variable_rate, &maximum_rate), 0);
    }
  }
}  // namespace pipewire
