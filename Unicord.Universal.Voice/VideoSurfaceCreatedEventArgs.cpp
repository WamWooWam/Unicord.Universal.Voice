#include "pch.h"
#include "VideoSurfaceCreatedEventArgs.h"
#include "VideoSurfaceCreatedEventArgs.g.cpp"

namespace winrt::Unicord::Universal::Voice::implementation {
    uint32_t VideoSurfaceCreatedEventArgs::UserId() {
        throw hresult_not_implemented();
    }
    uint32_t VideoSurfaceCreatedEventArgs::SSRC() {
        throw hresult_not_implemented();
    }
    Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface VideoSurfaceCreatedEventArgs::Surface() {
        throw hresult_not_implemented();
    }
}
