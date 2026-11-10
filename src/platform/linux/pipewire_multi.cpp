/**
 * @file src/platform/linux/pipewire_multi.cpp
 * @brief Packs several PipeWire screen streams into one frame, as display_plan.h lays them out.
 *
 * KWin and Mutter can each stream one monitor at its own refresh rate, but a desktop rectangle
 * spanning several (Mutter's RecordArea) is held to 60 fps, and a rectangle pulls in monitors
 * nobody asked for. Here each screen is its own pipewire_display_t; every frame the latest
 * dmabuf of each is blitted into its tile of a GBM buffer the plan's size, and that buffer goes
 * to the encoder exactly as a single stream's dmabuf would.
 *
 * Included by the capture backends that provide the streams (kwingrab.cpp), after pipewire.cpp.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <fcntl.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>

namespace pipewire_multi {
  /// One screen of the frame: where its stream is, where it goes in the frame, where it is on the desktop.
  struct screen_source_t {
    std::string label;
    uint32_t node = PW_ID_ANY;
    uint64_t serial = SPA_ID_INVALID;
    int frame_x = 0, frame_y = 0, width = 0, height = 0;
    int desktop_x = 0, desktop_y = 0;
    /// Ask for exactly this maximum rate (see add_framerate_parameters()); 0 for any.
    int max_framerate = 0;
    /// The session's primary screen: Meteor's host depth crops to it (see publish_primary_region()).
    bool is_primary = false;
  };

  /**
   * Meteor's host depth covers the primary screen only. With several screens in the frame, write
   * where the primary is (frame size, then x y w h) at the path Meteor names; with one, remove
   * the file. Same format as Windows and KWin's region capture.
   */
  inline void publish_primary_region(const std::vector<screen_source_t> &sources, int frame_w, int frame_h) {
    const char *path = std::getenv("METEOR_CAPTURE_REGION_FILE");
    if (!path || !*path) {
      return;
    }
    std::error_code ignored;
    const auto primary = std::find_if(sources.begin(), sources.end(), [](const auto &s) {
      return s.is_primary;
    });
    if (sources.size() < 2 || primary == sources.end()) {
      std::filesystem::remove(path, ignored);
      return;
    }
    std::ofstream out {path, std::ios::trunc};
    out << frame_w << ' ' << frame_h << ' ' << primary->frame_x << ' ' << primary->frame_y << ' ' << primary->width << ' '
        << primary->height << '\n';
  }

  /// One screen's stream. The packer owns input geometry, so this only negotiates and hands out frames.
  class source_display_t: public pipewire::pipewire_display_t {
  public:
    explicit source_display_t(screen_source_t s):
        source(std::move(s)) {}

    int configure_stream(const std::string &, int &out_fd, uint32_t &out_node, uint64_t &out_serial) override {
      out_fd = -1;  // The compositor's streams live on the session's own PipeWire core.
      out_node = source.node;
      out_serial = source.serial;
      offset_x = source.desktop_x;
      offset_y = source.desktop_y;
      width = source.width;
      height = source.height;
      logical_width = source.width;
      logical_height = source.height;
      // Meteor scales the cursor in the stream only; it comes as PipeWire metadata.
      const char *cursor_control = std::getenv("METEOR_CURSOR_SCALE_FILE");
      pipewire.set_cursor_metadata(cursor_control && *cursor_control);
      if (source.max_framerate > 0) {
        pipewire.set_fixed_max_framerate(source.max_framerate);
      }
      return 0;
    }

    void verify_and_update_display_parameters() override {}

    int start_stream() {
      return pipewire.ensure_stream(mem_type, width, height, framerate, dmabuf_infos.data(), n_dmabuf_infos, display_is_nvidia);
    }

    bool stream_ended() {
      return shared_state && shared_state->stream_dead.exchange(false);
    }

    bool nvidia() const {
      return display_is_nvidia;
    }

    /// Its newest frame when there is one since the last call, without waiting.
    bool take_frame(bool show_cursor) {
      if (!pipewire.is_frame_ready()) {
        return false;
      }
      if (!img) {
        img = alloc_img();
      }
      auto pull = [this](std::shared_ptr<platf::img_t> &out) {
        out = img;
        return true;
      };
      std::shared_ptr<platf::img_t> out;
      return snapshot(pull, out, 2ms, show_cursor) == platf::capture_e::ok;
    }

    screen_source_t source;
    std::shared_ptr<platf::img_t> img;
    std::optional<egl::rgb_t> texture;
    bool has_frame = false;
  };

  class multi_display_t: public platf::display_t {
  public:
    ~multi_display_t() override {
      if (!make_current()) {
        return;
      }
      for (auto &out : ring) {
        out.rgb.reset();
        if (out.fbo) {
          gl::ctx.DeleteFramebuffers(1, &out.fbo);
        }
        if (out.fd >= 0) {
          close(out.fd);
        }
        if (out.bo) {
          gbm_bo_destroy(out.bo);
        }
      }
      if (read_fbo) {
        gl::ctx.DeleteFramebuffers(1, &read_fbo);
      }
      for (auto &s : sources) {
        s->texture.reset();
      }
    }

    /**
     * @param sources_in The plan's enabled screens with their stream nodes.
     * @param frame_w, frame_h The plan's frame size.
     */
    int init(platf::mem_type_e type, std::vector<screen_source_t> sources_in, int frame_w, int frame_h, const ::video::config_t &config) {
      publish_primary_region(sources_in, frame_w, frame_h);
      mem_type = type;
      width = frame_w;
      height = frame_h;
      offset_x = offset_y = 0;
      logical_width = frame_w;
      logical_height = frame_h;
      framerate = config.framerate;
      delay = config.framerateX100 > 0 ?
                std::chrono::nanoseconds((static_cast<int64_t>(::video::framerateX100_to_rational(config.framerateX100).den) * 1'000'000'000LL) /
                                         ::video::framerateX100_to_rational(config.framerateX100).num) :
                std::chrono::nanoseconds {1s} / std::max(1, framerate);

      int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
      for (const auto &s : sources_in) {
        min_x = std::min(min_x, s.desktop_x);
        min_y = std::min(min_y, s.desktop_y);
        max_x = std::max(max_x, s.desktop_x + s.width);
        max_y = std::max(max_y, s.desktop_y + s.height);
      }
      frame_tiles.clear();
      for (const auto &s : sources_in) {
        frame_tiles.push_back({s.frame_x, s.frame_y, s.desktop_x - min_x, s.desktop_y - min_y, s.width, s.height});
      }
      env_width = env_logical_width = max_x - min_x;
      env_height = env_logical_height = max_y - min_y;

      for (auto &s : sources_in) {
        auto source = std::make_unique<source_display_t>(s);
        auto source_config = config;
        source_config.width = s.width;
        source_config.height = s.height;
        if (source->init(type, s.label, source_config)) {
          BOOST_LOG(error) << "[pipewire-multi] Couldn't start the stream of screen "sv << s.label;
          return -1;
        }
        BOOST_LOG(info) << "[pipewire-multi] Screen "sv << s.label << ' ' << source->width << 'x' << source->height << " into "sv
                        << s.width << 'x' << s.height << " at "sv << s.frame_x << ',' << s.frame_y;
        sources.push_back(std::move(source));
      }
      if (sources.empty()) {
        return -1;
      }
      return init_gl() ? 0 : -1;
    }

    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      for (auto &s : sources) {
        if (s->start_stream() < 0) {
          BOOST_LOG(error) << "[pipewire-multi] Couldn't start the stream of screen "sv << s->source.label;
          return platf::capture_e::error;
        }
      }
      auto next_frame = std::chrono::steady_clock::now();
      while (true) {
        for (auto &s : sources) {
          if (s->stream_ended()) {
            BOOST_LOG(warning) << "[pipewire-multi] The stream of screen "sv << s->source.label << " ended; restarting capture"sv;
            return platf::capture_e::reinit;
          }
        }
        auto now = std::chrono::steady_clock::now();
        while (next_frame < now) {
          next_frame += delay;
        }
        if (next_frame > now) {
          std::this_thread::sleep_until(next_frame);
        }

        std::shared_ptr<platf::img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }
        const auto composed = compose(*img_out, *cursor);
        if (composed == platf::capture_e::error || composed == platf::capture_e::reinit) {
          return composed;
        }
        if (!push_captured_image_cb(std::move(img_out), composed == platf::capture_e::ok)) {
          return platf::capture_e::ok;
        }
      }
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<pipewire::image_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->data = nullptr;
      std::fill_n(img->sd.fds, 4, -1);
      return img;
    }

    int dummy_img(platf::img_t *img) override {
      return img ? 0 : -1;
    }

    bool is_hdr() override {
      return false;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
      // Same choice as a single stream's: the encoder imports the packed dmabuf like any other.
      const auto &first = *sources.front();
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, true);
      }
#endif
#ifdef SUNSHINE_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan) {
        return vk::make_avcodec_encode_device_vram(width, height, 0, 0);
      }
#endif
#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda && first.nvidia()) {
        return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
      }
#endif
      (void) first;
      (void) pix_fmt;
      return std::make_unique<platf::avcodec_encode_device_t>();
    }

  private:
    struct output_t {
      gbm_bo *bo = nullptr;
      int fd = -1;
      uint32_t stride = 0, offset = 0;
      uint64_t modifier = DRM_FORMAT_MOD_INVALID;
      std::optional<egl::rgb_t> rgb;
      GLuint fbo = 0;
    };

    /// Capture can run on another thread than init(); GL needs our context current there.
    bool make_current() {
      if (!ctx || !display) {
        return false;
      }
      const auto context = std::get<1>(ctx->el);
      return eglGetCurrentContext() == context || eglMakeCurrent(display.get(), EGL_NO_SURFACE, EGL_NO_SURFACE, context);
    }

    bool init_gl() {
      const auto device = platf::resolve_render_device();
      render_fd = file_t {::open(device.c_str(), O_RDWR | O_CLOEXEC)};
      if (render_fd.el < 0 || !gbm::create_device) {
        BOOST_LOG(error) << "[pipewire-multi] Can't open the render device "sv << device;
        return false;
      }
      gbm.reset(gbm::create_device(render_fd.el));
      if (!gbm) {
        return false;
      }
      display = egl::make_display(gbm.get());
      if (!display) {
        return false;
      }
      ctx = egl::make_ctx(display.get());
      if (!ctx) {
        return false;
      }
      auto *gbm_device = reinterpret_cast<struct gbm_device *>(gbm.get());
      for (auto &out : ring) {
        out.bo = gbm_bo_create(gbm_device, width, height, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
        if (!out.bo) {
          BOOST_LOG(error) << "[pipewire-multi] Can't allocate a "sv << width << 'x' << height << " frame"sv;
          return false;
        }
        out.fd = gbm_bo_get_fd(out.bo);
        out.stride = gbm_bo_get_stride(out.bo);
        out.offset = gbm_bo_get_offset(out.bo, 0);
        out.modifier = gbm_bo_get_modifier(out.bo);
        egl::surface_descriptor_t sd {};
        sd.width = width;
        sd.height = height;
        sd.fourcc = DRM_FORMAT_XRGB8888;
        sd.modifier = out.modifier;
        sd.fds[0] = out.fd;
        sd.pitches[0] = out.stride;
        sd.offsets[0] = out.offset;
        out.rgb = egl::import_source(display.get(), sd);
        if (!out.rgb) {
          BOOST_LOG(error) << "[pipewire-multi] Can't render into the frame buffer"sv;
          return false;
        }
        gl::ctx.GenFramebuffers(1, &out.fbo);
        gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, out.fbo);
        gl::ctx.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (*out.rgb)->tex[0], 0);
        gl::ctx.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl::ctx.Clear(GL_COLOR_BUFFER_BIT);
      }
      gl::ctx.GenFramebuffers(1, &read_fbo);
      gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, 0);
      gl::ctx.Finish();
      return true;
    }

    /// Draws every screen's newest frame into the next frame buffer; timeout when none changed.
    platf::capture_e compose(platf::img_t &img_out, bool show_cursor) {
      if (!make_current()) {
        return platf::capture_e::error;
      }
      bool any_new = false;
      for (auto &s : sources) {
        if (!s->take_frame(show_cursor)) {
          continue;
        }
        auto *frame = static_cast<egl::img_descriptor_t *>(s->img.get());
        if (frame->sd.fds[0] < 0) {
          continue;  // SHM frames: not packed yet (KWin and Mutter give dmabufs).
        }
        s->texture = egl::import_source(display.get(), frame->sd);
        egl::reset_surface(frame->sd);
        if (!s->texture) {
          BOOST_LOG(warning) << "[pipewire-multi] Can't import a frame of screen "sv << s->source.label;
          continue;
        }
        s->has_frame = true;
        any_new = true;
      }
      if (!any_new) {
        return platf::capture_e::timeout;
      }
      auto &out = ring[next];
      next = (next + 1) % ring.size();
      gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, out.fbo);
      gl::ctx.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
      for (auto &s : sources) {
        if (!s->has_frame || !s->texture) {
          continue;  // Stays black until its first frame.
        }
        gl::ctx.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (*s->texture)->tex[0], 0);
        const auto &src = static_cast<egl::img_descriptor_t *>(s->img.get())->sd;
        const int sw = src.width > 0 ? src.width : s->width, sh = src.height > 0 ? src.height : s->height;
        const auto &t = s->source;
        gl::ctx.BlitFramebuffer(0, 0, sw, sh, t.frame_x, t.frame_y, t.frame_x + t.width, t.frame_y + t.height,
                                GL_COLOR_BUFFER_BIT, sw == t.width && sh == t.height ? GL_NEAREST : GL_LINEAR);
      }
      gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, 0);
      // The encoder samples this buffer from its own context.
      gl::ctx.Finish();

      auto *img = static_cast<egl::img_descriptor_t *>(&img_out);
      img->reset();
      img->data = nullptr;
      img->sd.width = width;
      img->sd.height = height;
      img->sd.fourcc = DRM_FORMAT_XRGB8888;
      img->sd.modifier = out.modifier;
      img->sd.fds[0] = dup(out.fd);
      img->sd.pitches[0] = out.stride;
      img->sd.offsets[0] = out.offset;
      img->sequence = ++sequence;
      // The cursor of whichever screen shows it, moved into that screen's tile.
      for (auto &s : sources) {
        auto *frame = static_cast<egl::img_descriptor_t *>(s->img.get());
        if (!frame || !frame->data || frame->buffer.empty()) {
          continue;
        }
        img->buffer = frame->buffer;
        img->data = img->buffer.data();
        img->src_w = frame->src_w;
        img->src_h = frame->src_h;
        img->width = frame->width;
        img->height = frame->height;
        img->x = frame->x + s->source.frame_x;
        img->y = frame->y + s->source.frame_y;
        img->serial = frame->serial;
        img->cursor_premultiplied = frame->cursor_premultiplied;
        break;
      }
      return platf::capture_e::ok;
    }

    platf::mem_type_e mem_type {};
    int framerate = 60;
    std::chrono::nanoseconds delay {};
    std::uint64_t sequence = 0;
    std::vector<std::unique_ptr<source_display_t>> sources;
    file_t render_fd;
    gbm::gbm_t gbm;
    egl::display_t display;
    std::optional<egl::ctx_t> ctx;
    std::array<output_t, 3> ring;
    std::size_t next = 0;
    GLuint read_fbo = 0;
  };
}  // namespace pipewire_multi
