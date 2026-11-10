/**
 * @file src/platform/macos/display_multi.mm
 * @brief Meteor's multi-monitor capture on macOS (see display_layout.h).
 *
 * Each of the session's displays is captured on its own, already scaled by AVFoundation to its
 * tile in the stream, in the encoder's NV12 format. The newest frame of each is copied into one
 * IOSurface-backed buffer the size of the stream, which goes to VideoToolbox without another
 * copy. A screen whose display isn't attached yet stays black; the capture picks it up when it
 * appears, so the frame, input and Meteor's depth crop keep one geometry for the whole session.
 */
// platform includes
#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

// standard includes
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

// local includes
#include "src/capture_outputs.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/av_video.h"
#include "src/platform/macos/display_layout.h"
#include "src/platform/macos/misc.h"
#include "src/platform/macos/nv12_zero_device.h"

// Avoid conflict between AVFoundation and libavutil both defining AVMediaType
#define AVMediaType AVMediaType_FFmpeg
#include "src/video.h"
#undef AVMediaType

using namespace std::literals;

/// Called with each new frame (retained for the call only).
typedef void (^MeteorFrameBlock)(CVPixelBufferRef);

/**
 * One display's ScreenCaptureKit stream, scaled to its tile in the encoder's format. Unlike
 * AVCaptureScreenInput (deprecated), it captures CGVirtualDisplay displays. Frames come only
 * when the screen changes.
 */
@interface MeteorDisplayStream: NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, retain) SCStream *stream;
@property (nonatomic, copy) MeteorFrameBlock onFrame;
@property (nonatomic, retain) dispatch_queue_t queue;
- (BOOL)startDisplay:(CGDirectDisplayID)display width:(int)width height:(int)height fps:(int)fps format:(OSType)format;
- (void)stop;
@end

@implementation MeteorDisplayStream

- (BOOL)startDisplay:(CGDirectDisplayID)display width:(int)width height:(int)height fps:(int)fps format:(OSType)format {
  // SCShareableContent only answers asynchronously; wait for it, briefly.
  __block SCDisplay *target = nil;
  dispatch_semaphore_t found = dispatch_semaphore_create(0);
  [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                             onScreenWindowsOnly:YES
                                               completionHandler:^(SCShareableContent *content, NSError *error) {
                                                 for (SCDisplay *candidate in content.displays) {
                                                   if (candidate.displayID == display) {
                                                     target = [candidate retain];
                                                   }
                                                 }
                                                 if (error) {
                                                   BOOST_LOG(warning) << "ScreenCaptureKit: "sv << error.localizedDescription.UTF8String;
                                                 }
                                                 dispatch_semaphore_signal(found);
                                               }];
  dispatch_semaphore_wait(found, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
  dispatch_release(found);
  if (!target) {
    return NO;
  }
  SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:target excludingWindows:@[]];
  [target release];
  SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
  config.width = (size_t) width;
  config.height = (size_t) height;
  config.pixelFormat = format;
  config.minimumFrameInterval = CMTimeMake(1, fps > 0 ? fps : 60);
  config.showsCursor = YES;
  config.queueDepth = 4;
  config.colorSpaceName = kCGColorSpaceSRGB;
  self.stream = [[[SCStream alloc] initWithFilter:filter configuration:config delegate:self] autorelease];
  [filter release];
  [config release];
  self.queue = dispatch_queue_create("meteor.display-stream", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
  dispatch_release(self.queue);  // The property retains it.
  NSError *error = nil;
  if (![self.stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:self.queue error:&error]) {
    BOOST_LOG(warning) << "ScreenCaptureKit: "sv << error.localizedDescription.UTF8String;
    return NO;
  }
  __block BOOL started = NO;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [self.stream startCaptureWithCompletionHandler:^(NSError *error) {
    started = error == nil;
    if (error) {
      BOOST_LOG(warning) << "ScreenCaptureKit: can't start capture: "sv << error.localizedDescription.UTF8String;
    }
    dispatch_semaphore_signal(done);
  }];
  dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
  dispatch_release(done);
  return started;
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) {
    return;
  }
  // Idle and blank frames carry no picture.
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (attachments && CFArrayGetCount(attachments) > 0) {
    NSDictionary *info = (NSDictionary *) CFArrayGetValueAtIndex(attachments, 0);
    NSNumber *status = info[SCStreamFrameInfoStatus];
    if (status && status.integerValue != SCFrameStatusComplete) {
      return;
    }
  }
  CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample);
  MeteorFrameBlock block = self.onFrame;
  if (image && block) {
    block(image);
  }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  BOOST_LOG(warning) << "ScreenCaptureKit: capture stopped: "sv << error.localizedDescription.UTF8String;
}

- (void)stop {
  self.onFrame = nil;
  if (!self.stream) {
    return;
  }
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [self.stream stopCaptureWithCompletionHandler:^(NSError *) {
    dispatch_semaphore_signal(done);
  }];
  dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
  dispatch_release(done);
  self.stream = nil;
}

- (void)dealloc {
  [self stop];
  [_queue release];
  [_onFrame release];
  [super dealloc];
}

@end

namespace {
  /// NV12 positions and sizes must be even.
  int even(double value) {
    return std::max(0, (int) std::lround(value / 2.0) * 2);
  }
}  // namespace

namespace platf::macos {
  namespace {
    std::vector<std::string> split_csv(const std::string &text) {
      std::vector<std::string> out;
      std::size_t start = 0;
      while (start <= text.size()) {
        const auto end = std::min(text.find(',', start), text.size());
        auto item = text.substr(start, end - start);
        const auto first = item.find_first_not_of(' ');
        const auto last = item.find_last_not_of(' ');
        if (first != std::string::npos) {
          out.push_back(item.substr(first, last - first + 1));
        }
        start = end + 1;
      }
      return out;
    }

    std::string display_name(CGDirectDisplayID id) {
      @autoreleasepool {
        for (NSScreen *screen in [NSScreen screens]) {
          NSNumber *number = screen.deviceDescription[@"NSScreenNumber"];
          if (number && number.unsignedIntValue == id && screen.localizedName.length) {
            return screen.localizedName.UTF8String;
          }
        }
      }
      return "Display " + std::to_string(id);
    }

    struct input_layout_t {
      std::mutex mutex;
      const void *owner = nullptr;
      std::vector<input_tile_t> tiles;
    };

    input_layout_t &input_layout() {
      static input_layout_t layout;
      return layout;
    }

    // Meteor's depth only covers the primary screen, but with several screens in one frame it
    // can't tell where that is. Publish its rectangle in the frame (frame size, then x y w h)
    // at the path Meteor names; remove the file when one screen fills the frame. As on Windows.
    void publish_capture_region(const display_plan::plan_t &plan) {
      const char *path = std::getenv("METEOR_CAPTURE_REGION_FILE");
      if (!path || !*path) {
        return;
      }
      const std::filesystem::path file {path};
      std::error_code ignored;
      const auto enabled = plan.enabled();
      const auto primary = std::find_if(enabled.begin(), enabled.end(), [](const auto *s) { return s->is_primary; });
      if (enabled.size() < 2 || primary == enabled.end()) {
        std::filesystem::remove(file, ignored);
        return;
      }
      std::ofstream out {file, std::ios::trunc};
      out << plan.frame_width << ' ' << plan.frame_height << ' ' << (*primary)->frame_x << ' ' << (*primary)->frame_y
          << ' ' << (*primary)->width << ' ' << (*primary)->height << '\n';
    }
  }  // namespace

  bool multimonitor_enabled() {
    const char *value = std::getenv("METEOR_MACOS_MULTIMONITOR");
    return value && *value && std::strcmp(value, "0") != 0;
  }

  std::vector<attached_output_t> attached_outputs() {
    CGDirectDisplayID displays[32];
    std::uint32_t count = 0;
    if (CGGetActiveDisplayList(32, displays, &count) != kCGErrorSuccess) {
      return {};
    }
    const auto virtual_ids = virtual_output_ids();
    std::vector<attached_output_t> outputs;
    std::map<std::string, int> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto id = displays[i];
      // A mirror shows another display's picture; that display is captured instead.
      if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay) {
        continue;
      }
      attached_output_t out;
      out.id = id;
      const auto bounds = CGDisplayBounds(id);
      out.x = bounds.origin.x;
      out.y = bounds.origin.y;
      out.points_w = bounds.size.width;
      out.points_h = bounds.size.height;
      if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
        out.width = (int) CGDisplayModeGetPixelWidth(mode) & ~1;
        out.height = (int) CGDisplayModeGetPixelHeight(mode) & ~1;
        CGDisplayModeRelease(mode);
      } else {
        // A virtual display may report no mode; its size is still known.
        out.width = (int) CGDisplayPixelsWide(id) & ~1;
        out.height = (int) CGDisplayPixelsHigh(id) & ~1;
      }
      out.primary = CGDisplayIsMain(id);
      const auto v = std::find(virtual_ids.begin(), virtual_ids.end(), id);
      out.is_virtual = v != virtual_ids.end();
      // Names are what the user picks in the headset, so keep them readable but unique.
      auto name = display_name(id);
      if (const int n = ++seen[name]; n > 1) {
        name += " (" + std::to_string(n) + ")";
      }
      out.name = std::move(name);
      if (out.width > 0 && out.height > 0) {
        outputs.push_back(std::move(out));
      }
    }
    // Physical displays first, then virtual ones in the order they were made.
    std::stable_sort(outputs.begin(), outputs.end(), [&](const auto &a, const auto &b) {
      if (a.is_virtual != b.is_virtual) {
        return !a.is_virtual;
      }
      if (!a.is_virtual) {
        return false;
      }
      return std::find(virtual_ids.begin(), virtual_ids.end(), a.id) < std::find(virtual_ids.begin(), virtual_ids.end(), b.id);
    });
    return outputs;
  }

  std::optional<display_plan::plan_t> make_plan(const std::string &requested, int virtual_count) {
    std::vector<display_plan::physical_t> physical;
    for (const auto &o : attached_outputs()) {
      if (o.is_virtual) {
        continue;
      }
      physical.push_back({o.name, (int) o.x, (int) o.y, o.width, o.height, o.primary});
    }
    const bool virtual_only = requested == capture_outputs::virtual_only_token;
    return display_plan::make(physical, virtual_only ? std::vector<std::string> {} : split_csv(requested), virtual_count, virtual_only);
  }

  std::optional<display_plan::plan_t> session_plan() {
    if (auto plan = display_plan::get()) {
      return plan;
    }
    // A capture no launch planned (e.g. the encoder probe): stream what is attached.
    return make_plan(capture_outputs::get(), (int) virtual_output_ids().size());
  }

  std::vector<std::optional<attached_output_t>> place(const display_plan::plan_t &plan, const std::vector<attached_output_t> &outputs) {
    std::vector<attached_output_t> virtuals;
    for (const auto &o : outputs) {
      if (o.is_virtual) {
        virtuals.push_back(o);
      }
    }
    std::vector<std::optional<attached_output_t>> placed;
    for (const auto &s : plan.screens) {
      std::optional<attached_output_t> match;
      if (s.enabled && s.is_virtual) {
        if (s.virtual_index >= 1 && s.virtual_index <= (int) virtuals.size()) {
          match = virtuals[s.virtual_index - 1];
        }
      } else if (s.enabled) {
        const auto it = std::find_if(outputs.begin(), outputs.end(), [&](const auto &o) {
          return !o.is_virtual && o.name == s.label;
        });
        if (it != outputs.end()) {
          match = *it;
        }
      }
      placed.push_back(match);
    }
    return placed;
  }

  void set_input_layout(const void *owner, std::vector<input_tile_t> tiles) {
    auto &layout = input_layout();
    std::lock_guard lock {layout.mutex};
    layout.owner = owner;
    layout.tiles = std::move(tiles);
  }

  void clear_input_layout(const void *owner) {
    auto &layout = input_layout();
    std::lock_guard lock {layout.mutex};
    if (layout.owner == owner) {
      layout.owner = nullptr;
      layout.tiles.clear();
    }
  }

  std::optional<std::pair<double, double>> frame_to_desktop(double x, double y) {
    auto &layout = input_layout();
    std::lock_guard lock {layout.mutex};
    if (layout.tiles.empty()) {
      return std::nullopt;
    }
    const input_tile_t *nearest = nullptr;
    double nearest_distance = 0;
    for (const auto &tile : layout.tiles) {
      const double dx = std::max({(double) tile.frame_x - x, 0.0, x - (double) (tile.frame_x + tile.frame_w - 1)});
      const double dy = std::max({(double) tile.frame_y - y, 0.0, y - (double) (tile.frame_y + tile.frame_h - 1)});
      const double distance = dx * dx + dy * dy;
      if (!nearest || distance < nearest_distance) {
        nearest = &tile;
        nearest_distance = distance;
      }
    }
    // Frame pixels to the display's points: a Retina display has two pixels a point.
    const double local_x = std::clamp(x - nearest->frame_x, 0.0, (double) nearest->frame_w - 1);
    const double local_y = std::clamp(y - nearest->frame_y, 0.0, (double) nearest->frame_h - 1);
    return std::pair {nearest->x + local_x * nearest->w / nearest->frame_w, nearest->y + local_y * nearest->h / nearest->frame_h};
  }

  std::optional<std::array<double, 4>> input_bounds() {
    auto &layout = input_layout();
    std::lock_guard lock {layout.mutex};
    if (layout.tiles.empty()) {
      return std::nullopt;
    }
    double left = layout.tiles[0].x, top = layout.tiles[0].y;
    double right = left + layout.tiles[0].w, bottom = top + layout.tiles[0].h;
    for (const auto &t : layout.tiles) {
      left = std::min(left, t.x);
      top = std::min(top, t.y);
      right = std::max(right, t.x + t.w);
      bottom = std::max(bottom, t.y + t.h);
    }
    return std::array {left, top, right - left, bottom - top};
  }
}  // namespace platf::macos

namespace platf {
  namespace {
    /// One display's capture and its newest frame.
    struct source_t {
      CGDirectDisplayID id = 0;
      std::string label;
      int frame_x = 0, frame_y = 0, width = 0, height = 0;  ///< Its tile in the plan's frame.
      int out_x = 0, out_y = 0, out_w = 0, out_h = 0;  ///< Its tile in the stream, once scaled.
      MeteorDisplayStream *stream = nil;
      std::mutex mutex;
      CVPixelBufferRef latest = nullptr;  ///< Retained.
    };

    void copy_planes(CVPixelBufferRef from, CVPixelBufferRef to, int x, int y, int w, int h, int bytes_per_sample) {
      if (CVPixelBufferLockBaseAddress(from, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return;
      }
      for (std::size_t plane = 0; plane < 2; ++plane) {
        const auto *src = static_cast<const std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(from, plane));
        auto *dst = static_cast<std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(to, plane));
        if (!src || !dst) {
          continue;
        }
        const auto src_stride = CVPixelBufferGetBytesPerRowOfPlane(from, plane);
        const auto dst_stride = CVPixelBufferGetBytesPerRowOfPlane(to, plane);
        // Plane 1 is interleaved Cb/Cr at half height: the same bytes per row as luma.
        const int rows = std::min<int>(plane ? h / 2 : h, (int) CVPixelBufferGetHeightOfPlane(from, plane));
        const std::size_t row_bytes = std::min<std::size_t>(
          (std::size_t) w * bytes_per_sample,
          plane ? CVPixelBufferGetWidthOfPlane(from, plane) * 2 * bytes_per_sample : CVPixelBufferGetWidthOfPlane(from, plane) * bytes_per_sample
        );
        const int top = plane ? y / 2 : y;
        for (int row = 0; row < rows; ++row) {
          std::memcpy(dst + (top + row) * dst_stride + (std::size_t) x * bytes_per_sample, src + row * src_stride, row_bytes);
        }
      }
      CVPixelBufferUnlockBaseAddress(from, kCVPixelBufferLock_ReadOnly);
    }

    void fill_black(CVPixelBufferRef buffer, bool ten_bit) {
      for (std::size_t plane = 0; plane < 2; ++plane) {
        auto *dst = static_cast<std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, plane));
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane);
        const auto rows = CVPixelBufferGetHeightOfPlane(buffer, plane);
        if (!ten_bit) {
          std::memset(dst, plane ? 128 : 16, stride * rows);
          continue;
        }
        // 10-bit samples in the top bits of 16: black is 64, neutral chroma 512.
        const std::uint16_t value = (plane ? 512 : 64) << 6;
        auto *samples = reinterpret_cast<std::uint16_t *>(dst);
        std::fill(samples, samples + stride * rows / 2, value);
      }
    }
  }  // namespace

  struct display_multi_t: public display_t {
    display_plan::plan_t plan;
    std::vector<std::shared_ptr<source_t>> sources;
    std::size_t missing = 0;
    int framerate = 60;
    OSType pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    int stream_w = 0, stream_h = 0;  ///< The encoder's size; the plan's frame until it says.
    int content_x = 0, content_y = 0;  ///< The scaled frame's place in the stream (centred).
    CVPixelBufferPoolRef pool = nullptr;
    std::atomic<bool> running {false};
    std::mutex frame_mutex;
    std::condition_variable frame_ready;
    bool fresh = false;

    ~display_multi_t() override {
      stop_sources();
      macos::clear_input_layout(this);
      if (pool) {
        CVPixelBufferPoolRelease(pool);
      }
    }

    int init(const video::config_t &config) {
      framerate = config.framerate > 0 ? config.framerate : 60;
      auto planned = macos::session_plan();
      if (!planned) {
        BOOST_LOG(error) << "Multi-display capture: no screens to stream"sv;
        return -1;
      }
      plan = std::move(*planned);
      width = env_width = stream_w = plan.frame_width;
      height = env_height = stream_h = plan.frame_height;
      place_sources();
      BOOST_LOG(info) << "Multi-display capture: "sv << sources.size() << " of "sv << plan.enabled().size()
                      << " screens, frame "sv << width << 'x' << height;
      macos::publish_capture_region(plan);
      return 0;
    }

    /// Matches the plan's screens to the displays attached now, and records where input goes.
    void place_sources() {
      const auto attached = macos::attached_outputs();
      const auto placed = macos::place(plan, attached);
      std::vector<std::shared_ptr<source_t>> next;
      std::vector<macos::input_tile_t> tiles;
      missing = 0;
      for (std::size_t i = 0; i < plan.screens.size(); ++i) {
        const auto &screen = plan.screens[i];
        if (!screen.enabled) {
          continue;
        }
        if (!placed[i]) {
          ++missing;
          BOOST_LOG(info) << "Multi-display capture: waiting for "sv << screen.label << " to attach"sv;
          continue;
        }
        auto source = std::make_shared<source_t>();
        source->id = placed[i]->id;
        source->label = screen.label;
        source->frame_x = screen.frame_x;
        source->frame_y = screen.frame_y;
        source->width = screen.width;
        source->height = screen.height;
        next.push_back(source);
        tiles.push_back({screen.frame_x, screen.frame_y, screen.width, screen.height,
                         placed[i]->x, placed[i]->y, placed[i]->points_w, placed[i]->points_h});
      }
      sources = std::move(next);
      scale_sources();
      macos::set_input_layout(this, std::move(tiles));
    }

    /// Fits the plan's frame into the stream, centred, and each tile with it.
    void scale_sources() {
      const double scale = std::min((double) stream_w / plan.frame_width, (double) stream_h / plan.frame_height);
      content_x = even((stream_w - plan.frame_width * scale) / 2);
      content_y = even((stream_h - plan.frame_height * scale) / 2);
      for (auto &s : sources) {
        s->out_x = content_x + even(s->frame_x * scale);
        s->out_y = content_y + even(s->frame_y * scale);
        s->out_w = std::min(even(s->width * scale), stream_w - s->out_x);
        s->out_h = std::min(even(s->height * scale), stream_h - s->out_y);
      }
    }

    void start_sources() {
      running = true;
      for (auto &s : sources) {
        std::weak_ptr<source_t> weak = s;
        auto *self = this;
        s->stream = [[MeteorDisplayStream alloc] init];
        s->stream.onFrame = ^(CVPixelBufferRef image) {
          auto source = weak.lock();
          if (!source || !self->running) {
            return;
          }
          CVPixelBufferRetain(image);
          {
            std::lock_guard lock {source->mutex};
            if (source->latest) {
              CVPixelBufferRelease(source->latest);
            }
            source->latest = image;
          }
          {
            std::lock_guard lock {self->frame_mutex};
            self->fresh = true;
          }
          self->frame_ready.notify_one();
        };
        if (![s->stream startDisplay:s->id width:s->out_w height:s->out_h fps:framerate format:pixel_format]) {
          BOOST_LOG(warning) << "Multi-display capture: can't capture "sv << s->label << " (Screen Recording permission?)"sv;
        }
      }
    }

    void stop_sources() {
      running = false;
      for (auto &s : sources) {
        [s->stream stop];
        [s->stream release];
        s->stream = nil;
        std::lock_guard lock {s->mutex};
        if (s->latest) {
          CVPixelBufferRelease(s->latest);
          s->latest = nullptr;
        }
      }
    }

    CVPixelBufferRef composite() {
      if (!pool) {
        NSDictionary *attributes = @{
          (NSString *) kCVPixelBufferPixelFormatTypeKey: @(pixel_format),
          (NSString *) kCVPixelBufferWidthKey: @(stream_w),
          (NSString *) kCVPixelBufferHeightKey: @(stream_h),
          (NSString *) kCVPixelBufferIOSurfacePropertiesKey: @{},
        };
        if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (CFDictionaryRef) attributes, &pool) != kCVReturnSuccess) {
          BOOST_LOG(error) << "Multi-display capture: can't make a "sv << stream_w << 'x' << stream_h << " frame"sv;
          return nullptr;
        }
      }
      CVPixelBufferRef frame = nullptr;
      if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &frame) != kCVReturnSuccess) {
        return nullptr;
      }
      const bool ten_bit = pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
      CVPixelBufferLockBaseAddress(frame, 0);
      fill_black(frame, ten_bit);
      for (auto &s : sources) {
        CVPixelBufferRef latest = nullptr;
        {
          std::lock_guard lock {s->mutex};
          latest = s->latest ? CVPixelBufferRetain(s->latest) : nullptr;
        }
        if (latest) {
          copy_planes(latest, frame, s->out_x, s->out_y, s->out_w, s->out_h, ten_bit ? 2 : 1);
          CVPixelBufferRelease(latest);
        }
      }
      CVPixelBufferUnlockBaseAddress(frame, 0);
      return frame;
    }

    void fill_image(img_t &img, CVPixelBufferRef frame) {
      auto &av_img = static_cast<av_img_t &>(img);
      av_img.sample_buffer = nullptr;
      av_img.pixel_buffer = std::make_shared<av_pixel_buf_t>(frame, av_pixel_buf_t::adopt_t {});
      img.data = av_img.pixel_buffer->data();
      img.width = stream_w;
      img.height = stream_h;
      img.row_pitch = (int) CVPixelBufferGetBytesPerRowOfPlane(frame, 0);
      img.pixel_pitch = 1;
    }

    /// Picks up screens whose display has attached since the capture started.
    bool screens_appeared() {
      if (missing == 0) {
        return false;
      }
      const auto placed = macos::place(plan, macos::attached_outputs());
      const auto now = (std::size_t) std::count_if(placed.begin(), placed.end(), [](const auto &p) { return p.has_value(); });
      return now > sources.size();
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      start_sources();
      auto next_check = std::chrono::steady_clock::now() + 500ms;
      const auto idle_repeat = std::chrono::milliseconds(std::max(1000 / framerate, 1)) * 10;
      while (true) {
        {
          std::unique_lock lock {frame_mutex};
          frame_ready.wait_for(lock, idle_repeat, [&] { return fresh; });
          fresh = false;
        }
        if (std::chrono::steady_clock::now() >= next_check) {
          next_check = std::chrono::steady_clock::now() + 500ms;
          if (screens_appeared()) {
            BOOST_LOG(info) << "Multi-display capture: a screen attached; restarting the capture"sv;
            stop_sources();
            place_sources();
            start_sources();
          }
        }
        std::shared_ptr<img_t> img;
        if (!pull_free_image_cb(img)) {
          break;
        }
        CVPixelBufferRef frame = composite();
        if (!frame) {
          stop_sources();
          return capture_e::error;
        }
        fill_image(*img, frame);
        if (!push_captured_image_cb(std::move(img), true)) {
          break;
        }
      }
      stop_sources();
      return capture_e::ok;
    }

    std::shared_ptr<img_t> alloc_img() override {
      return std::make_shared<av_img_t>();
    }

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
      if (pix_fmt != pix_fmt_e::nv12 && pix_fmt != pix_fmt_e::p010) {
        BOOST_LOG(error) << "Multi-display capture needs VideoToolbox (NV12 or P010)"sv;
        return nullptr;
      }
      auto device = std::make_unique<nv12_zero_device>();
      device->init(static_cast<void *>(this), pix_fmt, set_resolution, set_pixel_format);
      return device;
    }

    int dummy_img(img_t *img) override {
      if (!platf::is_screen_capture_allowed()) {
        return 1;
      }
      return capture_once(img);
    }

    /// One composite of every screen's first frame (or black where none comes within 2 s).
    int capture_once(img_t *img) {
      start_sources();
      const auto all_have_frames = [&] {
        return std::all_of(sources.begin(), sources.end(), [](const auto &s) {
          std::lock_guard lock {s->mutex};
          return s->latest != nullptr;
        });
      };
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!all_have_frames() && std::chrono::steady_clock::now() < deadline) {
        std::unique_lock lock {frame_mutex};
        frame_ready.wait_until(lock, deadline, [&] { return fresh; });
        fresh = false;
      }
      for (const auto &s : sources) {
        std::lock_guard lock {s->mutex};
        if (!s->latest) {
          BOOST_LOG(info) << "Multi-display capture: no frame yet from "sv << s->label;
        }
      }
      CVPixelBufferRef frame = composite();
      stop_sources();
      if (!frame) {
        return 1;
      }
      fill_image(*img, frame);
      return 0;
    }

    static void set_resolution(void *display, int w, int h) {
      auto *self = static_cast<display_multi_t *>(display);
      if (self->stream_w == w && self->stream_h == h) {
        return;
      }
      self->stream_w = w & ~1;
      self->stream_h = h & ~1;
      if (self->pool) {
        CVPixelBufferPoolRelease(self->pool);
        self->pool = nullptr;
      }
      self->scale_sources();
    }

    static void set_pixel_format(void *display, OSType format) {
      static_cast<display_multi_t *>(display)->pixel_format = format;
    }
  };

  std::shared_ptr<display_t> create_multi_display(const video::config_t &config) {
    auto display = std::make_shared<display_multi_t>();
    if (display->init(config)) {
      return nullptr;
    }
    return display;
  }
}  // namespace platf

namespace platf::macos {
  int capture_probe(const std::string &outputs, int virtual_count, const std::string &path) {
    std::string error;
    if (!prepare_virtual_outputs(virtual_count, 60, error)) {
      std::fprintf(stderr, "virtual displays: %s\n", error.c_str());
      return 1;
    }
    for (const auto id : virtual_output_ids()) {
      const auto bounds = CGDisplayBounds(id);
      std::printf("virtual id=%u active=%d mirrors=%u in_mirror_set=%d bounds=%g,%g,%gx%g mode=%s\n", id, CGDisplayIsActive(id),
                  CGDisplayMirrorsDisplay(id), CGDisplayIsInMirrorSet(id), bounds.origin.x, bounds.origin.y, bounds.size.width,
                  bounds.size.height, CGDisplayCopyDisplayMode(id) ? "yes" : "none");
    }
    if (!CGPreflightScreenCaptureAccess()) {
      std::fprintf(stderr, "this process has no Screen Recording permission\n");
    }
    for (const auto &o : attached_outputs()) {
      std::printf("display id=%u name=%s virtual=%d primary=%d pixels=%dx%d points=%g,%g,%gx%g\n", o.id, o.name.c_str(),
                  o.is_virtual, o.primary, o.width, o.height, o.x, o.y, o.points_w, o.points_h);
    }
    // What a stream's refresh_displays() calls; it threw on a virtual display NSScreen didn't know.
    for (const auto &name : platf::display_names(mem_type_e::videotoolbox)) {
      std::printf("display_names: %s\n", name.c_str());
    }
    auto plan = make_plan(outputs, virtual_count);
    if (!plan) {
      std::fprintf(stderr, "no plan\n");
      release_virtual_outputs();
      return 1;
    }
    display_plan::set(*plan);
    video::config_t config {};
    config.framerate = 30;
    auto display = std::make_shared<display_multi_t>();
    int status = 1;
    if (!display->init(config)) {
      auto device = display->make_avcodec_encode_device(pix_fmt_e::nv12);
      auto img = display->alloc_img();
      if (device && !display->capture_once(img.get())) {
        auto &av = static_cast<av_img_t &>(*img);
        CVPixelBufferRef frame = av.pixel_buffer->buf;
        std::ofstream out {path, std::ios::binary | std::ios::trunc};
        for (std::size_t plane = 0; plane < 2; ++plane) {
          const auto *base = static_cast<const char *>(CVPixelBufferGetBaseAddressOfPlane(frame, plane));
          const auto stride = CVPixelBufferGetBytesPerRowOfPlane(frame, plane);
          for (std::size_t row = 0; row < CVPixelBufferGetHeightOfPlane(frame, plane); ++row) {
            out.write(base + row * stride, (std::streamsize) display->stream_w);
          }
        }
        std::printf("frame=%dx%d sources=%zu missing=%zu file=%s\n", display->stream_w, display->stream_h,
                    display->sources.size(), display->missing, path.c_str());
        status = 0;
      } else {
        std::fprintf(stderr, "capture failed (Screen Recording permission?)\n");
      }
    }
    display.reset();
    release_virtual_outputs();
    return status;
  }
}  // namespace platf::macos
