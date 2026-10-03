/**
 * @file src/platform/linux/cuda_shared.cpp
 * @brief Minimal CUDA driver access shared by NvFBC capture and the PyroWave encoder.
 */
#include "cuda_shared.h"

#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <unistd.h>

namespace linux_cuda {
  namespace {
    std::mutex mutex;
    driver_t state;
    bool loaded = false;

    template<class T>
    bool load(void *lib, T *&out, const char *name) {
      out = reinterpret_cast<T *>(dlsym(lib, name));
      return out != nullptr;
    }

    bool load_functions(functions_t &f, std::string &error) {
      void *lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
      if (!lib) {
        lib = dlopen("libcuda.so", RTLD_NOW | RTLD_GLOBAL);
      }
      if (!lib) {
        error = "libcuda could not be loaded";
        return false;
      }
      const bool ok = load(lib, f.cuInit, "cuInit") && load(lib, f.cuDeviceGetCount, "cuDeviceGetCount") && load(lib, f.cuDeviceGet, "cuDeviceGet") &&
                      load(lib, f.cuDeviceGetUuid, "cuDeviceGetUuid") && load(lib, f.cuCtxCreate, "cuCtxCreate_v2") &&
                      load(lib, f.cuCtxPushCurrent, "cuCtxPushCurrent_v2") && load(lib, f.cuCtxPopCurrent, "cuCtxPopCurrent_v2") &&
                      load(lib, f.cuMemAlloc, "cuMemAlloc_v2") && load(lib, f.cuMemFree, "cuMemFree_v2") && load(lib, f.cuMemcpyDtoD, "cuMemcpyDtoD_v2") &&
                      load(lib, f.cuMemcpyHtoD, "cuMemcpyHtoD_v2") && load(lib, f.cuMemcpyDtoH, "cuMemcpyDtoH_v2") &&
                      load(lib, f.cuStreamSynchronize, "cuStreamSynchronize") && load(lib, f.cuGetErrorString, "cuGetErrorString") &&
                      load(lib, f.cuImportExternalMemory, "cuImportExternalMemory") &&
                      load(lib, f.cuExternalMemoryGetMappedBuffer, "cuExternalMemoryGetMappedBuffer") && load(lib, f.cuDestroyExternalMemory, "cuDestroyExternalMemory");
      if (!ok) {
        error = "CUDA driver is missing required entry points";
      }
      return ok;
    }
  }  // namespace

  std::string describe(driver_t &driver, CUresult result) {
    const char *text = nullptr;
    if (driver.fn.cuGetErrorString) {
      driver.fn.cuGetErrorString(result, &text);
    }
    return (text ? std::string(text) : std::string("unknown")) + " (" + std::to_string(int(result)) + ")";
  }

  driver_t *acquire(const std::uint8_t *vulkan_uuid, std::string &error) {
    std::lock_guard lock(mutex);
    if (!loaded) {
      if (!load_functions(state.fn, error)) {
        return nullptr;
      }
      auto *functions = &state.fn;
      if (functions->cuInit(0) != CUDA_SUCCESS) {
        error = "cuInit failed";
        return nullptr;
      }
      int count = 0;
      if (functions->cuDeviceGetCount(&count) != CUDA_SUCCESS || count <= 0) {
        error = "no CUDA device";
        return nullptr;
      }
      int chosen = -1;
      for (int i = 0; i < count && chosen < 0; ++i) {
        CUdevice dev;
        if (functions->cuDeviceGet(&dev, i) != CUDA_SUCCESS) {
          continue;
        }
        if (!vulkan_uuid) {
          chosen = i;
          break;
        }
        CUuuid uuid {};
        if (functions->cuDeviceGetUuid && functions->cuDeviceGetUuid(&uuid, dev) == CUDA_SUCCESS && !std::memcmp(uuid.bytes, vulkan_uuid, 16)) {
          chosen = i;
        }
      }
      if (chosen < 0) {
        error = "no CUDA device matches the Vulkan device";
        return nullptr;
      }
      functions->cuDeviceGet(&state.device, chosen);
      if (auto r = functions->cuCtxCreate(&state.context, 0, state.device); r != CUDA_SUCCESS) {
        error = "cuCtxCreate failed: " + describe(state, r);
        return nullptr;
      }
      // cuCtxCreate leaves the new context current on this thread; callers push explicitly.
      CUcontext popped;
      functions->cuCtxPopCurrent(&popped);
      loaded = true;
    } else if (vulkan_uuid) {
      CUuuid uuid {};
      if (!state.fn.cuDeviceGetUuid || state.fn.cuDeviceGetUuid(&uuid, state.device) != CUDA_SUCCESS || std::memcmp(uuid.bytes, vulkan_uuid, 16)) {
        error = "CUDA device in use differs from the Vulkan device";
        return nullptr;
      }
    }
    return &state;
  }

  scoped_context_t::scoped_context_t(driver_t &d):
      driver(d),
      pushed(d.fn.cuCtxPushCurrent(d.context) == CUDA_SUCCESS) {
  }

  scoped_context_t::~scoped_context_t() {
    if (pushed) {
      CUcontext popped;
      driver.fn.cuCtxPopCurrent(&popped);
    }
  }

  external_buffer_t::external_buffer_t(driver_t &d):
      driver(d) {
  }

  external_buffer_t::~external_buffer_t() {
    scoped_context_t ctx(driver);
    if (memory) {
      driver.fn.cuDestroyExternalMemory(memory);
    }
  }

  std::unique_ptr<external_buffer_t> external_buffer_t::import(driver_t &driver, int fd, std::size_t allocation_size, std::size_t size, std::string &error) {
    scoped_context_t ctx(driver);
    if (!ctx.ok()) {
      error = "could not make the CUDA context current";
      return {};
    }
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC handle {};
    handle.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    handle.handle.fd = fd;
    handle.size = allocation_size;
    std::unique_ptr<external_buffer_t> buffer(new external_buffer_t(driver));
    if (auto r = driver.fn.cuImportExternalMemory(&buffer->memory, &handle); r != CUDA_SUCCESS) {
      error = "cuImportExternalMemory failed: " + describe(driver, r);
      return {};  // The descriptor is only consumed on success.
    }
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC map {};
    map.size = size;
    if (auto r = driver.fn.cuExternalMemoryGetMappedBuffer(&buffer->mapped, buffer->memory, &map); r != CUDA_SUCCESS) {
      error = "cuExternalMemoryGetMappedBuffer failed: " + describe(driver, r);
      return {};
    }
    return buffer;
  }

  bool external_buffer_t::copy_from(CUdeviceptr source, std::size_t bytes, std::string &error) {
    scoped_context_t ctx(driver);
    if (!ctx.ok()) {
      error = "could not make the CUDA context current";
      return false;
    }
    if (auto r = driver.fn.cuMemcpyDtoD(mapped, source, bytes); r != CUDA_SUCCESS) {
      error = "CUDA copy into the Vulkan buffer failed: " + describe(driver, r);
      return false;
    }
    // Vulkan runs after this call returns; make the copy complete first.
    if (auto r = driver.fn.cuStreamSynchronize(nullptr); r != CUDA_SUCCESS) {
      error = "CUDA synchronize failed: " + describe(driver, r);
      return false;
    }
    return true;
  }
}  // namespace linux_cuda
