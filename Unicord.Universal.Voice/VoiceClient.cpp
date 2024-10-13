#include "pch.h"
#include "VoiceClient.h"
#include "VoiceClient.g.cpp"

using namespace std::chrono;
using namespace winrt;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Networking;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Unicord::Universal::Voice::Interop;

class debugStreambuf : public std::streambuf {
public:
    virtual int_type overflow(int_type c = EOF) {
        if (c != EOF) {
            TCHAR buf[] = { c, '\0' };
            OutputDebugString(buf);
        }
        return c;
    }
};


#define VP8 0

namespace winrt::Unicord::Universal::Voice::implementation {

    static const webrtc::SdpAudioFormat kOpusFormat = { "opus", 48000, 2, { { "stereo", "1" }, { "usedtx", "1" }, { "useinbandfec", "1" } } };
    static const webrtc::SdpVideoFormat kH264Format = { "h264 ", {} };

    hstring VoiceClient::OpusVersion() {
        return to_hstring(opus_get_version_string());
    }

    hstring VoiceClient::SodiumVersion() {
        return to_hstring(sodium_version_string());
    }

    hstring VoiceClient::WebRTCVersion() {
        return L"Unavailable??";
    }

    VoiceClient::VoiceClient(VoiceClientOptions const& options) {
        std::cout.rdbuf(new debugStreambuf());

        if (sodium_init() == -1) {
            throw hresult_error(E_UNEXPECTED, L"Failed to initialize libsodium!");
        }

        if (options.Token().size() == 0 && options.ChannelId() == 0) {
            throw hresult_invalid_argument();
        }

        this->_voiceOptions = options;

		InitialiseDirect3D();

        std::wstring_view raw_endpoint = options.Endpoint();
        std::size_t index = raw_endpoint.find_last_of(':');

        if (index != std::wstring::npos) {
            std::wstring str(raw_endpoint.substr(index + 1));
            _webSocketEndpoint.hostname = raw_endpoint.substr(0, index);
            _webSocketEndpoint.port = (uint16_t)std::stoul(str);
        }
        else {
            _webSocketEndpoint.hostname = raw_endpoint;
            _webSocketEndpoint.port = 443;
        }

        InitialiseSockets();
    }

    uint32_t VoiceClient::WebSocketPing() {
        return ws_ping;
    }

    uint32_t VoiceClient::UdpSocketPing() {
        return udp_ping;
    }

    event_token VoiceClient::WebSocketPingUpdated(Windows::Foundation::EventHandler<uint32_t> const& handler) {
        return wsPingUpdated.add(handler);
    }

    void VoiceClient::WebSocketPingUpdated(event_token const& token) noexcept {
        wsPingUpdated.remove(token);
    }

    event_token VoiceClient::UdpSocketPingUpdated(Windows::Foundation::EventHandler<uint32_t> const& handler) {
        return udpPingUpdated.add(handler);
    }

    void VoiceClient::UdpSocketPingUpdated(event_token const& token) noexcept {
        udpPingUpdated.remove(token);
    }

    winrt::event_token VoiceClient::VideoSurfaceCreated(Windows::Foundation::TypedEventHandler<Unicord::Universal::Voice::VoiceClient, Unicord::Universal::Voice::VideoSurfaceCreatedEventArgs> const& handler) {
        return videoSurfaceCreated.add(handler);
    }

    void VoiceClient::VideoSurfaceCreated(winrt::event_token const& token) noexcept {
        videoSurfaceCreated.remove(token);
    }

    bool VoiceClient::Muted() {
        return is_muted;
    }

    void VoiceClient::Muted(bool value) {
        is_muted = value;
        if (!is_muted && is_deafened)
            is_deafened = false;

        UpdateMutedDeafened();
    }

    bool VoiceClient::Deafened() {
        return is_deafened;
    }

    void VoiceClient::Deafened(bool value) {
        is_muted = value;
        is_deafened = value;

        UpdateMutedDeafened();
    }

    void VoiceClient::InitialiseSockets() {
        _udpSocket = DatagramSocket();
        _udpSocket.Control().QualityOfService(SocketQualityOfService::LowLatency);
        _udpSocket.MessageReceived({ this, &VoiceClient::OnUdpMessage });

        _webSocket = MessageWebSocket();
        _webSocket.Control().MessageType(SocketMessageType::Utf8);
        _webSocket.MessageReceived({ this, &VoiceClient::OnWsMessage });
        _webSocket.Closed({ this, &VoiceClient::OnWsClosed });
    }

    void VoiceClient::InitialiseDirect3D() {
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_9_3 };

        D3D_FEATURE_LEVEL featureLevel;
        winrt::throw_hresult(
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevels, 1, D3D11_SDK_VERSION, _d3d11Device.put(), &featureLevel, _d3d11DeviceContext.put()));

        _dxgiDevice = _d3d11Device.as<IDXGIDevice2>();
    }

    Windows::Foundation::IAsyncAction VoiceClient::ConnectAsync() {
        Windows::Foundation::Uri url{ L"wss://" + _webSocketEndpoint.hostname + L"/?encoding=json&v=7" };
        co_await _webSocket.ConnectAsync(url);
        _webSocketOpen = true;
    }

    Windows::Foundation::IAsyncAction VoiceClient::SendIdentifyAsync() {
        _heartbeatTimer = ThreadPoolTimer::CreatePeriodicTimer({ this, &VoiceClient::OnWsHeartbeat }, milliseconds(heartbeat_interval));

        JsonObject payload{};
        payload.SetNamedValue(L"server_id", JsonValue::CreateStringValue(to_hstring(_voiceOptions.GuildId())));
        payload.SetNamedValue(L"user_id", JsonValue::CreateStringValue(to_hstring(_voiceOptions.CurrentUserId())));
        payload.SetNamedValue(L"session_id", JsonValue::CreateStringValue(_voiceOptions.SessionId()));
        payload.SetNamedValue(L"token", JsonValue::CreateStringValue(_voiceOptions.Token()));
        payload.SetNamedValue(L"video", JsonValue::CreateBooleanValue(true));

        JsonArray streams{};
        JsonObject stream1{};
        stream1.SetNamedValue(L"type", JsonValue::CreateStringValue(L"video"));
        stream1.SetNamedValue(L"rid", JsonValue::CreateStringValue(L"100"));
        stream1.SetNamedValue(L"quality", JsonValue::CreateNumberValue(100));
        streams.Append(stream1);

        payload.SetNamedValue(L"streams", streams);

        JsonObject disp{};
        disp.SetNamedValue(L"op", JsonValue::CreateNumberValue(0));
        disp.SetNamedValue(L"d", payload);


        co_await SendJsonPayloadAsync(disp);
    }

    Windows::Foundation::IAsyncAction VoiceClient::Stage1(JsonObject obj) {
        if (obj != nullptr) {
            _audioSSRC = (uint32_t)obj.GetNamedNumber(L"ssrc");
            _udpSocketEndpoint = ConnectionEndpoint{};
            _udpSocketEndpoint.hostname = obj.GetNamedString(L"ip");
            _udpSocketEndpoint.port = (uint16_t)obj.GetNamedNumber(L"port");

            auto streams = obj.GetNamedArray(L"streams");
            for (auto& stream : streams) {
                auto streamObj = stream.GetObject();

                auto rid = streamObj.GetNamedString(L"rid");
                auto ssrc = streamObj.GetNamedNumber(L"ssrc");
                auto rtx_ssrc = streamObj.GetNamedNumber(L"rtx_ssrc");
                auto type = streamObj.GetNamedString(L"type");
                auto quality = streamObj.GetNamedNumber(L"quality");
                auto active = streamObj.GetNamedBoolean(L"active");

                StreamInfo info{};
                info.ssrc = (uint32_t)ssrc;
                info.rtx_ssrc = (uint32_t)rtx_ssrc;
                info.type = to_string(type);
                info.quality = (uint32_t)quality;
                info.active = active;

                info.max_resolution = { 640,
                    360,
                    "fixed" };

                info.max_bitrate = 2500000;
                info.max_framerate = 30;

                this->_streamMap[to_string(rid)] = info;
            }
        }

        HostName remoteHost{ _udpSocketEndpoint.hostname };
        EndpointPair pair{ nullptr, L"", remoteHost, to_hstring(_udpSocketEndpoint.port) };
        co_await _udpSocket.ConnectAsync(pair);

        mode = SodiumWrapper::SelectEncryptionMode(obj.GetNamedArray(L"modes"));

        uint8_t buff[70]{ 0 };
        std::copy(&_audioSSRC, &_audioSSRC + sizeof _audioSSRC, buff);

        _udpWriter = DataWriter{ _udpSocket.OutputStream() };
        _udpWriter.WriteBytes(buff);
        co_await _udpWriter.StoreAsync();
    }

    Windows::Foundation::IAsyncAction VoiceClient::Stage2(std::string& ip, const uint16_t& port) {
        JsonObject data;
        data.SetNamedValue(L"address", JsonValue::CreateStringValue(to_hstring(ip)));
        data.SetNamedValue(L"port", JsonValue::CreateNumberValue(port));
        data.SetNamedValue(L"mode", JsonValue::CreateStringValue(mode.first));

        JsonObject opus;
        opus.SetNamedValue(L"name", JsonValue::CreateStringValue(L"opus"));
        opus.SetNamedValue(L"type", JsonValue::CreateStringValue(L"audio"));
        opus.SetNamedValue(L"priority", JsonValue::CreateNumberValue(1000));
        opus.SetNamedValue(L"payload_type", JsonValue::CreateNumberValue(Rtp::RTP_TYPE_OPUS));

        JsonObject h264;
#if VP8
        h264.SetNamedValue(L"name", JsonValue::CreateStringValue(L"VP8"));
#else
        h264.SetNamedValue(L"name", JsonValue::CreateStringValue(L"H264"));
#endif
        h264.SetNamedValue(L"type", JsonValue::CreateStringValue(L"video"));
        h264.SetNamedValue(L"priority", JsonValue::CreateNumberValue(1001));
        h264.SetNamedValue(L"payload_type", JsonValue::CreateNumberValue(Rtp::RTP_TYPE_H264));
        h264.SetNamedValue(L"rtx_payload_type", JsonValue::CreateNumberValue(Rtp::RTP_TYPE_H264_RTX));
        h264.SetNamedValue(L"encode", JsonValue::CreateBooleanValue(true));
        h264.SetNamedValue(L"decode", JsonValue::CreateBooleanValue(true));

        JsonArray codecs;
        codecs.Append(h264);
        codecs.Append(opus);

        JsonObject protocol_select;
        protocol_select.SetNamedValue(L"protocol", JsonValue::CreateStringValue(L"udp"));
        protocol_select.SetNamedValue(L"data", data);
        protocol_select.SetNamedValue(L"address", JsonValue::CreateStringValue(to_hstring(ip)));
        protocol_select.SetNamedValue(L"port", JsonValue::CreateNumberValue(port));
        protocol_select.SetNamedValue(L"mode", JsonValue::CreateStringValue(mode.first));
        protocol_select.SetNamedValue(L"codecs", codecs);

        JsonObject dispatch;
        dispatch.SetNamedValue(L"op", JsonValue::CreateNumberValue(1));
        dispatch.SetNamedValue(L"d", protocol_select);

        co_await SendJsonPayloadAsync(dispatch);

        connection_stage = 1;
    }

    Windows::Foundation::IAsyncAction VoiceClient::Stage3(JsonObject data) {
        auto secret_key = data.GetNamedArray(L"secret_key");
        auto new_mode = data.GetNamedString(L"mode");

        uint8_t key[32];
        for (uint32_t i = 0; i < 32; i++) {
            key[i] = (uint8_t)secret_key.GetNumberAt(i);
        }

        _sodium = std::make_shared<SodiumWrapper>(array_view<const uint8_t>(&key[0], &key[secret_key.Size()]), SodiumWrapper::GetEncryptionMode(new_mode));
        _keepaliveTimer = ThreadPoolTimer::CreatePeriodicTimer({ this, &VoiceClient::OnUdpHeartbeat }, 5000ms);

        _webrtcThread = rtc::Thread::Create();
        _webrtcThread->Start();

        _webrtcThread->Invoke<void>(RTC_FROM_HERE, [this]() {
            this->StartCall();
        });

        return Windows::Foundation::IAsyncAction{};
    }

    // init code taken and tweaked from webrtc itself
    void VoiceClient::InitAdm(webrtc::AudioDeviceWasapi* adm) {
        static const webrtc::AudioDeviceModule::WindowsDeviceType AUDIO_DEVICE_ID = webrtc::AudioDeviceModule::WindowsDeviceType::kDefaultDevice;

        adm->SetPreferredDevices(_voiceOptions.PreferredRecordingDevice(), _voiceOptions.PreferredPlaybackDevice());

        if (!this->_audioDeviceManager) {
            RTC_CHECK_EQ(0, adm->Init()) << "Failed to initialize the ADM.";
            this->_audioDeviceManager = adm;
        }

        // Playout device.
        {
            //int16_t deviceId = -1;
            //for (size_t i = 0; i < adm->PlayoutDevices(); i++)
            //{
            //    auto device = wasapi->GetListDevice(DeviceClass::AudioRender, i);
            //    if (device.Id() == _voiceOptions.PreferredPlaybackDevice()) {
            //        deviceId = i;
            //    }
            //}

            //if ((deviceId != -1 ? adm->SetPlayoutDevice(deviceId) : adm->SetPlayoutDevice(AUDIO_DEVICE_ID)) != 0) {
            //    RTC_LOG(LS_ERROR) << "Unable to set playout device.";
            //        return;
            //}

            if (adm->InitSpeaker() != 0) {
                RTC_LOG(LS_ERROR) << "Unable to access speaker.";
            }
            // Set number of channels
            bool available = false;
            if (adm->StereoPlayoutIsAvailable(&available) != 0) {
                RTC_LOG(LS_ERROR) << "Failed to query stereo playout.";
            }
            if (adm->SetStereoPlayout(available) != 0) {
                RTC_LOG(LS_ERROR) << "Failed to set stereo playout mode.";
            }
        }

        // Recording device.
        {
            /*if ((deviceId != -1 ? adm->SetRecordingDevice(deviceId) : adm->SetRecordingDevice(AUDIO_DEVICE_ID)) != 0) {
				RTC_LOG(LS_ERROR) << "Unable to set recording device.";
				return;
			}*/

            if (adm->InitMicrophone() != 0) {
                RTC_LOG(LS_ERROR) << "Unable to access microphone.";
            }
            // Set number of channels
            bool available = false;
            if (adm->StereoRecordingIsAvailable(&available) != 0) {
                RTC_LOG(LS_ERROR) << "Failed to query stereo recording.";
            }
            if (adm->SetStereoRecording(available) != 0) {
                RTC_LOG(LS_ERROR) << "Failed to set stereo recording mode.";
            }
        }
    }

    void VoiceClient::InitApm(webrtc::AudioProcessing* apm) {
        RTC_DCHECK(apm);

        constexpr int kMinVolumeLevel = 64;
        constexpr int kMaxVolumeLevel = 224;

        auto level = _voiceOptions.SuppressionLevel();
        if (level > NoiseSuppressionLevel::Disabled) {
            apm->noise_suppression()->set_level((webrtc::NoiseSuppression::Level)((int32_t)level - 1));
            apm->noise_suppression()->Enable(true);
        }
        else {
            apm->noise_suppression()->Enable(false);
        }

        // This is the initialization which used to happen in VoEBase::Init(), but
        // which is not covered by the WVoE::ApplyOptions().
        if (apm->echo_cancellation()->enable_drift_compensation(false) != 0) {
            RTC_DLOG(LS_ERROR) << "Failed to disable drift compensation.";
        }

        auto echo_cancellation = apm->echo_cancellation();
        echo_cancellation->Enable(_voiceOptions.EchoCancellation());

        auto gc = apm->gain_control();
        if (gc->set_analog_level_limits(kMinVolumeLevel, kMaxVolumeLevel) != 0) {
            RTC_DLOG(LS_ERROR) << "Failed to set analog level limits with minimum: "
                               << kMinVolumeLevel
                               << " and maximum: " << kMaxVolumeLevel;
        }

        gc->set_mode(webrtc::GainControl::Mode::kAdaptiveAnalog);
        gc->Enable(_voiceOptions.AutomaticGainControl()); // TODO: Configurable

        auto voice_detection = apm->voice_detection();
        voice_detection->Enable(true);
    }

    void VoiceClient::EnableVideoStreams() {
        for (auto& stream : _streamMap) {
            this->_activeVideoSSRC = stream.second.ssrc;
            this->_activeVideoRtxSSRC = stream.second.rtx_ssrc;
            _streamMap[stream.first].active = true;

            _webrtcThread->Invoke<void>(RTC_FROM_HERE, [this, stream]() {
#if VP8
                auto sendStream = this->CreateVideoSendStream(stream.second.ssrc, stream.second.rtx_ssrc, "VP8", Rtp::RTP_TYPE_H264, Rtp::RTP_TYPE_H264_RTX);
#else
                auto sendStream = this->CreateVideoSendStream(stream.second.ssrc, stream.second.rtx_ssrc, "H264", Rtp::RTP_TYPE_H264, Rtp::RTP_TYPE_H264_RTX);
#endif
                sendStream->Start();

                _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::NetworkState::kNetworkUp);
            });
        }

        SendStreamsUpdateAsync();
    }

    void VoiceClient::StartCall() {
        //rtc::LogMessage::LogToDebug(rtc::LS_WARNING);

        this->_audioDecoderFactory = webrtc::CreateBuiltinAudioDecoderFactory();
        this->_audioEncoderFactory = webrtc::CreateBuiltinAudioEncoderFactory();


#if VP8
        this->_videoEncoderFactory = webrtc::CreateBuiltinVideoEncoderFactory();
        this->_videoDecoderFactory = webrtc::CreateBuiltinVideoDecoderFactory();
#else
        auto videoEncoderFactory = std::make_unique<webrtc::WinUWPH264EncoderFactory>();
        auto videoDecoderFactory = std::make_unique<webrtc::WinUWPH264DecoderFactory>();
        this->_videoEncoderFactory = cricket::ConvertVideoEncoderFactory(std::move(videoEncoderFactory));
        this->_videoDecoderFactory = cricket::ConvertVideoDecoderFactory(std::move(videoDecoderFactory));
#endif
        webrtc::IAudioDeviceWasapi::CreationProperties props = {};
        props.id_ = "Unicord";
        props.playoutEnabled_ = true;
        props.recordingEnabled_ = true;

        auto audioDeviceModule = webrtc::IAudioDeviceWasapi::create(props);

        webrtc::AudioState::Config stateConfig = {};
        stateConfig.audio_processing = webrtc::AudioProcessingBuilder()
                                           .SetCaptureAnalyzer(std::make_unique<SpeakingAudioAnalyzer>(this))
                                           .Create();

        stateConfig.audio_device_module = audioDeviceModule;
        stateConfig.audio_mixer = webrtc::AudioMixerImpl::Create();

        InitAdm((webrtc::AudioDeviceWasapi*)audioDeviceModule.get());
        InitApm(stateConfig.audio_processing);

        _audioState = webrtc::AudioState::Create(stateConfig);

        audioDeviceModule->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice);
        audioDeviceModule->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultDevice);
        audioDeviceModule->RegisterAudioCallback(_audioState->audio_transport());

        auto logger = webrtc::RtcEventLog::Create(webrtc::RtcEventLog::EncodingType::Legacy);
        webrtc::Call::Config callConfig{ logger.release() };
        callConfig.audio_state = _audioState;
        callConfig.audio_processing = _audioState->audio_processing();
        //callConfig.task_queue_factory task_queue_factory

        _call = std::unique_ptr<webrtc::Call>{ webrtc::Call::Create(callConfig) };
        _outboundTransport = std::make_unique<VoiceOutboundTransport>(_sodium, _udpWriter);
        _outboundTransport->Start();

        _audioSendStream = CreateAudioSendStream(_audioSSRC, Rtp::RTP_TYPE_OPUS);
        //_videoSendStream = CreateVideoSendStream()

        _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::NetworkState::kNetworkUp);

        for (auto stream : _audioRecieveStreams) {
            if (stream.second == nullptr) {
                _audioRecieveStreams[stream.first] = _webrtcThread->Invoke<webrtc::AudioReceiveStream*>(RTC_FROM_HERE, [this, stream]() {
                    return this->CreateAudioRecieveStream(stream.first, 120);
                });
            }
        }

        SendSpeakingAsync(true);
        SendStreamsUpdateAsync();

        EnableVideoStreams();
    }

    webrtc::AudioSendStream* VoiceClient::CreateAudioSendStream(uint32_t ssrc, uint8_t payloadType) {
        webrtc::AudioSendStream::Config config{ _outboundTransport.get() };
        config.rtp.ssrc = ssrc;
        config.rtp.extensions = { { "urn:ietf:params:rtp-hdrext:ssrc-audio-level", 1 } }; // TODO: Discord use more extensions than this.
        config.encoder_factory = _audioEncoderFactory;
        config.send_codec_spec = webrtc::AudioSendStream::Config::SendCodecSpec(payloadType, kOpusFormat);

        webrtc::AudioSendStream* audioStream = _call->CreateAudioSendStream(config);
        audioStream->Start();

        return audioStream;
    }

    webrtc::AudioReceiveStream* VoiceClient::CreateAudioRecieveStream(uint32_t remoteSsrc, uint8_t payloadType) {
        webrtc::AudioReceiveStream::Config config;
        config.rtp.local_ssrc = _audioSSRC;
        config.rtp.remote_ssrc = remoteSsrc;
        config.rtp.extensions = { { "urn:ietf:params:rtp-hdrext:ssrc-audio-level", 1 } };
        config.decoder_factory = _audioDecoderFactory;
        config.decoder_map = { { payloadType, kOpusFormat } };
        config.rtcp_send_transport = _outboundTransport.get();

        webrtc::AudioReceiveStream* audioStream = _call->CreateAudioReceiveStream(config);
        audioStream->Start();

        return audioStream;
    }

    webrtc::VideoSendStream* VoiceClient::CreateVideoSendStream(uint32_t ssrc, uint32_t rtx_ssrc, std::string payloadName, uint8_t payloadType, uint8_t rtxPayloadType) {
        webrtc::VideoSendStream::Config videoConfig{ this->_outboundTransport.get() };
        videoConfig.encoder_settings.encoder_factory = this->_videoEncoderFactory.get();
        // not a thing in this older version of webrtc?
        // videoConfig.encoder_settings.bitrate_allocator_factory = _bitrateAllocatorFactory.get();

        videoConfig.rtp.rtcp_mode = webrtc::RtcpMode::kReducedSize;
        videoConfig.rtp.ssrcs.push_back(ssrc);
        videoConfig.rtp.payload_name = payloadName;
        videoConfig.rtp.payload_type = payloadType;
        videoConfig.rtp.rtx.ssrcs.push_back(rtx_ssrc);
        videoConfig.rtp.rtx.payload_type = rtxPayloadType;
        videoConfig.send_transport = this->_outboundTransport.get();

        webrtc::VideoEncoderConfig encoderConfig;
        encoderConfig.content_type = webrtc::VideoEncoderConfig::ContentType::kRealtimeVideo;
        encoderConfig.codec_type = webrtc::PayloadStringToCodecType(payloadName);
        encoderConfig.min_transmit_bitrate_bps = 500000;
        encoderConfig.max_bitrate_bps = 2500000; // 5 Mbps
        encoderConfig.video_format.name = payloadName;

#if VP8
        webrtc::VideoCodecVP8 settings = webrtc::VideoEncoder::GetDefaultVp8Settings();
        encoderConfig.encoder_specific_settings = new rtc::RefCountedObject<webrtc::VideoEncoderConfig::Vp8EncoderSpecificSettings>(settings);
#else
        webrtc::VideoCodecH264 settings = webrtc::VideoEncoder::GetDefaultH264Settings();
        encoderConfig.encoder_specific_settings = new rtc::RefCountedObject<webrtc::VideoEncoderConfig::H264EncoderSpecificSettings>(settings);
#endif
        encoderConfig.number_of_streams = 1;
        encoderConfig.bitrate_priority = 1.0;
        encoderConfig.simulcast_layers.resize(encoderConfig.number_of_streams);
        encoderConfig.simulcast_layers[0].active = true;
        encoderConfig.simulcast_layers[0].min_bitrate_bps = 30000;
        encoderConfig.simulcast_layers[0].max_bitrate_bps = 2500000;
        encoderConfig.simulcast_layers[0].max_framerate = 30;

        encoderConfig.video_stream_factory = new rtc::RefCountedObject<cricket::EncoderStreamFactory>(
            videoConfig.rtp.payload_name, 56, false, false);

        webrtc::test::FrameGeneratorCapturer* frame_generator =
            webrtc::test::FrameGeneratorCapturer::Create(320, 180, absl::nullopt, 5, 15, webrtc::Clock::GetRealTimeClock());


        //QuickVideoCapturer* capturer = new QuickVideoCapturer();

        webrtc::VideoSendStream* videoSendStream = this->_call->CreateVideoSendStream(std::move(videoConfig), std::move(encoderConfig));
        videoSendStream->SetSource(frame_generator, webrtc::DegradationPreference::MAINTAIN_FRAMERATE);
        videoSendStream->Start();

        frame_generator->Start();
        //capturer->_videoCaptureModule->StartCapture(webrtc::VideoCaptureCapability::)

        return videoSendStream;
    }

    void VoiceClient::ProcessRawPacket(array_view<uint8_t> data) {
        uint8_t nonce[24] = { 0 };

        bool isRtcp = webrtc::RtpHeaderParser::IsRtcp(data.data(), data.size());
        uint8_t* decrypted = new uint8_t[data.size() - 16];
        if (isRtcp) {
            memcpy(nonce, data.data(), 8);
            memcpy(decrypted, data.data(), 8);
            crypto_secretbox_open_easy(decrypted + 8, data.data() + 8, data.size() - 8, nonce, _sodium->Key());

            _webrtcThread->Invoke<void>(RTC_FROM_HERE, [this, decrypted, data]() {
                _call->Receiver()->DeliverPacket(webrtc::MediaType::ANY, rtc::CopyOnWriteBuffer(decrypted, data.size() - 16), rtc::TimeMicros());
                delete decrypted;
            });
        }
        else {
            memcpy(nonce, data.data(), 12);
            memcpy(decrypted, data.data(), 12); // Todo: resolve potential buffer overrun

            crypto_secretbox_open_easy(decrypted + 12, data.data() + 12, data.size() - 12, nonce, _sodium->Key());

            webrtc::RTPHeader header;

            std::unique_ptr<webrtc::RtpHeaderParser> parser{ webrtc::RtpHeaderParser::Create() };
            parser->Parse(data.data(), data.size(), &header);

            webrtc::MediaType type = header.payloadType == 100 || header.payloadType == 101 || header.payloadType == 102
                                         ? webrtc::MediaType::VIDEO
                                         : webrtc::MediaType::AUDIO;


            _webrtcThread->Invoke<void>(RTC_FROM_HERE, [this, decrypted, data, type]() {
                _call->Receiver()->DeliverPacket(type, rtc::CopyOnWriteBuffer(decrypted, data.size() - 16), rtc::TimeMicros());
                delete decrypted;
            });
        }

        //webrtc::MediaType type = webrtc::MediaType::ANY;
        //size_t headerSize = isRtcp ? 8 : 12;

        //if (!isRtcp) { // this probably isn't the most efficient but hey
        //    std::unique_ptr<webrtc::RtpHeaderParser> parser{ webrtc::RtpHeaderParser::Create() };

        //    webrtc::RTPHeader header = {};
        //    parser->Parse(data.data(), data.size(), &header);

        //    if (header.payloadType == Rtp::RTP_TYPE_OPUS) {
        //        type = webrtc::MediaType::AUDIO;
        //    }

        //    if (header.payloadType == Rtp::RTP_TYPE_H264 || header.payloadType == Rtp::RTP_TYPE_H264_RTX) {
        //        type = webrtc::MediaType::VIDEO;
        //    }
        //}

        //size_t decryptedSize = _sodium->CalculateTargetSize(data.size());
        //uint8_t* decrypted = new uint8_t[decryptedSize];

        //// copy header to the decrypted data
        //std::copy(data.begin(), data.begin() + headerSize, decrypted);

        //gsl::span<uint8_t> encryptedData;
        //switch (_sodium->GetCurrentEncryptionMode()) {
        //case XSalsa20_Poly1305:
        //    encryptedData = gsl::make_span(data.begin() + headerSize, data.end());
        //    break;
        //case XSalsa20_Poly1305_Suffix:
        //    encryptedData = gsl::make_span(data.begin() + headerSize, data.end() - crypto_secretbox_xsalsa20poly1305_NONCEBYTES);
        //    break;
        //case XSalsa20_Poly1305_Lite:
        //    encryptedData = gsl::make_span(data.begin() + headerSize, data.end() - 4);
        //    break;
        //default:
        //    throw hresult_invalid_argument();
        //}

        //_sodium->GetNonce(data, nonce, isRtcp);
        //_sodium->Decrypt(encryptedData, nonce, gsl::make_span(decrypted, decryptedSize).subspan(headerSize));

        //_webrtcThread->Invoke<void>(RTC_FROM_HERE, [this, decrypted, decryptedSize, type]() {
        //    rtc::PacketTime pTime = rtc::CreatePacketTime(0);

        //    if (_call)
        //        _call->Receiver()->DeliverPacket(type, rtc::CopyOnWriteBuffer(decrypted, decryptedSize), pTime.timestamp);

        //    delete decrypted;
        //});
    }

    Windows::Foundation::IAsyncAction VoiceClient::OnWsHeartbeat(ThreadPoolTimer timer) {
        try {
            auto stamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            last_heartbeat = stamp;

            JsonObject disp;
            disp.SetNamedValue(L"op", JsonValue::CreateNumberValue(3));
            disp.SetNamedValue(L"d", JsonValue::CreateStringValue(to_hstring(stamp)));

            co_await SendJsonPayloadAsync(disp);
        }
        catch (const winrt::hresult_error& ex) {
            std::cout << "ERROR: " << to_string(ex.message()) << "\n";
        }
    }

    Windows::Foundation::IAsyncAction VoiceClient::OnWsMessage(IWebSocket socket, MessageWebSocketMessageReceivedEventArgs ev) {
        try {
            auto reader = ev.GetDataReader();
            reader.UnicodeEncoding(UnicodeEncoding::Utf8);
            auto json_data = reader.ReadString(reader.UnconsumedBufferLength());
            reader.Close();

            std::cout << "> " << to_string(json_data) << "\n";

            auto json = JsonObject::Parse(json_data);
            auto op = (int)json.GetNamedNumber(L"op");
            auto value = json.GetNamedValue(L"d");

            if (value.ValueType() == JsonValueType::Object) {
                auto data = value.GetObject();
                switch (op) {
                case 8: // hello
                    heartbeat_interval = (int32_t)data.GetNamedNumber(L"heartbeat_interval");
                    co_await SendIdentifyAsync();
                    break;
                case 2: // ready
                    co_await Stage1(data);
                    break;
                case 4: // session description
                    Stage3(data);
                    break;
                case 5: // speaking
                {
                    uint64_t user_id = std::stoll(to_string(data.GetNamedString(L"user_id")));
                    uint32_t speaking_ssrc = (uint32_t)data.GetNamedNumber(L"ssrc");
                    _ssrcUserMap[user_id] = speaking_ssrc; // keep track of this

                    auto stream = _audioRecieveStreams.find(speaking_ssrc);
                    if (stream == _audioRecieveStreams.end()) {
                        webrtc::AudioReceiveStream* recieveStream = nullptr;
                        if (_call != nullptr) {
                            recieveStream = _webrtcThread->Invoke<webrtc::AudioReceiveStream*>(RTC_FROM_HERE, [this, speaking_ssrc]() {
                                return this->CreateAudioRecieveStream(speaking_ssrc, 120);
                            });
                        }

                        this->_audioRecieveStreams[speaking_ssrc] = recieveStream;
                    }

                    // if not, it doesn't super matter

                    break;
                }
                case 12: // video stuff
                {
                    uint32_t audio_ssrc = (uint32_t)data.GetNamedNumber(L"audio_ssrc");
                    uint32_t video_ssrc = 0;
                    uint32_t rtx_ssrc = 0;

                    if (data.HasKey(L"video_ssrc")) {
                        video_ssrc = (uint32_t)data.GetNamedNumber(L"video_ssrc"); // just to be safe
                    }

                    if (data.HasKey(L"rtx_ssrc")) {
                        rtx_ssrc = (uint32_t)data.GetNamedNumber(L"rtx_ssrc");
                    }

                    if (video_ssrc == 0 && rtx_ssrc == 0) {
                        // no video
                    }
                    else {
                        // has video

                        //auto stream = _videoRecieveStreams.find(video_ssrc);
                        //if (stream == _videoRecieveStreams.end()) {
                        //    webrtc::VideoReceiveStream* recieveStream = nullptr;
                        //    if (_call != nullptr) {
                        //        recieveStream = _webrtcThread->Invoke<webrtc::VideoReceiveStream*>(RTC_FROM_HERE, [this, video_ssrc, rtx_ssrc]() {
                        //            //webrtc::VideoReceiveStream::Config videoConfig{ this->_outboundTransport.get() };
                        //            //videoConfig.rtp.remote_ssrc = video_ssrc;
                        //            //videoConfig.rtp.rtx_ssrc = rtx_ssrc;
                        //            //videoConfig.rtp.local_ssrc = rtx_ssrc;
                        //            //videoConfig.rtp.rtx_associated_payload_types.insert(std::make_pair(Rtp::RTP_TYPE_H264_RTX, Rtp::RTP_TYPE_H264));

                        //            //videoConfig.renderer = new Render::VideoFrameSink(video_ssrc);

                        //            //cricket::VideoDecoderParams params;
                        //            //params.receive_stream_id = std::to_string(video_ssrc);
                        //            //webrtc::VideoDecoder* decoder = _videoDecoderFactory->CreateVideoDecoderWithParams(webrtc::VideoCodecType::kVideoCodecH264, params);
                        //            //webrtc::VideoReceiveStream::Decoder recieveDecoder;
                        //            //recieveDecoder.decoder = decoder;
                        //            //recieveDecoder.payload_type = Rtp::RTP_TYPE_H264;
                        //            ////recieveDecoder.video_format.

                        //            //videoConfig.decoders.push_back(recieveDecoder);

                        //            //webrtc::VideoReceiveStream* stream = _call->CreateVideoReceiveStream(videoConfig.Copy());
                        //            //stream->Start();

                        //            //return stream;
                        //        });
                        //    }


                        //    this->_videoRecieveStreams[video_ssrc] = recieveStream;
                        //}
                    }

                    break;
                }
                case 13: // client_disconnected
                {
                    uint64_t user_id = std::stoll(to_string(data.GetNamedString(L"user_id")));
                    uint32_t audio_ssrc = _ssrcUserMap[user_id];

                    auto stream = _audioRecieveStreams.find(audio_ssrc);
                    if (stream != _audioRecieveStreams.end()) {
                        _webrtcThread->Invoke<void>(RTC_FROM_HERE, [this, stream]() {
                            _call->DestroyAudioReceiveStream(stream->second);
                        });
                    }
                    break;
                }
                }
            }

            if (value.ValueType() == JsonValueType::String) {
                auto data = value.GetString();
                switch (op) {
                case 6: // heartbeat ack
                    auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
                    ws_ping = now - last_heartbeat;
                    wsPingUpdated(*this, (const uint32_t)ws_ping);
                    std::cout << "- WS Ping " << ws_ping << "ms\n";
                    break;
                }
            }

            if (value.ValueType() == JsonValueType::Null) {
                switch (op) {
                case 9:
                    HostName remoteHost{ _udpSocketEndpoint.hostname };
                    EndpointPair pair{ nullptr, L"", remoteHost, to_hstring(_udpSocketEndpoint.port) };
                    co_await _udpSocket.ConnectAsync(pair);

                    Stage3(nullptr);
                    break;
                }
            }
        }
        catch (const hresult_error& ex) {
            if (_webSocketOpen && _webSocket != nullptr)
                _webSocket.Close();

            std::cout << "ERROR: " << to_string(ex.message()) << "\n";
        }
    }

    Windows::Foundation::IAsyncAction VoiceClient::OnWsClosed(IWebSocket socket, WebSocketClosedEventArgs ev) {
        try {
            _webSocketOpen = false;
            std::cout << "WebSocket closed with code " << ev.Code() << " and reason " << to_string(ev.Reason()) << "\n";

            if (ev.Code() == 4006 || ev.Code() == 4009) {
                Close();
            }
            else if (ev.Code() > 1100) {
                co_await ReconnectLoop();
            }
        }
        catch (const std::exception&) {
        }
    }

    Windows::Foundation::IAsyncAction VoiceClient::ReconnectLoop() {
        bool connected = false;
        uint32_t reconnection_count = 0;

        while (!connected) {
            try {
                Close();
                is_disposed = false;

                std::chrono::seconds timeout = std::min<std::chrono::seconds>(5s * reconnection_count, 30s);
                std::cout << "- Reconnecting in " << timeout.count() << "s!" << std::endl;
                co_await winrt::resume_after(timeout);

                InitialiseSockets();

                _webSocketOpen = false;
                _canResume = true;

                co_await this->ConnectAsync();
                connected = true;
            }
            catch (const hresult_error& ex) {
                _webSocketOpen = false;
                std::cout << "ERROR: " << to_string(ex.message()) << "\n";
            }

            reconnection_count += 1;
        }
    }

    winrt::fire_and_forget VoiceClient::SendSpeakingAsync(bool speaking) {
        if (is_speaking == speaking)
            return;

        is_speaking = speaking;

        JsonObject payload;
        payload.SetNamedValue(L"speaking", JsonValue::CreateBooleanValue(speaking));
        payload.SetNamedValue(L"delay", JsonValue::CreateNumberValue(0));
        payload.SetNamedValue(L"ssrc", JsonValue::CreateNumberValue(_audioSSRC));

        JsonObject disp;
        disp.SetNamedValue(L"op", JsonValue::CreateNumberValue(5));
        disp.SetNamedValue(L"d", payload);

        co_await SendJsonPayloadAsync(disp);
    }

    Windows::Foundation::IAsyncAction VoiceClient::SendStreamsUpdateAsync() {
        JsonObject payload;
        payload.SetNamedValue(L"audio_ssrc", JsonValue::CreateNumberValue(_audioSSRC));
        payload.SetNamedValue(L"video_ssrc", JsonValue::CreateNumberValue(_activeVideoSSRC));
        payload.SetNamedValue(L"rtx_ssrc", JsonValue::CreateNumberValue(_activeVideoRtxSSRC));

        JsonArray streams;

        for (auto& stream : _streamMap) {
            JsonObject jStream;
            jStream.SetNamedValue(L"type", JsonValue::CreateStringValue(to_hstring(stream.second.type)));
            jStream.SetNamedValue(L"rid", JsonValue::CreateStringValue(to_hstring(stream.first)));
            jStream.SetNamedValue(L"ssrc", JsonValue::CreateNumberValue(stream.second.ssrc));
            jStream.SetNamedValue(L"active", JsonValue::CreateBooleanValue(stream.second.active));
            jStream.SetNamedValue(L"quality", JsonValue::CreateNumberValue(stream.second.quality));
            jStream.SetNamedValue(L"rtx_ssrc", JsonValue::CreateNumberValue(stream.second.rtx_ssrc));

            jStream.SetNamedValue(L"max_bitrate", JsonValue::CreateNumberValue(stream.second.max_bitrate));
            jStream.SetNamedValue(L"max_framerate", JsonValue::CreateNumberValue(stream.second.max_framerate));

            JsonObject max_resolution;
            max_resolution.SetNamedValue(L"width", JsonValue::CreateNumberValue(stream.second.max_resolution.width));
            max_resolution.SetNamedValue(L"height", JsonValue::CreateNumberValue(stream.second.max_resolution.height));
            max_resolution.SetNamedValue(L"type", JsonValue::CreateStringValue(to_hstring(stream.second.max_resolution.type)));

            jStream.SetNamedValue(L"max_resolution", max_resolution);

            streams.Append(jStream);
        }

        payload.SetNamedValue(L"streams", streams);

        JsonObject disp;
        disp.SetNamedValue(L"op", JsonValue::CreateNumberValue(12));
        disp.SetNamedValue(L"d", payload);

        co_await SendJsonPayloadAsync(disp);
    }

    void VoiceClient::UpdateAudioDevices() {
        if (_call != nullptr && _webrtcThread != nullptr) {
            _webrtcThread->Invoke<void>(RTC_FROM_HERE, [this]() {
                this->_audioDeviceManager->SetPreferredDevices(_voiceOptions.PreferredRecordingDevice(), _voiceOptions.PreferredPlaybackDevice());
                this->_audioDeviceManager->DefaultAudioCaptureDeviceChanged(nullptr);
                this->_audioDeviceManager->DefaultAudioRenderDeviceChanged(nullptr);
            });
        }
    }

    void VoiceClient::UpdateMutedDeafened() {
        if (_audioDeviceManager != nullptr) {
            _webrtcThread->Invoke<void>(RTC_FROM_HERE, [this] {
                // this->_audioState->SetRecording(!is_muted);
                this->_audioState->SetPlayout(!is_deafened);
            });
        }
    }

    Windows::Foundation::IAsyncAction VoiceClient::OnUdpHeartbeat(ThreadPoolTimer timer) {
        try {
            uint64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            uint64_t count = keepalive_count;
            keepalive_count = count + 1;
            keepalive_timestamps.insert({ count, now });

            std::cout << "↑ Sending UDP Heartbeat " << count << "\n";

            _udpWriter.WriteUInt64(count);
            co_await _udpWriter.StoreAsync();
        }
        catch (const std::exception& ex) {
            std::cout << "ERROR: " << ex.what() << "\n";
            Close();
        }
    }

    void VoiceClient::HandleUdpHeartbeat(uint64_t count) {
        uint64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        std::cout << "↓ Got UDP Heartbeat " << count << "!\n";

        auto itr = keepalive_timestamps.find(count);
        if (itr != keepalive_timestamps.end()) {
            uint64_t then = keepalive_timestamps.at(count);
            udp_ping = now - then;

            udpPingUpdated(*this, (const uint32_t)udp_ping);
            keepalive_timestamps.unsafe_erase(count);

            std::cout << "- UDP Ping " << udp_ping << "ms\n";
        }
    }

    Windows::Foundation::IAsyncAction VoiceClient::OnUdpMessage(DatagramSocket socket, DatagramSocketMessageReceivedEventArgs ev) {
        auto reader = ev.GetDataReader();

        if (connection_stage == 0) {
            uint8_t buff[70];
            reader.ReadBytes(buff);

            std::string ip{ &buff[4], &buff[64] };
            ip = ip.substr(0, ip.find_first_of('\0'));

            uint16_t port = *(uint16_t*)&buff[68];

            co_await Stage2(ip, port);
            co_await SendStreamsUpdateAsync();
        }
        else {
            auto len = reader.UnconsumedBufferLength();
            if (len == 8) {
                HandleUdpHeartbeat(reader.ReadUInt64());
            }
            else {
                // prolly voice data
                auto data = new uint8_t[len];
                auto data_view = array_view<uint8_t>(data, data + len);
                reader.ReadBytes(data_view);
                ProcessRawPacket(data_view);

                delete[] data;
            }
        }
    }

    Windows::Foundation::IAsyncAction VoiceClient::SendJsonPayloadAsync(JsonObject& payload) {
        if (!_webSocket)
            return;

        DataWriter writer{ _webSocket.OutputStream() };
        auto str = payload.Stringify();

        std::cout << "< " << to_string(str) << std::endl;

        writer.WriteString(str);
        co_await writer.StoreAsync();
        writer.DetachStream();
    }

    void VoiceClient::Close() {
        if (is_disposed)
            return;

        is_disposed = true;

        if (_outboundTransport) {
            _outboundTransport->Stop();
        }

        if (_heartbeatTimer)
            _heartbeatTimer.Cancel();
        _heartbeatTimer = nullptr;

        if (_keepaliveTimer)
            _keepaliveTimer.Cancel();
        _keepaliveTimer = nullptr;

        keepalive_timestamps.clear();

        if (_webrtcThread != nullptr) {
            _webrtcThread->Invoke<void>(RTC_FROM_HERE, [this]() {
                for (auto streamKey : _audioRecieveStreams) {
                    _call->DestroyAudioReceiveStream(streamKey.second);
                }

                if (_audioSendStream) {
                    _audioSendStream->Stop();
                    _call->DestroyAudioSendStream(_audioSendStream);
                }

                _audioSendStream = nullptr;
                _audioDeviceManager->StopPlayout();
                _audioDeviceManager->StopRecording();

                _audioState.release();

                if (_call)
                    delete _call.release();

                delete _audioEncoderFactory.release();
                delete _audioDecoderFactory.release();
            });

            _webrtcThread.reset();
        }

        if (_webSocket)
            _webSocket.Close();
        _webSocket = nullptr;

        if (_udpSocket)
            _udpSocket.Close();
        _udpSocket = nullptr;
    }

    VoiceClient::~VoiceClient() {
        Close();
    }
}
