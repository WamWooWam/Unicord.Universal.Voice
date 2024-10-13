#pragma once
#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>

#include <d3d11.h>

namespace winrt::Unicord::Universal::Voice::Render {
    class VideoFrameSink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
    public:
        VideoFrameSink(uint32_t ssrc, winrt::com_ptr<ID3D11Device> d3d11Device) : videoSSRC(ssrc), _d3d11Device(d3d11Device) {
        }

        void OnFrame(const webrtc::VideoFrame& frame);

    private:
        uint32_t videoSSRC;

		uint32_t _width = 0;
        uint32_t _height = 0;

        winrt::com_ptr<ID3D11Device> _d3d11Device;
        winrt::com_ptr<ID3D11Texture2D> _d3d11Texture;
    };
}