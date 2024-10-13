#pragma once
#include "VideoSurfaceCreatedEventArgs.g.h"

namespace winrt::Unicord::Universal::Voice::implementation
{
    struct VideoSurfaceCreatedEventArgs : VideoSurfaceCreatedEventArgsT<VideoSurfaceCreatedEventArgs>
    {
        VideoSurfaceCreatedEventArgs() = default;

        uint32_t UserId();
        uint32_t SSRC();
        Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface Surface();
    };
}
