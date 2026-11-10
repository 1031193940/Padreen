#include "capture.h"

#include <algorithm>

namespace ph {

CursorShape fallbackArrow() {
    static const char* A[19] = {"X...........", "XX..........", "X.X.........", "X..X........", "X...X.......", "X....X......", "X.....X.....",
                                "X......X....", "X.......X...", "X........X..", "X.....XXXXX.", "X..X..X.....", "X.X.X..X....", "XX..X..X....",
                                "X....X..X...", ".....X..X...", "......X..X..", "......X..X..", ".......XX..."};
    CursorShape c;
    c.w = 14;
    c.h = 21;
    c.hotX = 1;
    c.hotY = 1;
    c.gray.assign((size_t)c.w * c.h, 255);
    c.alpha.assign((size_t)c.w * c.h, 0);
    auto mask = [&](int x, int y) { return x >= 0 && y >= 0 && y < 19 && x < 12 && A[y][x] == 'X'; };
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
            bool halo = false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) halo = halo || mask(x - 1 + dx, y - 1 + dy);
            size_t o = (size_t)y * c.w + x;
            if (mask(x - 1, y - 1)) {
                c.gray[o] = 0;
                c.alpha[o] = 255;
            } else if (halo) {
                c.gray[o] = 255;
                c.alpha[o] = 255;
            }
        }
    return c;
}
bool Duplicator::init(const std::wstring& gdiName) {
    shutdown();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a, adapter.Reset()) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i, o.Reset()) {
            DXGI_OUTPUT_DESC d;
            o->GetDesc(&d);
            if (_wcsicmp(d.DeviceName, gdiName.c_str()) == 0) {
                output = o;
                break;
            }
        }
        if (output) break;
    }
    if (!output) {
        logf("capture: output %s not found in DXGI", narrow(gdiName).c_str());
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, (UINT)(sizeof(levels) / sizeof(levels[0])),
                                   D3D11_SDK_VERSION, &dev_, &got, &ctx_);
    if (FAILED(hr)) {
        logf("capture: D3D11CreateDevice failed 0x%08x", (unsigned)hr);
        return false;
    }
    ComPtr<IDXGIOutput1> o1;
    if (FAILED(output.As(&o1))) return false;
    hr = o1->DuplicateOutput(dev_.Get(), &dup_);
    if (FAILED(hr)) {
        logf("capture: DuplicateOutput failed 0x%08x", (unsigned)hr);
        return false;
    }
    DXGI_OUTDUPL_DESC dd;
    dup_->GetDesc(&dd);
    w_ = (int)dd.ModeDesc.Width;
    h_ = (int)dd.ModeDesc.Height;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w_;
    td.Height = h_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging_))) return false;
    shadow_.assign((size_t)w_ * h_ * 4, 0);
    first_ = true;
    ptr_ = PointerInfo();
    cursor_ = CursorShape();
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    logf("capture: duplicating %s %dx%d on adapter '%s'", narrow(gdiName).c_str(), w_, h_, narrow(ad.Description).c_str());
    return true;
}

void Duplicator::shutdown() {
    staging_.Reset();
    dup_.Reset();
    ctx_.Reset();
    dev_.Reset();
}

CapResult Duplicator::acquire(UINT timeoutMs, CapFrame& out) {
    out.rects.clear();
    out.pointer = PointerInfo();
    if (!dup_) return CapResult::Fail;
    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return CapResult::Timeout;
    if (hr == DXGI_ERROR_ACCESS_LOST) return CapResult::Lost;
    if (FAILED(hr)) {
        logf("capture: AcquireNextFrame 0x%08x", (unsigned)hr);
        return CapResult::Fail;
    }
    bool haveImage = info.LastPresentTime.QuadPart != 0;
    if (haveImage) {
        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        std::vector<RECT> rects;
        if (first_) {
            rects.push_back(RECT{0, 0, w_, h_});
            first_ = false;
        } else if (info.TotalMetadataBufferSize) {
            meta_.resize(info.TotalMetadataBufferSize);
            UINT used = 0;
            // move rects first: their destinations changed too (the texture already holds the moved pixels)
            UINT movBytes = info.TotalMetadataBufferSize;
            if (SUCCEEDED(dup_->GetFrameMoveRects(movBytes, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data()), &used))) {
                auto* mr = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data());
                for (UINT i = 0; i < used / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) rects.push_back(mr[i].DestinationRect);
            }
            UINT dirtyBytes = info.TotalMetadataBufferSize;
            UINT usedD = 0;
            std::vector<uint8_t> dbuf(dirtyBytes);
            if (SUCCEEDED(dup_->GetFrameDirtyRects(dirtyBytes, reinterpret_cast<RECT*>(dbuf.data()), &usedD))) {
                auto* dr = reinterpret_cast<RECT*>(dbuf.data());
                for (UINT i = 0; i < usedD / sizeof(RECT); ++i) rects.push_back(dr[i]);
            }
        }
        // clamp, drop empties
        std::vector<RECT> clean;
        for (RECT r : rects) {
            r.left = std::max<LONG>(0, r.left);
            r.top = std::max<LONG>(0, r.top);
            r.right = std::min<LONG>(w_, r.right);
            r.bottom = std::min<LONG>(h_, r.bottom);
            if (r.right > r.left && r.bottom > r.top) clean.push_back(r);
        }
        if (clean.size() > 8) {                    // many small rects: one bounding copy is cheaper than many tiny ones
            RECT u = clean[0];
            for (auto& r : clean) {
                u.left = std::min(u.left, r.left);
                u.top = std::min(u.top, r.top);
                u.right = std::max(u.right, r.right);
                u.bottom = std::max(u.bottom, r.bottom);
            }
            clean.assign(1, u);
        }
        if (!clean.empty()) {
            for (auto& r : clean) {
                D3D11_BOX box = {(UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1};
                ctx_->CopySubresourceRegion(staging_.Get(), 0, r.left, r.top, 0, tex.Get(), 0, &box);
            }
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                for (auto& r : clean) {
                    size_t bytes = (size_t)(r.right - r.left) * 4;
                    for (LONG y = r.top; y < r.bottom; ++y)
                        memcpy(&shadow_[((size_t)y * w_ + r.left) * 4], static_cast<const uint8_t*>(m.pData) + (size_t)y * m.RowPitch + (size_t)r.left * 4, bytes);
                }
                ctx_->Unmap(staging_.Get(), 0);
            }
        }
        out.rects = std::move(clean);
    }
    out.doneNs = nowNs();
    // pointer
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        bool vis = info.PointerPosition.Visible != 0;
        int px = info.PointerPosition.Position.x, py = info.PointerPosition.Position.y;
        if (vis != ptr_.visible || px != ptr_.x || py != ptr_.y) out.pointer.moved = true;
        ptr_.visible = vis;
        ptr_.x = px;
        ptr_.y = py;
        if (info.PointerShapeBufferSize > 0) {
            readPointerShape(info);
            out.pointer.shapeUpdated = true;
        }
    }
    out.pointer.visible = ptr_.visible;
    out.pointer.x = ptr_.x;
    out.pointer.y = ptr_.y;
    dup_->ReleaseFrame();
    return CapResult::Ok;
}

void Duplicator::readPointerShape(const DXGI_OUTDUPL_FRAME_INFO& info) {
    shapeBuf_.resize(info.PointerShapeBufferSize);
    UINT need = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
    if (FAILED(dup_->GetFramePointerShape((UINT)shapeBuf_.size(), shapeBuf_.data(), &need, &si))) return;
    CursorShape c;
    c.hotX = si.HotSpot.x;
    c.hotY = si.HotSpot.y;
    c.w = (int)si.Width;
    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        c.h = (int)si.Height / 2;
        c.gray.assign((size_t)c.w * c.h, 0);
        c.alpha.assign((size_t)c.w * c.h, 0);
        c.inv.assign((size_t)c.w * c.h, 0);
        for (int y = 0; y < c.h; ++y)
            for (int x = 0; x < c.w; ++x) {
                const uint8_t* andRow = shapeBuf_.data() + (size_t)y * si.Pitch;
                const uint8_t* xorRow = shapeBuf_.data() + (size_t)(y + c.h) * si.Pitch;
                int a = (andRow[x >> 3] >> (7 - (x & 7))) & 1;
                int xr = (xorRow[x >> 3] >> (7 - (x & 7))) & 1;
                size_t o = (size_t)y * c.w + x;
                if (a == 0) {
                    c.gray[o] = xr ? 255 : 0;
                    c.alpha[o] = 255;
                } else if (xr) {
                    c.inv[o] = 1;                    // AND=1, XOR=1: invert whatever is underneath
                    c.alpha[o] = 255;
                }
            }
    } else {   // COLOR (BGRA, alpha) or MASKED_COLOR (alpha = XOR flag)
        c.h = (int)si.Height;
        c.gray.assign((size_t)c.w * c.h, 0);
        c.alpha.assign((size_t)c.w * c.h, 0);
        for (int y = 0; y < c.h; ++y)
            for (int x = 0; x < c.w; ++x) {
                const uint8_t* p = shapeBuf_.data() + (size_t)y * si.Pitch + (size_t)x * 4;
                size_t o = (size_t)y * c.w + x;
                c.gray[o] = (uint8_t)((p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8);
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    c.alpha[o] = p[3];
                } else if (p[3] == 0xFF) {            // MASKED_COLOR: mask 0xFF = XOR the color with the screen
                    if (c.inv.empty()) c.inv.assign((size_t)c.w * c.h, 0);
                    if (c.gray[o] > 127) {
                        c.inv[o] = 1;
                        c.alpha[o] = 255;
                    }
                } else {
                    c.alpha[o] = 255;
                }
            }
    }
    cursor_ = std::move(c);
}

}  // namespace ph
