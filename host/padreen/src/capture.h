#pragma once
#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace ph {

using Microsoft::WRL::ComPtr;

// Mouse pointer as gray + alpha (Desktop Duplication delivers it separately from the desktop image).
struct CursorShape {
    int w = 0, h = 0, hotX = 0, hotY = 0;
    std::vector<uint8_t> gray, alpha;        // w*h each
    std::vector<uint8_t> inv;                // w*h: 1 = invert the pixel underneath (XOR cursors such as the text I-beam)
    bool valid() const { return w > 0 && h > 0; }
};

struct PointerInfo {
    bool visible = false;
    int x = 0, y = 0;            // top-left of the shape (position already minus hot spot is NOT applied: x,y = pointer position)
    bool moved = false;          // position / visibility changed since the previous frame
    bool shapeUpdated = false;
};

// Built-in arrow used until Windows delivers the real pointer shape.
CursorShape fallbackArrow();

enum class CapResult { Ok, Timeout, Lost, Fail };

struct CapFrame {
    std::vector<RECT> rects;     // regions of the BGRA shadow that were refreshed
    PointerInfo pointer;
    int64_t doneNs = 0;          // when the pixels were in CPU memory
};

// Desktop Duplication of ONE output (the virtual monitor).  Keeps a CPU shadow of the whole desktop image.
class Duplicator {
public:
    bool init(const std::wstring& gdiName);
    void shutdown();
    CapResult acquire(UINT timeoutMs, CapFrame& out);

    int width() const { return w_; }
    int height() const { return h_; }
    const uint8_t* bgra() const { return shadow_.data(); }
    int stride() const { return w_ * 4; }
    const CursorShape& cursor() const { return cursor_; }
    PointerInfo pointer() const { return ptr_; }

private:
    void readPointerShape(const DXGI_OUTDUPL_FRAME_INFO& info);

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> staging_;
    std::vector<uint8_t> shadow_;
    std::vector<uint8_t> meta_;
    std::vector<uint8_t> shapeBuf_;
    CursorShape cursor_;
    PointerInfo ptr_;
    int w_ = 0, h_ = 0;
    bool first_ = true;
};

}  // namespace ph
