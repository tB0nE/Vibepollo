/**
 * @file src/platform/linux/kwingrab.cpp
 * @brief KWin direct ScreenCast capture via zkde_screencast_unstable_v1 Wayland protocol.
 *
 * Bypasses xdg-desktop-portal entirely. Apollo connects directly to KWin's
 * Wayland protocol to obtain a PipeWire node_id, then streams frames via PipeWire.
 *
 * Chain: KWin -> Wayland kde_screencast -> PipeWire -> Apollo
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <pwd.h>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

// lib includes
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <nlohmann/json.hpp>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

// generated protocol header
#include <kde-output-order-v1.h>
#include <zkde-screencast-unstable-v1.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "kwin_virtual_layout.h"
#include "kwin_workspace.h"
#include "wayland_workspace.h"
#include "src/capture_outputs.h"
#include "pipewire.cpp"
#include "pipewire_multi.cpp"
#include "src/display_plan.h"
#include "private_display.h"
#include "private_display_capture_policy.h"
#include "src/platform/common.h"
#include "src/video.h"

using namespace std::literals;

namespace kwin {
  /**
   * KWin Wayland ScreenCast permissions
   *
   * To have access to zkde_screencast_unstable_v1 KWin checks for a .desktop file with
   * X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1 and the current executable name
   * in the Exec= parameter.
   */
  class screencast_permission_helper_t {
  public:
    static bool is_permission_system_deactivated() {
      return getenvstr("KWIN_WAYLAND_NO_PERMISSION_CHECKS") == "1";
    }

    static void setup() {
      if (initialized) {
        return;
      }
      auto filenameprefix = std::format("{}.kwin", PROJECT_FQDN);
      auto executablepath = get_executable_full_path();

      // System: Check system XDG applications for permission (usually installed with Sunshine)
      if (check_kwin_system_permissions(filenameprefix, executablepath)) {
        create_file = false;
        initialized = true;
        return;
      }

      // If we do not have a system permission, check if we need a temporary permission via user's application directory
      if (is_permission_system_deactivated()) {
        BOOST_LOG(info) << "[kwingrab] No permission desktop file necessary. KWin permission system deactivated.";
        create_file = false;
        initialized = true;
        return;
      }

      // User: Check and (if necessary) update user's XDG applications for permission
      auto user_applications = get_xdg_user_applications_path();
      if (user_applications.empty()) {
        BOOST_LOG(error) << "[kwingrab] Failed to determine user application directory. Cannot continue with permission setup.";
        return;
      }
      // Create non-existing application directory so we can write into it
      if (!std::filesystem::exists(user_applications) && !std::filesystem::create_directories(user_applications)) {
        // In case of failure log and return
        BOOST_LOG(error) << "[kwingrab] Failed to create application directory. Cannot continue with permission setup.";
        create_file = false;
        initialized = true;
        return;
      }
      auto user_filepathprefix = (std::filesystem::path(user_applications) / filenameprefix).string();
      for (const auto &path : std::filesystem::directory_iterator(user_applications)) {
        // List existing files for prefix and check if they contain this executable or remove them
        const auto entry = path.path().string();
        if (entry.starts_with(user_filepathprefix)) {
          auto entry_executablepath = get_executable_from_desktop_file(entry);
          if (!entry_executablepath.empty() && entry_executablepath == executablepath) {
            // This entry is exactly the one we need
            BOOST_LOG(debug) << "[kwingrab] Ignoring current temporary KWin wayland permission file: "sv << entry;
            create_file = false;
            continue;
          }
          if (!entry_executablepath.empty() && std::filesystem::exists(entry_executablepath)) {
            // This entry is for another sunshine executable that still exists
            BOOST_LOG(debug) << "[kwingrab] Ignoring other valid temporary KWin wayland permission file: "sv << entry;
            continue;
          }
          if (std::filesystem::remove(path)) {
            BOOST_LOG(info) << "[kwingrab] Removed stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          } else {
            BOOST_LOG(warning) << "[kwingrab] Failed to remove stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          }
        }
      }
      if (create_file) {
        // Generate a unique file identifier based on current unixtime
        auto user_filepathidentifier = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
        auto user_filepath = std::format("{}{}.desktop", user_filepathprefix, user_filepathidentifier);
        // Write new file if necessary
        std::ofstream filestream(user_filepath);
        if (filestream.is_open()) {
          filestream << "[Desktop Entry]" << std::endl
                     << "Exec=" << executablepath << std::endl
                     << "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1" << std::endl
                     << "Type=Application" << std::endl
                     << "Name="sv << PROJECT_FQDN << "-kwin-wayland-permission" << std::endl
                     << "Comment=Apollo KWin screencast permission" << std::endl
                     << "NoDisplay=true" << std::endl;
          filestream.close();
          // Give KWin time to catch up to the new desktop file
          BOOST_LOG(info) << "[kwingrab] Created temporary KWin wayland permission file: "sv << user_filepath << " - Waiting 3 seconds for KDE to pick up new file.";
          std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        } else {
          BOOST_LOG(warning) << "[kwingrab] Failed to open temporary KWin wayland permission file: "sv << user_filepath;
        }
      }

      initialized = true;
    }

    static bool is_newly_initialized() {
      return create_file;
    }

  private:
    static inline bool initialized = false;
    static inline bool create_file = true;

    static std::string getenvstr(std::string const &key) {
      char const *val = std::getenv(key.c_str());
      if (!val) {
        return "";
      }
      return val;
    }

    static std::filesystem::path get_home_dir() {
      // Check HOME environment variable
      if (std::string homedir = getenvstr("HOME"); !homedir.empty()) {
        return homedir;
      }
      // Fall back to home directory from NSS passwd
      // Note: This should be thread-safe as we're always accessing the same entry for Sunshine
      return getpwuid(geteuid())->pw_dir;
    }

    static std::filesystem::path get_xdg_user_applications_path() {
      // Follow the XDG base directory specification for user data home:
      // https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
      std::filesystem::path xdg_data_home;
      if (std::string dir = getenvstr("XDG_DATA_HOME"); !dir.empty()) {
        xdg_data_home = std::filesystem::path(dir);
      } else {
        const auto homedir = get_home_dir();
        if (homedir.empty()) {
          return "";
        }
        xdg_data_home = std::filesystem::path(homedir) / ".local"sv / "share"sv;
      }
      return xdg_data_home / "applications";
    }

    static std::string get_executable_full_path() {
      // Adapted from https://linuxvox.com/blog/how-do-i-find-the-location-of-the-executable-in-c/
      constexpr auto path_len = PATH_MAX;  // PATH_MAX is defined in limits.h (e.g., 4096 on Linux)
      auto path_exe = std::make_unique<char[]>(path_len);
      // Read the symlink /proc/self/exe into path_exe
      const ssize_t len = readlink("/proc/self/exe", &path_exe[0], path_len - 1);
      if (len == -1) {
        return "";
      }
      // Return path_exe as a proper std::string with len returned by readlink
      return std::string(path_exe.get(), len);
    }

    static std::string get_executable_from_desktop_file(const std::string &path) {
      if (std::ifstream file(path); file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
          if (line.starts_with("Exec=") && line.length() > 5) {
            return line.substr(5);
          }
        }
      }
      return "";
    }

    static bool check_kwin_system_permissions(const std::string_view &filenameprefix, const std::string_view &executablepath) {
      // Find data dirs to check from XDG_DATA_DIRS
      std::vector<std::string> xdg_data_dirs;
      if (const std::string e = getenvstr("XDG_DATA_DIRS"); !e.empty()) {
        std::stringstream ss(e);
        std::string item;

        while (getline(ss, item, ':')) {  // : is likely valid for all OSes supported, if a constant is available it should be used instead
          xdg_data_dirs.push_back(item);
        }
      }
      // Use defaults from https://specifications.freedesktop.org/basedir/latest/ if ENV var was empty
      if (xdg_data_dirs.empty()) {
        xdg_data_dirs.emplace_back("/usr/local/share/");
        xdg_data_dirs.emplace_back("/usr/share/");
      }
      // Check for ${filenameprefix}.desktop in each directory
      for (auto const &dir : xdg_data_dirs) {
        std::string filename = std::format("{0}{1}applications{1}{2}.desktop", dir, boost::filesystem::path::preferred_separator, filenameprefix);
        if (std::filesystem::exists(filename)) {
          auto file_executablepath = get_executable_from_desktop_file(filename);
          if (file_executablepath == executablepath) {
            BOOST_LOG(info) << "[kwingrab] Found matching system KWin desktop permission file: "sv << filename;
            return true;
          }
        }
      }
      return false;
    }
  };

  // Output parameters
  struct output_parameter_t {
    std::string name = "";
    int width = 0;
    int height = 0;
    int pos_x = 0;
    int pos_y = 0;
    // order is needed to get a sorted output list and should be updated before sorting to have current values
    size_t order = SIZE_MAX;  // Use high number to keep monitors with uninitialized order value to the back
  };

  /**
   * Wayland KDE ScreenCast session
   *
   * Owns its own wl_display connection. Binds zkde_screencast_unstable_v1
   * and wl_output from the registry, then calls stream_output() to start
   * a ScreenCast. Waits for the created(node_id) event from KWin.
   */
  class screencast_t {
  public:
    screencast_t &operator=(screencast_t &&) = delete;  // Do not allow to copying

    ~screencast_t() {
      if (kde_screencast_stream_v1_) {
        zkde_screencast_stream_unstable_v1_close(kde_screencast_stream_v1_);
        kde_screencast_stream_v1_ = nullptr;
      }
      if (kde_screencast_v1_) {
        zkde_screencast_unstable_v1_destroy(kde_screencast_v1_);
        kde_screencast_v1_ = nullptr;
      }

      if (kde_output_order) {
        kde_output_order_v1_destroy(kde_output_order);
        kde_output_order = nullptr;
      }

      // wl_output is owned by the registry, released on disconnect
      for (const auto &out : outputs | std::views::keys) {
        wl_output_destroy(out);
      }
      outputs.clear();

      if (wl_registry) {
        wl_registry_destroy(wl_registry);
        wl_registry = nullptr;
      }

      if (wl_display) {
        wl_display_disconnect(wl_display);
        wl_display = nullptr;
      }
    }

    /**
     * @brief Connect to KWin wayland, enumerate outputs.
     * @param setup_permissions - Try to setup KWin permissions (default: true)
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    int init(const bool setup_permissions = true) {
      if (setup_permissions) {
        // Try to set up permissions for zkde_screencast_unstable_v1
        screencast_permission_helper_t::setup();
      }

      const char *wl_name = std::getenv("WAYLAND_DISPLAY");
      if (!wl_name) {
        BOOST_LOG(error) << "[kwingrab] WAYLAND_DISPLAY not set"sv;
        return -1;
      }

      wl_display = wl_display_connect(wl_name);
      if (!wl_display) {
        BOOST_LOG(error) << "[kwingrab] cannot connect to Wayland display: "sv << wl_name;
        return -1;
      }

      wl_registry = wl_display_get_registry(wl_display);
      wl_registry_add_listener(wl_registry, &registry_listener, this);
      wl_display_roundtrip(wl_display);

      // We need a second roundtrip after binding outputs to get wl_output events
      wl_display_roundtrip(wl_display);

      return 0;
    }

    /**
     * @brief Generate a sorted list of known output names.
     * @return List of strings with output names to pass to start()
     */
    std::vector<std::string> get_output_names() {
      std::vector<std::shared_ptr<output_parameter_t>> sorted_outputs;
      for (const auto &output_parameter : outputs | std::views::values) {
        output_parameter->order = get_order_for_output_name(output_parameter->name);
        sorted_outputs.emplace_back(output_parameter);
      }
      std::ranges::sort(sorted_outputs, [](const auto &a, const auto &b) {
        return std::tie(a->order, a->pos_x, a->pos_y, a->name) < std::tie(b->order, b->pos_x, b->pos_y, b->name);
      });
      std::vector<std::string> output_names;
      for (const auto &output_parameter : sorted_outputs) {
        BOOST_LOG(info) << "[kwingrab] Found output: "sv << output_parameter->name << " order: "sv << output_parameter->order << " position: "sv << output_parameter->pos_x << "x"sv << output_parameter->pos_y << " resolution: "sv << output_parameter->width << "x"sv << output_parameter->height;
        output_names.emplace_back(output_parameter->name);
      }
      return output_names;
    }

    /**
     * @brief Check if KWin is available for potential screencasting
     * @return True if KWin is detected
     */
    bool kwin_available() const {
      // Detect KWin using kde_output_order_v1 extension
      if (kde_output_order) {
        return true;
      }
      return false;
    }

    /**
     * @brief Request a screencast stream.
     * @param output_name Which wl_output to capture.
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    bool supports_workspace() const {
      return kde_screencast_v1_ &&
        wl_proxy_get_version(reinterpret_cast<wl_proxy *>(kde_screencast_v1_)) >= 3;
    }

    bool supports_virtual_outputs() const {
      return kde_screencast_v1_ &&
        wl_proxy_get_version(reinterpret_cast<wl_proxy *>(kde_screencast_v1_)) >= 2;
    }

    int start(const std::string_view &output_name, const std::optional<platf::wayland_workspace_t> &workspace = std::nullopt) {
      if (workspace && !supports_workspace()) {
        BOOST_LOG(error) << "[kwingrab] Multi-monitor capture requires KWin stream_region v3"sv;
        return -1;
      }
      // Try find correct output by name
      if (outputs.empty()) {
        BOOST_LOG(error) << "[kwingrab] no wl_output found"sv;
        return -1;
      }
      struct wl_output *output = nullptr;
      if (!output_name.empty()) {
        for (auto const &[output_, params_] : outputs) {
          if (params_->name == output_name) {
            output = output_;
            out_params = params_;
          }
        }
      }
      // Fall back to first element from the map in case of error
      if (!output || !out_params) {
        const auto ordered = get_output_names();
        const auto preferred = std::find_if(outputs.begin(), outputs.end(), [&](const auto &entry) {
          return !ordered.empty() && entry.second->name == ordered.front();
        });
        const auto output_ = preferred == outputs.end() ? outputs.begin() : preferred;
        output = output_->first;
        out_params = output_->second;
      }

      // Meteor composites the cursor separately so scaling never changes KDE's
      // desktop cursor. Non-Meteor hosts retain embedded-cursor capture.
      const bool cursor_metadata = [] { const char *p = std::getenv("METEOR_CURSOR_SCALE_FILE"); return p && *p; }();
      const auto pointer_mode = cursor_metadata ? ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA : ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED;
      BOOST_LOG(info) << "[kwingrab] Cursor: " << (cursor_metadata ? "PipeWire metadata (stream-only scaling)" : "embedded");
      if (kde_screencast_v1_) {
        if (workspace) {
          kde_screencast_stream_v1_ = zkde_screencast_unstable_v1_stream_region(
            kde_screencast_v1_, workspace->capture_x, workspace->capture_y,
            workspace->manifest.frame_width, workspace->manifest.frame_height,
            wl_fixed_from_double(1.0), pointer_mode);
          out_params = std::make_shared<output_parameter_t>();
          out_params->name = "Nightfall workspace";
          out_params->pos_x = workspace->capture_x;
          out_params->pos_y = workspace->capture_y;
          out_params->width = workspace->manifest.frame_width;
          out_params->height = workspace->manifest.frame_height;
        } else {
          kde_screencast_stream_v1_ = zkde_screencast_unstable_v1_stream_output(kde_screencast_v1_, output, pointer_mode);
        }
        zkde_screencast_stream_unstable_v1_add_listener(kde_screencast_stream_v1_, &stream_listener, this);
      } else {
        // No screencast protocol found. Output an error based on newly initialized permission file.
        if (screencast_permission_helper_t::is_newly_initialized()) {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. "sv
                              "A new permission desktop file was automatically created but might now have been recognized yet. "sv
                              "Try restarting Apollo or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        } else {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. Check permission desktop file "sv
                              "for Apollo or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        }
        return -1;
      }

      if (wait_for_stream() < 0) {
        return -1;
      }

      if (stream_failed) {
        BOOST_LOG(error) << "[kwingrab] stream_output failed: "sv << stream_error_msg;
        return -1;
      }
      // Check for valid node_id and/or object serial values here, stream_ready is just an internal flag
      if (out_node_id == PW_ID_ANY && (out_objectserial & SPA_ID_INVALID) == SPA_ID_INVALID) {
        BOOST_LOG(error) << "[kwingrab] timeout waiting for created event"sv;
        return -1;
      }

      BOOST_LOG(info) << "[kwingrab] stream created, PipeWire node "sv << out_node_id;

      if (out_params->width == 0 || out_params->height == 0) {
        BOOST_LOG(error) << "[kwingrab] could not determine output dimensions"sv;
        return -1;
      }

      BOOST_LOG(info) << "[kwingrab] Screencasting output"sv
                      << " name "sv << out_params->name
                      << " position "sv << out_params->pos_x << "x"sv << out_params->pos_y
                      << " resolution "sv << out_params->width << "x"sv << out_params->height;
      return 0;
    }

    uint32_t out_node_id = PW_ID_ANY;
    uint64_t out_objectserial = SPA_ID_INVALID;
    std::shared_ptr<output_parameter_t> out_params = nullptr;

  private:
    // Wayland objects
    struct wl_display *wl_display = nullptr;
    struct wl_registry *wl_registry = nullptr;
    struct kde_output_order_v1 *kde_output_order = nullptr;
    struct zkde_screencast_unstable_v1 *kde_screencast_v1_ = nullptr;
    struct zkde_screencast_stream_unstable_v1 *kde_screencast_stream_v1_ = nullptr;
    std::map<struct wl_output *, std::shared_ptr<output_parameter_t>> outputs;
    std::vector<std::string> output_order;
    bool stream_failed = false;
    bool stream_ready = false;
    std::string stream_error_msg;

    // Misc functions
    int wait_for_stream() {
      // Dispatch until we get created/failed, with a 5s timeout
      auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!stream_ready && !stream_failed && std::chrono::steady_clock::now() < deadline) {
        wl_display_flush(wl_display);

        struct pollfd pfd = {};
        pfd.fd = wl_display_get_fd(wl_display);
        pfd.events = POLLIN;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()
        );
        if (remaining.count() <= 0) {
          break;
        }

        if (poll(&pfd, 1, remaining.count()) > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(wl_display) < 0) {
          BOOST_LOG(error) << "[kwingrab] wl_display_dispatch failed"sv;
          return -1;
        }
      }
      return 0;
    }

    size_t get_order_for_output_name(const std::string_view &name) const {
      for (size_t i = 0; i < output_order.size(); i++) {
        if (output_order[i] == name) {
          return i;
        }
      }
      // If nothing matches return list size (to ensure highest order)
      return output_order.size();
    }

    // Registry listener
    static void on_registry_global(void *data, struct wl_registry *reg, const uint32_t name, const char *interface, const uint32_t version) {
      auto *self = static_cast<screencast_t *>(data);
      if (!std::strcmp(interface, kde_output_order_v1_interface.name)) {
        // Bind version 1
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(1));
        self->kde_output_order = static_cast<struct kde_output_order_v1 *>(
          wl_registry_bind(reg, name, &kde_output_order_v1_interface, bind_ver)
        );
        kde_output_order_v1_add_listener(self->kde_output_order, &output_order_listener, self);
        BOOST_LOG(debug) << "[kwingrab] bound kde_output_order_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, zkde_screencast_unstable_v1_interface.name)) {
        // Bind version 1 to 6 — We use stream_output from v1 for node_id (deprecated but good as a fall-back)
        //                       but also try to get the newer (re-use safe) pipewire objectserial from v6
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(6));
        self->kde_screencast_v1_ = static_cast<struct zkde_screencast_unstable_v1 *>(
          wl_registry_bind(reg, name, &zkde_screencast_unstable_v1_interface, bind_ver)
        );
        BOOST_LOG(debug) << "[kwingrab] bound zkde_screencast_unstable_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, wl_output_interface.name)) {
        // Bind version 4 - we need wl_output name for matching
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(4));
        auto *output = static_cast<struct wl_output *>(
          wl_registry_bind(reg, name, &wl_output_interface, bind_ver)
        );

        const auto [_, inserted] = self->outputs.try_emplace(output, std::make_shared<output_parameter_t>());
        if (inserted) {
          wl_output_add_listener(output, &output_listener, self);
          BOOST_LOG(debug) << "[kwingrab] bound wl_output version "sv << bind_ver << " instance: "sv << output;
        } else {
          // If we for some odd reason cannot add the output to the map clean it up and log a warning
          BOOST_LOG(warning) << "[kwingrab] Ignoring output "sv << output << " because map emplace failed."sv;
          wl_output_destroy(output);
        }
      }
    }

    static void on_registry_global_remove(void *data [[maybe_unused]], struct wl_registry *reg [[maybe_unused]], uint32_t name [[maybe_unused]]) {
      // We don't handle output hot-unplug during init
    }

    static constexpr struct wl_registry_listener registry_listener = {
      .global = on_registry_global,
      .global_remove = on_registry_global_remove,
    };

    // wl_output listener (for mode/dimensions/name)
    static void on_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t pw [[maybe_unused]], int32_t ph [[maybe_unused]], int32_t subpixel [[maybe_unused]], const char *make [[maybe_unused]], const char *model [[maybe_unused]], int32_t transform [[maybe_unused]]) {
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->pos_x = x;
      output_parameter->pos_y = y;
    }

    static void on_output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh [[maybe_unused]]) {
      if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
        return;
      }
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->width = width;
      output_parameter->height = height;
    }

    static void on_output_done(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]]) {
      // Currently unused
    }

    static void on_output_scale(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]], int32_t factor [[maybe_unused]]) {
      // Currently unused
    }

    static void on_output_name(void *data, struct wl_output *output, const char *name) {
      const auto *self = static_cast<screencast_t *>(data);
      self->outputs.at(output)->name = name;
    }

    static void on_output_description(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]], const char *description [[maybe_unused]]) {
      // Currently unused
    }

    static constexpr struct wl_output_listener output_listener = {
      .geometry = on_output_geometry,
      .mode = on_output_mode,
      .done = on_output_done,
      .scale = on_output_scale,
      .name = on_output_name,
      .description = on_output_description,
    };

    // Output order listener
    static void on_output_order_output(void *data, struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]], const char *output_name) {
      auto *self = static_cast<screencast_t *>(data);
      self->output_order.emplace_back(output_name);
    }

    static void on_output_order_done(void *data [[maybe_unused]], struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]]) {
      // Currently unused
    }

    static constexpr kde_output_order_v1_listener output_order_listener = {
      .output = on_output_order_output,
      .done = on_output_order_done,
    };

    // ScreenCast v1 stream listener
    static void on_stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]]) {
      auto *self = static_cast<screencast_t *>(data);
      BOOST_LOG(warning) << "[kwingrab] stream closed by server"sv;
      self->stream_failed = false;
      self->stream_ready = false;
      self->stream_error_msg = "stream closed by server";
    }

    static void on_stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const uint32_t node) {
      auto *self = static_cast<screencast_t *>(data);
      self->out_node_id = node;
      self->stream_failed = false;
      self->stream_ready = true;
      BOOST_LOG(debug) << "[kwingrab] created event, node_id="sv << node;
    }

    static void on_stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const char *err_msg) {
      auto *self = static_cast<screencast_t *>(data);
      self->stream_failed = true;
      self->stream_ready = false;
      self->stream_error_msg = err_msg ? err_msg : "unknown error";
      BOOST_LOG(error) << "[kwingrab] failed event: "sv << self->stream_error_msg;
    }

    static void on_stream_serial(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], uint32_t object_serial_hi, uint32_t object_serial_low) {
      auto *self = static_cast<screencast_t *>(data);
      self->out_objectserial = static_cast<uint64_t>(object_serial_hi) << 32 | object_serial_low;
      // serial event always preceded the created event with the node id, so we only set stream_ready in created for v1
      BOOST_LOG(debug) << "[kwingrab] serial event, objectserial="sv << self->out_objectserial;
    }

    static constexpr struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
      .closed = on_stream_closed,
      .created = on_stream_created,
      .failed = on_stream_failed,
      .serial = on_stream_serial,
    };
  };

  /// The desktop's outputs in KWin's order, in logical coordinates; the first is the primary.
  std::optional<std::vector<platf::output_descriptor_t>> output_descriptors(screencast_t &screencast) {
    const auto order = screencast.get_output_names();
    const auto monitors = wl::monitors();
    std::vector<platf::output_descriptor_t> outputs;
    for (const auto &name : order) {
      const auto match = std::find_if(monitors.begin(), monitors.end(), [&](const auto &monitor) {
        return monitor->name == name;
      });
      if (match == monitors.end()) {
        // Partial geometry cannot describe the whole desktop's input extent safely.
        return std::nullopt;
      }
      const auto &viewport = (*match)->viewport;
      outputs.push_back({name, viewport.offset_x, viewport.offset_y,
        viewport.logical_width, viewport.logical_height, outputs.empty(), true});
    }
    return outputs;
  }

  std::optional<platf::wayland_workspace_t> workspace_layout(
    screencast_t &screencast, const std::vector<std::string> &selected
  ) {
    auto outputs = output_descriptors(screencast);
    if (!outputs) {
      return std::nullopt;
    }
    platf::kwin_virtual::mark_virtual(*outputs, selected);
    return platf::make_wayland_workspace(*outputs, selected);
  }

  /**
   * Meteor's host depth covers the primary monitor only. When several monitors share the
   * frame, publish where the primary is (frame size, then x y w h) at the path Meteor names,
   * and remove the file when one monitor fills the frame. Same format as on Windows.
   */
  void publish_capture_region(const std::optional<platf::wayland_workspace_t> &workspace) {
    const char *path = std::getenv("METEOR_CAPTURE_REGION_FILE");
    if (!path || !*path) {
      return;
    }
    std::error_code ignored;
    const platf::monitor_manifest_entry_t *primary = nullptr;
    int enabled = 0;
    if (workspace) {
      for (const auto &m : workspace->manifest.monitors) {
        if (!m.enabled) {
          continue;
        }
        ++enabled;
        if (m.is_primary && !primary) {
          primary = &m;
        }
      }
    }
    if (enabled < 2 || !primary) {
      std::filesystem::remove(path, ignored);
      return;
    }
    std::ofstream out {path, std::ios::trunc};
    out << workspace->manifest.frame_width << ' ' << workspace->manifest.frame_height << ' ' << primary->frame_x << ' '
        << primary->frame_y << ' ' << primary->frame_w << ' ' << primary->frame_h << '\n';
  }

  /**
   * The session's virtual monitors.
   *
   * KWin keeps an output made by stream_virtual_output while that stream is open, so this owns
   * the streams on its own Wayland connection and thread, apart from capture: capture can
   * restart without the monitors (and the windows on them) going away. Nothing reads these
   * streams; capture streams the outputs like any other.
   */
  class virtual_outputs_t {
  public:
    /// Lives for the whole process; KWin drops the monitors when the host exits.
    static virtual_outputs_t &get() {
      static auto *instance = new virtual_outputs_t;
      return *instance;
    }

    /// Makes exactly `count` virtual monitors of `width` x `height` exist.
    bool ensure(int count, int width, int height, std::string &error) {
      return run([&] {
        return do_ensure(count, width, height, error);
      });
    }

    void release() {
      cancel_scheduled_release();
      run([&] {
        close_all();
        return true;
      });
    }

    int count() {
      std::lock_guard lock {state_mutex};
      return static_cast<int>(streams.size());
    }

    void schedule_release(std::chrono::seconds delay) {
      const auto generation = ++release_generation;
      std::thread([this, delay, generation] {
        std::this_thread::sleep_for(delay);
        if (release_generation.load() != generation) {
          return;
        }
        BOOST_LOG(info) << "[kwin-virtual] Removing virtual monitors after "sv << delay.count() << " s without a stream"sv;
        run([&] {
          close_all();
          return true;
        });
      }).detach();
    }

    void cancel_scheduled_release() {
      ++release_generation;
    }

  private:
    struct stream_t {
      zkde_screencast_stream_unstable_v1 *stream = nullptr;
      bool created = false;
      bool failed = false;
      bool closed = false;
      std::string error;
    };

    template<class F>
    bool run(F &&work) {
      std::unique_lock lock {work_mutex};
      if (std::this_thread::get_id() == worker_id.load()) {
        return work();
      }
      start_worker();
      // One job at a time: wait for the worker to take the previous caller's.
      work_done.wait(lock, [&] {
        return !pending || !worker_running;
      });
      bool done = false;
      bool result = false;
      pending = [&] {
        result = work();
        done = true;
      };
      wake();
      work_done.wait(lock, [&] {
        return done || !worker_running;
      });
      return result;
    }

    void start_worker() {
      if (worker_running) {
        return;
      }
      if (worker.joinable()) {
        worker.join();
      }
      if (wake_fds[0] < 0 && pipe(wake_fds) != 0) {
        wake_fds[0] = wake_fds[1] = -1;
      }
      worker_running = true;
      worker = std::thread([this] {
        worker_id = std::this_thread::get_id();
        loop();
      });
    }

    void wake() {
      if (wake_fds[1] >= 0) {
        const char byte = 1;
        (void) !write(wake_fds[1], &byte, 1);
      }
    }

    void loop() {
      while (true) {
        std::function<void()> job;
        {
          std::unique_lock lock {work_mutex};
          if (quitting) {
            break;
          }
          job = std::move(pending);
          pending = nullptr;
          if (job) {
            work_done.notify_all();
          }
        }
        if (job) {
          job();
          std::lock_guard lock {work_mutex};
          work_done.notify_all();
          continue;
        }
        pump(250);
      }
      close_all();
      disconnect();
      std::lock_guard lock {work_mutex};
      worker_running = false;
      work_done.notify_all();
    }

    /// Reads KWin's events for up to `timeout_ms`, waking early for new work.
    void pump(int timeout_ms) {
      std::array<pollfd, 2> fds {};
      int count = 0;
      fds[count++] = {wake_fds[0], POLLIN, 0};
      bool reading = false;
      if (display) {
        while (wl_display_prepare_read(display) != 0) {
          wl_display_dispatch_pending(display);
        }
        reading = true;
        wl_display_flush(display);
        fds[count++] = {wl_display_get_fd(display), POLLIN, 0};
      }
      poll(fds.data(), count, timeout_ms);
      if (fds[0].revents & POLLIN) {
        char buf[64];
        (void) !read(wake_fds[0], buf, sizeof buf);
      }
      if (reading) {
        if (count > 1 && (fds[1].revents & POLLIN)) {
          wl_display_read_events(display);
        } else {
          wl_display_cancel_read(display);
        }
        if (wl_display_dispatch_pending(display) < 0) {
          BOOST_LOG(warning) << "[kwin-virtual] Lost the KWin connection; the virtual monitors are gone"sv;
          drop_connection();
        }
      }
    }

    bool connect(std::string &error) {
      if (display) {
        return true;
      }
      screencast_permission_helper_t::setup();
      const char *wl_name = std::getenv("WAYLAND_DISPLAY");
      display = wl_name ? wl_display_connect(wl_name) : nullptr;
      if (!display) {
        error = "Can't connect to KWin to make virtual monitors";
        return false;
      }
      registry = wl_display_get_registry(display);
      wl_registry_add_listener(registry, &registry_listener, this);
      wl_display_roundtrip(display);
      if (!screencast || screencast_version < 2) {
        error = "This KWin can't make virtual monitors (needs zkde_screencast v2)";
        disconnect();
        return false;
      }
      return true;
    }

    bool do_ensure(int count, int width, int height, std::string &error) {
      count = std::clamp(count, 0, platf::kwin_virtual::max_count);
      std::lock_guard lock {state_mutex};
      std::erase_if(streams, [](const auto &s) {
        return s->closed || s->failed;
      });
      if (!streams.empty() && (width != size_w || height != size_h)) {
        // KWin can't resize a virtual output; make them again at the new size.
        close_locked();
      }
      while (static_cast<int>(streams.size()) > count) {
        close_stream(*streams.back());
        streams.pop_back();
      }
      if (static_cast<int>(streams.size()) == count) {
        return true;
      }
      if (!connect(error)) {
        return false;
      }
      size_w = width;
      size_h = height;
      for (int index = static_cast<int>(streams.size()) + 1; index <= count; ++index) {
        auto entry = std::make_unique<stream_t>();
        const auto name = platf::kwin_virtual::requested_name(index);
        const auto description = "Nightfall virtual monitor " + std::to_string(index);
        const auto scale = wl_fixed_from_double(1.0);
        entry->stream = screencast_version >= 4 ?
                          zkde_screencast_unstable_v1_stream_virtual_output_with_description(
                            screencast, name.c_str(), description.c_str(), width, height, scale, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN
                          ) :
                          zkde_screencast_unstable_v1_stream_virtual_output(
                            screencast, name.c_str(), width, height, scale, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN
                          );
        zkde_screencast_stream_unstable_v1_add_listener(entry->stream, &stream_listener, entry.get());
        streams.push_back(std::move(entry));
      }
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        wl_display_roundtrip(display);
        const bool settled = std::all_of(streams.begin(), streams.end(), [](const auto &s) {
          return s->created || s->failed || s->closed;
        });
        if (settled) {
          break;
        }
        std::this_thread::sleep_for(20ms);
      }
      for (const auto &s : streams) {
        if (!s->created) {
          error = s->error.empty() ? "KWin didn't make the virtual monitor in time" : "KWin refused a virtual monitor: " + s->error;
          close_locked();
          return false;
        }
      }
      BOOST_LOG(info) << "[kwin-virtual] "sv << count << " virtual monitor(s) of "sv << width << 'x' << height;
      return true;
    }

    void close_stream(stream_t &s) {
      if (s.stream) {
        zkde_screencast_stream_unstable_v1_close(s.stream);
        s.stream = nullptr;
      }
    }

    void close_locked() {
      for (auto &s : streams) {
        close_stream(*s);
      }
      if (!streams.empty() && display) {
        wl_display_roundtrip(display);
        BOOST_LOG(info) << "[kwin-virtual] Removed "sv << streams.size() << " virtual monitor(s)"sv;
      }
      streams.clear();
    }

    void close_all() {
      std::lock_guard lock {state_mutex};
      close_locked();
    }

    void drop_connection() {
      std::lock_guard lock {state_mutex};
      streams.clear();
      disconnect();
    }

    void disconnect() {
      if (screencast) {
        zkde_screencast_unstable_v1_destroy(screencast);
        screencast = nullptr;
      }
      if (registry) {
        wl_registry_destroy(registry);
        registry = nullptr;
      }
      if (display) {
        wl_display_disconnect(display);
        display = nullptr;
      }
    }

    static void on_global(void *data, wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
      auto *self = static_cast<virtual_outputs_t *>(data);
      if (!std::strcmp(interface, zkde_screencast_unstable_v1_interface.name) && !self->screencast) {
        self->screencast_version = std::min(version, 6u);
        self->screencast = static_cast<zkde_screencast_unstable_v1 *>(
          wl_registry_bind(registry, id, &zkde_screencast_unstable_v1_interface, self->screencast_version)
        );
      }
    }

    static void on_global_remove(void *, wl_registry *, uint32_t) {}

    static void on_closed(void *data, zkde_screencast_stream_unstable_v1 *) {
      static_cast<stream_t *>(data)->closed = true;
      BOOST_LOG(warning) << "[kwin-virtual] KWin closed a virtual monitor"sv;
    }

    static void on_created(void *data, zkde_screencast_stream_unstable_v1 *, uint32_t) {
      static_cast<stream_t *>(data)->created = true;
    }

    static void on_failed(void *data, zkde_screencast_stream_unstable_v1 *, const char *message) {
      auto *s = static_cast<stream_t *>(data);
      s->failed = true;
      s->error = message ? message : "unknown error";
    }

    static void on_serial(void *data, zkde_screencast_stream_unstable_v1 *, uint32_t, uint32_t) {
      static_cast<stream_t *>(data)->created = true;
    }

    static constexpr wl_registry_listener registry_listener {on_global, on_global_remove};
    static constexpr zkde_screencast_stream_unstable_v1_listener stream_listener {on_closed, on_created, on_failed, on_serial};

    std::mutex work_mutex;
    std::condition_variable work_done;
    std::function<void()> pending;
    std::thread worker;
    std::atomic<std::thread::id> worker_id;
    bool worker_running = false;
    bool quitting = false;
    int wake_fds[2] {-1, -1};

    std::mutex state_mutex;
    std::vector<std::unique_ptr<stream_t>> streams;
    int size_w = 0, size_h = 0;
    std::atomic_uint64_t release_generation {0};

    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    zkde_screencast_unstable_v1 *screencast = nullptr;
    uint32_t screencast_version = 0;
  };

  /// Runs kscreen-doctor with `args`; its standard output in `out` when given.
  bool run_kscreen_doctor(const std::vector<std::string> &args, std::string *out = nullptr) {
    std::vector<std::string> owned {"kscreen-doctor"};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &arg : owned) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    int pipe_fds[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (out) {
      if (pipe(pipe_fds) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return false;
      }
      posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
      posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    }
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, "kscreen-doctor", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (out) {
      close(pipe_fds[1]);
      if (spawned == 0) {
        char buf[4096];
        for (ssize_t n; (n = read(pipe_fds[0], buf, sizeof buf)) > 0;) {
          out->append(buf, static_cast<std::size_t>(n));
        }
      }
      close(pipe_fds[0]);
    }
    if (spawned != 0) {
      return false;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  /**
   * KWin makes virtual monitors at 60 Hz, and KWin renders a capture of virtual monitors alone
   * at their rate: a 120 fps stream of them stutters. Give each the stream's rate, adding a
   * custom mode the first time (KWin remembers it for that output name).
   */
  void set_virtual_refresh(const std::vector<platf::output_descriptor_t> &outputs, int refresh_millihz) {
    if (refresh_millihz <= 0) {
      return;
    }
    const double wanted_hz = refresh_millihz / 1000.0;
    const auto read_modes = [] {
      std::string json_text;
      run_kscreen_doctor({"-j"}, &json_text);
      return nlohmann::json::parse(json_text, nullptr, false);
    };
    // The mode of `out` closest to the wanted rate within 1 Hz, and whether it's current.
    const auto find_mode = [&](const nlohmann::json &doc, const platf::output_descriptor_t &out) -> std::pair<std::string, bool> {
      if (!doc.is_object() || !doc.contains("outputs")) {
        return {"", false};
      }
      for (const auto &o : doc["outputs"]) {
        if (o.value("name", "") != out.name || !o.contains("modes")) {
          continue;
        }
        const std::string current = o.contains("currentModeId") ? o["currentModeId"].get<std::string>() : "";
        for (const auto &m : o["modes"]) {
          if (!m.contains("size") || m["size"].value("width", 0) != out.width || m["size"].value("height", 0) != out.height) {
            continue;
          }
          if (std::abs(m.value("refreshRate", 0.0) - wanted_hz) < 1.0) {
            const std::string id = m["id"].get<std::string>();
            return {id, id == current};
          }
        }
      }
      return {"", false};
    };
    auto doc = read_modes();
    std::vector<std::string> add, select;
    for (const auto &out : outputs) {
      if (!out.is_virtual) {
        continue;
      }
      if (find_mode(doc, out).first.empty()) {
        add.push_back("output." + out.name + ".addCustomMode." + std::to_string(out.width) + "." + std::to_string(out.height) + "." +
                      std::to_string(refresh_millihz) + ".full");
      }
    }
    if (!add.empty()) {
      run_kscreen_doctor(add);
      doc = read_modes();
    }
    for (const auto &out : outputs) {
      if (!out.is_virtual) {
        continue;
      }
      const auto [id, current] = find_mode(doc, out);
      if (id.empty()) {
        BOOST_LOG(warning) << "[kwin-virtual] "sv << out.name << " has no " << wanted_hz << " Hz mode; it stays at its default rate"sv;
      } else if (!current) {
        select.push_back("output." + out.name + ".mode." + id);
      }
    }
    if (!select.empty()) {
      run_kscreen_doctor(select);
      BOOST_LOG(info) << "[kwin-virtual] Virtual monitors set to "sv << wanted_hz << " Hz"sv;
    }
  }

  /// Moves the virtual monitors to `wanted` with kscreen-doctor, and waits until KWin reports them there.
  bool place_virtual_outputs(const std::vector<platf::output_descriptor_t> &wanted, std::string &error) {
    std::vector<std::string> args {"kscreen-doctor"};
    for (const auto &out : wanted) {
      if (out.is_virtual) {
        args.push_back("output." + out.name + ".position." + std::to_string(out.x) + "," + std::to_string(out.y));
      }
    }
    const auto at_place = [&] {
      screencast_t screencast;
      if (screencast.init(false) < 0) {
        return false;
      }
      const auto outputs = output_descriptors(screencast);
      if (!outputs) {
        return false;
      }
      return std::all_of(wanted.begin(), wanted.end(), [&](const auto &want) {
        if (!want.is_virtual) {
          return true;
        }
        return std::any_of(outputs->begin(), outputs->end(), [&](const auto &have) {
          return have.name == want.name && have.x == want.x && have.y == want.y;
        });
      });
    };
    // A new output can take a moment to reach KScreen.
    for (int attempt = 0; attempt < 10; ++attempt) {
      if (at_place()) {
        return true;
      }
      if (attempt % 3 == 0) {
        std::vector<char *> argv;
        for (auto &arg : args) {
          argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawnp(&pid, "kscreen-doctor", nullptr, nullptr, argv.data(), environ) != 0) {
          error = "kscreen-doctor is needed to place virtual monitors";
          return false;
        }
        int status = 0;
        waitpid(pid, &status, 0);
      }
      std::this_thread::sleep_for(300ms);
    }
    error = "KWin didn't move the virtual monitors where the stream expects them";
    return false;
  }

  /// The output a plan screen streams: its own name, or the virtual monitor KWin made for it.
  std::string plan_output_name(const display_plan::screen_t &screen) {
    return screen.is_virtual ? platf::kwin_virtual::output_name(screen.virtual_index) : screen.label;
  }

  /**
   * Several screens, each its own KWin stream, packed into the plan's frame. KWin streams one
   * output at that output's rate, so any set of monitors and virtual monitors keeps full rate.
   */
  class packed_t: public pipewire_multi::multi_display_t {
  public:
    int init(platf::mem_type_e type, const display_plan::plan_t &plan, const ::video::config_t &config) {
      screencast_t probe;
      if (probe.init(true) < 0) {
        return -1;
      }
      const auto descriptors = output_descriptors(probe).value_or(std::vector<platf::output_descriptor_t> {});
      std::vector<pipewire_multi::screen_source_t> sources;
      for (const auto *screen : plan.enabled()) {
        const auto name = plan_output_name(*screen);
        auto cast = std::make_unique<screencast_t>();
        if (cast->init(false) < 0 || cast->start(name) < 0 || !cast->out_params || cast->out_params->name != name) {
          BOOST_LOG(error) << "[kwingrab] Can't stream output "sv << name << " for the packed frame"sv;
          return -1;
        }
        pipewire_multi::screen_source_t source;
        source.label = name;
        source.node = cast->out_node_id;
        source.serial = cast->out_objectserial;
        source.frame_x = screen->frame_x;
        source.frame_y = screen->frame_y;
        source.width = screen->width;
        source.height = screen->height;
        source.frame_w = screen->frame_w;
        source.frame_h = screen->frame_h;
        source.is_primary = screen->is_primary;
        const auto at = std::find_if(descriptors.begin(), descriptors.end(), [&](const auto &out) {
          return out.name == name;
        });
        source.desktop_x = at != descriptors.end() ? at->x : cast->out_params->pos_x;
        source.desktop_y = at != descriptors.end() ? at->y : cast->out_params->pos_y;
        sources.push_back(source);
        casts.push_back(std::move(cast));
      }
      BOOST_LOG(info) << "[kwingrab] Packing "sv << sources.size() << " screens into "sv << plan.frame_width << 'x' << plan.frame_height;
      return multi_display_t::init(type, std::move(sources), plan.frame_width, plan.frame_height, config);
    }

  private:
    // Each holds its output's KWin stream open for as long as capture runs.
    std::vector<std::unique_ptr<screencast_t>> casts;
  };

  /**
   * Display backend
   *
   * Orchestrates screencast_t and implements pipewire_display_t
   */
  class kwin_t: public pipewire::pipewire_display_t {
  public:
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      screencast = std::make_unique<screencast_t>();
      if (screencast->init(true) < 0) {
        return -1;
      }
      auto selected = platf::split_csv(capture_outputs::get());
      const bool explicit_selection = !selected.empty();
      if (selected.empty() && !platf::linux_private_display::is_private_output(display_name)) {
        auto name = display_name;
        const auto names = screencast->get_output_names();
        if (name.empty() && !names.empty()) {
          name = names.front();
        }
        selected.push_back(name);
      }
      if (!selected.empty()) {
        workspace = workspace_layout(*screencast, selected);
        if (!workspace && explicit_selection) {
          BOOST_LOG(error) << "[kwingrab] Invalid, incomplete or oversized multi-monitor selection"sv;
          return -1;
        }
      }
      publish_capture_region(workspace);
      const char *cursor_control = std::getenv("METEOR_CURSOR_SCALE_FILE");
      pipewire.set_cursor_metadata(cursor_control && *cursor_control);
      if (screencast->start(display_name, workspace) < 0) {
        return -1;
      }
      if (screencast->out_params) {
        // Return values for pipewire init
        out_pipewire_fd = -1;  // KWin screencast capture runs on the local pipewire core
        out_pipewire_node = screencast->out_node_id;
        out_pipewire_objectserial = screencast->out_objectserial;
        // Set/update basic stream parameters on display_t
        this->offset_x = screencast->out_params->pos_x;
        this->offset_y = screencast->out_params->pos_y;
        this->width = screencast->out_params->width;
        this->height = screencast->out_params->height;
        this->logical_width = 0;  // Explicitly mark for pipewire_display_t to try to figure this out.
        this->logical_height = 0;  // Explicitly Mark for pipewire_display_t to try to figure this out.
        if (workspace) {
          apply_workspace_input();
        }
        return 0;
      }
      return -1;
    }

    int init(platf::mem_type_e type, const std::string &name, const video::config_t &config) {
      if (pipewire_display_t::init(type, name, config)) {
        return -1;
      }
      if (workspace && !platf::wayland_workspace_buffer_matches(*workspace, width, height)) {
        BOOST_LOG(error) << "[kwingrab] Workspace PipeWire size disagrees with logical monitor layout"sv;
        return -1;
      }
      return 0;
    }

    void verify_and_update_display_parameters() override {
      if (workspace) {
        apply_workspace_input();
      } else {
        pipewire_display_t::verify_and_update_display_parameters();
      }
    }

    void apply_workspace_input() {
      offset_x = workspace->input_x;
      offset_y = workspace->input_y;
      logical_width = workspace->manifest.frame_width;
      logical_height = workspace->manifest.frame_height;
      env_width = env_logical_width = workspace->manifest.desktop_width;
      env_height = env_logical_height = workspace->manifest.desktop_height;
    }

    std::optional<platf::wayland_workspace_t> workspace;
    std::unique_ptr<screencast_t> screencast;
  };
}  // namespace kwin

// Public API for misc.cpp
namespace platf {
  std::shared_ptr<display_t> kwin_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[kwingrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    // A session of several screens packs each screen's own stream (see packed_t).
    if (const auto plan = display_plan::get(); plan && plan->enabled().size() > 1) {
      auto packed = std::make_shared<kwin::packed_t>();
      if (packed->init(hwdevice_type, *plan, config)) {
        BOOST_LOG(error) << "[kwingrab] Couldn't start the packed capture"sv;
        return nullptr;
      }
      return packed;
    }

    auto display = std::make_shared<kwin::kwin_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return display;
  }

  std::optional<desktop_manifest_t> kwin_query_desktop_manifest(const std::string &outputs) {
    if (window_system != window_system_e::WAYLAND ||
        (!config::video.capture.empty() && config::video.capture != "kwin")) {
      return std::nullopt;
    }
    kwin::screencast_t screencast;
    if (screencast.init(false) < 0 || !screencast.kwin_available() || !screencast.supports_workspace()) {
      return std::nullopt;
    }
    auto selected = split_csv(outputs);
    if (selected.empty()) {
      const auto names = screencast.get_output_names();
      auto name = config::get_active_output_name();
      if (linux_private_display::is_private_output(name)) {
        // Private displays retain their existing physical-resolution capture path.
        return std::nullopt;
      }
      if (name.empty() && !names.empty()) {
        name = names.front();
      }
      selected.push_back(name);
    }
    const auto workspace = kwin::workspace_layout(screencast, selected);
    return workspace ? std::optional(workspace->manifest) : std::nullopt;
  }

  std::optional<std::string> kwin_workspace_selection_error(const std::string &outputs) {
    if (window_system != window_system_e::WAYLAND || split_csv(outputs).empty()) {
      return std::nullopt;
    }
    if (!config::video.capture.empty() && config::video.capture != "kwin") {
      return "Wayland outputs= selection currently requires automatic or KWin capture";
    }
    // Several outputs are packed (packed_t), so any set works; the plan checks the frame fits.
    if (!kwin_make_plan(outputs, 0)) {
      return "Wayland outputs= selection names no output KWin knows, or doesn't fit one stream";
    }
    return std::nullopt;
  }

  std::vector<std::string> kwin_display_names() {
    const bool retain_kms_capability =
      platf::linux_private_display_capture::retain_kms_capability(
        linux_private_display::kernel_hdr_pool_available()
      );
    if (platf::linux_private_display_capture::use_dummy_compositor_names(
          has_elevated_privileges(false),
          retain_kms_capability
        )) {
      // We're still in the probing phase of Sunshine startup. Dropping portal security early will break KMS.
      // Just return a dummy screen for now. Display re-enumeration after encoder probing will yield full result.
      std::vector<std::string> display_names;
      display_names.emplace_back("");
      return display_names;
    }

    // A managed HDR pool retains permitted CAP_SYS_ADMIN for future KMS use.
    // Enumerate KWin directly in that case instead of returning dummy names
    // forever merely because the permitted capability remains available.

    const auto screencast = std::make_unique<kwin::screencast_t>();
    if (screencast->init() < 0) {
      return {};
    }
    return screencast->get_output_names();
  }

  bool kwin_virtual_outputs_supported() {
    if (window_system != window_system_e::WAYLAND || (!config::video.capture.empty() && config::video.capture != "kwin")) {
      return false;
    }
    kwin::screencast_t screencast;
    return screencast.init(false) == 0 && screencast.kwin_available() && screencast.supports_virtual_outputs() &&
           screencast.supports_workspace();
  }

  namespace {
    /// The layout a launch with these options gets, from the desktop as it is now.
    std::optional<kwin_virtual::layout_t> virtual_layout(const std::string &outputs, int count, std::string &error) {
      kwin::screencast_t screencast;
      if (screencast.init(false) < 0 || !screencast.kwin_available() || !screencast.supports_workspace()) {
        error = "Virtual monitors need KWin's region capture";
        return std::nullopt;
      }
      const auto descriptors = kwin::output_descriptors(screencast);
      if (!descriptors) {
        error = "Can't read the monitor layout from KWin";
        return std::nullopt;
      }
      const bool virtual_only = outputs == capture_outputs::virtual_only_token;
      // Each screen is captured on its own and packed (packed_t), so the row only has to sit beside the desktop.
      return kwin_virtual::plan(*descriptors, virtual_only ? std::vector<std::string> {} : split_csv(outputs), count, virtual_only, error, true);
    }
  }  // namespace

  std::optional<display_plan::plan_t> kwin_make_plan(const std::string &outputs, int virtual_count) {
    if (window_system != window_system_e::WAYLAND || (!config::video.capture.empty() && config::video.capture != "kwin")) {
      return std::nullopt;
    }
    kwin::screencast_t screencast;
    if (screencast.init(false) < 0 || !screencast.kwin_available()) {
      return std::nullopt;
    }
    const auto descriptors = kwin::output_descriptors(screencast);
    if (!descriptors) {
      return std::nullopt;
    }
    std::vector<display_plan::physical_t> physical;
    for (const auto &out : *descriptors) {
      if (!kwin_virtual::is_virtual_output(out.name)) {
        physical.push_back({out.name, out.x, out.y, out.width, out.height, out.primary});
      }
    }
    const bool virtual_only = outputs == capture_outputs::virtual_only_token;
    return display_plan::make(physical, virtual_only ? std::vector<std::string> {} : split_csv(outputs), virtual_count, virtual_only);
  }

  /// A plan as a manifest: frame rects from the plan, desktop rects from `outputs`.
  std::optional<desktop_manifest_t> plan_manifest(const display_plan::plan_t &plan, const std::vector<output_descriptor_t> &outputs) {
    desktop_manifest_t manifest;
    manifest.frame_width = plan.frame_width;
    manifest.frame_height = plan.frame_height;
    manifest.tiled = true;
    manifest.any_selection = true;
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    for (const auto &screen : plan.screens) {
      monitor_manifest_entry_t entry;
      entry.id = screen.id;
      entry.label = screen.is_virtual ? kwin_virtual::output_name(screen.virtual_index) : screen.label;
      entry.enabled = screen.enabled;
      entry.is_primary = screen.is_primary;
      entry.is_virtual = screen.is_virtual;
      if (screen.enabled) {
        entry.frame_x = screen.frame_x;
        entry.frame_y = screen.frame_y;
        entry.frame_w = screen.frame_w;
        entry.frame_h = screen.frame_h;
      }
      entry.desktop_w = screen.width;
      entry.desktop_h = screen.height;
      const auto at = std::find_if(outputs.begin(), outputs.end(), [&](const auto &out) {
        return out.name == entry.label;
      });
      if (at != outputs.end()) {
        entry.desktop_x = at->x;
        entry.desktop_y = at->y;
        entry.desktop_w = at->width;
        entry.desktop_h = at->height;
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

  std::optional<desktop_manifest_t> kwin_plan_manifest(const std::string &outputs, int count, bool virtual_only) {
    const auto request = virtual_only ? std::string {capture_outputs::virtual_only_token} : outputs;
    const auto plan = kwin_make_plan(request, count);
    if (!plan) {
      return std::nullopt;
    }
    // Where the virtual monitors will sit, before they exist.
    std::string error;
    std::vector<output_descriptor_t> positions;
    if (count > 0 || virtual_only) {
      if (const auto layout = virtual_layout(request, count, error)) {
        positions = layout->outputs;
      }
    } else {
      kwin::screencast_t screencast;
      if (screencast.init(false) == 0) {
        positions = kwin::output_descriptors(screencast).value_or(std::vector<output_descriptor_t> {});
      }
    }
    return plan_manifest(*plan, positions);
  }

  std::optional<desktop_manifest_t> kwin_session_manifest() {
    const auto plan = display_plan::get();
    if (!plan || plan->enabled().size() < 2) {
      return std::nullopt;
    }
    kwin::screencast_t screencast;
    std::vector<output_descriptor_t> positions;
    if (screencast.init(false) == 0) {
      positions = kwin::output_descriptors(screencast).value_or(std::vector<output_descriptor_t> {});
    }
    return plan_manifest(*plan, positions);
  }

  std::optional<std::string> kwin_prepare_virtual_outputs(const std::string &outputs, int count, int refresh_millihz, std::string &error) {
    auto &owner = kwin::virtual_outputs_t::get();
    owner.cancel_scheduled_release();
    const bool virtual_only = outputs == capture_outputs::virtual_only_token;
    if (count <= 0 && !virtual_only) {
      if (owner.count() > 0) {
        owner.release();
      }
      return outputs;
    }
    if (!kwin_virtual_outputs_supported()) {
      error = "This desktop can't make virtual monitors (KDE Plasma on Wayland only)";
      return std::nullopt;
    }
    const auto layout = virtual_layout(outputs, count, error);
    if (!layout) {
      return std::nullopt;
    }
    const auto row = std::find_if(layout->outputs.begin(), layout->outputs.end(), [](const auto &out) {
      return out.is_virtual;
    });
    if (row == layout->outputs.end() || !owner.ensure(count, row->width, row->height, error)) {
      return std::nullopt;
    }
    if (!kwin::place_virtual_outputs(layout->outputs, error)) {
      return std::nullopt;
    }
    kwin::set_virtual_refresh(layout->outputs, refresh_millihz);
    const auto selected = kwin_virtual::join(layout->selected);
    BOOST_LOG(info) << "[kwin-virtual] Session captures ["sv << selected << ']';
    return selected;
  }

  void kwin_release_virtual_outputs() {
    kwin::virtual_outputs_t::get().release();
  }

  void kwin_schedule_virtual_outputs_release(std::chrono::seconds delay) {
    auto &owner = kwin::virtual_outputs_t::get();
    if (owner.count() > 0) {
      owner.schedule_release(delay);
    }
  }

  void kwin_cancel_virtual_outputs_release() {
    kwin::virtual_outputs_t::get().cancel_scheduled_release();
  }

  bool kwin_available() {
    // Init screencast without permission setup (to not cause unneeded logs / temporary desktop files) and check KWin availability
    if (const auto screencast = std::make_unique<kwin::screencast_t>(); screencast->init(false) < 0 || !screencast->kwin_available()) {
      return false;
    }
    return true;
  }
}  // namespace platf
