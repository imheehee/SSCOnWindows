#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "driver_mode.h"
#include "a760_diagnostics.h"
#include "driver_switch.h"
#include "ssc_encoder.h"

#include <thread>
#include <cwchar>
#include <optional>
#include <utility>
#include <winrt/Windows.UI.ViewManagement.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace
{
    bool IsKorean()
    {
        static const bool korean = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
        return korean;
    }

    const char* UiText(const char* english, const char* korean)
    {
        return IsKorean() ? korean : english;
    }

    const wchar_t* UiText(const wchar_t* english, const wchar_t* korean)
    {
        return IsKorean() ? korean : english;
    }

    struct Rtl8761Adapter {
        uint16_t vid;
        uint16_t pid;
        uint16_t firmware_pid;
        DongleDriverStatus status;
    };

    std::optional<Rtl8761Adapter> FindRtl8761Adapter()
    {
        constexpr uint16_t firmware_pid = 0x8771;
        std::optional<Rtl8761Adapter> selected;
        for (const auto& id : {std::pair{0x2357, 0x0604}, std::pair{0x0BDA, 0x8771}}) {
            auto vid = static_cast<uint16_t>(id.first);
            auto pid = static_cast<uint16_t>(id.second);
            auto status = detect_dongle_driver(vid, pid);
            if (status.mode == DongleDriverMode::NotPresent) continue;
            if (selected) return std::nullopt;
            selected = Rtl8761Adapter{vid, pid, firmware_pid, std::move(status)};
        }
        return selected;
    }

    std::string WToA(hstring const& ws)
    {
        if (ws.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
        std::string result(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), result.data(), len, nullptr, nullptr);
        return result;
    }

    hstring AToW(const std::string& s)
    {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring result(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), result.data(), len);
        return hstring(result);
    }

    /* Effective theme of the main surface. A plain ResourceDictionary lookup cannot
     * see ThemeDictionaries under C++/WinRT, so the palette is stored as explicit
     * Light/Dark pairs and the right one is picked here. */
    bool g_dark_theme = false;

    /* PLAN_UX_DESIGN Faz 3: user opt-out from optional motion. The app has no
     * decorative animation today; this guards whatever is added next. */
    bool g_reduce_motion = false;

    /* Live severity brushes. Their colours are copied from the App.xaml palette,
     * which stays the single source of truth. Because callers keep a reference to
     * these objects instead of the App.xaml ones, a theme change recolours every
     * element in place - including log lines written minutes ago - with no walk of
     * the visual tree. */
    struct SeverityToken {
        const wchar_t* key;
        Microsoft::UI::Xaml::Media::SolidColorBrush brush;
    };
    SeverityToken g_severity[] = {
        { L"TagOkBrush",    nullptr },
        { L"TagWarnBrush",  nullptr },
        { L"TagErrorBrush", nullptr },
        { L"TagDataBrush",  nullptr },
    };

    void RefreshSeverityBrushes()
    {
        try {
            auto resources = Application::Current().Resources();
            for (auto& token : g_severity) {
                hstring themed = hstring(token.key) + (g_dark_theme ? L"Dark" : L"Light");
                auto value = resources.TryLookup(winrt::box_value(themed));
                if (!value) continue;
                auto src = value.as<Microsoft::UI::Xaml::Media::SolidColorBrush>();
                if (!token.brush) token.brush = Microsoft::UI::Xaml::Media::SolidColorBrush();
                token.brush.Color(src.Color());
            }
        } catch (...) {}
    }

    /* True while the user has a high-contrast theme active. Severity colours are
     * dropped then, so the system palette wins and text stays readable. */
    bool InHighContrast()
    {
        try {
            Windows::UI::ViewManagement::AccessibilitySettings settings;
            return settings.HighContrast();
        } catch (...) {
            return false;
        }
    }

    /* Severity brush lookup for the current theme (App.xaml tokens).
     * Returns null for unknown keys and under high contrast, so callers fall back to
     * the default text colour. */
    Microsoft::UI::Xaml::Media::SolidColorBrush TagBrush(const wchar_t* key)
    {
        if (InHighContrast()) return nullptr;
        for (auto& token : g_severity) {
            if (wcscmp(token.key, key) == 0) return token.brush;
        }
        return nullptr;
    }

    /* ------------------------------------------------------------------
     * User-facing copy for the status card (docs/dev/PLAN_UX_DESIGN.md Faz 1).
     *
     * A2dpService::notify_state() hands the UI localization *keys*
     * ("status.ready", "error.pairing_hint", ...), not prose. Showing them
     * verbatim meant the app said "status.connected" to the user. Every key now
     * maps to three fields:
     *   headline - the state, in one or two words
     *   detail   - the technical term PLUS a plain-language explanation
     *   action   - the single most useful next step ("do this, then that")
     * The raw key is still written to the activity log for diagnostics, so
     * nothing is lost.
     *
     * Phase copy for a healthy transition is picked by the State enum; the
     * per-key entries below only refine it (or override it for errors).
     * ------------------------------------------------------------------ */
    /* Severity of a status-card update. Mirrors implementation::LogTag but lives
     * at file scope so the copy table does not depend on the implementation
     * namespace being declared first. */
    enum class Tone { Info, Ok, Warn, Error };

    struct StateCopy {
        const char* headline;
        const char* detail;
        const char* action;   /* "" -> the action line is hidden */
        wchar_t glyph;        /* single Segoe Fluent Icons code point */
        Tone tone;
    };

    /* Segoe Fluent Icons glyphs: color is never the only signal (Faz 1).
     * Info / Sync / Accept (checkmark) / Error / Warning. */
    constexpr wchar_t kGlyphInfo    = 0xE946;
    constexpr wchar_t kGlyphSync    = 0xE895;
    constexpr wchar_t kGlyphCheck   = 0xE73E;
    constexpr wchar_t kGlyphError   = 0xE783;
    constexpr wchar_t kGlyphWarning = 0xE7BA;

    const StateCopy& CopyForKey(const std::string& key)
    {
        static const StateCopy kUnknown{
            UiText("Working on it", "처리 중"),
                UiText("The app is busy setting things up.", "앱이 준비 중입니다."),
                "",
                kGlyphSync, Tone::Info };

        static const std::pair<const char*, StateCopy> kTable[] = {
            /* ---- healthy transitions ---- */
            { "status.ready",
              { UiText("Ready", "준비됨"),
                UiText("No device connected yet.", "연결된 장치가 없습니다."),
                UiText("Press 'Scan Devices' to look for headsets, or 'Direct Connect' if you know the address.", "장치 검색으로 헤드셋을 찾거나 주소로 연결하세요."),
                kGlyphInfo, Tone::Info } },
            { "status.initializing",
              { UiText("Starting up", "시작 중"),
                UiText("Bringing up the Bluetooth stack.", "Bluetooth 스택을 시작하고 있습니다."),
                "",
                kGlyphSync, Tone::Info } },
            { "status.initializing_btstack",
              { UiText("Starting the dongle", "동글 시작 중"),
                UiText("Opening the USB Bluetooth dongle and loading its firmware.", "USB Bluetooth 동글을 열고 펌웨어를 불러옵니다."),
                UiText("This takes a second. Keep the dongle plugged in.", "동글을 연결한 상태로 기다리세요."),
                kGlyphSync, Tone::Info } },
            { "status.connecting_device",
              { UiText("Pairing with the headset", "헤드셋 페어링 중"),
                UiText("Negotiating the A2DP profile.", "A2DP 프로필을 협상하고 있습니다."),
                UiText("Keep the headset close, powered on and out of the case.", "헤드셋 전원을 켜고 케이스에서 꺼낸 뒤 가까이 두세요."),
                kGlyphSync, Tone::Info } },
            { "status.initializing_audio",
              { UiText("Starting audio", "오디오 시작 중"),
                UiText("Opening Windows loopback capture and the audio encoder.", "Windows 루프백 캡처와 인코더를 시작합니다."),
                "",
                kGlyphSync, Tone::Info } },
            { "status.connected",
              { UiText("Connected", "연결됨"),
                UiText("The headset accepted the connection.", "헤드셋이 연결을 수락했습니다."),
                "",
                kGlyphCheck, Tone::Ok } },
            { "status.reconnected",
              { UiText("Reconnected", "다시 연결됨"),
                UiText("The headset link came back up.", "헤드셋 연결이 복구됐습니다."),
                "",
                kGlyphCheck, Tone::Ok } },
            { "status.connection_lost",
              { UiText("Connection lost", "연결 끊김"),
                UiText("The headset link dropped.", "헤드셋 연결이 끊겼습니다."),
                UiText("Reconnect, or move closer to the dongle.", "동글에 가까이 이동한 뒤 다시 연결하세요."),
                kGlyphWarning, Tone::Warn } },

            /* ---- actionable errors ---- */
            { "error.invalid_address",
              { UiText("Check the address", "주소를 확인하세요"),
                UiText("That Bluetooth address is not in the expected XX:XX:XX:XX:XX:XX form.", "Bluetooth 주소 형식은 XX:XX:XX:XX:XX:XX입니다."),
                UiText("Fix the address, then press 'Direct Connect'.", "주소를 수정한 뒤 주소로 연결을 누르세요."),
                kGlyphError, Tone::Error } },
            { "error.pairing_hint",
              { UiText("Headset not found", "헤드셋을 찾을 수 없음"),
                UiText("The headset did not answer the scan. It may be switched off, already connected to "
                "another device, or out of pairing mode.", "헤드셋의 전원, 다른 장치와의 연결 및 페어링 모드를 확인하세요."),
                UiText("Put both earbuds in the case, hold them until the LED blinks, then press "
                "'Scan Devices' again.", "이어버드를 페어링 모드로 전환한 뒤 다시 검색하세요."),
                kGlyphError, Tone::Error } },
            { "error.no_compatible_codec",
              { UiText("No compatible codec", "호환 코덱 없음"),
                UiText("This headset does not offer a codec the app can use (SSC, AAC or SBC), so it cannot "
                "be streamed to.", "헤드셋이 앱에서 사용할 수 있는 SSC, AAC, SBC 코덱을 제공하지 않습니다."),
                UiText("Try another codec under 'Audio settings'. If none work, this headset is not supported.", "오디오 설정에서 지원 코덱을 확인하세요."),
                kGlyphError, Tone::Error } },
            { "error.btstack_init",
              { UiText("Dongle not available", "동글을 열 수 없음"),
                UiText("The USB Bluetooth dongle could not be opened, so no connection is possible.", "USB Bluetooth 동글을 열지 못했습니다."),
                UiText("Open 'Troubleshooting' and check the dongle shows 'WinUSB (Streaming mode)'. "
                "If needed, open the WinUSB setup instructions.", "문제 해결에서 동글의 WinUSB 상태와 설정 안내를 확인하세요."),
                kGlyphError, Tone::Error } },
            { "error.codec_configure",
              { UiText("Codec rejected", "코덱 설정 거부됨"),
                UiText("The headset refused the codec configuration - usually an unsupported bitrate or "
                "sample rate for this device.", "헤드셋이 코덱 설정을 거부했습니다. 비트 전송률과 샘플링 주파수를 확인하세요."),
                UiText("Set bitrate to 0 (auto), pick sample rate 48k, then reconnect.", "비트 전송률을 0(자동), 샘플링 주파수를 48 kHz로 설정하고 다시 연결하세요."),
                kGlyphError, Tone::Error } },
            { "error.encoder_init",
              { UiText("Encoder could not start", "인코더를 시작할 수 없음"),
                UiText("The audio encoder (Samsung SSC) failed to initialise, so no audio can be produced.", "SSC 인코더를 초기화하지 못했습니다."),
                UiText("Check the activity log for the reason. The SSC encoder needs WSL2 (or --ssc-native "
                "Qiling) running.", "활동 로그와 WSL2 또는 --ssc-native Qiling 실행 상태를 확인하세요."),
                kGlyphError, Tone::Error } },
            { "error.stream_start",
              { UiText("Could not start audio", "오디오를 시작할 수 없음"),
                UiText("The headset accepted the connection but the audio stream would not start.", "헤드셋이 연결을 수락했지만 오디오 스트림을 시작하지 못했습니다."),
                UiText("Disconnect and reconnect. If it repeats, unplug and replug the dongle.", "다시 연결하세요. 문제가 반복되면 동글을 뺐다가 연결하세요."),
                kGlyphError, Tone::Error } },
            { "error.wasapi_init",
              { UiText("Audio capture failed", "오디오 캡처 실패"),
                UiText("Windows loopback capture could not be opened, so there is no audio to stream.", "Windows 루프백 캡처를 열지 못했습니다."),
                UiText("Check that the default playback device is active and its format is set to 48 kHz.", "기본 재생 장치가 활성화됐는지, 형식이 48 kHz인지 확인하세요."),
                kGlyphError, Tone::Error } },
            { "error.encode_thread",
              { UiText("Encoder stopped", "인코더 중지됨"),
                UiText("The thread that encodes and sends audio stopped unexpectedly.", "오디오 인코딩 및 전송이 예기치 않게 중지됐습니다."),
                UiText("Read the activity log, then reconnect.", "활동 로그를 확인한 후 다시 연결하세요."),
                kGlyphError, Tone::Error } },
            { "error.reconnect_failed",
              { UiText("Reconnect failed", "재연결 실패"),
                UiText("Automatic reconnection gave up after several attempts.", "여러 차례 자동 재연결을 시도했지만 연결되지 않았습니다."),
                UiText("Press 'Reconnect Last', or scan again if the headset was reset.", "마지막 장치에 다시 연결하거나 헤드셋을 다시 검색하세요."),
                kGlyphError, Tone::Error } },
            { "error.btstack_crash_scan",
              { UiText("Bluetooth stack stopped while scanning", "검색 중 Bluetooth 스택 중지"),
                UiText("The Bluetooth stack stopped unexpectedly during the device scan.", "장치 검색 중 Bluetooth 스택이 예기치 않게 중지됐습니다."),
                UiText("Unplug and replug the dongle, then scan again.", "동글을 뺐다가 연결한 후 다시 검색하세요."),
                kGlyphError, Tone::Error } },
            { "error.btstack_crash_stream",
              { UiText("Bluetooth stack stopped while streaming", "스트리밍 중 Bluetooth 스택 중지"),
                UiText("The Bluetooth stack stopped unexpectedly, so audio stopped.", "Bluetooth 스택이 중지되어 오디오가 끊겼습니다."),
                UiText("Unplug and replug the dongle, then reconnect.", "동글을 뺐다가 연결한 후 다시 연결하세요."),
                kGlyphError, Tone::Error } },
            { "error.aac_unavailable",
              { UiText("AAC not available", "AAC를 사용할 수 없음"),
                UiText("This build has no AAC encoder, so the stream falls back to SBC.", "이 빌드에는 AAC 인코더가 없어 SBC를 사용합니다."),
                UiText("Prefer SSC (Samsung Scalable Codec) for the best quality on Galaxy earbuds.", "Galaxy 이어버드를 사용한다면 SSC를 선택할 수 있습니다."),
                kGlyphWarning, Tone::Warn } },
            { "error.virtual_capture_init",
              { UiText("Virtual audio device unavailable", "가상 오디오 장치를 사용할 수 없음"),
                UiText("The virtual audio endpoint needed to capture microphone input could not be created.", "마이크 입력 캡처에 필요한 가상 오디오 장치를 만들지 못했습니다."),
                UiText("Install the virtual output driver, or switch the capture mode to loopback.", "가상 출력 드라이버를 설치하거나 루프백 캡처로 전환하세요."),
                kGlyphError, Tone::Error } },
        };

        for (const auto& entry : kTable) {
            if (key == entry.first) return entry.second;
        }
        return kUnknown;
    }

    /* Base copy for a State enum value; per-key text from CopyForKey()
     * overrides detail/action when the service sent a known key. */
    StateCopy BaseCopyForState(A2dpService::State state)
    {
        switch (state) {
        case A2dpService::State::Connecting:
            return { UiText("Connecting...", "연결 중…"),
                UiText("Talking to the headset.", "헤드셋과 연결을 설정하고 있습니다."),
                UiText("Keep the headset close and powered on.", "헤드셋 전원을 켜고 가까이 두세요."),
                kGlyphSync, Tone::Info };
        case A2dpService::State::Streaming:
            return { UiText("Streaming audio", "오디오 스트리밍 중"),
                UiText("Windows audio is on its way to the headset.", "Windows 오디오를 헤드셋에 보내고 있습니다."),
                "",
                kGlyphCheck, Tone::Ok };
        case A2dpService::State::Reconnecting:
            return { UiText("Reconnecting...", "재연결 중…"),
                UiText("The link dropped; retrying automatically.", "연결이 끊겨 자동으로 다시 연결하고 있습니다."),
                UiText("If this keeps happening, press 'Reconnect Last'.", "반복되면 마지막 장치에 다시 연결을 누르세요."),
                kGlyphSync, Tone::Warn };
        case A2dpService::State::Error:
            return { UiText("Something went wrong", "작업 실패"),
                UiText("The last operation did not finish.", "이전 작업을 완료하지 못했습니다."),
                UiText("See the activity log below for the raw error.", "활동 로그에서 오류를 확인하세요."),
                kGlyphError, Tone::Error };
        case A2dpService::State::Idle:
        default:
            return { UiText("Ready", "준비됨"),
                UiText("Not connected to a device.", "연결된 장치가 없습니다."),
                UiText("Press 'Scan Devices' to look for headsets.", "장치 검색을 눌러 헤드셋을 찾으세요."),
                kGlyphInfo, Tone::Info };
        }
    }
}

namespace winrt::A2DPWBWinUI::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();

        VersionCreditText().Text(L"v" + AToW(std::string(APP_VERSION)) + L" \u00B7 by Salvetum");

        // Mica is decided after the settings load (ApplyBackdrop), because the
        // user can opt out and high contrast must win over the request.

        // Severity palette follows the effective theme (PLAN_UX_DESIGN Faz 3).
        try {
            auto root = this->Content().try_as<Microsoft::UI::Xaml::FrameworkElement>();
            if (root) {
                g_dark_theme =
                    (root.ActualTheme() == Microsoft::UI::Xaml::ElementTheme::Dark);
                root.ActualThemeChanged([](auto const& sender, auto const&) {
                        g_dark_theme = (sender.ActualTheme() ==
                                        Microsoft::UI::Xaml::ElementTheme::Dark);
                        RefreshSeverityBrushes();
                    });
            }
            RefreshSeverityBrushes();
        } catch (...) {}

        dispatcher_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        service_ = std::make_unique<A2dpService>();

        // Load settings and saved devices (mirrors wxWidgets MainFrame init)
        settings_.load();
        service_->load_saved_devices();
        const auto adapter = FindRtl8761Adapter();
        const bool adapter_ready = adapter && adapter->status.mode == DongleDriverMode::WinUsbStream;
        const auto chip_pid = settings_.bt_chip_pid == 0 && adapter_ready
            ? adapter->firmware_pid : settings_.bt_chip_pid;
        service_->set_bt_chip_pid(chip_pid);
        service_->set_bt_chip_fw_stem(settings_.bt_chip_fw_stem);
        service_->set_debug_mode(settings_.debug_mode);
        service_->set_auto_mute_output(true);
        AutoMuteCheck().IsChecked(true);

        // State callback (worker thread â†’ UI)
        service_->set_state_callback([this](A2dpService::State state, const std::string& text) {
            auto captured = this;
            dispatcher_.TryEnqueue([captured, state, text]() {
                captured->UpdateUI(state, text);
            });
        });

        // Stream info callback (worker thread â†’ UI)
        service_->set_stream_info_callback([this](const A2dpService::StreamInfo& info) {
            auto captured = this;
            dispatcher_.TryEnqueue([captured, info]() {
                captured->CodecText().Text(UiText(L"Codec: ", L"코덱: ") + AToW(info.codec));
                captured->BitrateText().Text(UiText(L"Bitrate: ", L"비트 전송률: ") + AToW(std::to_string(info.bitrate_kbps) + " kbps"));
                captured->SampleRateText().Text(UiText(L"Sample Rate: ", L"샘플링 주파수: ") + AToW(std::to_string(info.sample_rate) + " Hz"));
                captured->ChannelsText().Text(UiText(L"Channels: ", L"채널: ") + AToW(std::to_string(info.channels)));

                if (info.source_sample_rate > 0) {
                    std::string src = std::to_string(info.source_sample_rate) + " Hz, "
                                    + std::to_string(info.source_channels) + " ch, "
                                    + std::to_string(info.source_bit_depth) + " bit";
                    captured->SourceInfoText().Text(UiText(L"Source: ", L"소스: ") + AToW(src));
                }
                /* Faz 1: explain what the numbers mean, not just show them. */
                std::string hint;
                if (info.sample_rate >= 96000) {
                    hint = "96 kHz UHQ is upsampled from 48 kHz loopback capture; the headset must "
                           "advertise UHQ support or you will hear silence.";
                } else if (info.source_sample_rate > 0 &&
                           info.source_sample_rate != info.sample_rate) {
                    hint = "Capture and stream rates differ, so the audio is resampled before encoding.";
                } else {
                    hint = "Loopback capture is taken before the Windows volume mixer, so the speaker "
                           "volume slider does not change what the headset plays.";
                }
                if (auto h = captured->AudioStatusHintText()) h.Text(AToW(hint));
            });
        });

        // Live stats callback (worker thread â†’ UI, ~2 s cadence)
        service_->set_stats_callback([this](const A2dpService::StreamStats& stats) {
            auto captured = this;
            dispatcher_.TryEnqueue([captured, stats]() {
                captured->UpdateStats(stats);
            });
        });

        // Device volume callback (AVRCP absolute volume, worker thread â†’ UI)
        service_->set_volume_changed_callback([this](uint8_t vol) {
            auto captured = this;
            int pct = (static_cast<int>(vol) * 100 + 63) / 127;
            dispatcher_.TryEnqueue([captured, pct]() {
                captured->ApplyDeviceVolumePercent(pct);
            });
        });

        // Scan complete callback (worker thread â†’ UI)
        service_->set_scan_complete_callback([this]() {
            auto captured = this;
            dispatcher_.TryEnqueue([captured]() {
                auto devices = captured->service_->get_devices();
                captured->ScanButton().IsEnabled(true);
                if (devices.empty()) {
                    captured->AppendLog(LogTag::Warn, "No devices found.");
                } else {
                    captured->AppendLog(LogTag::Ok, "Found " + std::to_string(devices.size()) + " device(s).");
                    for (const auto& dev : devices) {
                        captured->DeviceListView().Items().Append(
                            winrt::box_value(AToW(dev.name + " (" + dev.addr_str + ")")));
                    }
                }
            });
        });

        AppendLog(LogTag::Data, "Settings loaded. Chip PID: 0x" + [&]() {
            char buf[8]; snprintf(buf, sizeof(buf), "%04X", settings_.bt_chip_pid); return std::string(buf);
        }() + ", FW stem: " + (settings_.bt_chip_fw_stem.empty() ? "(none)" : settings_.bt_chip_fw_stem) + ". Ready.");

        // Last-device / auto-connect UI state (persisted in settings.json)
        AutoConnectCheck().IsChecked(settings_.auto_connect_on_start);
        ReconnectLastButton().IsEnabled(!settings_.last_device_mac.empty());
        UpdateStatusDevice(settings_.last_device_name, settings_.last_device_mac);

        /* Appearance (PLAN_UX_DESIGN Faz 3). Set the control values without
         * letting the change handler save them straight back, then decide the
         * backdrop once the user preference is known. */
        MicaCheck().IsChecked(settings_.mica_enabled);
        ReduceMotionCheck().IsChecked(settings_.reduce_motion);
        g_reduce_motion = settings_.reduce_motion;
        ApplyBackdrop();

        // Detect the dongle's driver mode (WinUSB vs BTHUSB) once at startup
        RefreshDriverMode();

        // Normalize bit-depth control after UI is fully loaded
        uiReady_ = true;
        UpdateBitDepthForCodec();

        /* Plan Faz 2: nothing pushes stats while the stream is stopped, so the
         * freshness label and the "grey out stale values" rule need their own
         * heartbeat to stay honest. */
        if (auto dq = dispatcher_) {
            freshness_timer_ = dq.CreateTimer();
            freshness_timer_.Interval(std::chrono::milliseconds(1000));
            freshness_timer_.Tick([this](Microsoft::UI::Dispatching::DispatcherQueueTimer const&,
                                         IInspectable const&) {
                UpdateFreshness();
            });
            freshness_timer_.Start();
        }
        UpdateFreshness();

        // Auto-connect (plan section 3): resume the last device at launch
        if (settings_.auto_connect_on_start && !settings_.last_device_mac.empty()) {
            if (!adapter_ready) {
                AppendLog(LogTag::Warn, "Auto-connect skipped: dongle is not in WinUSB (Streaming) mode. "
                          "See the WinUSB setup instructions in Troubleshooting.");
            } else {
                AppendLog("Auto-connecting to last device " + settings_.last_device_name +
                          " [" + settings_.last_device_mac + "]...");
                StartStream(settings_.last_device_mac, settings_.last_device_name, "Auto-connect");
            }
        }
    }

    MainWindow::~MainWindow()
    {
        if (service_) {
            service_->stop_streaming();
        }
    }

    void MainWindow::OnScanClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (!service_ || service_->is_scanning()) return;

        AppendLog("Scanning for Bluetooth audio devices (BTstack inquiry ~8s)...");
        ScanButton().IsEnabled(false);
        DeviceListView().Items().Clear();
        hasSelectedDevice_ = false;
        ConnectButton().IsEnabled(false);

        service_->start_scan();
        // Scan result handled by scan_complete_callback (wired in constructor)
    }

    void MainWindow::OnConnectClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (!service_) return;

        if (isStreaming_) {
            StopStream();
            return;
        }

        if (!hasSelectedDevice_) {
            AppendLog(LogTag::Warn, "No device selected. Use 'Scan Devices' or 'Direct Connect'.");
            return;
        }

        ConnectionProfile profile{};
        profile.device_address = selectedDevice_.addr_str;
        profile.device_name = selectedDevice_.name;
        profile.codec = ProfileManager::index_to_codec(selectedCodecIndex_);
        profile.quality = ProfileManager::index_to_quality(selectedQualityIndex_);
        profile.bitrate_kbps = static_cast<uint32_t>(selectedBitrateKbps_);
        profile.bit_depth = ProfileManager::index_to_bit_depth(selectedBitDepthIndex_);
        profile.capture_mode = "loopback";
        profile.sample_rate = SelectedSampleRate();

        AppendLog("Connecting to " + selectedDevice_.name + " [" + profile.codec + "]...");
        RememberLastDevice(profile.device_address, profile.device_name);

        service_->start_streaming(profile);
        SetStreamingUi(true);
    }

    void MainWindow::OnDirectConnectClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (!service_) return;

        if (isStreaming_) {
            StopStream();
            return;
        }

        // Read MAC from text box
        auto macHstring = MacAddressBox().Text();
        std::string mac = WToA(macHstring);
        if (mac.empty() || mac.size() < 17) {
            AppendLog(LogTag::Error, "Invalid MAC address.");
            return;
        }

        // Validate MAC format
        unsigned int a[6];
        if (sscanf_s(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
                     &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
            AppendLog(LogTag::Error, "Invalid MAC format. Use XX:XX:XX:XX:XX:XX");
            return;
        }

        // Convert to uppercase
        for (auto& c : mac) c = (char)toupper((unsigned char)c);

        ConnectionProfile profile{};
        profile.device_address = mac;
        profile.device_name = "Buds3 FE";
        profile.codec = ProfileManager::index_to_codec(selectedCodecIndex_);
        profile.quality = ProfileManager::index_to_quality(selectedQualityIndex_);
        profile.bitrate_kbps = static_cast<uint32_t>(selectedBitrateKbps_);
        profile.bit_depth = ProfileManager::index_to_bit_depth(selectedBitDepthIndex_);
        profile.capture_mode = "loopback";
        profile.sample_rate = SelectedSampleRate();

        AppendLog("Direct connect to " + mac + " [" + profile.codec + "]...");
        RememberLastDevice(profile.device_address, profile.device_name);

        service_->start_streaming(profile);
        SetStreamingUi(true);
    }

    void MainWindow::OnClearLogClick(IInspectable const&, RoutedEventArgs const&)
    {
        LogLines().Children().Clear();
    }

    void MainWindow::OnDeviceSelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        if (!uiReady_ || !service_) return;
        auto devices = service_->get_devices();
        int idx = DeviceListView().SelectedIndex();

        if (idx >= 0 && idx < (int)devices.size()) {
            selectedDevice_ = devices[idx];
            hasSelectedDevice_ = true;
            ConnectButton().IsEnabled(true);
            UpdateStatusDevice(selectedDevice_.name, selectedDevice_.addr_str);
            AppendLog(LogTag::Data, "Selected: " + selectedDevice_.name);
        } else {
            hasSelectedDevice_ = false;
            ConnectButton().IsEnabled(false);
            UpdateStatusDevice(settings_.last_device_name, settings_.last_device_mac);
        }
    }

    void MainWindow::OnCodecChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        selectedCodecIndex_ = CodecCombo().SelectedIndex();
        if (uiReady_) UpdateBitDepthForCodec();
    }

    void MainWindow::OnQualityChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        selectedQualityIndex_ = QualityCombo().SelectedIndex();
    }

    void MainWindow::OnPageSizeChanged(IInspectable const&, SizeChangedEventArgs const& args)
    {
        if (!DevicesPanel() || !SettingsPanel() || !DevicesColumn()) return;
        const bool narrow = args.NewSize().Width < 760;
        DevicesColumn().Width(narrow ? GridLength{0, GridUnitType::Auto}
                                      : GridLength{300, GridUnitType::Pixel});
        Grid::SetColumnSpan(DevicesPanel(), narrow ? 2 : 1);
        Grid::SetColumn(SettingsPanel(), narrow ? 0 : 1);
        Grid::SetColumnSpan(SettingsPanel(), narrow ? 2 : 1);
        Grid::SetRow(SettingsPanel(), narrow ? 1 : 0);
    }

    uint32_t MainWindow::SelectedSampleRate() const
    {
        switch (selectedRateIndex_) {
        case 1: return 96000;
        case 2: return 44100;
        default: return 48000;
        }
    }

    void MainWindow::OnRateChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        selectedRateIndex_ = RateCombo().SelectedIndex();
        /* Re-snap the bitrate to the new mode's valid set (48k vs 96k UHQ).
         * Guarded: during XBF load IsSelected="True" fires this before the
         * BitrateSlider/BitrateValueText elements are connected. */
        if (uiReady_) ApplyBitrateSnap(selectedBitrateKbps_);
    }

    void MainWindow::OnBitDepthChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        selectedBitDepthIndex_ = BitDepthCombo().SelectedIndex();
    }

    void MainWindow::UpdateBitDepthForCodec()
    {
        // Codec-based bit-depth display (sample width is fixed per codec):
        //   SSC     : fixed int32 (daemon wire protocol)
        //   AAC, SBC: fixed int16
        switch (selectedCodecIndex_) {
        case 0: /* SSC */
            BitDepthCombo().IsEnabled(false);
            BitDepthCombo().SelectedIndex(3); /* forced 32-bit */
            BitDepthHintText().Text(L"32-bit (int32, fixed)");
            break;
        case 1: case 2: /* AAC, SBC */
            BitDepthCombo().IsEnabled(false);
            BitDepthCombo().SelectedIndex(1); /* forced 16-bit */
            BitDepthHintText().Text(L"16-bit (int16, fixed)");
            break;
        default:
            BitDepthCombo().IsEnabled(true);
            BitDepthHintText().Text(L"resolved per codec");
            break;
        }
    }

    void MainWindow::OnBitrateChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (!uiReady_) return;
        ApplyBitrateSnap(static_cast<int>(e.NewValue()));
    }

    void MainWindow::ApplyBitrateSnap(int requestedKbps)
    {
        /* SSC bitrates are mode-gated; snap so the slider can never hand the
         * blob an unsupported value (which garbles audio). */
        uint32_t rate = SelectedSampleRate();
        int snapped = static_cast<int>(SscEncoder::snap_bitrate_kbps(
            static_cast<uint32_t>(requestedKbps), rate));
        selectedBitrateKbps_ = snapped;
        /* Null-safe: these elements may not be connected yet during XBF load. */
        if (auto slider = BitrateSlider()) {
            if (static_cast<int>(slider.Value()) != snapped)
                slider.Value(snapped);
        }
        if (auto text = BitrateValueText()) {
            text.Text(AToW(snapped == 0 ? std::string("auto")
                                        : std::to_string(snapped) + " kbps"));
        }
    }

    void MainWindow::OnVolumeChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (!uiReady_) return;
        int pct = static_cast<int>(e.NewValue());
        if (auto text = VolumeValueText())
            text.Text(AToW(std::to_string(pct) + "%"));
        /* Ignore the echo produced while mirroring a device-initiated change. */
        if (updatingVolumeFromDevice_) return;
        if (service_)
            service_->set_device_volume(static_cast<float>(pct) / 100.0f);
    }

    void MainWindow::ApplyDeviceVolumePercent(int pct)
    {
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        updatingVolumeFromDevice_ = true;
        if (auto slider = VolumeSlider())
            slider.Value(pct);
        if (auto text = VolumeValueText())
            text.Text(AToW(std::to_string(pct) + "%"));
        updatingVolumeFromDevice_ = false;
    }

    void MainWindow::OnAutoMuteChanged(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (service_) {
            bool enabled = AutoMuteCheck().IsChecked().Value();
            service_->set_auto_mute_output(enabled);
            AppendLog(LogTag::Data, enabled ? "Auto-mute output: ON (speakers muted while streaming)"
                                            : "Auto-mute output: OFF");
        }
    }

    void MainWindow::OnAutoConnectChanged(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        settings_.auto_connect_on_start = AutoConnectCheck().IsChecked().Value();
        settings_.save();
        AppendLog(LogTag::Data, settings_.auto_connect_on_start ? "Auto-connect on start: ON"
                                                              : "Auto-connect on start: OFF");
    }

    void MainWindow::ApplyBackdrop()
    {
        if (!uiReady_ && !this->Content()) return;

        const bool wanted = settings_.mica_enabled;
        const bool hc = InHighContrast();

        if (!wanted || hc) {
            /* Opaque fallback: clearing SystemBackdrop leaves the window on the
             * solid theme background, which is exactly what the plan requires
             * where Mica cannot be used. */
            this->SystemBackdrop(nullptr);
            if (auto t = BackdropStateText()) {
                if (hc) {
                    t.Text(L"High contrast is on, so the window uses a solid background.");
                } else {
                    t.Text(L"Solid background. Turn this on to use the Windows 11 material.");
                }
            }
            return;
        }

        try {
            this->SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop());
            if (auto t = BackdropStateText()) {
                t.Text(L"Translucent background active.");
            }
        } catch (...) {
            /* Older Windows build without Mica: fall back to the solid theme
             * background and say so instead of failing silently. */
            this->SystemBackdrop(nullptr);
            if (auto t = BackdropStateText()) {
                t.Text(L"This Windows version does not support the translucent background; using a solid background.");
            }
        }
    }

    void MainWindow::OnAppearanceChanged(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!uiReady_) return;
        if (auto c = MicaCheck()) {
            if (c.IsChecked()) settings_.mica_enabled = true; else settings_.mica_enabled = false;
        }
        if (auto c = ReduceMotionCheck()) {
            g_reduce_motion = c.IsChecked() ? true : false;
            settings_.reduce_motion = g_reduce_motion;
        }
        settings_.save();
        ApplyBackdrop();
    }

    void MainWindow::RememberLastDevice(const std::string& mac, const std::string& name)
    {
        settings_.last_device_mac = mac;
        settings_.last_device_name = name;
        settings_.save();
        ReconnectLastButton().IsEnabled(true);
        UpdateStatusDevice(name, mac);
    }

    void MainWindow::SetStreamingUi(bool streaming)
    {
        isStreaming_ = streaming;
        if (!streaming) {
            /* The stats callback stops firing, so the last values are no longer
             * current - grey them out instead of leaving them to look live. */
            stats_are_fresh_ = false;
            if (StatsFreshnessText()) UpdateFreshness();
            if (DeviceCapsText()) UpdateDeviceCaps();
        }
        ConnectButton().Content(winrt::box_value(streaming ? UiText(L"Disconnect", L"연결 끊기") : UiText(L"Connect", L"연결")));
        DirectConnectButton().Content(winrt::box_value(streaming ? UiText(L"Disconnect", L"연결 끊기") : UiText(L"Direct Connect", L"주소로 연결")));
    }

    void MainWindow::StopStream()
    {
        if (!service_) return;
        service_->stop_streaming();
        SetStreamingUi(false);
        AppendLog(LogTag::Ok, "Disconnected.");
    }

    void MainWindow::StartStream(const std::string& mac, const std::string& name, const std::string& logText)
    {
        if (!service_ || isStreaming_) {
            if (isStreaming_) AppendLog(LogTag::Warn, "Disconnect first.");
            return;
        }

        ConnectionProfile profile{};
        profile.device_address = mac;
        profile.device_name = name;
        profile.codec = ProfileManager::index_to_codec(selectedCodecIndex_);
        profile.quality = ProfileManager::index_to_quality(selectedQualityIndex_);
        profile.bitrate_kbps = static_cast<uint32_t>(selectedBitrateKbps_);
        profile.bit_depth = ProfileManager::index_to_bit_depth(selectedBitDepthIndex_);
        profile.capture_mode = "loopback";
        profile.sample_rate = SelectedSampleRate();

        AppendLog(logText + " -> " + name + " [" + profile.codec + "]...");
        RememberLastDevice(mac, name);

        service_->start_streaming(profile);
        SetStreamingUi(true);
    }

    void MainWindow::OnReconnectLastClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (!service_) return;
        if (settings_.last_device_mac.empty()) {
            AppendLog(LogTag::Warn, "No previous device. Connect once via Scan/Direct Connect first.");
            return;
        }
        StartStream(settings_.last_device_mac, settings_.last_device_name, "Reconnecting");
    }

    void MainWindow::OnSetupClick(IInspectable const&, RoutedEventArgs const&)
    {
        ShowSetupGuide();
    }

    void MainWindow::ShowSetupGuide()
    {
        auto dialog = ContentDialog();
        dialog.Title(winrt::box_value(UiText(L"Setup & Help", L"설정 및 도움말")));

        auto scroll = ScrollViewer();
        scroll.MaxHeight(520);
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);

        auto stack = StackPanel();
        stack.Spacing(8);

        auto heading = [](const std::wstring& t) {
            auto tb = TextBlock();
            tb.Text(t);
            tb.FontWeight(winrt::Windows::UI::Text::FontWeight{ 600 });
            return tb;
        };
        auto para = [](const std::wstring& t) {
            auto tb = TextBlock();
            tb.Text(t);
            tb.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
            tb.Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0.0, 0.0, 0.0, 4.0 });
            return tb;
        };

        if (IsKorean()) {
            stack.Children().Append(heading(L"설치 및 페어링"));
            stack.Children().Append(para(L"Secure Boot를 켠 상태에서 지원 동글용 서명된 WinUSB 드라이버를 사용하세요. "
                                         L"A760은 문제 해결 메뉴에서 감지와 HCI 진단을 할 수 있습니다."));
            stack.Children().Append(para(L"헤드셋을 페어링 모드로 전환한 뒤 ‘장치 검색’을 누르세요. "
                                         L"목록에서 헤드셋과 코덱을 선택하고 ‘연결’을 누르세요."));
            stack.Children().Append(heading(L"Windows Bluetooth 복원"));
            stack.Children().Append(para(L"스트리밍을 중지하고 문제 해결 메뉴에서 ‘Windows Bluetooth 복원 안내’를 여세요. "
                                         L"자세한 절차는 배포 ZIP의 docs/ko/setup.md를 참조하세요."));
        } else {
            stack.Children().Append(heading(L"How streaming works"));
            stack.Children().Append(para(L"SSC On Windows streams Windows audio over a USB Bluetooth dongle "
                                         L"(TP-Link UB500 / Realtek RTL8761B) with BTstack in USB (WinUSB) mode. "
                                         L"This bypasses the Windows Bluetooth stack entirely."));
            stack.Children().Append(para(L"With WinUSB active, this dongle is dedicated to SSC On Windows. "
                                         L"The Troubleshooting panel provides Device Manager instructions."));

            stack.Children().Append(heading(L"First-time setup"));
            stack.Children().Append(para(L"1) Connect the dongle. If Windows shows it as a Bluetooth adapter "
                                         L"(BTHUSB), use a trusted signed WinUSB package for that device."));
            stack.Children().Append(para(L"2) Put the Buds3 FE into pairing mode (hold both earbuds until the LED blinks)."));
            stack.Children().Append(para(L"3) Click 'Scan Devices' (about 8 s), then select your buds in the list."));
            stack.Children().Append(para(L"4) Pick a codec (SSC recommended for Buds3 FE; AAC/SBC fall back if unsupported) and click 'Connect'."));
            stack.Children().Append(para(L"5) Optional: tick 'Auto-connect last device on start' to resume streaming at launch."));

            stack.Children().Append(heading(L"Troubleshooting / FAQ"));
            stack.Children().Append(para(L"Q: No sound?  A: Verify the dongle state shows 'WinUSB (Streaming mode)' and a source "
                                         L"is playing. Windows output volume does not change the captured mix."));
            stack.Children().Append(para(L"Q: Reconnect fails?  A: 'Reconnect Last' reuses the saved link key. If the buds were "
                                         L"reset, delete %APPDATA%\\A2DPWB\\profiles.json so a fresh pair happens."));
            stack.Children().Append(para(L"Q: SSC bitrate?  A: 48 kHz High 229 / Standard 192 / Mobile 128 kbps; "
                                         L"'96k UHQ' upsamples to 96 kHz (250 / 442 / 584 kbps). Note: UHQ needs "
                                         L"the device to advertise the SSC UHQ capability bit - if it does not "
                                         L"(e.g. Buds3 FE), the app falls back to 48 kHz."));
            stack.Children().Append(para(L"Q: Back to normal Windows Bluetooth?  A: Open the restore instructions "
                                         L"under Troubleshooting and follow them in Device Manager."));
        }

        scroll.Content(stack);
        dialog.Content(scroll);
        dialog.PrimaryButtonText(UiText(L"Close", L"닫기"));
        dialog.XamlRoot(this->Content().XamlRoot());
        dialog.ShowAsync();
    }

    void MainWindow::ShowDriverInstructions(bool enable_winusb)
    {
        if (isStreaming_) {
            AppendLog(LogTag::Warn, "Stop streaming before switching the dongle driver.");
            return;
        }

        const auto adapter = FindRtl8761Adapter();
        const auto report = adapter
            ? get_dongle_driver_instructions(adapter->vid, adapter->pid, enable_winusb, IsKorean())
            : DongleSwitchReport{DongleSwitchResult::NotPresent,
                  UiText("Connect one supported RTL8761BU adapter to identify the device. For other models, check the hardware ID and driver in Device Manager.",
                         "지원되는 RTL8761BU 동글 한 대를 연결하세요. 다른 모델은 장치 관리자에서 하드웨어 ID와 드라이버를 확인하세요.")};

        ContentDialog dialog;
        dialog.Title(box_value(enable_winusb ? UiText(L"WinUSB Setup", L"WinUSB 설정") : UiText(L"Windows Bluetooth Restore", L"Windows Bluetooth 복원")));
        TextBlock instructions;
        instructions.Text(AToW(report.message));
        instructions.TextWrapping(TextWrapping::Wrap);
        dialog.Content(instructions);
        dialog.CloseButtonText(UiText(L"Close", L"닫기"));
        dialog.XamlRoot(this->Content().XamlRoot());
        dialog.ShowAsync();
    }

    void MainWindow::OnEnableStreamingClick(IInspectable const&, RoutedEventArgs const&)
    {
        ShowDriverInstructions(true);
    }

    void MainWindow::OnRestoreBtClick(IInspectable const&, RoutedEventArgs const&)
    {
        ShowDriverInstructions(false);
    }

    void MainWindow::UpdateStats(const A2dpService::StreamStats& stats)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), UiText("Latency: %.2f ms", "지연 시간: %.2f ms"), stats.latency_ms);
        LatencyText().Text(AToW(buf));
        snprintf(buf, sizeof(buf), UiText("Error Rate: %.2f%%", "오류율: %.2f%%"), stats.error_rate * 100.0);
        ErrorRateText().Text(AToW(buf));
        snprintf(buf, sizeof(buf), UiText("Loss Rate: %.2f%%", "손실률: %.2f%%"), stats.loss_rate * 100.0);
        LossRateText().Text(AToW(buf));
        snprintf(buf, sizeof(buf), UiText("Queue: %u  (sends=%llu fails=%llu)", "대기열: %u (전송=%llu 실패=%llu)"), stats.queue_depth,
                 (unsigned long long)stats.total_sends, (unsigned long long)stats.send_fails);
        QueueText().Text(AToW(buf));

        /* Severity highlighting, mirroring the CLI health rows (warn latency > 15 ms,
         * error/loss above the sparkline's 1 % visibility threshold are red). */
        LatencyText().Foreground(nullptr);
        ErrorRateText().Foreground(nullptr);
        LossRateText().Foreground(nullptr);
        if (stats.latency_ms > 15.0)
            LatencyText().Foreground(TagBrush(L"TagWarnBrush"));
        if (stats.error_rate >= 0.01f)
            ErrorRateText().Foreground(TagBrush(L"TagErrorBrush"));
        if (stats.loss_rate >= 0.01f)
            LossRateText().Foreground(TagBrush(L"TagErrorBrush"));

        /* Overall verdict from the same real numbers. Thresholds come from the
         * frame budget: an SSC frame is 864 samples, i.e. 18 ms at 48 kHz, so an
         * encode round-trip above that can no longer keep up.
         * docs/dev/PLAN_UX_DESIGN.md Faz 2: a short health word beside the
         * values, never a bare number. */
        const char* verdict = UiText("Good", "양호");
        LogTag tone = LogTag::Ok;
        if (stats.latency_ms > 18.0 || stats.error_rate >= 0.05 ||
            stats.loss_rate >= 0.02 || stats.queue_depth >= 32) {
            verdict = UiText("Poor", "불량");
            tone = LogTag::Error;
        } else if (stats.latency_ms > 10.0 || stats.error_rate >= 0.01 ||
                   stats.loss_rate >= 0.005 || stats.queue_depth >= 8) {
            verdict = UiText("Fair", "보통");
            tone = LogTag::Warn;
        }
        std::string health(verdict);
        health += "  -  ";
        std::string why;
        auto why_add = [&why](const char* s) {
            if (!why.empty()) why += "; ";
            why += s;
        };
        if (stats.latency_ms > 18.0)
            why_add("encoder too slow for the frame budget");
        if (stats.error_rate >= 0.01)
            why_add("send errors");
        if (stats.loss_rate >= 0.005)
            why_add("audio dropped");
        if (stats.queue_depth >= 8)
            why_add("transport queue backing up");
        health += why.empty() ? "no dropped frames or send errors" : why;
        StreamHealthText().Text(AToW(health));
        if (auto brush = TagBrush(tone == LogTag::Ok    ? L"TagOkBrush"
                                 : tone == LogTag::Warn  ? L"TagWarnBrush"
                                                         : L"TagErrorBrush")) {
            StreamHealthText().Foreground(brush);
        }

        /* Mark the block fresh again; UpdateFreshness() ages it from here so a
         * dead stats feed cannot masquerade as live numbers. */
        last_stats_tick_ = GetTickCount64();
        stats_are_fresh_ = true;
        UpdateFreshness();

        UpdateDeviceCaps();

        // Feed history for the sparkline (~120 samples â‰ˆ 4 min at 2 Hz)
        sparkLatency_.push_back(static_cast<float>(stats.latency_ms));
        sparkError_.push_back(static_cast<float>(stats.error_rate));
        while (sparkLatency_.size() > 120) sparkLatency_.pop_front();
        while (sparkError_.size() > 120) sparkError_.pop_front();
        UpdateSparkline();
    }

    /* Plan Faz 2: dynamic values must not look current when they are not.
     * The core pushes stats ~1 Hz while streaming and nothing at all when
     * stopped, so after a few seconds without a tick the panel is greyed out
     * and the freshness line says so explicitly. */
    void MainWindow::UpdateFreshness()
    {
        if (!StatsFreshnessText()) return;
        if (!stats_are_fresh_) {
            StatsFreshnessText().Text(UiText(L"no stream - statistics are not being collected", L"스트림이 없어 통계를 수집하지 않습니다"));
            for (auto t : { LatencyText(), ErrorRateText(), LossRateText(), QueueText() }) {
                if (t) t.Opacity(0.45);
            }
            return;
        }
        uint64_t age_ms = GetTickCount64() - last_stats_tick_;
        char buf[64];
        snprintf(buf, sizeof(buf), UiText("updated %.1f s ago", "%.1f초 전에 업데이트"), age_ms / 1000.0);
        StatsFreshnessText().Text(AToW(buf));
        const bool stale = age_ms > 6000;
        for (auto t : { LatencyText(), ErrorRateText(), LossRateText(), QueueText() }) {
            if (t) t.Opacity(stale ? 0.45 : 1.0);
        }
        if (stale) StatsFreshnessText().Text(UiText(L"last update over 6 s ago - values may be stale", L"마지막 업데이트 후 6초 이상 지났습니다"));
    }

    /* Plan Faz 2: show what the headset really advertises. Nothing is inferred;
     * before the link is up the panel says the capabilities are unknown rather
     * than rendering an all-false list. */
    void MainWindow::UpdateDeviceCaps()
    {
        auto caps = service_ ? service_->get_device_caps() : A2dpService::DeviceCaps{};
        if (!caps.known) {
            DeviceCapsText().Text(UiText(L"Not known until connected.", L"장치를 연결하면 확인할 수 있습니다."));
            return;
        }
        std::string list;
        auto add = [&list](const char* name) {
            if (!list.empty()) list += ", ";
            list += name;
        };
        if (caps.sbc) add("SBC");
        if (caps.aac) add("AAC");
        if (caps.ssc) add("SSC");
        if (list.empty()) list = "none advertised";
        std::string text = "Codecs: " + list;
        if (caps.ssc) {
            char buf[32];
            snprintf(buf, sizeof(buf), " (cap 0x%02X)", caps.ssc_cap);
            text += buf;
        }
        DeviceCapsText().Text(AToW(text));

        /* UHQ gets its own line because it is the one capability whose absence
         * silently produces silence: the app then keeps the stream at 48 kHz. */
        std::string uhq;
        if (!caps.ssc) {
            uhq = "UHQ 96 kHz: not applicable (this headset does not offer SSC).";
        } else if (caps.ssc_uhq) {
            uhq = "UHQ 96 kHz: supported - 96k can be selected in Audio settings.";
        } else {
            uhq = "UHQ 96 kHz: not advertised by this headset - the stream stays at 48 kHz. "
                  "Selecting 96k anyway produces silence, so the app falls back automatically.";
        }
        DeviceCapsHintText().Text(AToW(uhq));
    }

    void MainWindow::UpdateSparkline()
    {
        constexpr float kPad = 2.0f;
        double w = SparklineLatency().ActualWidth();
        double h = SparklineLatency().ActualHeight();
        if (w <= 0) w = 240.0;
        if (h <= 0) h = 46.0;
        float innerW = static_cast<float>(w - 2.0 * kPad);
        float innerH = static_cast<float>(h - 2.0 * kPad);

        // Latency line: 0..25 ms vertically centered on the graph
        auto latPts = winrt::Microsoft::UI::Xaml::Media::PointCollection();
        size_t n = sparkLatency_.size();
        if (n > 1) {
            for (size_t i = 0; i < n; ++i) {
                float v = sparkLatency_[i];
                if (v < 0.0f) v = 0.0f;
                if (v > 25.0f) v = 25.0f;
                float x = kPad + static_cast<float>(i) / (n - 1) * innerW;
                float y = kPad + innerH - (v / 25.0f) * innerH;
                latPts.Append(winrt::Windows::Foundation::Point(x, y));
            }
        }
        SparklineLatency().Points(latPts);

        // Error line: 0..5% overlaid in red
        auto errPts = winrt::Microsoft::UI::Xaml::Media::PointCollection();
        if (n > 1) {
            for (size_t i = 0; i < n; ++i) {
                float v = sparkError_[i];
                if (v < 0.0f) v = 0.0f;
                if (v > 0.05f) v = 0.05f;
                float x = kPad + static_cast<float>(i) / (n - 1) * innerW;
                float y = kPad + innerH - (v / 0.05f) * innerH;
                errPts.Append(winrt::Windows::Foundation::Point(x, y));
            }
        }
        SparklineError().Points(errPts);
    }

    void MainWindow::UpdateUI(const A2dpService::State& state, const std::string& text)
    {
        UpdateStatusCard(state, text);

        switch (state) {
        case A2dpService::State::Idle:
            SetStreamingUi(false);
            ScanButton().IsEnabled(true);
            break;
        case A2dpService::State::Connecting:
            ScanButton().IsEnabled(false);
            ConnectButton().IsEnabled(false);
            break;
        case A2dpService::State::Streaming:
            ScanButton().IsEnabled(false);
            ConnectButton().IsEnabled(true);
            SetStreamingUi(true);
            break;
        case A2dpService::State::Reconnecting:
            ScanButton().IsEnabled(false);
            ConnectButton().IsEnabled(false);
            break;
        case A2dpService::State::Error:
            ScanButton().IsEnabled(true);
            SetStreamingUi(false);
            if (hasSelectedDevice_) ConnectButton().IsEnabled(true);
            break;
        }
        AppendLog(LogTag::Data, "[State] " + text);
    }

    /* Status card = the one place that answers "is it working, and what do I do
     * next?". Glyph + headline + explanation + explicit next step, so the state
     * never depends on color alone (PLAN_UX_DESIGN.md Faz 1). */
    void MainWindow::UpdateStatusCard(const A2dpService::State& state, const std::string& key)
    {
        StateCopy copy = BaseCopyForState(state);

        /* A known localization key refines (and for errors overrides) the copy.
         * status.connected while streaming should still read as streaming. */
        if (!key.empty()) {
            const StateCopy& byKey = CopyForKey(key);
            const bool isErrorKey = key.rfind("error.", 0) == 0;
            const bool keyDrivesHeadline = isErrorKey || state == A2dpService::State::Idle;
            if (keyDrivesHeadline) copy.headline = byKey.headline;
            if (byKey.detail && byKey.detail[0]) copy.detail = byKey.detail;
            if (byKey.action && byKey.action[0]) copy.action = byKey.action;
            if (isErrorKey || state != A2dpService::State::Streaming) {
                copy.glyph = byKey.glyph;
                copy.tone = byKey.tone;
            }
        }

        StatusHeadlineText().Text(AToW(copy.headline));
        StatusDetailText().Text(AToW(copy.detail));

        if (copy.action && copy.action[0]) {
            StatusActionText().Text(AToW(std::string(UiText("Next: ", "다음 단계: ")) + copy.action));
            StatusActionText().Visibility(Visibility::Visible);
        } else {
            StatusActionText().Text(L"");
            StatusActionText().Visibility(Visibility::Collapsed);
        }

        StatusGlyph().Glyph(hstring(std::wstring(1, copy.glyph)));

        /* Tone drives only the glyph color - the headline text carries the same
         * information, so color is never the sole signal. */
        switch (copy.tone) {
        case Tone::Ok:    StatusGlyph().Foreground(TagBrush(L"TagOkBrush"));    break;
        case Tone::Warn:  StatusGlyph().Foreground(TagBrush(L"TagWarnBrush"));  break;
        case Tone::Error: StatusGlyph().Foreground(TagBrush(L"TagErrorBrush")); break;
        default:          StatusGlyph().Foreground(TagBrush(L"TagDataBrush"));  break;
        }
    }

    void MainWindow::UpdateStatusDevice(const std::string& name, const std::string& mac)
    {
        StatusDeviceText().Text(name.empty() ? hstring(L"\u2014") : AToW(name));
        StatusDeviceMacText().Text(mac.empty() ? hstring(L"") : AToW(mac));
    }

    void MainWindow::OnToggleLogClick(IInspectable const&, RoutedEventArgs const&)
    {
        /* Collapse by hiding the panel only - the TextBlocks stay in LogLines so
         * history survives (PLAN_UX_DESIGN.md Faz 1: "keeps entries"). */
        auto panel = LogPanel();
        if (!panel) return;
        const bool collapsed = panel.Visibility() == Visibility::Collapsed;
        panel.Visibility(collapsed ? Visibility::Visible : Visibility::Collapsed);
        LogToggleButton().Content(winrt::box_value(collapsed ? UiText(L"Hide log", L"로그 숨기기") : UiText(L"Show log", L"로그 표시")));
        if (collapsed) {
            dispatcher_.TryEnqueue([this] {
                auto scrollable = LogScroller().ScrollableHeight();
                LogScroller().ScrollToVerticalOffset(scrollable);
            });
        }
    }

    void MainWindow::OnA760DiagnosticsClick(IInspectable const&, RoutedEventArgs const&)
    {
        RunA760Diagnostics();
    }

    winrt::fire_and_forget MainWindow::RunA760Diagnostics()
    {
        auto weak = get_weak();
        const bool korean = IsKorean();
        A760DiagnosticsButton().IsEnabled(false);
        winrt::apartment_context ui;
        co_await winrt::resume_background();
        std::string result;
        try {
            result = diagnose_a760(korean);
        } catch (...) {
            result = korean ? "A760 진단 중 오류가 발생했습니다." : "A760 diagnostics failed.";
        }
        co_await ui;
        if (auto self = weak.get()) {
            self->A760DiagnosticsButton().IsEnabled(true);
            self->A760DiagnosticsResult().Text(AToW(result));
            self->AppendLog(LogTag::Data, result);
        }
    }

    void MainWindow::OnRefreshDriverClick(IInspectable const&, RoutedEventArgs const&)
    {
        RefreshDriverMode();
    }

    void MainWindow::RefreshDriverMode()
    {
        const auto adapter = FindRtl8761Adapter();
        const auto st = adapter ? adapter->status : DongleDriverStatus{};

        std::wstring state;
        std::string detail;
        switch (st.mode) {
        case DongleDriverMode::WinUsbStream:
            state = UiText(L"WinUSB (Streaming mode)", L"WinUSB (스트리밍 모드)");
            detail = std::string(UiText("Service: ", "서비스: ")) + st.service + UiText(" | Device: ", " | 장치: ") + st.device_name;
            break;
        case DongleDriverMode::BthUsb:
            state = L"BTHUSB (Windows Bluetooth)";
            detail = std::string(UiText("Service: ", "서비스: ")) + st.service + UiText(" | Device: ", " | 장치: ") + st.device_name;
            break;
        case DongleDriverMode::Other:
            state = UiText(L"Other driver", L"기타 드라이버");
            detail = std::string(UiText("Service: ", "서비스: ")) + st.service + UiText(" | Device: ", " | 장치: ") + st.device_name;
            break;
        case DongleDriverMode::NotPresent:
        default:
            state = UiText(L"No single supported dongle detected", L"지원 동글 한 대를 감지하지 못했습니다");
            detail = UiText("Connect one RTL8761BU (2357:0604 or 0BDA:8771). Check other hardware IDs in Device Manager.", "RTL8761BU(2357:0604 또는 0BDA:8771) 한 대를 연결하세요. 기타 하드웨어 ID는 장치 관리자에서 확인하세요.");
            break;
        }

        const auto a760 = detect_dongle_driver(A760_VID, A760_PID);
        if (a760.mode != DongleDriverMode::NotPresent) {
            detail += UiText("\nA760 detected. Use A760 HCI diagnostics.",
                             "\nA760을 감지했습니다. A760 HCI 진단을 사용하세요.");
        }
        DriverStateText().Text(state);
        DriverDetailText().Text(AToW(detail));
        AppendLog(LogTag::Data, "[DriverMode] " + detail);
    }

    void MainWindow::AppendLog(const std::string& text)
    {
        AppendLog(LogTag::Info, text);
    }

    void MainWindow::AppendLog(LogTag tag, const std::string& text)
    {
        Microsoft::UI::Xaml::Media::SolidColorBrush brush{ nullptr };
        switch (tag) {
        case LogTag::Ok:    brush = TagBrush(L"TagOkBrush");    break;
        case LogTag::Warn:  brush = TagBrush(L"TagWarnBrush");  break;
        case LogTag::Error: brush = TagBrush(L"TagErrorBrush"); break;
        case LogTag::Data:  brush = TagBrush(L"TagDataBrush");  break;
        default:            break;
        }
        AppendLogLine(std::wstring(AToW(text).c_str()), brush);
    }

    void MainWindow::AppendLogLine(const std::wstring& text,
                                   const Microsoft::UI::Xaml::Media::SolidColorBrush& brush)
    {
        using namespace Microsoft::UI::Xaml::Controls;

        /* One TextBlock per log line. Whole-line severity tint mirrors the CLI tag
         * language; the mono log style comes from App.xaml (MonoLogTextStyle). */
        TextBlock line;
        line.Text(hstring(text));
        if (brush) line.Foreground(brush);
        try {
            auto resources = Application::Current().Resources();
            auto styleValue = resources.TryLookup(winrt::box_value(L"MonoLogTextStyle"));
            if (styleValue) line.Style(styleValue.as<Microsoft::UI::Xaml::Style>());
        } catch (...) {}

        auto children = LogLines().Children();
        children.Append(line);
        while (children.Size() > kMaxLogLines) children.RemoveAt(0);

        /* Mirror the newest line into the collapsed-log summary. */
        if (auto summary = LogSummaryText()) summary.Text(hstring(text));

        /* Scroll after layout settles so the newest line is visible. */
        dispatcher_.TryEnqueue([this] {
            auto scrollable = LogScroller().ScrollableHeight();
            LogScroller().ScrollToVerticalOffset(scrollable);
        });
    }
}
