/**
 * @file src/platform/macos/virtual_display.mm
 * @brief The session's virtual displays on macOS, made with CoreGraphics' CGVirtualDisplay.
 *
 * CGVirtualDisplay is private API (macOS 11+; DeskPad and BetterDisplay use it), so its classes
 * are looked up at run time: without them the host just can't make virtual displays. The
 * displays belong to this process. macOS removes them when it exits, even after a crash, so no
 * driver or cleanup is needed.
 */
// platform includes
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

// standard includes
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

// local includes
#include "src/logging.h"
#include "src/platform/macos/display_layout.h"

using namespace std::literals;

// The private interface, as DeskPad and BetterDisplay declare it.
@interface CGVirtualDisplayDescriptor: NSObject
@property (retain, nonatomic) dispatch_queue_t queue;
@property (retain, nonatomic) NSString *name;
@property (nonatomic) unsigned int maxPixelsHigh;
@property (nonatomic) unsigned int maxPixelsWide;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) unsigned int serialNum;
@property (nonatomic) unsigned int productID;
@property (nonatomic) unsigned int vendorID;
@property (copy, nonatomic) void (^terminationHandler)(id, id);
@end

@interface CGVirtualDisplayMode: NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings: NSObject
@property (retain, nonatomic) NSArray *modes;
@property (nonatomic) unsigned int hiDPI;
@end

@interface CGVirtualDisplay: NSObject
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@property (readonly, nonatomic) unsigned int displayID;
@end

namespace platf::macos {
  namespace {
    // "NF": Nightfall. Kept apart from real monitors' EDID vendor IDs.
    constexpr unsigned int vendor_id = 0x4E46;
    constexpr unsigned int product_id = 0x0001;

    struct virtual_displays_t {
      std::mutex mutex;
      NSMutableArray *displays = nil;  ///< CGVirtualDisplay objects; a display lives while retained.
      std::vector<std::uint32_t> ids;
      int width = 0, height = 0;
      double refresh = 0;
      std::uint64_t release_generation = 0;  ///< Bumped to cancel a scheduled release.
    };

    virtual_displays_t &state() {
      static virtual_displays_t state;
      return state;
    }

    bool is_active(std::uint32_t id) {
      CGDirectDisplayID displays[32];
      std::uint32_t count = 0;
      if (CGGetActiveDisplayList(32, displays, &count) != kCGErrorSuccess) {
        return false;
      }
      return std::find(displays, displays + count, id) != displays + count;
    }

    /// Extends the desktop with the virtual displays, to the right of the physical ones, rather
    /// than letting them join a mirror set. For this process only: macOS restores the user's
    /// arrangement when the host exits, even after a crash.
    /// Which display each physical display mirrors (kCGNullDirectDisplay if none), read before
    /// a virtual display can join a mirror set and change it.
    std::vector<std::pair<CGDirectDisplayID, CGDirectDisplayID>> mirroring() {
      CGDirectDisplayID displays[32];
      std::uint32_t count = 0;
      std::vector<std::pair<CGDirectDisplayID, CGDirectDisplayID>> result;
      if (CGGetOnlineDisplayList(32, displays, &count) == kCGErrorSuccess) {
        for (std::uint32_t i = 0; i < count; ++i) {
          result.emplace_back(displays[i], CGDisplayMirrorsDisplay(displays[i]));
        }
      }
      return result;
    }

    void arrange(const virtual_displays_t &s, const std::vector<std::pair<CGDirectDisplayID, CGDirectDisplayID>> &mirrors) {
      CGDirectDisplayID displays[32];
      std::uint32_t count = 0;
      if (CGGetActiveDisplayList(32, displays, &count) != kCGErrorSuccess) {
        return;
      }
      double right = 0, top = 0;
      bool any = false;
      for (std::uint32_t i = 0; i < count; ++i) {
        if (std::find(s.ids.begin(), s.ids.end(), displays[i]) != s.ids.end()) {
          continue;
        }
        const auto mirrored = std::find_if(mirrors.begin(), mirrors.end(), [&](const auto &m) { return m.first == displays[i]; });
        if (mirrored != mirrors.end() && mirrored->second != kCGNullDirectDisplay) {
          continue;
        }
        const auto bounds = CGDisplayBounds(displays[i]);
        if (!any || CGRectGetMaxX(bounds) > right) {
          right = CGRectGetMaxX(bounds);
          top = bounds.origin.y;
        }
        any = true;
      }
      CGDisplayConfigRef config = nullptr;
      if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
        return;
      }
      // A virtual display that joined a mirror set as its master took the set apart when it
      // left; keep the user's mirroring as it was.
      for (const auto &[display, master] : mirrors) {
        if (master != kCGNullDirectDisplay) {
          CGConfigureDisplayMirrorOfDisplay(config, display, master);
        }
      }
      double x = right;
      for (const auto id : s.ids) {
        CGConfigureDisplayMirrorOfDisplay(config, id, kCGNullDirectDisplay);
        CGConfigureDisplayOrigin(config, id, (int32_t) x, (int32_t) top);
        x += s.width;  // One point per pixel (hiDPI off).
      }
      if (const auto error = CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly); error != kCGErrorSuccess) {
        BOOST_LOG(warning) << "Virtual displays: can't arrange them on the desktop (error "sv << error << ')';
      }
    }

    void release_locked(virtual_displays_t &s) {
      if (s.ids.empty()) {
        return;
      }
      BOOST_LOG(info) << "Virtual displays: removing "sv << s.ids.size();
      [s.displays release];
      s.displays = nil;
      s.ids.clear();
      s.width = s.height = 0;
      s.refresh = 0;
    }

    /// The size virtual displays are made at: the main physical display's mode, in pixels.
    std::pair<int, int> primary_size(const virtual_displays_t &s) {
      CGDirectDisplayID displays[32];
      std::uint32_t count = 0;
      CGGetActiveDisplayList(32, displays, &count);
      CGDirectDisplayID primary = CGMainDisplayID();
      if (std::find(s.ids.begin(), s.ids.end(), primary) != s.ids.end()) {
        for (std::uint32_t i = 0; i < count; ++i) {
          if (std::find(s.ids.begin(), s.ids.end(), displays[i]) == s.ids.end()) {
            primary = displays[i];
            break;
          }
        }
      }
      int w = 1920, h = 1080;
      if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(primary)) {
        w = (int) CGDisplayModeGetPixelWidth(mode);
        h = (int) CGDisplayModeGetPixelHeight(mode);
        CGDisplayModeRelease(mode);
      }
      // NV12 and the encoders want even sizes.
      return {w & ~1, h & ~1};
    }
  }  // namespace

  bool virtual_outputs_supported() {
    return NSClassFromString(@"CGVirtualDisplay") && NSClassFromString(@"CGVirtualDisplayDescriptor") &&
           NSClassFromString(@"CGVirtualDisplaySettings") && NSClassFromString(@"CGVirtualDisplayMode");
  }

  std::vector<std::uint32_t> virtual_output_ids() {
    auto &s = state();
    std::lock_guard lock {s.mutex};
    return s.ids;
  }

  bool prepare_virtual_outputs(int count, int fps, std::string &error) {
    auto &s = state();
    std::lock_guard lock {s.mutex};
    ++s.release_generation;
    count = std::clamp(count, 0, 4);
    if (count == 0) {
      release_locked(s);
      return true;
    }
    if (!virtual_outputs_supported()) {
      error = "This version of macOS can't make virtual displays";
      return false;
    }
    // The stream's rate; launch requests carry it in millihertz on some paths.
    const double refresh = fps > 1000 ? fps / 1000.0 : (fps > 0 ? fps : 60);
    const auto [width, height] = primary_size(s);
    if ((int) s.ids.size() == count && s.width == width && s.height == height && std::abs(s.refresh - refresh) < 0.01) {
      return true;
    }
    release_locked(s);
    const auto mirrors = mirroring();

    @autoreleasepool {
      Class descriptor_class = NSClassFromString(@"CGVirtualDisplayDescriptor");
      Class display_class = NSClassFromString(@"CGVirtualDisplay");
      Class settings_class = NSClassFromString(@"CGVirtualDisplaySettings");
      Class mode_class = NSClassFromString(@"CGVirtualDisplayMode");
      s.displays = [[NSMutableArray alloc] init];
      for (int n = 1; n <= count; ++n) {
        CGVirtualDisplayDescriptor *descriptor = [[descriptor_class alloc] init];
        descriptor.queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        descriptor.name = [NSString stringWithFormat:@"Nightfall Virtual %d", n];
        descriptor.maxPixelsWide = width;
        descriptor.maxPixelsHigh = height;
        // About 110 pixels per inch, so macOS picks a sensible default scale.
        descriptor.sizeInMillimeters = CGSizeMake(width * 25.4 / 110.0, height * 25.4 / 110.0);
        descriptor.vendorID = vendor_id;
        descriptor.productID = product_id;
        descriptor.serialNum = (unsigned int) n;

        CGVirtualDisplay *display = [[display_class alloc] initWithDescriptor:descriptor];
        [descriptor release];
        if (!display) {
          error = "macOS refused to make a virtual display";
          release_locked(s);
          return false;
        }
        CGVirtualDisplaySettings *settings = [[settings_class alloc] init];
        // One pixel per point, so the frame shows what the desktop shows at this size.
        settings.hiDPI = 0;
        CGVirtualDisplayMode *mode = [[mode_class alloc] initWithWidth:width height:height refreshRate:refresh];
        settings.modes = @[mode];
        [mode release];
        const BOOL applied = [display applySettings:settings];
        [settings release];
        if (!applied) {
          [display release];
          error = "macOS refused the virtual display's mode";
          release_locked(s);
          return false;
        }
        [s.displays addObject:display];
        s.ids.push_back(display.displayID);
        [display release];
      }
    }
    s.width = width;
    s.height = height;
    s.refresh = refresh;

    // They join the desktop shortly after being made.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           !std::all_of(s.ids.begin(), s.ids.end(), [](auto id) { return is_active(id); })) {
      std::this_thread::sleep_for(50ms);
    }
    arrange(s, mirrors);
    // Let the arrangement settle before the plan reads it.
    const auto settled = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < settled &&
           std::any_of(s.ids.begin(), s.ids.end(), [](auto id) { return CGDisplayIsInMirrorSet(id); })) {
      std::this_thread::sleep_for(50ms);
    }
    BOOST_LOG(info) << "Virtual displays: made "sv << count << " at "sv << width << 'x' << height << '@' << refresh << " Hz"sv;
    return true;
  }

  void release_virtual_outputs() {
    auto &s = state();
    std::lock_guard lock {s.mutex};
    ++s.release_generation;
    release_locked(s);
  }

  void schedule_virtual_outputs_release(std::chrono::seconds delay) {
    auto &s = state();
    std::uint64_t generation;
    {
      std::lock_guard lock {s.mutex};
      if (s.ids.empty()) {
        return;
      }
      generation = ++s.release_generation;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) delay.count() * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      auto &s = state();
      std::lock_guard lock {s.mutex};
      if (s.release_generation == generation) {
        release_locked(s);
      }
    });
  }

  void cancel_virtual_outputs_release() {
    auto &s = state();
    std::lock_guard lock {s.mutex};
    ++s.release_generation;
  }
}  // namespace platf::macos
