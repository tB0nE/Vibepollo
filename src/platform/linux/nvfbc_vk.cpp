/**
 * @file src/platform/linux/nvfbc_vk.cpp
 * @brief NvFBC capture that keeps frames in GPU memory for the Vulkan PyroWave encoder.
 */
// standard includes
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <thread>

// local includes
#include "nvfbc_vk.h"

#include <NvFBC.h>
#ifdef SUNSHINE_BUILD_VAAPI
  #include "vaapi.h"
#endif
#include "src/logging.h"
#include "src/utility.h"
#include "src/video.h"

using namespace std::literals;

namespace platf {
  namespace {
    linux_cuda::driver_t *cuda_driver() {
      std::string reason;
      auto *driver = linux_cuda::acquire(nullptr, reason);
      if (!driver) {
        BOOST_LOG(error) << "NvFBC capture: "sv << reason;
      }
      return driver;
    }

    NVFBC_API_FUNCTION_LIST api {NVFBC_VERSION};

    bool load_nvfbc() {
      static bool loaded = false;
      if (loaded) {
        return true;
      }
      void *lib = dlopen("libnvidia-fbc.so.1", RTLD_NOW);
      if (!lib) {
        lib = dlopen("libnvidia-fbc.so", RTLD_NOW);
      }
      if (!lib) {
        return false;
      }
      auto create_instance = reinterpret_cast<PNVFBCCREATEINSTANCE>(dlsym(lib, "NvFBCCreateInstance"));
      if (!create_instance || create_instance(&api)) {
        BOOST_LOG(error) << "Unable to create NvFBC instance"sv;
        dlclose(lib);
        return false;
      }
      loaded = true;
      return true;
    }

    constexpr NVFBC_BOOL nv_bool(bool b) {
      return b ? NVFBC_TRUE : NVFBC_FALSE;
    }

    /** Binds the NvFBC session to this thread for the scope. */
    class bound_t {
    public:
      explicit bound_t(NVFBC_SESSION_HANDLE handle):
          handle(handle) {
        NVFBC_BIND_CONTEXT_PARAMS params {NVFBC_BIND_CONTEXT_PARAMS_VER};
        if (api.nvFBCBindContext(handle, &params)) {
          BOOST_LOG(error) << "Couldn't bind NvFBC context to current thread: "sv << api.nvFBCGetLastErrorStr(handle);
        }
      }

      ~bound_t() {
        NVFBC_RELEASE_CONTEXT_PARAMS params {NVFBC_RELEASE_CONTEXT_PARAMS_VER};
        if (api.nvFBCReleaseContext(handle, &params)) {
          BOOST_LOG(error) << "Couldn't release NvFBC context from current thread: "sv << api.nvFBCGetLastErrorStr(handle);
        }
      }

    private:
      NVFBC_SESSION_HANDLE handle;
    };

    /** Owns an NvFBC session handle and, while it exists, its capture session. */
    class session_t {
    public:
      session_t() = default;
      session_t(const session_t &) = delete;
      session_t &operator=(const session_t &) = delete;

      ~session_t() {
        reset();
      }

      bool make() {
        NVFBC_CREATE_HANDLE_PARAMS params {NVFBC_CREATE_HANDLE_PARAMS_VER};
        // Set privateData to allow NvFBC on consumer NVIDIA GPUs.
        // Based on https://github.com/keylase/nvidia-patch/blob/3193b4b1cea91527bf09ea9b8db5aade6a3f3c0a/win/nvfbcwrp/nvfbcwrp_main.cpp#L23-L25 .
        static const unsigned int MAGIC_PRIVATE_DATA[4] = {0xAEF57AC5, 0x401D1A39, 0x1B856BBE, 0x9ED0CEBA};
        params.privateData = MAGIC_PRIVATE_DATA;
        params.privateDataSize = sizeof(MAGIC_PRIVATE_DATA);
        if (api.nvFBCCreateHandle(&handle, &params)) {
          BOOST_LOG(error) << "Failed to create NvFBC session: "sv << api.nvFBCGetLastErrorStr(handle);
          return false;
        }
        have_handle = true;
        return true;
      }

      const char *last_error() const {
        return api.nvFBCGetLastErrorStr(handle);
      }

      bool status(NVFBC_GET_STATUS_PARAMS &out) {
        out = NVFBC_GET_STATUS_PARAMS {NVFBC_GET_STATUS_PARAMS_VER};
        if (api.nvFBCGetStatus(handle, &out)) {
          BOOST_LOG(error) << "Failed to get NvFBC status: "sv << last_error();
          return false;
        }
        return true;
      }

      bool start(NVFBC_CREATE_CAPTURE_SESSION_PARAMS &params) {
        if (api.nvFBCCreateCaptureSession(handle, &params)) {
          BOOST_LOG(error) << "Failed to start NvFBC capture session: "sv << last_error();
          return false;
        }
        have_capture = true;
        NVFBC_TOCUDA_SETUP_PARAMS setup {NVFBC_TOCUDA_SETUP_PARAMS_VER, NVFBC_BUFFER_FORMAT_BGRA};
        if (api.nvFBCToCudaSetUp(handle, &setup)) {
          BOOST_LOG(error) << "Failed to set up NvFBC CUDA capture: "sv << last_error();
          return false;
        }
        return true;
      }

      void stop() {
        if (!have_capture) {
          return;
        }
        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS params {NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER};
        if (api.nvFBCDestroyCaptureSession(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy NvFBC capture session: "sv << last_error();
        }
        have_capture = false;
      }

      void reset() {
        if (!have_handle) {
          return;
        }
        stop();
        NVFBC_DESTROY_HANDLE_PARAMS params {NVFBC_DESTROY_HANDLE_PARAMS_VER};
        // Destroying the handle releases its context; releasing again afterwards would fail.
        NVFBC_BIND_CONTEXT_PARAMS bind {NVFBC_BIND_CONTEXT_PARAMS_VER};
        api.nvFBCBindContext(handle, &bind);
        if (api.nvFBCDestroyHandle(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy NvFBC session handle: "sv << last_error();
        }
        have_handle = false;
      }

      NVFBC_SESSION_HANDLE handle {};
      bool have_handle {false};
      bool have_capture {false};
    };

    class nvfbc_vk_display_t: public display_t {
    public:
      int init(mem_type_e type, const std::string &display_name, const video::config_t &config) {
        mem_type = type;
        driver = cuda_driver();
        if (!driver) {
          return -1;
        }
        // PyroWave reads the GPU copy directly; every other codec needs host memory.
        gpu_only = config.videoFormat == 3;

        if (!session.make()) {
          return -1;
        }
        bound_t bound {session.handle};
        NVFBC_GET_STATUS_PARAMS status;
        if (!session.status(status) || !status.bIsCapturePossible) {
          BOOST_LOG(error) << "NvFBC cannot capture on this system"sv;
          return -1;
        }

        int streamed = -1;
        if (!display_name.empty() && status.bXRandRAvailable) {
          const int index = util::from_view(display_name);
          if (index >= 0 && static_cast<unsigned>(index) < status.dwOutputNum) {
            streamed = index;
          } else {
            BOOST_LOG(warning) << "Can't stream monitor ["sv << index << "], defaulting to the virtual desktop"sv;
          }
        }

        params = NVFBC_CREATE_CAPTURE_SESSION_PARAMS {NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER};
        params.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
        params.bDisableAutoModesetRecovery = nv_bool(true);
        params.dwSamplingRateMs = 1000 / std::max(1, config.framerate);
        params.bPushModel = nv_bool(false);
        params.bAllowDirectCapture = nv_bool(false);

        if (streamed != -1) {
          auto &output = status.outputs[streamed];
          width = output.trackedBox.w;
          height = output.trackedBox.h;
          offset_x = output.trackedBox.x;
          offset_y = output.trackedBox.y;
          params.eTrackingType = NVFBC_TRACKING_OUTPUT;
          params.dwOutputId = output.dwId;
        } else {
          width = status.screenSize.w;
          height = status.screenSize.h;
          params.eTrackingType = NVFBC_TRACKING_SCREEN;
        }
        env_width = status.screenSize.w;
        env_height = status.screenSize.h;
        delay = std::chrono::nanoseconds {1s} / std::max(1, config.framerate);
        BOOST_LOG(info) << "NvFBC capture: "sv << width << 'x' << height << (gpu_only ? " (GPU-resident)"sv : " (with host copy)"sv);
        return 0;
      }

      capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        linux_cuda::scoped_context_t cuda {*driver};
        if (!cuda.ok()) {
          BOOST_LOG(error) << "NvFBC capture: could not make the CUDA context current"sv;
          return capture_e::error;
        }
        bound_t bound {session.handle};
        auto cleanup = util::fail_guard([&]() {
          session.stop();
        });

        // Force the first snapshot to create the capture session.
        cursor_visible = !*cursor;
        auto next_frame = std::chrono::steady_clock::now();
        while (true) {
          auto now = std::chrono::steady_clock::now();
          if (next_frame > now) {
            std::this_thread::sleep_for(next_frame - now);
          }
          next_frame += delay;
          if (next_frame < now) {
            next_frame = now + delay;
          }

          std::shared_ptr<img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, *cursor);
          switch (status) {
            case capture_e::reinit:
            case capture_e::error:
            case capture_e::interrupted:
              return status;
            case capture_e::timeout:
            case capture_e::ok:
              if (!push_captured_image_cb(std::move(img_out), status == capture_e::ok)) {
                return capture_e::ok;
              }
              break;
            default:
              return status;
          }
        }
      }

      std::shared_ptr<img_t> alloc_img() override {
        auto img = std::make_shared<nvfbc_vk_img_t>();
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = width * 4;
        img->bytes = std::size_t(img->row_pitch) * height;
        if (gpu_only) {
          linux_cuda::scoped_context_t cuda {*driver};
          if (!cuda.ok() || driver->fn.cuMemAlloc(&img->device_ptr, img->bytes) != CUDA_SUCCESS) {
            BOOST_LOG(error) << "NvFBC capture: GPU image allocation failed"sv;
            return nullptr;
          }
        } else {
          img->data = new std::uint8_t[img->bytes];
          img->owns_host_data = true;
        }
        return img;
      }

      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
        if (mem_type == mem_type_e::vaapi) {
          return va::make_avcodec_encode_device(width, height, false);
        }
#endif
        return std::make_unique<avcodec_encode_device_t>();
      }

      int dummy_img(img_t *img) override {
        auto *frame = static_cast<nvfbc_vk_img_t *>(img);
        if (frame->data) {
          std::memset(frame->data, 0, frame->bytes);
          return 0;
        }
        std::vector<std::uint8_t> zeros(frame->bytes);
        linux_cuda::scoped_context_t cuda {*driver};
        return cuda.ok() && driver->fn.cuMemcpyHtoD(frame->device_ptr, zeros.data(), zeros.size()) == CUDA_SUCCESS ? 0 : -1;
      }

    private:
      capture_e restart(bool cursor) {
        session.stop();
        cursor_visible = cursor;
        params.bWithCursor = nv_bool(cursor);
        if (!session.start(params)) {
          return capture_e::error;
        }
        return capture_e::ok;
      }

      capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<img_t> &img_out, bool cursor) {
        if (cursor != cursor_visible) {
          if (auto status = restart(cursor); status != capture_e::ok) {
            return status;
          }
        }

        CUdeviceptr grabbed {};
        NVFBC_FRAME_GRAB_INFO frame {};
        NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab {NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER, NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT, &grabbed, &frame, 150};
        if (auto status = api.nvFBCToCudaGrabFrame(session.handle, &grab)) {
          if (status == NVFBC_ERR_MUST_RECREATE) {
            return capture_e::reinit;
          }
          BOOST_LOG(error) << "Couldn't capture NvFBC frame: "sv << session.last_error();
          return capture_e::error;
        }
        const auto grabbed_at = std::chrono::steady_clock::now();
        if (static_cast<int>(frame.dwWidth) != width || static_cast<int>(frame.dwHeight) != height) {
          BOOST_LOG(info) << "NvFBC frame size changed to "sv << frame.dwWidth << 'x' << frame.dwHeight << "; reinitializing"sv;
          return capture_e::reinit;
        }

        if (!pull_free_image_cb(img_out)) {
          return capture_e::interrupted;
        }
        auto *img = static_cast<nvfbc_vk_img_t *>(img_out.get());
        const CUresult copied = gpu_only ? driver->fn.cuMemcpyDtoD(img->device_ptr, grabbed, img->bytes) :
                                           driver->fn.cuMemcpyDtoH(img->data, grabbed, img->bytes);
        // NvFBC reuses its buffer on the next grab; the copy must be finished first.
        if (copied != CUDA_SUCCESS || driver->fn.cuStreamSynchronize(nullptr) != CUDA_SUCCESS) {
          BOOST_LOG(error) << "NvFBC capture: frame copy failed: "sv << linux_cuda::describe(*driver, copied);
          return capture_e::error;
        }
        img->frame_timestamp = grabbed_at;
        img->host_processing_timestamp = std::chrono::steady_clock::now();
        return capture_e::ok;
      }

      linux_cuda::driver_t *driver {};
      session_t session;
      NVFBC_CREATE_CAPTURE_SESSION_PARAMS params {};
      std::chrono::nanoseconds delay {};
      bool cursor_visible {false};
      bool gpu_only {false};
      mem_type_e mem_type {mem_type_e::system};
    };
  }  // namespace

  nvfbc_vk_img_t::~nvfbc_vk_img_t() {
    if (owns_host_data) {
      delete[] data;
      data = nullptr;
    }
    if (device_ptr) {
      if (auto *driver = cuda_driver()) {
        linux_cuda::scoped_context_t cuda {*driver};
        driver->fn.cuMemFree(device_ptr);
      }
    }
  }

  std::vector<std::string> nvfbc_vk_display_names() {
    if (!load_nvfbc() || !cuda_driver()) {
      return {};
    }
    session_t session;
    if (!session.make()) {
      return {};
    }
    bound_t bound {session.handle};
    NVFBC_GET_STATUS_PARAMS status;
    if (!session.status(status)) {
      return {};
    }
    if (!status.bIsCapturePossible) {
      BOOST_LOG(error) << "NVIDIA driver doesn't support NvFBC screencasting"sv;
      return {};
    }
    BOOST_LOG(info) << "NvFBC found ["sv << status.dwOutputNum << "] outputs, virtual desktop "sv << status.screenSize.w << 'x' << status.screenSize.h;
    std::vector<std::string> names;
    for (unsigned i = 0; i < status.dwOutputNum; ++i) {
      BOOST_LOG(info) << "  Output "sv << i << ": "sv << status.outputs[i].name << ' ' << status.outputs[i].trackedBox.w << 'x' << status.outputs[i].trackedBox.h << '+' << status.outputs[i].trackedBox.x << '+' << status.outputs[i].trackedBox.y;
      names.emplace_back(std::to_string(i));
    }
    return names;
  }

  std::shared_ptr<display_t> nvfbc_vk_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != mem_type_e::system && hwdevice_type != mem_type_e::cuda && hwdevice_type != mem_type_e::vaapi) {
      BOOST_LOG(error) << "Could not initialize NvFBC display with the given hw device type"sv;
      return nullptr;
    }
    if (!load_nvfbc()) {
      return nullptr;
    }
    auto display = std::make_shared<nvfbc_vk_display_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }
    return display;
  }
}  // namespace platf
