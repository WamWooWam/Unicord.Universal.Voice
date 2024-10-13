#pragma once

#include <cctype>
#include <chrono>
#include <concurrent_queue.h>
#include <concurrent_unordered_map.h>
#include <debugapi.h>
#include <iomanip>
#include <iostream>
#include <opus.h>
#include <sodium.h>
#include <sstream>
#include <string>
#include <thread>

#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>

#include "VoiceClient.g.h"

#include "AudioFormat.h"
#include "AudioRenderer.h"
#include "ConnectionEndpoint.h"
#include "OpusWrapper.h"
#include "SodiumWrapper.h"
#include "VideoFrameSink.h"
#include "VoiceTransport.h"
#include "SpeakingAudioAnalyzer.h"
#include "Rtp.h"

#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <api/video/video_source_interface.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/video_decoder_factory.h>
#include <api/video_codecs/video_encoder_factory.h>
#include <api/video_codecs/video_encoder.h>
#include <api/video_codecs/video_decoder.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video/video_bitrate_allocator_factory.h>
#include <call/call.h>

#include <common_audio/include/audio_util.h>

#include <media/engine/adm_helpers.h>
#include <media/engine/webrtcvoiceengine.h>
#include <media/engine/webrtcvideoengine.h>
#include <media/engine/convert_legacy_video_factory.h>

#include <modules/audio_mixer/audio_mixer_impl.h>
#include <modules/audio_processing/audio_buffer.h>
#include <modules/audio_processing/include/audio_processing.h>
#include <modules/rtp_rtcp/include/rtp_header_parser.h>

#include <modules/video_capture/video_capture_factory.h>
#include <modules/video_capture/windows/device_info_mf.h>
#include <test/frame_generator_capturer.h>

#include <third_party/winuwp_h264/winuwp_h264_factory.h>

#include "external/AudioDeviceWasapi.h"
#include "external/IAudioDeviceWasapi.h"
#include "external/VideoRenderer.h"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::System::Threading;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Unicord::Universal::Voice::Interop;
using namespace winrt::Unicord::Universal::Voice::Render;
using namespace winrt::Unicord::Universal::Voice::Transport;

struct StreamResolution {
    uint32_t width;
    uint32_t height;
    std::string type;
};

struct StreamInfo {
    std::string type;
    uint32_t ssrc;
    uint32_t rtx_ssrc;
    uint32_t quality;
    bool active;

    StreamResolution max_resolution;
    uint32_t max_bitrate;
    uint32_t max_framerate;
};

namespace winrt::Unicord::Universal::Voice::implementation {
    struct VoiceClient : VoiceClientT<VoiceClient> {
        friend VoiceOutboundTransport;
        friend SpeakingAudioAnalyzer;

    public:
        VoiceClient() = default;
        VoiceClient(VoiceClientOptions const& options);

        std::unique_ptr<webrtc::Call> _call = nullptr;

        rtc::scoped_refptr<webrtc::AudioState> _audioState;
        rtc::scoped_refptr<webrtc::AudioEncoderFactory> _audioEncoderFactory = nullptr;
        rtc::scoped_refptr<webrtc::AudioDecoderFactory> _audioDecoderFactory = nullptr;

        /*
		std::shared_ptr<webrtc::WinUWPH264EncoderFactory> _videoEncoderFactory = nullptr;
        std::shared_ptr<webrtc::WinUWPH264DecoderFactory> _videoDecoderFactory = nullptr;
		*/

        std::unique_ptr<webrtc::VideoEncoderFactory> _videoEncoderFactory = nullptr;
        std::unique_ptr<webrtc::VideoDecoderFactory> _videoDecoderFactory = nullptr;

        std::unique_ptr<webrtc::VideoBitrateAllocatorFactory> _bitrateAllocatorFactory = nullptr;

        webrtc::AudioSendStream* _audioSendStream = nullptr;      // i dont like this rawptr
        webrtc::AudioDeviceWasapi* _audioDeviceManager = nullptr; // nor this one

        concurrency::concurrent_unordered_map<uint32_t, webrtc::AudioReceiveStream*> _audioRecieveStreams;
        concurrency::concurrent_unordered_map<uint32_t, webrtc::VideoReceiveStream*> _videoRecieveStreams;
        concurrency::concurrent_unordered_map<uint64_t, uint32_t> _ssrcUserMap;

        webrtc::AudioSendStream* CreateAudioSendStream(uint32_t ssrc, uint8_t payloadType);
        webrtc::AudioReceiveStream* CreateAudioRecieveStream(uint32_t remoteSsrc, uint8_t payloadType);

        webrtc::VideoSendStream* CreateVideoSendStream(uint32_t ssrc, uint32_t rtx_ssrc, std::string payloadName, uint8_t payloadType, uint8_t rtxPayloadType);

        static hstring OpusVersion();
        static hstring SodiumVersion();
        static hstring WebRTCVersion();

        uint32_t WebSocketPing();
        uint32_t UdpSocketPing();

        winrt::event_token WebSocketPingUpdated(Windows::Foundation::EventHandler<uint32_t> const& handler);
        void WebSocketPingUpdated(winrt::event_token const& token) noexcept;
        winrt::event_token UdpSocketPingUpdated(Windows::Foundation::EventHandler<uint32_t> const& handler);
        void UdpSocketPingUpdated(winrt::event_token const& token) noexcept;

        winrt::event_token VideoSurfaceCreated(Windows::Foundation::TypedEventHandler<Unicord::Universal::Voice::VoiceClient, Unicord::Universal::Voice::VideoSurfaceCreatedEventArgs> const& handler);
        void VideoSurfaceCreated(winrt::event_token const& token) noexcept;

        winrt::Windows::Foundation::IAsyncAction ConnectAsync();
        winrt::fire_and_forget SendSpeakingAsync(bool speaking);
        Windows::Foundation::IAsyncAction SendStreamsUpdateAsync();
        void UpdateAudioDevices();
        void UpdateMutedDeafened();
        void Close();

        void EnableVideoStreams();

        bool Muted();
        void Muted(bool value);
        bool Deafened();
        void Deafened(bool value);

        ~VoiceClient();

    private:
        VoiceClientOptions _voiceOptions;
        MessageWebSocket _webSocket{ nullptr };
        DatagramSocket _udpSocket{ nullptr };
        ThreadPoolTimer _heartbeatTimer{ nullptr };
        ThreadPoolTimer _keepaliveTimer{ nullptr };
        DataWriter _udpWriter{ nullptr };

        winrt::event<Windows::Foundation::EventHandler<uint32_t>> wsPingUpdated;
        winrt::event<Windows::Foundation::EventHandler<uint32_t>> udpPingUpdated;
        winrt::event<Windows::Foundation::TypedEventHandler<Unicord::Universal::Voice::VoiceClient, Unicord::Universal::Voice::VideoSurfaceCreatedEventArgs>> videoSurfaceCreated;

        winrt::com_ptr<IDXGIDevice2> _dxgiDevice;
		winrt::com_ptr<IDXGIFactory> _dxgiFactory;
        winrt::com_ptr<IDXGIAdapter> _dxgiAdapter;
		winrt::com_ptr<ID3D11Device> _d3d11Device;
        winrt::com_ptr<ID3D11DeviceContext> _d3d11DeviceContext;

        std::mutex startMutex;
        std::shared_ptr<SodiumWrapper> _sodium = nullptr;
        std::unique_ptr<VoiceOutboundTransport> _outboundTransport = nullptr;
        std::unique_ptr<rtc::Thread> _webrtcThread = nullptr;

        std::pair<hstring, EncryptionMode> mode;
        ConnectionEndpoint _webSocketEndpoint;
        ConnectionEndpoint _udpSocketEndpoint;

        bool is_speaking = false;
        bool is_muted = false;
        bool is_deafened = false;
        bool _webSocketOpen = false;
        bool _canResume = true;

        uint32_t _audioSSRC = 0;
        uint32_t heartbeat_interval = 0;
        uint32_t connection_stage = 0;

        uint32_t _activeVideoSSRC = 0;
        uint32_t _activeVideoRtxSSRC = 0;

        concurrency::concurrent_unordered_map<std::string, StreamInfo> _streamMap;

        volatile uint32_t ws_ping = 0;
        volatile uint32_t last_heartbeat = 0;

        volatile uint32_t udp_ping = 0;
        volatile uint64_t keepalive_count = 0;
        concurrency::concurrent_unordered_map<uint64_t, uint64_t> keepalive_timestamps;

        bool is_disposed = false;

		void InitialiseDirect3D();
        void InitialiseSockets();
        winrt::Windows::Foundation::IAsyncAction SendIdentifyAsync();
        winrt::Windows::Foundation::IAsyncAction SendJsonPayloadAsync(JsonObject& payload);
        winrt::Windows::Foundation::IAsyncAction Stage1(JsonObject obj);
        winrt::Windows::Foundation::IAsyncAction Stage3(JsonObject obj);
        winrt::Windows::Foundation::IAsyncAction Stage2(std::string& ip, const uint16_t& port);

        void InitAdm(webrtc::AudioDeviceWasapi* adm);
        void InitApm(webrtc::AudioProcessing* adm);
        void StartCall();

        void ProcessRawPacket(array_view<uint8_t> data);
        winrt::Windows::Foundation::IAsyncAction OnWsHeartbeat(ThreadPoolTimer sender);
        winrt::Windows::Foundation::IAsyncAction OnWsMessage(IWebSocket socket, MessageWebSocketMessageReceivedEventArgs ev);
        winrt::Windows::Foundation::IAsyncAction OnWsClosed(IWebSocket socket, WebSocketClosedEventArgs ev);

        winrt::Windows::Foundation::IAsyncAction ReconnectLoop();

        winrt::Windows::Foundation::IAsyncAction OnUdpHeartbeat(ThreadPoolTimer sender);
        winrt::Windows::Foundation::IAsyncAction OnUdpMessage(DatagramSocket socket, DatagramSocketMessageReceivedEventArgs ev);
        void HandleUdpHeartbeat(uint64_t reader);
    };
}

class dbg_stream_for_cout : public std::stringbuf {
public:
    virtual int_type overflow(int_type c = EOF) {
        if (c != EOF) {
            TCHAR buf[] = { c, '\0' };
            OutputDebugString(buf);
        }
        return c;
    }
};

namespace winrt::Unicord::Universal::Voice::factory_implementation {
    struct VoiceClient : VoiceClientT<VoiceClient, implementation::VoiceClient> {
    };
}
