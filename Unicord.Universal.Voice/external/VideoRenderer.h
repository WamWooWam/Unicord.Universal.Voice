#pragma once

#include <unknwn.h>
#include <stddef.h>
#include <memory>

#include <d3d11.h>

#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>

#include <api/video/i420_buffer.h>
#include <modules/video_capture/video_capture_factory.h>
#include <modules/video_capture/windows/device_info_mf.h>
#include <libyuv.h>

#include <api/video/video_frame.h>
#include <api/video/video_source_interface.h>
#include <media/base/videoadapter.h>
#include <media/base/videobroadcaster.h>
#include <rtc_base/criticalsection.h>

struct VideoRenderer : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
    VideoRenderer(){

    }

    void OnFrame(const webrtc::VideoFrame &frame) override {
        rtc::scoped_refptr<webrtc::I420BufferInterface> buffer(frame.video_frame_buffer()->ToI420());

        webrtc::VideoRotation rotation = frame.rotation();
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
};
