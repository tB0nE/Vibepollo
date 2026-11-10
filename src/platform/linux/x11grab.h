/**
 * @file src/platform/linux/x11grab.h
 * @brief Declarations for x11 capture.
 */
#pragma once

// standard includes
#include <optional>
#include <string>

// local includes
#include "display_manifest.h"
#include "src/platform/common.h"
#include "src/utility.h"

// X11 Display
extern "C" struct _XDisplay;

namespace egl {
  class cursor_t;
}

namespace platf::x11 {
  struct cursor_ctx_raw_t;
  void freeCursorCtx(cursor_ctx_raw_t *ctx);
  void freeDisplay(_XDisplay *xdisplay);

  using cursor_ctx_t = util::safe_ptr<cursor_ctx_raw_t, freeCursorCtx>;
  using xdisplay_t = util::safe_ptr<_XDisplay, freeDisplay>;

  class cursor_t {
  public:
    static std::optional<cursor_t> make();

    void capture(egl::cursor_t &img);

    /**
     * Capture and blend the cursor into the image
     *
     * img <-- destination image
     * offsetX, offsetY <--- Top left corner of the virtual screen
     */
    void blend(img_t &img, int offsetX, int offsetY);

    cursor_ctx_t ctx;
  };

  xdisplay_t make_display();
}  // namespace platf::x11

namespace platf {
  /**
   * @brief The X11 region the current session's outputs= selects, for any X11 capture
   *        backend. Nothing when the session didn't ask or X11 isn't available.
   */
  std::optional<capture_region_t> x11_session_region();

  /**
   * @brief The desktop manifest for `outputs` (empty: what capture uses without outputs=).
   * @return Nothing when X11 or XRandR isn't available.
   */
  std::optional<desktop_manifest_t> x11_query_desktop_manifest(const std::string &outputs);
}  // namespace platf
