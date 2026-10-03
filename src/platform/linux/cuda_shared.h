/**
 * @file src/platform/linux/cuda_shared.h
 * @brief Minimal CUDA driver access (dlopen only, no nvcc) shared by the NvFBC capture
 *        backend and the PyroWave Vulkan encoder.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <ffnvcodec/dynlink_cuda.h>
#include <memory>
#include <string>

namespace linux_cuda {
  /**
   * The driver entry points we use, resolved straight from libcuda by their `_v2` names.
   * (The ffnvcodec loader resolves through cuGetProcAddress and returns entry points
   * that reject valid calls here, so only its type definitions are used.)
   */
  struct functions_t {
    tcuInit *cuInit = nullptr;
    tcuDeviceGetCount *cuDeviceGetCount = nullptr;
    tcuDeviceGet *cuDeviceGet = nullptr;
    tcuDeviceGetUuid *cuDeviceGetUuid = nullptr;
    tcuCtxCreate_v2 *cuCtxCreate = nullptr;
    tcuCtxPushCurrent_v2 *cuCtxPushCurrent = nullptr;
    tcuCtxPopCurrent_v2 *cuCtxPopCurrent = nullptr;
    tcuMemAlloc_v2 *cuMemAlloc = nullptr;
    tcuMemFree_v2 *cuMemFree = nullptr;
    tcuMemcpyDtoD_v2 *cuMemcpyDtoD = nullptr;
    tcuMemcpyHtoD_v2 *cuMemcpyHtoD = nullptr;
    tcuMemcpyDtoH_v2 *cuMemcpyDtoH = nullptr;
    tcuStreamSynchronize *cuStreamSynchronize = nullptr;
    tcuGetErrorString *cuGetErrorString = nullptr;
    tcuImportExternalMemory *cuImportExternalMemory = nullptr;
    tcuExternalMemoryGetMappedBuffer *cuExternalMemoryGetMappedBuffer = nullptr;
    tcuDestroyExternalMemory *cuDestroyExternalMemory = nullptr;
  };

  /**
   * One CUDA device with a private context. Never destroyed. A private context (not the
   * primary one) cannot clash with FFmpeg's NVENC, which sets its own primary-context flags.
   */
  struct driver_t {
    functions_t fn;
    CUdevice device = 0;
    CUcontext context = nullptr;
  };

  /**
   * @brief Load libcuda and create a private context on one device.
   * @param vulkan_uuid 16-byte VkPhysicalDeviceIDProperties::deviceUUID, or null for device 0.
   * @details The first successful call fixes the device for the process. A later call that
   *          names a different device fails, because frames cannot move between GPUs.
   */
  driver_t *acquire(const std::uint8_t *vulkan_uuid, std::string &error);

  /** Makes the context current on this thread for the scope. */
  class scoped_context_t {
  public:
    explicit scoped_context_t(driver_t &driver);
    ~scoped_context_t();
    scoped_context_t(const scoped_context_t &) = delete;
    scoped_context_t &operator=(const scoped_context_t &) = delete;

    bool ok() const {
      return pushed;
    }

  private:
    driver_t &driver;
    bool pushed;
  };

  /** A Vulkan buffer (opaque-fd export) mapped as CUDA device memory. */
  class external_buffer_t {
  public:
    /** Takes ownership of @p fd on success. */
    static std::unique_ptr<external_buffer_t> import(driver_t &driver, int fd, std::size_t allocation_size, std::size_t size, std::string &error);
    ~external_buffer_t();

    /** Device-to-device copy into the shared buffer; returns once the copy has finished. */
    bool copy_from(CUdeviceptr source, std::size_t bytes, std::string &error);

  private:
    external_buffer_t(driver_t &driver);
    driver_t &driver;
    CUexternalMemory memory = nullptr;
    CUdeviceptr mapped = 0;
  };

  /** Describe a CUDA error for logs. */
  std::string describe(driver_t &driver, CUresult result);
}  // namespace linux_cuda
