#pragma once
// Experimental SDR same-adapter desktop composition, enabled explicitly.
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace platf::dxgi {
  // Meteor's depth only covers the primary monitor, but with several monitors tiled into one
  // frame it can't tell where that is. Publish its rectangle in the capture frame (frame size,
  // then x y w h) at the path Meteor names; remove the file when one monitor fills the frame.
  inline void publish_capture_region(const desktop_manifest_t &manifest, size_t outputs) {
    const wchar_t *path = _wgetenv(L"METEOR_CAPTURE_REGION_FILE");
    if (!path || !*path) return;
    const std::filesystem::path file {path};
    std::error_code ignored;
    const auto primary = std::find_if(manifest.monitors.begin(), manifest.monitors.end(),
                                      [](const auto &m) { return m.enabled && m.is_primary; });
    if (outputs < 2 || primary == manifest.monitors.end()) {
      std::filesystem::remove(file, ignored);
      return;
    }
    std::ofstream out {file, std::ios::trunc};
    out << manifest.frame_width << ' ' << manifest.frame_height << ' ' << primary->frame_x << ' ' << primary->frame_y
        << ' ' << primary->frame_w << ' ' << primary->frame_h << '\n';
  }

  class display_multi_t: public display_vram_t {
    struct source_t { DXGI_OUTPUT_DESC desc; dup_t duplication; bool ready = false; int frame_x = 0, frame_y = 0; };
    bool tiled = false;
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
        const auto monitor = std::find_if(manifest->monitors.begin(),manifest->monitors.end(),[&](const auto &m) { return m.label == label && m.enabled; });
        if (monitor == manifest->monitors.end()) continue;
        source.frame_x = monitor->frame_x; source.frame_y = monitor->frame_y;
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
      tiled = manifest->tiled;
      if (tiled) {
        // Too large for one picture: the monitors sit in rows of a smaller frame.
        // Input arrives as a point in that frame, so publish the tiles and let the
        // shared input path map it back to the desktop (absolute coordinates, hence
        // the virtual-screen origin as the offset).
        width = width_before_rotation = manifest->frame_width;
        height = height_before_rotation = manifest->frame_height;
        frame_tiles.clear();
        for (const auto &m : manifest->monitors) {
          if (m.enabled) frame_tiles.push_back({m.frame_x, m.frame_y, m.desktop_x, m.desktop_y, m.frame_w, m.frame_h});
        }
      } else {
        width = width_before_rotation = bounds.right-bounds.left;
        height = height_before_rotation = bounds.bottom-bounds.top;
      }
      if (width > 8192 || height > 8192 || (int64_t)width*height > 35389440) return -1;
      offset_x = tiled ? -GetSystemMetrics(SM_XVIRTUALSCREEN) : bounds.left-GetSystemMetrics(SM_XVIRTUALSCREEN);
      offset_y = tiled ? -GetSystemMetrics(SM_YVIRTUALSCREEN) : bounds.top-GetSystemMetrics(SM_YVIRTUALSCREEN);
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
      BOOST_LOG(info) << "Experimental DXGI multi-output capture: " << sources.size() << " outputs, " << width << 'x' << height << (tiled ? " (tiled)" : "");
      publish_capture_region(*manifest, sources.size());
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
        device_ctx->CopySubresourceRegion(composite.get(),0,tiled ? source.frame_x : r.left-bounds.left,tiled ? source.frame_y : r.top-bounds.top,0,image.get(),0,nullptr);
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
        // The cursor is drawn where its monitor sits in the frame; in a tiled frame that is
        // the monitor's tile, and a cursor outside every captured monitor is left out.
        int cursor_x = cursor.ptScreenPos.x-bounds.left, cursor_y = cursor.ptScreenPos.y-bounds.top;
        bool cursor_on_frame = true;
        if (tiled) {
          cursor_on_frame = false;
          for (const auto &source : sources) {
            const auto r = source.desc.DesktopCoordinates;
            if (cursor.ptScreenPos.x >= r.left && cursor.ptScreenPos.x < r.right &&
                cursor.ptScreenPos.y >= r.top && cursor.ptScreenPos.y < r.bottom) {
              cursor_x = source.frame_x + cursor.ptScreenPos.x-r.left;
              cursor_y = source.frame_y + cursor.ptScreenPos.y-r.top;
              cursor_on_frame = true;
              break;
            }
          }
        }
        if (cursor_on_frame && GetIconInfo(cursor.hCursor,&icon)) {
          DrawIconEx(dc,cursor_x-icon.xHotspot,
            cursor_y-icon.yHotspot,cursor.hCursor,0,0,0,nullptr,DI_NORMAL);
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
