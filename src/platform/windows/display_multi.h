#pragma once
// Experimental SDR same-adapter desktop composition, enabled explicitly.
namespace platf::dxgi {
  class display_multi_t: public display_vram_t {
    struct source_t { DXGI_OUTPUT_DESC desc; dup_t duplication; bool ready = false; };
    std::vector<source_t> sources;
    texture2d_t composite;
    texture2d_t cursor_surface;
    RECT bounds {};
  public:
    int init(const ::video::config_t &config, const std::string &name, const std::optional<LUID> &luid) {
      if (display_base_t::init(config, name, false, luid)) return -1;
      const auto manifest = query_multi_manifest(capture_outputs::get());
      if (!manifest) return -1;
      for (UINT i = 0;; ++i) {
        output_t::pointer raw = nullptr;
        HRESULT status = adapter->EnumOutputs(i, &raw);
        if (status == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(status)) return -1;
        output_t candidate {raw};
        source_t source {};
        if (FAILED(candidate->GetDesc(&source.desc))) return -1;
        if (!source.desc.AttachedToDesktop) continue;
        const auto label = utf_utils::to_utf8(source.desc.DeviceName);
        if (std::none_of(manifest->monitors.begin(),manifest->monitors.end(),[&](const auto &m) { return m.label == label && m.enabled; })) continue;
        if (source.desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) return -1;
        output1_t output1;
        if (FAILED(candidate->QueryInterface(IID_IDXGIOutput1, (void **)&output1))) return -1;
        if (FAILED(output1->DuplicateOutput(device.get(), &source.duplication))) return -1;
        const auto r = source.desc.DesktopCoordinates;
        if (sources.empty()) bounds = r;
        else { bounds.left = std::min(bounds.left,r.left); bounds.top = std::min(bounds.top,r.top);
          bounds.right = std::max(bounds.right,r.right); bounds.bottom = std::max(bounds.bottom,r.bottom); }
        sources.push_back(std::move(source));
      }
      if (sources.empty()) return -1;
      width = width_before_rotation = bounds.right-bounds.left;
      height = height_before_rotation = bounds.bottom-bounds.top;
      if (width > 8192 || height > 8192 || (int64_t)width*height > 35389440) return -1;
      offset_x = bounds.left-GetSystemMetrics(SM_XVIRTUALSCREEN);
      offset_y = bounds.top-GetSystemMetrics(SM_YVIRTUALSCREEN);
      display_rotation = DXGI_MODE_ROTATION_IDENTITY;
      capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
      desc.Format = capture_format; desc.SampleDesc.Count = 1;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (FAILED(device->CreateTexture2D(&desc,nullptr,&composite))) return -1;
      desc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
      if (FAILED(device->CreateTexture2D(&desc,nullptr,&cursor_surface))) return -1;
      render_target_t target;
      if (FAILED(device->CreateRenderTargetView(composite.get(),nullptr,&target))) return -1;
      const float black[4] = {0,0,0,1}; device_ctx->ClearRenderTargetView(target.get(),black);
      BOOST_LOG(info) << "Experimental DXGI multi-output capture: " << sources.size() << " outputs, " << width << 'x' << height;
      return 0;
    }
    bool is_hdr() override { return false; }
    int dummy_img(img_t *image) override {
      if (!std::getenv("METEOR_WINDOWS_MULTIMONITOR_PROBE")) return display_vram_t::dummy_img(image);
      const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
      while (std::chrono::steady_clock::now() < deadline) {
        std::shared_ptr<platf::img_t> captured;
        auto status = snapshot([&](std::shared_ptr<platf::img_t> &target) {
          target = std::shared_ptr<platf::img_t>(image,[](platf::img_t *) {}); return true;
        },captured,std::chrono::milliseconds(100),true);
        if (status == capture_e::ok) {
          BOOST_LOG(info) << "Multi-output encoder probe supplied captured desktop pixels";
          return 0;
        }
        if (status != capture_e::timeout) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return -1;
    }
    bool get_hdr_metadata(SS_HDR_METADATA &) override { return false; }
    capture_e release_snapshot() override { return capture_e::ok; }
    capture_e snapshot(const pull_free_image_cb_t &pull, std::shared_ptr<platf::img_t> &result,
                       std::chrono::milliseconds, bool cursor_visible) override {
      bool changed = false;
      LONGLONG latest_qpc = 0;
      for (auto &source : sources) {
        DXGI_OUTDUPL_FRAME_INFO info {};
        resource_t resource;
        HRESULT status = source.duplication->AcquireNextFrame(source.ready ? 0 : 100, &info, &resource);
        if (status == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (status == DXGI_ERROR_ACCESS_LOST) return capture_e::reinit;
        if (FAILED(status)) return capture_e::error;
        latest_qpc = std::max(latest_qpc,std::max(info.LastPresentTime.QuadPart,info.LastMouseUpdateTime.QuadPart));
        auto release = util::fail_guard([&] { source.duplication->ReleaseFrame(); });
        texture2d_t image;
        if (FAILED(resource->QueryInterface(IID_ID3D11Texture2D,(void **)&image))) return capture_e::error;
        D3D11_TEXTURE2D_DESC desc; image->GetDesc(&desc);
        const auto r = source.desc.DesktopCoordinates;
        if (desc.Format != capture_format || desc.Width != (UINT)(r.right-r.left) || desc.Height != (UINT)(r.bottom-r.top)) return capture_e::reinit;
        device_ctx->CopySubresourceRegion(composite.get(),0,r.left-bounds.left,r.top-bounds.top,0,image.get(),0,nullptr);
        source.ready = true; changed = true;
      }
      if (!changed || std::any_of(sources.begin(),sources.end(),[](const auto &s) { return !s.ready; })) return capture_e::timeout;
      if (!pull(result)) return capture_e::interrupted;
      auto image = std::static_pointer_cast<img_d3d_t>(result);
      if (complete_img(image.get(),false)) return capture_e::error;
      texture_lock_helper lock(image->capture_mutex.get());
      if (!lock.lock()) return capture_e::error;
      ID3D11Texture2D *source_image = composite.get();
      CURSORINFO cursor {sizeof(CURSORINFO)};
      if (cursor_visible && GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING)) {
        // Draw on a separate surface so a moved cursor never leaves pixels
        // behind on monitors that have not produced a new desktop frame.
        device_ctx->CopyResource(cursor_surface.get(),composite.get());
        Microsoft::WRL::ComPtr<IDXGISurface1> surface;
        if (FAILED(cursor_surface->QueryInterface(IID_PPV_ARGS(&surface)))) return capture_e::error;
        HDC dc;
        if (FAILED(surface->GetDC(FALSE,&dc))) return capture_e::error;
        ICONINFO icon {};
        if (GetIconInfo(cursor.hCursor,&icon)) {
          DrawIconEx(dc,cursor.ptScreenPos.x-bounds.left-icon.xHotspot,
            cursor.ptScreenPos.y-bounds.top-icon.yHotspot,cursor.hCursor,0,0,0,nullptr,DI_NORMAL);
          if (icon.hbmMask) DeleteObject(icon.hbmMask);
          if (icon.hbmColor) DeleteObject(icon.hbmColor);
        }
        if (FAILED(surface->ReleaseDC(nullptr))) return capture_e::error;
        source_image = cursor_surface.get();
      }
      device_ctx->CopyResource(image->capture_texture.get(),source_image);
      image->blank = false;
      result->host_processing_timestamp = std::chrono::steady_clock::now();
      result->frame_timestamp = latest_qpc ? std::chrono::steady_clock::now()-qpc_time_difference(qpc_counter(),latest_qpc)
                                           : result->host_processing_timestamp;
      result->capture_pacing_timestamp = result->frame_timestamp;
      return capture_e::ok;
    }
  };
  std::shared_ptr<display_t> create_multi_display(const ::video::config_t &config, const std::string &name, const std::optional<LUID> &luid) {
    auto display = std::make_shared<display_multi_t>();
    if (display->init(config,name,luid)) return nullptr;
    return display;
  }
}
