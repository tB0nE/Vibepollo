/**
 * @file src/platform/linux/muttergrab.cpp
 * @brief GNOME (Mutter) capture and virtual monitors through Mutter's own D-Bus interfaces.
 *
 * org.gnome.Mutter.ScreenCast streams a monitor at its own refresh rate with no consent dialog
 * (RecordMonitor), and RecordVirtual makes a virtual monitor sized by the first consumer's
 * format. Several screens are packed from their own streams (pipewire_multi.cpp): Mutter holds a
 * desktop rectangle (RecordArea) to 60 fps. org.gnome.Mutter.DisplayConfig places monitors.
 *
 * Verified on headless Mutter 50.6: a virtual monitor stays after its consumer disconnects, until
 * its session stops, and RecordMonitor captures it at its full rate.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <gio/gio.h>
#include <pipewire/pipewire.h>

// local includes
#include "graphics.h"
#include "kwin_virtual_layout.h"
#include "mutter_workspace.h"
#include "pipewire.cpp"
#include "pipewire_multi.cpp"
#include "src/capture_outputs.h"
#include "src/display_plan.h"
#include "src/platform/common.h"
#include "src/video.h"

using namespace std::literals;

namespace mutter {
  constexpr auto screencast_name = "org.gnome.Mutter.ScreenCast";
  constexpr auto screencast_path = "/org/gnome/Mutter/ScreenCast";
  constexpr auto display_config_name = "org.gnome.Mutter.DisplayConfig";
  constexpr auto display_config_path = "/org/gnome/Mutter/DisplayConfig";
  /// cursor-mode: 1 embedded, 2 metadata.
  constexpr guint32 cursor_embedded = 1;
  constexpr guint32 cursor_metadata = 2;

  GDBusConnection *bus() {
    static GDBusConnection *connection = [] {
      GError *error = nullptr;
      auto *c = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
      if (error) {
        BOOST_LOG(warning) << "[mutter] No session bus: "sv << error->message;
        g_error_free(error);
      }
      return c;
    }();
    return connection;
  }

  /// A D-Bus call; nullptr (and `error` set) on failure. The caller unrefs the reply.
  GVariant *call(const char *name, const char *path, const char *iface, const char *method, GVariant *args, const char *reply_type,
                 std::string *error = nullptr, int timeout_ms = 5000) {
    if (!bus()) {
      if (error) {
        *error = "no session bus";
      }
      if (args) {
        g_variant_unref(g_variant_ref_sink(args));
      }
      return nullptr;
    }
    GError *err = nullptr;
    auto *reply = g_dbus_connection_call_sync(bus(), name, path, iface, method, args, reply_type ? G_VARIANT_TYPE(reply_type) : nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, timeout_ms, nullptr, &err);
    if (err) {
      if (error) {
        *error = err->message;
      }
      g_error_free(err);
      return nullptr;
    }
    return reply;
  }

  bool available() {
    if (window_system != window_system_e::WAYLAND || (!config::video.capture.empty() && config::video.capture != "mutter")) {
      return false;
    }
    auto *reply = call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                       g_variant_new("(s)", screencast_name), "(b)");
    if (!reply) {
      return false;
    }
    gboolean has = FALSE;
    g_variant_get(reply, "(b)", &has);
    g_variant_unref(reply);
    return has;
  }

  struct monitor_t {
    std::string connector;
    int x = 0, y = 0, width = 0, height = 0;  ///< Logical (layout) coordinates.
    int mode_width = 0, mode_height = 0;
    std::string mode_id;
    double scale = 1.0;
    guint32 transform = 0;
    bool primary = false;
  };

  struct state_t {
    guint32 serial = 0;
    std::vector<monitor_t> monitors;
  };

  /// DisplayConfig.GetCurrentState: every enabled monitor with its logical rectangle.
  std::optional<state_t> current_state() {
    auto *reply = call(display_config_name, display_config_path, "org.gnome.Mutter.DisplayConfig", "GetCurrentState", nullptr,
                       "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})");
    if (!reply) {
      return std::nullopt;
    }
    state_t state;
    GVariant *monitors = nullptr, *logical = nullptr, *props = nullptr;
    g_variant_get(reply, "(u@a((ssss)a(siiddada{sv})a{sv})@a(iiduba(ssss)a{sv})@a{sv})", &state.serial, &monitors, &logical, &props);
    gboolean physical_layout = FALSE;
    {
      guint32 layout_mode = 1;
      if (g_variant_lookup(props, "layout-mode", "u", &layout_mode)) {
        physical_layout = layout_mode == 2;
      }
    }
    // Each connector's current mode.
    struct mode_t {
      std::string id;
      int w = 0, h = 0;
    };
    std::vector<std::pair<std::string, mode_t>> modes;
    {
      GVariantIter iter;
      g_variant_iter_init(&iter, monitors);
      GVariant *monitor;
      while ((monitor = g_variant_iter_next_value(&iter))) {
        const char *connector = nullptr, *vendor = nullptr, *product = nullptr, *serial = nullptr;
        GVariant *mode_list = nullptr, *mprops = nullptr;
        g_variant_get(monitor, "((&s&s&s&s)@a(siiddada{sv})@a{sv})", &connector, &vendor, &product, &serial, &mode_list, &mprops);
        GVariantIter mode_iter;
        g_variant_iter_init(&mode_iter, mode_list);
        GVariant *mode;
        while ((mode = g_variant_iter_next_value(&mode_iter))) {
          const char *id = nullptr;
          gint32 w = 0, h = 0;
          gdouble refresh = 0, preferred_scale = 1;
          GVariant *scales = nullptr, *mode_props = nullptr;
          g_variant_get(mode, "(&siidd@ad@a{sv})", &id, &w, &h, &refresh, &preferred_scale, &scales, &mode_props);
          gboolean current = FALSE;
          if (g_variant_lookup(mode_props, "is-current", "b", &current) && current) {
            modes.push_back({connector, {id, w, h}});
          }
          g_variant_unref(scales);
          g_variant_unref(mode_props);
          g_variant_unref(mode);
        }
        g_variant_unref(mode_list);
        g_variant_unref(mprops);
        g_variant_unref(monitor);
      }
    }
    GVariantIter iter;
    g_variant_iter_init(&iter, logical);
    GVariant *lm;
    while ((lm = g_variant_iter_next_value(&iter))) {
      gint32 x = 0, y = 0;
      gdouble scale = 1;
      guint32 transform = 0;
      gboolean primary = FALSE;
      GVariant *specs = nullptr, *lprops = nullptr;
      g_variant_get(lm, "(iidub@a(ssss)@a{sv})", &x, &y, &scale, &transform, &primary, &specs, &lprops);
      GVariantIter spec_iter;
      g_variant_iter_init(&spec_iter, specs);
      const char *connector = nullptr, *vendor = nullptr, *product = nullptr, *serial = nullptr;
      while (g_variant_iter_next(&spec_iter, "(&s&s&s&s)", &connector, &vendor, &product, &serial)) {
        const auto mode = std::find_if(modes.begin(), modes.end(), [&](const auto &m) {
          return m.first == connector;
        });
        if (mode == modes.end()) {
          continue;
        }
        monitor_t m;
        m.connector = connector;
        m.x = x;
        m.y = y;
        m.mode_width = mode->second.w;
        m.mode_height = mode->second.h;
        m.mode_id = mode->second.id;
        m.scale = scale > 0 ? scale : 1.0;
        m.transform = transform;
        const bool rotated = transform % 2 == 1;
        const int w = rotated ? m.mode_height : m.mode_width, h = rotated ? m.mode_width : m.mode_height;
        m.width = physical_layout ? w : static_cast<int>(w / m.scale);
        m.height = physical_layout ? h : static_cast<int>(h / m.scale);
        m.primary = primary;
        state.monitors.push_back(m);
        break;  // Mirrored monitors share one logical monitor; the first stands for it.
      }
      g_variant_unref(specs);
      g_variant_unref(lprops);
      g_variant_unref(lm);
    }
    g_variant_unref(monitors);
    g_variant_unref(logical);
    g_variant_unref(props);
    g_variant_unref(reply);
    // The primary first, as KWin orders outputs; display_plan treats the flagged one as main.
    std::stable_sort(state.monitors.begin(), state.monitors.end(), [](const auto &a, const auto &b) {
      return a.primary && !b.primary;
    });
    return state;
  }

  /// One ScreenCast session with one stream.
  class session_t {
  public:
    ~session_t() {
      stop();
    }

    bool record_monitor(const std::string &connector, bool cursor_as_metadata, std::string &error) {
      if (!create(error)) {
        return false;
      }
      GVariantBuilder props;
      g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&props, "{sv}", "cursor-mode", g_variant_new_uint32(cursor_as_metadata ? cursor_metadata : cursor_embedded));
      return record("RecordMonitor", g_variant_new("(sa{sv})", connector.c_str(), &props), error);
    }

    bool record_virtual(std::string &error) {
      if (!create(error)) {
        return false;
      }
      GVariantBuilder props;
      g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&props, "{sv}", "cursor-mode", g_variant_new_uint32(cursor_embedded));
      g_variant_builder_add(&props, "{sv}", "is-platform", g_variant_new_boolean(TRUE));
      return record("RecordVirtual", g_variant_new("(a{sv})", &props), error);
    }

    void stop() {
      if (!session_path.empty()) {
        if (auto *reply = call(screencast_name, session_path.c_str(), "org.gnome.Mutter.ScreenCast.Session", "Stop", nullptr, nullptr)) {
          g_variant_unref(reply);
        }
        session_path.clear();
      }
    }

    uint32_t node = PW_ID_ANY;

  private:
    bool create(std::string &error) {
      GVariantBuilder props;
      g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
      auto *reply = call(screencast_name, screencast_path, "org.gnome.Mutter.ScreenCast", "CreateSession", g_variant_new("(a{sv})", &props), "(o)", &error);
      if (!reply) {
        return false;
      }
      const char *path = nullptr;
      g_variant_get(reply, "(&o)", &path);
      session_path = path;
      g_variant_unref(reply);
      return true;
    }

    /// Records, starts the session and waits for the stream's PipeWire node.
    bool record(const char *method, GVariant *args, std::string &error) {
      auto *reply = call(screencast_name, session_path.c_str(), "org.gnome.Mutter.ScreenCast.Session", method, args, "(o)", &error);
      if (!reply) {
        return false;
      }
      const char *stream = nullptr;
      g_variant_get(reply, "(&o)", &stream);
      const std::string stream_path = stream;
      g_variant_unref(reply);

      // The node arrives as a signal; take it on a private main context.
      auto *context = g_main_context_new();
      g_main_context_push_thread_default(context);
      std::atomic<uint32_t> received {PW_ID_ANY};
      const auto subscription = g_dbus_connection_signal_subscribe(
        bus(), screencast_name, "org.gnome.Mutter.ScreenCast.Stream", "PipeWireStreamAdded", stream_path.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *params, gpointer data) {
          guint32 id = PW_ID_ANY;
          g_variant_get(params, "(u)", &id);
          static_cast<std::atomic<uint32_t> *>(data)->store(id);
        },
        &received, nullptr
      );
      bool started = false;
      if (auto *start = call(screencast_name, session_path.c_str(), "org.gnome.Mutter.ScreenCast.Session", "Start", nullptr, nullptr, &error)) {
        g_variant_unref(start);
        started = true;
      }
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (started && received.load() == PW_ID_ANY && std::chrono::steady_clock::now() < deadline) {
        g_main_context_iteration(context, FALSE);
        std::this_thread::sleep_for(10ms);
      }
      g_dbus_connection_signal_unsubscribe(bus(), subscription);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
      node = received.load();
      if (node == PW_ID_ANY && error.empty()) {
        error = "Mutter didn't start the stream";
      }
      return node != PW_ID_ANY;
    }

    std::string session_path;
  };

  /// Places monitors at the given logical positions (others unchanged), temporarily.
  bool place(const std::vector<std::pair<std::string, std::pair<int, int>>> &positions, std::string &error) {
    const auto state = current_state();
    if (!state) {
      error = "Can't read the monitor layout from Mutter";
      return false;
    }
    // Reconfiguring rebuilds every view; skip it when the monitors already stand there (Mutter
    // adds a virtual monitor at the right edge, which is where a 1+N plan puts it).
    const bool unchanged = std::all_of(positions.begin(), positions.end(), [&](const auto &wanted) {
      return std::any_of(state->monitors.begin(), state->monitors.end(), [&](const auto &m) {
        return m.connector == wanted.first && m.x == wanted.second.first && m.y == wanted.second.second;
      });
    });
    if (unchanged) {
      return true;
    }
    GVariantBuilder logical;
    g_variant_builder_init(&logical, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
    for (const auto &m : state->monitors) {
      int x = m.x, y = m.y;
      for (const auto &[name, at] : positions) {
        if (name == m.connector) {
          x = at.first;
          y = at.second;
        }
      }
      GVariantBuilder specs;
      g_variant_builder_init(&specs, G_VARIANT_TYPE("a(ssa{sv})"));
      GVariantBuilder props;
      g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&specs, "(ssa{sv})", m.connector.c_str(), m.mode_id.c_str(), &props);
      g_variant_builder_add(&logical, "(iiduba(ssa{sv}))", x, y, m.scale, m.transform, m.primary ? TRUE : FALSE, &specs);
    }
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    // Method 1 is temporary: nothing written to monitors.xml, no confirmation prompt.
    auto *reply = call(display_config_name, display_config_path, "org.gnome.Mutter.DisplayConfig", "ApplyMonitorsConfig",
                       g_variant_new("(uua(iiduba(ssa{sv}))a{sv})", state->serial, 1u, &logical, &props), nullptr, &error);
    if (!reply) {
      return false;
    }
    g_variant_unref(reply);
    return true;
  }

  /// A plain one-monitor capture.
  class monitor_display_t: public pipewire::pipewire_display_t {
  public:
    int configure_stream(const std::string &display_name, int &out_fd, uint32_t &out_node, uint64_t &out_serial) override {
      const auto state = current_state();
      if (!state || state->monitors.empty()) {
        return -1;
      }
      auto chosen = std::find_if(state->monitors.begin(), state->monitors.end(), [&](const auto &m) {
        return m.connector == display_name;
      });
      if (chosen == state->monitors.end()) {
        chosen = state->monitors.begin();  // The primary.
      }
      const char *cursor_control = std::getenv("METEOR_CURSOR_SCALE_FILE");
      const bool metadata = cursor_control && *cursor_control;
      pipewire.set_cursor_metadata(metadata);
      std::string why;
      if (!session.record_monitor(chosen->connector, metadata, why)) {
        BOOST_LOG(error) << "[mutter] Can't stream "sv << chosen->connector << ": "sv << why;
        return -1;
      }
      out_fd = -1;
      out_node = session.node;
      out_serial = SPA_ID_INVALID;
      offset_x = chosen->x;
      offset_y = chosen->y;
      width = chosen->width;
      height = chosen->height;
      logical_width = chosen->width;
      logical_height = chosen->height;
      int max_x = 0, max_y = 0;
      for (const auto &m : state->monitors) {
        max_x = std::max(max_x, m.x + m.width);
        max_y = std::max(max_y, m.y + m.height);
      }
      env_width = env_logical_width = max_x;
      env_height = env_logical_height = max_y;
      return 0;
    }

    void verify_and_update_display_parameters() override {}

    session_t session;
  };

  /// Several screens, each its own Mutter stream at its own rate, packed into the plan's frame.
  class packed_t: public pipewire_multi::multi_display_t {
  public:
    int init(platf::mem_type_e type, const display_plan::plan_t &plan, const std::vector<std::string> &virtual_connectors,
             const ::video::config_t &config) {
      const auto state = current_state();
      if (!state) {
        return -1;
      }
      const char *cursor_control = std::getenv("METEOR_CURSOR_SCALE_FILE");
      const bool metadata = cursor_control && *cursor_control;
      std::vector<pipewire_multi::screen_source_t> sources;
      for (const auto *screen : plan.enabled()) {
        std::string connector = screen->label;
        if (screen->is_virtual) {
          if (screen->virtual_index < 1 || screen->virtual_index > static_cast<int>(virtual_connectors.size())) {
            BOOST_LOG(error) << "[mutter] Virtual monitor "sv << screen->virtual_index << " doesn't exist"sv;
            return -1;
          }
          connector = virtual_connectors[screen->virtual_index - 1];
        }
        auto session = std::make_unique<session_t>();
        std::string why;
        if (!session->record_monitor(connector, metadata, why)) {
          BOOST_LOG(error) << "[mutter] Can't stream "sv << connector << " for the packed frame: "sv << why;
          return -1;
        }
        pipewire_multi::screen_source_t source;
        source.label = connector;
        source.node = session->node;
        source.frame_x = screen->frame_x;
        source.frame_y = screen->frame_y;
        source.width = screen->width;
        source.height = screen->height;
        for (const auto &m : state->monitors) {
          if (m.connector == connector) {
            source.desktop_x = m.x;
            source.desktop_y = m.y;
          }
        }
        sources.push_back(source);
        sessions.push_back(std::move(session));
      }
      BOOST_LOG(info) << "[mutter] Packing "sv << sources.size() << " screens into "sv << plan.frame_width << 'x' << plan.frame_height;
      return multi_display_t::init(type, std::move(sources), plan.frame_width, plan.frame_height, config);
    }

  private:
    std::vector<std::unique_ptr<session_t>> sessions;
  };

  /**
   * The session's virtual monitors: one RecordVirtual session each. Mutter makes the monitor
   * when a consumer first negotiates the stream (its size and maximum rate set the monitor's
   * mode), and keeps it until the session stops; so each is negotiated once here and released.
   */
  class virtual_owner_t {
  public:
    static virtual_owner_t &get() {
      static auto *instance = new virtual_owner_t;
      return *instance;
    }

    bool ensure(int count, int width, int height, int refresh_hz, std::string &error) {
      std::lock_guard lock {mutex};
      count = std::clamp(count, 0, platf::kwin_virtual::max_count);
      std::vector<std::string> removed;
      if (!monitors.empty() && (width != size_w || height != size_h || refresh_hz != size_hz)) {
        for (const auto &m : monitors) {
          removed.push_back(m->connector);
        }
        release_locked();
      }
      while (static_cast<int>(monitors.size()) > count) {
        removed.push_back(monitors.back()->connector);
        monitors.pop_back();
      }
      // Stopping a session removes its monitor some time after Stop returns. Reading or applying
      // the layout before then sees a monitor that is going away, and ApplyMonitorsConfig in that
      // window crashed gnome-shell 51 (meta_crtc_mode_get_info on the removed CRTC's mode).
      wait_until_removed(removed);
      size_w = width;
      size_h = height;
      size_hz = refresh_hz;
      while (static_cast<int>(monitors.size()) < count) {
        const auto before = current_state();
        auto entry = std::make_unique<entry_t>();
        if (!entry->session.record_virtual(error)) {
          return false;
        }
        if (!negotiate(entry->session.node, width, height, refresh_hz)) {
          error = "Mutter didn't make the virtual monitor";
          return false;
        }
        // The new connector is the one that wasn't there before.
        const auto after = current_state();
        if (before && after) {
          for (const auto &m : after->monitors) {
            const bool known = std::any_of(before->monitors.begin(), before->monitors.end(), [&](const auto &b) {
              return b.connector == m.connector;
            });
            if (!known) {
              entry->connector = m.connector;
            }
          }
        }
        if (entry->connector.empty()) {
          error = "Mutter's new virtual monitor didn't appear";
          return false;
        }
        monitors.push_back(std::move(entry));
      }
      BOOST_LOG(info) << "[mutter] "sv << count << " virtual monitor(s) of "sv << width << 'x' << height << " at "sv << refresh_hz << " Hz"sv;
      return true;
    }

    std::vector<std::string> connectors() {
      std::lock_guard lock {mutex};
      std::vector<std::string> names;
      for (const auto &m : monitors) {
        names.push_back(m->connector);
      }
      return names;
    }

    void release() {
      std::lock_guard lock {mutex};
      ++release_generation;
      release_locked();
    }

    void schedule_release(std::chrono::seconds delay) {
      const auto generation = ++release_generation;
      std::thread([this, delay, generation] {
        std::this_thread::sleep_for(delay);
        std::lock_guard lock {mutex};
        if (release_generation.load() == generation) {
          release_locked();
        }
      }).detach();
    }

    void cancel_scheduled_release() {
      ++release_generation;
    }

    int count() {
      std::lock_guard lock {mutex};
      return static_cast<int>(monitors.size());
    }

  private:
    struct entry_t {
      session_t session;
      std::string connector;
    };

    static void wait_until_removed(const std::vector<std::string> &connectors) {
      if (connectors.empty()) {
        return;
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (std::chrono::steady_clock::now() < deadline) {
        const auto state = current_state();
        const bool present = state && std::any_of(state->monitors.begin(), state->monitors.end(), [&](const auto &m) {
          return std::find(connectors.begin(), connectors.end(), m.connector) != connectors.end();
        });
        if (state && !present) {
          // Let Mutter finish the rebuild that follows the removal before the next request.
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      BOOST_LOG(warning) << "[mutter] Removed virtual monitors were still listed after 3 s"sv;
    }

    /// Connects once with the wanted size and maximum rate, which become the monitor's mode.
    static bool negotiate(uint32_t node, int width, int height, int refresh_hz) {
      pipewire_multi::screen_source_t source;
      source.label = "virtual";
      source.node = node;
      source.width = width;
      source.height = height;
      source.max_framerate = refresh_hz;
      pipewire_multi::source_display_t consumer {source};
      ::video::config_t config {};
      config.width = width;
      config.height = height;
      config.framerate = std::max(1, refresh_hz);
      return consumer.init(platf::mem_type_e::system, source.label, config) == 0;
    }

    void release_locked() {
      if (!monitors.empty()) {
        BOOST_LOG(info) << "[mutter] Removing "sv << monitors.size() << " virtual monitor(s)"sv;
      }
      monitors.clear();
    }

    std::mutex mutex;
    std::vector<std::unique_ptr<entry_t>> monitors;
    int size_w = 0, size_h = 0, size_hz = 0;
    std::atomic_uint64_t release_generation {0};
  };

  /// Physical monitors, in the form display_plan and kwin_virtual_layout take.
  std::vector<platf::output_descriptor_t> descriptors(const state_t &state, const std::vector<std::string> &virtual_connectors) {
    std::vector<platf::output_descriptor_t> out;
    for (const auto &m : state.monitors) {
      platf::output_descriptor_t d;
      d.name = m.connector;
      d.x = m.x;
      d.y = m.y;
      d.width = m.width;
      d.height = m.height;
      d.primary = m.primary;
      d.scale = m.scale;
      d.is_virtual = std::find(virtual_connectors.begin(), virtual_connectors.end(), m.connector) != virtual_connectors.end();
      out.push_back(d);
    }
    return out;
  }
}  // namespace mutter

namespace platf {
  bool mutter_available() {
    return mutter::available();
  }

  bool mutter_virtual_outputs_supported() {
    return mutter::available();
  }

  std::vector<std::string> mutter_display_names() {
    std::vector<std::string> names;
    if (const auto state = mutter::current_state()) {
      const auto virtuals = mutter::virtual_owner_t::get().connectors();
      for (const auto &m : state->monitors) {
        if (std::find(virtuals.begin(), virtuals.end(), m.connector) == virtuals.end()) {
          names.push_back(m.connector);
        }
      }
    }
    return names;
  }

  std::optional<display_plan::plan_t> mutter_make_plan(const std::string &outputs, int virtual_count) {
    const auto state = mutter::current_state();
    if (!state) {
      return std::nullopt;
    }
    const auto virtuals = mutter::virtual_owner_t::get().connectors();
    std::vector<display_plan::physical_t> physical;
    for (const auto &m : state->monitors) {
      if (std::find(virtuals.begin(), virtuals.end(), m.connector) == virtuals.end()) {
        physical.push_back({m.connector, m.x, m.y, m.width, m.height, m.primary});
      }
    }
    const bool virtual_only = outputs == capture_outputs::virtual_only_token;
    return display_plan::make(physical, virtual_only ? std::vector<std::string> {} : split_csv(outputs), virtual_count, virtual_only);
  }

  std::optional<std::string> mutter_prepare_virtual_outputs(const std::string &outputs, int count, int refresh_millihz, std::string &error) {
    auto &owner = mutter::virtual_owner_t::get();
    owner.cancel_scheduled_release();
    const bool virtual_only = outputs == capture_outputs::virtual_only_token;
    if (count <= 0 && !virtual_only) {
      if (owner.count() > 0) {
        owner.release();
      }
      return outputs;
    }
    const auto state = mutter::current_state();
    if (!state) {
      error = "Can't read the monitor layout from Mutter";
      return std::nullopt;
    }
    // Sized like the main monitor, as display_plan sizes virtual screens.
    const auto physical = mutter::descriptors(*state, owner.connectors());
    const auto layout = kwin_virtual::plan(physical, virtual_only ? std::vector<std::string> {} : split_csv(outputs), count, virtual_only, error, true);
    if (!layout) {
      return std::nullopt;
    }
    const auto row = std::find_if(layout->outputs.begin(), layout->outputs.end(), [](const auto &out) {
      return out.is_virtual;
    });
    const int hz = std::max(60, (refresh_millihz + 500) / 1000);
    if (row == layout->outputs.end() || !owner.ensure(count, row->width, row->height, hz, error)) {
      return std::nullopt;
    }
    // Put the monitors where the layout wants them (Mutter adds them at the right edge already).
    const auto connectors = owner.connectors();
    std::vector<std::pair<std::string, std::pair<int, int>>> positions;
    int index = 0;
    for (const auto &out : layout->outputs) {
      if (out.is_virtual && index < static_cast<int>(connectors.size())) {
        positions.push_back({connectors[index++], {out.x, out.y}});
      }
    }
    std::string place_error;
    if (!mutter::place(positions, place_error)) {
      BOOST_LOG(warning) << "[mutter] Couldn't place the virtual monitors: "sv << place_error;
    }
    std::vector<std::string> selected = virtual_only ? std::vector<std::string> {} : split_csv(outputs);
    selected.insert(selected.end(), connectors.begin(), connectors.end());
    return kwin_virtual::join(selected);
  }

  namespace {
    std::optional<desktop_manifest_t> mutter_manifest(const display_plan::plan_t &plan) {
      const auto state = mutter::current_state();
      const auto virtuals = mutter::virtual_owner_t::get().connectors();
      desktop_manifest_t manifest;
      manifest.frame_width = plan.frame_width;
      manifest.frame_height = plan.frame_height;
      manifest.tiled = true;
      manifest.any_selection = true;
      int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
      int virtual_x = 0;
      if (state) {
        for (const auto &m : state->monitors) {
          virtual_x = std::max(virtual_x, m.x + m.width);
        }
      }
      for (const auto &screen : plan.screens) {
        monitor_manifest_entry_t entry;
        entry.id = screen.id;
        entry.label = screen.is_virtual && screen.virtual_index <= static_cast<int>(virtuals.size()) ? virtuals[screen.virtual_index - 1] : screen.label;
        entry.enabled = screen.enabled;
        entry.is_primary = screen.is_primary;
        entry.is_virtual = screen.is_virtual;
        if (screen.enabled) {
          entry.frame_x = screen.frame_x;
          entry.frame_y = screen.frame_y;
          entry.frame_w = screen.width;
          entry.frame_h = screen.height;
        }
        entry.desktop_w = screen.width;
        entry.desktop_h = screen.height;
        bool found = false;
        if (state) {
          for (const auto &m : state->monitors) {
            if (m.connector == entry.label) {
              entry.desktop_x = m.x;
              entry.desktop_y = m.y;
              found = true;
            }
          }
        }
        if (!found && screen.is_virtual) {
          // Not made yet: where it will go, at the desktop's right edge.
          entry.desktop_x = virtual_x;
          virtual_x += screen.width;
        }
        min_x = std::min(min_x, entry.desktop_x);
        min_y = std::min(min_y, entry.desktop_y);
        max_x = std::max(max_x, entry.desktop_x + entry.desktop_w);
        max_y = std::max(max_y, entry.desktop_y + entry.desktop_h);
        manifest.monitors.push_back(entry);
      }
      if (manifest.monitors.empty()) {
        return std::nullopt;
      }
      manifest.desktop_x = min_x;
      manifest.desktop_y = min_y;
      manifest.desktop_width = max_x - min_x;
      manifest.desktop_height = max_y - min_y;
      return manifest;
    }
  }  // namespace

  std::optional<desktop_manifest_t> mutter_plan_manifest(const std::string &outputs, int count, bool virtual_only) {
    const auto plan = mutter_make_plan(virtual_only ? std::string {capture_outputs::virtual_only_token} : outputs, count);
    return plan ? mutter_manifest(*plan) : std::nullopt;
  }

  std::optional<desktop_manifest_t> mutter_session_manifest() {
    const auto plan = display_plan::get();
    return plan ? mutter_manifest(*plan) : std::nullopt;
  }

  void mutter_release_virtual_outputs() {
    mutter::virtual_owner_t::get().release();
  }

  void mutter_schedule_virtual_outputs_release(std::chrono::seconds delay) {
    auto &owner = mutter::virtual_owner_t::get();
    if (owner.count() > 0) {
      owner.schedule_release(delay);
    }
  }

  void mutter_cancel_virtual_outputs_release() {
    mutter::virtual_owner_t::get().cancel_scheduled_release();
  }

  std::shared_ptr<display_t> mutter_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      return nullptr;
    }
    if (const auto plan = display_plan::get(); plan && plan->enabled().size() > 1) {
      auto packed = std::make_shared<mutter::packed_t>();
      if (packed->init(hwdevice_type, *plan, mutter::virtual_owner_t::get().connectors(), config)) {
        BOOST_LOG(error) << "[mutter] Couldn't start the packed capture"sv;
        return nullptr;
      }
      return packed;
    }
    // One screen: the plan's, or the named monitor.
    std::string name = display_name;
    if (const auto plan = display_plan::get(); plan && plan->enabled().size() == 1) {
      const auto *screen = plan->enabled().front();
      if (screen->is_virtual) {
        const auto virtuals = mutter::virtual_owner_t::get().connectors();
        if (screen->virtual_index >= 1 && screen->virtual_index <= static_cast<int>(virtuals.size())) {
          name = virtuals[screen->virtual_index - 1];
        }
      } else {
        name = screen->label;
      }
    }
    auto display = std::make_shared<mutter::monitor_display_t>();
    if (display->init(hwdevice_type, name, config)) {
      return nullptr;
    }
    return display;
  }
}  // namespace platf
