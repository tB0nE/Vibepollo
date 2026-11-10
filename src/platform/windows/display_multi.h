#pragma once
// Experimental SDR same-adapter desktop composition, enabled explicitly.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace platf::dxgi {
  // Meteor's depth only covers the primary screen, but with several screens tiled into one
  // frame it can't tell where that is. Publish its rectangle in the frame (frame size, then
  // x y w h) at the path Meteor names; remove the file when one screen fills the frame.
  inline void publish_capture_region(const display_plan::plan_t &plan) {
    const wchar_t *path = _wgetenv(L"METEOR_CAPTURE_REGION_FILE");
    if (!path || !*path) return;
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
        << ' ' << (*primary)->frame_w << ' ' << (*primary)->frame_h << '\n';
  }

  // Composes the session's screens into the frame its plan describes. The frame never changes
  // size while the session runs: a screen whose output isn't attached yet stays black, and the
  // capture restarts once it appears, so clients, input and depth all keep one geometry.
  class display_multi_t: public display_vram_t {
    struct source_t {
      DXGI_OUTPUT_DESC desc;
      dup_t duplication;
      bool ready = false;
      int frame_x = 0, frame_y = 0, tile_w = 0, tile_h = 0;
      int out_w = 0, out_h = 0;  // the output's size on the desktop
      // Shrunk to fit the encoder: drawn whole into its smaller tile, from a copy a shader can read.
      bool scaled = false;
      texture2d_t copy;
      shader_res_t copy_view;
    };
    display_plan::plan_t plan;
    std::vector<source_t> sources;
    std::size_t missing = 0;
    std::chrono::steady_clock::time_point next_missing_check {};
    texture2d_t composite;
    texture2d_t cursor_surface;
    render_target_t composite_target;
    vs_t scale_vs;
    ps_t scale_ps;
    sampler_state_t scale_sampler;
    buf_t scale_rotation;
  public:
    int init(const ::video::config_t &config, const std::string &name, const std::optional<LUID> &luid) {
      if (display_base_t::init(config, name, false, luid)) return -1;
      auto planned = session_plan();
      const auto attached = attached_outputs();
      if (!planned || !attached) return -1;
      plan = std::move(*planned);
      const auto placed = place(plan, *attached);
      frame_tiles.clear();
      for (std::size_t i = 0; i < plan.screens.size(); ++i) {
        const auto &screen = plan.screens[i];
        if (!screen.enabled) continue;
        if (!placed[i]) {
          ++missing;
          BOOST_LOG(info) << "Multi-output capture: waiting for " << screen.label << " to attach";
          continue;
        }
        for (UINT o = 0;; ++o) {
          output_t::pointer raw = nullptr;
          HRESULT status = adapter->EnumOutputs(o, &raw);
          if (status == DXGI_ERROR_NOT_FOUND) break;
          if (FAILED(status)) return -1;
          output_t candidate {raw};
          source_t source {};
          if (FAILED(candidate->GetDesc(&source.desc)) || !source.desc.AttachedToDesktop) continue;
          if (utf_utils::to_utf8(source.desc.DeviceName) != placed[i]->name) continue;
          if (source.desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) return -1;
          output1_t output1;
          if (FAILED(candidate->QueryInterface(IID_IDXGIOutput1, (void **) &output1))) return -1;
          if (FAILED(output1->DuplicateOutput(device.get(), &source.duplication))) return -1;
          const auto r = source.desc.DesktopCoordinates;
          source.frame_x = screen.frame_x;
          source.frame_y = screen.frame_y;
          source.out_w = r.right - r.left;
          source.out_h = r.bottom - r.top;
          const int frame_w = screen.frame_w > 0 ? screen.frame_w : screen.width;
          const int frame_h = screen.frame_h > 0 ? screen.frame_h : screen.height;
          source.scaled = frame_w < screen.width || frame_h < screen.height;
          if (source.scaled) {
            source.tile_w = frame_w;
            source.tile_h = frame_h;
          } else {
            source.tile_w = std::min<int>(frame_w, source.out_w);
            source.tile_h = std::min<int>(frame_h, source.out_h);
          }
          // Input lands on the output's desktop position, read now, for the tile it fills; a
          // scaled tile spreads across the whole output.
          frame_tiles.push_back({screen.frame_x, screen.frame_y, (int) r.left, (int) r.top, source.tile_w, source.tile_h,
                                 source.scaled ? source.out_w : 0, source.scaled ? source.out_h : 0});
          sources.push_back(std::move(source));
          break;
        }
      }
      width = width_before_rotation = plan.frame_width;
      height = height_before_rotation = plan.frame_height;
      // Frame points map to absolute desktop positions (frame_tiles); the mouse API's origin
      // is the virtual screen's top-left.
      offset_x = -GetSystemMetrics(SM_XVIRTUALSCREEN);
      offset_y = -GetSystemMetrics(SM_YVIRTUALSCREEN);
      display_rotation = DXGI_MODE_ROTATION_IDENTITY;
      capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
      desc.Format = capture_format; desc.SampleDesc.Count = 1;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (FAILED(device->CreateTexture2D(&desc,nullptr,&composite))) return -1;
      desc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
      if (FAILED(device->CreateTexture2D(&desc,nullptr,&cursor_surface))) return -1;
      if (FAILED(device->CreateRenderTargetView(composite.get(),nullptr,&composite_target))) return -1;
      const float black[4] = {0,0,0,1}; device_ctx->ClearRenderTargetView(composite_target.get(),black);
      if (std::any_of(sources.begin(), sources.end(), [](const auto &s) { return s.scaled; }) && init_scaling()) return -1;
      next_missing_check = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      BOOST_LOG(info) << "Multi-output capture: " << sources.size() << " of " << plan.enabled().size()
                      << " screens, frame " << width << 'x' << height;
      publish_capture_region(plan);
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
      if (missing && std::chrono::steady_clock::now() >= next_missing_check) {
        next_missing_check = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        if (const auto attached = attached_outputs()) {
          const auto placed = place(plan, *attached);
          const auto found = std::count_if(placed.begin(), placed.end(), [](const auto &p) { return p.has_value(); });
          if ((std::size_t) found > sources.size()) return capture_e::reinit;
        }
      }
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
        if (source.scaled) {
          if (draw_scaled(source, image.get())) return capture_e::error;
        } else {
          // A screen larger than its planned tile is cropped to it rather than spilling into the next.
          const D3D11_BOX box {0, 0, 0, (UINT) source.tile_w, (UINT) source.tile_h, 1};
          device_ctx->CopySubresourceRegion(composite.get(),0,source.frame_x,source.frame_y,0,image.get(),0,&box);
        }
        source.ready = true; changed = true;
      }
      if (sources.empty()) {
        // Nothing attached yet: send one black frame so the stream starts at its final size.
        if (sent_blank) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          return capture_e::timeout;
        }
        sent_blank = changed = true;
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
        // behind on screens that have not produced a new desktop frame.
        device_ctx->CopyResource(cursor_surface.get(),composite.get());
        Microsoft::WRL::ComPtr<IDXGISurface1> surface;
        if (FAILED(cursor_surface->QueryInterface(IID_PPV_ARGS(&surface)))) return capture_e::error;
        HDC dc;
        if (FAILED(surface->GetDC(FALSE,&dc))) return capture_e::error;
        // The cursor is drawn in the tile of the screen it is on; off every streamed screen,
        // it is left out.
        for (const auto &source : sources) {
          const auto r = source.desc.DesktopCoordinates;
          // The part of the output its tile shows: all of it when scaled, else what fits.
          const int shown_w = source.scaled ? source.out_w : source.tile_w;
          const int shown_h = source.scaled ? source.out_h : source.tile_h;
          if (cursor.ptScreenPos.x < r.left || cursor.ptScreenPos.x >= r.left + shown_w ||
              cursor.ptScreenPos.y < r.top || cursor.ptScreenPos.y >= r.top + shown_h) continue;
          ICONINFO icon {};
          if (GetIconInfo(cursor.hCursor,&icon)) {
            // In a scaled tile the cursor shrinks with the screen around it.
            const double sx = (double) source.tile_w / shown_w, sy = (double) source.tile_h / shown_h;
            int cursor_w = 0, cursor_h = 0;
            if (source.scaled) {
              BITMAP bm {};
              if (icon.hbmColor && GetObject(icon.hbmColor, sizeof(bm), &bm)) {
                cursor_w = bm.bmWidth; cursor_h = bm.bmHeight;
              } else if (icon.hbmMask && GetObject(icon.hbmMask, sizeof(bm), &bm)) {
                cursor_w = bm.bmWidth; cursor_h = bm.bmHeight / 2;  // AND and XOR masks stacked
              }
              cursor_w = std::max(1, (int) std::lround(cursor_w * sx));
              cursor_h = std::max(1, (int) std::lround(cursor_h * sy));
            }
            DrawIconEx(dc,source.frame_x + (int) std::lround((cursor.ptScreenPos.x - r.left - (LONG) icon.xHotspot) * sx),
              source.frame_y + (int) std::lround((cursor.ptScreenPos.y - r.top - (LONG) icon.yHotspot) * sy),
              cursor.hCursor,cursor_w,cursor_h,0,nullptr,DI_NORMAL);
            if (icon.hbmMask) DeleteObject(icon.hbmMask);
            if (icon.hbmColor) DeleteObject(icon.hbmColor);
          }
          break;
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
  private:
    bool sent_blank = false;

    int init_scaling() {
      if (!cursor_vs_hlsl || !cursor_ps_hlsl) {
        BOOST_LOG(error) << "Multi-output capture: the scaling shaders are missing";
        return -1;
      }
      if (FAILED(device->CreateVertexShader(cursor_vs_hlsl->GetBufferPointer(), cursor_vs_hlsl->GetBufferSize(), nullptr, &scale_vs)) ||
          FAILED(device->CreatePixelShader(cursor_ps_hlsl->GetBufferPointer(), cursor_ps_hlsl->GetBufferSize(), nullptr, &scale_ps))) {
        return -1;
      }
      D3D11_SAMPLER_DESC sampler {};
      sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sampler.MaxLOD = D3D11_FLOAT32_MAX;
      if (FAILED(device->CreateSamplerState(&sampler, &scale_sampler))) return -1;
      int32_t no_rotation[16 / sizeof(int32_t)] {};
      scale_rotation = make_buffer(device.get(), no_rotation);
      if (!scale_rotation) return -1;
      for (auto &source : sources) {
        if (!source.scaled) continue;
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = source.out_w; desc.Height = source.out_h; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = capture_format; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &source.copy)) ||
            FAILED(device->CreateShaderResourceView(source.copy.get(), nullptr, &source.copy_view))) {
          return -1;
        }
        BOOST_LOG(info) << "Multi-output capture: " << source.out_w << 'x' << source.out_h << " screen scaled to "
                        << source.tile_w << 'x' << source.tile_h << " to fit the encoder";
      }
      return 0;
    }

    // Draws the whole output, shrunk, into its tile.
    int draw_scaled(source_t &source, ID3D11Texture2D *image) {
      device_ctx->CopyResource(source.copy.get(), image);
      const D3D11_VIEWPORT viewport {(float) source.frame_x, (float) source.frame_y, (float) source.tile_w, (float) source.tile_h, 0.0f, 1.0f};
      auto *target = composite_target.get();
      auto *view = source.copy_view.get();
      auto *sampler = scale_sampler.get();
      auto *rotation = scale_rotation.get();
      device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      device_ctx->IASetInputLayout(nullptr);
      device_ctx->VSSetShader(scale_vs.get(), nullptr, 0);
      device_ctx->VSSetConstantBuffers(2, 1, &rotation);
      device_ctx->PSSetShader(scale_ps.get(), nullptr, 0);
      device_ctx->PSSetShaderResources(0, 1, &view);
      device_ctx->PSSetSamplers(0, 1, &sampler);
      device_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
      device_ctx->OMSetRenderTargets(1, &target, nullptr);
      device_ctx->RSSetViewports(1, &viewport);
      device_ctx->Draw(3, 0);
      // Unbind, so the copies that follow don't find the composite still bound as a target.
      ID3D11ShaderResourceView *no_view = nullptr;
      device_ctx->PSSetShaderResources(0, 1, &no_view);
      device_ctx->OMSetRenderTargets(0, nullptr, nullptr);
      return 0;
    }
  };
  std::shared_ptr<display_t> create_multi_display(const ::video::config_t &config, const std::string &name, const std::optional<LUID> &luid) {
    auto display = std::make_shared<display_multi_t>();
    if (display->init(config,name,luid)) return nullptr;
    return display;
  }
}
