/**
 * @file src/platform/linux/nvfbc_vk.h
 * @brief NvFBC capture that keeps frames in GPU memory for the Vulkan PyroWave encoder.
 * @details Works without nvcc: NvFBC grabs into CUDA device memory, the images carry that device
 *          pointer, and the PyroWave core shares a Vulkan buffer with CUDA to read it directly.
 */
#pragma once

#include "cuda_shared.h"
#include "src/platform/common.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace video {
  struct config_t;
}

namespace platf {
  /** Captured frame. PyroWave sessions hold only @ref device_ptr; other codecs also get host @c data. */
  struct nvfbc_vk_img_t: public img_t {
    ~nvfbc_vk_img_t() override;

    CUdeviceptr device_ptr {};  ///< Pitch-linear BGRA8 in the shared CUDA context; row_pitch bytes per row
    std::size_t bytes {};
    bool owns_host_data {};
  };

  std::vector<std::string> nvfbc_vk_display_names();
  std::shared_ptr<display_t> nvfbc_vk_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);
}  // namespace platf
