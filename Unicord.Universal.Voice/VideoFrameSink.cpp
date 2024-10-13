#include "pch.h"
#include "VideoFrameSink.h"
#include <iostream>

namespace winrt::Unicord::Universal::Voice::Render {
    void VideoFrameSink::OnFrame(const webrtc::VideoFrame& frame) {
        rtc::scoped_refptr<webrtc::I420BufferInterface> buffer(frame.video_frame_buffer()->ToI420());
        webrtc::VideoRotation rotation = frame.rotation();

        if (!_d3d11Texture || _width != frame.width() || _height != frame.height()) {

        }

        /*if (rotation != webrtc::kVideoRotation_0) {
            buffer = webrtc::I420Buffer::Rotate(*buffer, rotation);
        }*/

        //int32_t width = buffer->width();
        //int32_t height = buffer->height();

        //size_t bits = 32;
        //size_t size = width * height * (bits >> 3);

        //std::unique_ptr<uint8_t[]> data(new uint8_t[size]);
        //libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), buffer->DataU(), buffer->StrideU(), buffer->DataV(),
        //    buffer->StrideV(), data.get(), width * bits / 8, width, height);

        //PaintFrameAsync(std::move(data), size, width, height);
    }
}