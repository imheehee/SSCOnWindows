// SPDX-License-Identifier: MIT
#include "a760_diagnostics.h"

#include "a760_firmware.h"
#include "bt_adapter_enum.h"
#include "driver_mode.h"

#include <windows.h>
#include <winusb.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

namespace {
struct FileCloser {
    void operator()(void *handle) const {
        CloseHandle(handle);
    }
};
struct WinUsbCloser {
    void operator()(void *handle) const {
        WinUsb_Free(handle);
    }
};
using FileHandle = std::unique_ptr<void, FileCloser>;

bool finish_transfer(BOOL completed, HANDLE device, WINUSB_INTERFACE_HANDLE usb,
                     OVERLAPPED &io, ULONG &size, DWORD timeout) {
    if (completed) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    const DWORD wait = WaitForSingleObject(io.hEvent, timeout);
    if (wait == WAIT_OBJECT_0)
        return WinUsb_GetOverlappedResult(usb, &io, &size, FALSE) != FALSE;

    // Drain the cancelled I/O before its OVERLAPPED and buffer leave scope.
    const DWORD error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    CancelIoEx(device, &io);
    WinUsb_GetOverlappedResult(usb, &io, &size, TRUE);
    SetLastError(error);
    return false;
}

unsigned le16(const UCHAR *data) {
    return data[0] | (static_cast<unsigned>(data[1]) << 8);
}
struct HciReply {
    enum class Result { Complete,
                        SendFailed,
                        ReadFailed,
                        Timeout } result = Result::Timeout;
    std::array<UCHAR, 257> bytes{};
    ULONG size = 0;
    DWORD error = ERROR_SUCCESS;
};

HciReply read_hci(uint16_t opcode, HANDLE device, WINUSB_INTERFACE_HANDLE usb,
                  UCHAR pipe, HANDLE event, const uint8_t *parameters = nullptr,
                  size_t length = 0) {
    HciReply reply;
    if (length > 255) return reply;
    std::array<UCHAR, 258> command{};
    command[0] = static_cast<UCHAR>(opcode);
    command[1] = static_cast<UCHAR>(opcode >> 8);
    command[2] = static_cast<UCHAR>(length);
    if (length) std::copy_n(parameters, length, command.data() + 3);
    const auto command_size = static_cast<USHORT>(length + 3);
    WINUSB_SETUP_PACKET setup{0x20, 0, 0, 0, command_size};
    ResetEvent(event);
    OVERLAPPED io{};
    io.hEvent = event;
    ULONG sent = 0;
    if (!finish_transfer(WinUsb_ControlTransfer(usb, setup, command.data(), command_size, &sent, &io),
                         device, usb, io, sent, 1000)) {
        reply.result = HciReply::Result::SendFailed;
        reply.error = GetLastError();
        return reply;
    }
    if (sent != command_size) {
        reply.result = HciReply::Result::SendFailed;
        reply.error = ERROR_BAD_LENGTH;
        return reply;
    }

    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (auto now = GetTickCount64(); now < deadline; now = GetTickCount64()) {
        ResetEvent(event);
        io = {};
        io.hEvent = event;
        if (!finish_transfer(WinUsb_ReadPipe(usb, pipe, reply.bytes.data(),
                                             static_cast<ULONG>(reply.bytes.size()), &reply.size, &io),
                             device, usb, io, reply.size, static_cast<DWORD>(deadline - now))) {
            reply.result = HciReply::Result::ReadFailed;
            reply.error = GetLastError();
            return reply;
        }
        const auto *p = reply.bytes.data();
        if (reply.size >= 6 && reply.size == 2u + p[1] && p[0] == 0x0e && le16(p + 3) == opcode) {
            reply.result = HciReply::Result::Complete;
            return reply;
        }
    }
    return reply;
}

std::string hci_failure(uint16_t opcode, const HciReply &reply, bool korean) {
    const char *phase = korean ? "응답 시간 초과" : "response timeout";
    switch (reply.result) {
    case HciReply::Result::SendFailed:
        phase = korean ? "전송 실패" : "send failed";
        break;
    case HciReply::Result::ReadFailed:
        phase = korean ? "응답 읽기 실패" : "read failed";
        break;
    case HciReply::Result::Complete:
        phase = reply.bytes[5] ? (korean ? "컨트롤러 오류" : "controller error")
                               : (korean ? "잘못된 응답 길이" : "unexpected response length");
        break;
    case HciReply::Result::Timeout:
        break;
    }
    char details[192];
    snprintf(details, sizeof(details), "HCI 0x%04X: %s (Win32=%lu, status=0x%02X, bytes=%lu)",
             static_cast<unsigned>(opcode), phase, reply.error,
             reply.result == HciReply::Result::Complete ? static_cast<unsigned>(reply.bytes[5]) : 0u,
             reply.size);
    return details;
}

bool is_a760_descriptor(WINUSB_INTERFACE_HANDLE handle) {
    USB_DEVICE_DESCRIPTOR descriptor{};
    ULONG size = 0;
    return WinUsb_GetDescriptor(handle, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                                reinterpret_cast<PUCHAR>(&descriptor), sizeof(descriptor), &size) &&
           size == sizeof(descriptor) && descriptor.idVendor == A760_VID &&
           descriptor.idProduct == A760_PID && descriptor.bcdDevice == A760_REV;
}

bool matches_version(const HciReply &version, unsigned revision, unsigned subversion) {
    const auto *reply = version.bytes.data();
    return version.result == HciReply::Result::Complete && version.size == 14 && !reply[5] &&
           reply[6] == 0x0a && reply[9] == 0x0a && le16(reply + 10) == 0x005d &&
           le16(reply + 7) == revision && le16(reply + 12) == subversion;
}

std::string describe_version(const HciReply &version, bool korean) {
    if (version.result != HciReply::Result::Complete || version.bytes[5] || version.size != 14)
        return hci_failure(0x1001, version, korean);
    const auto *p = version.bytes.data();
    char details[160];
    snprintf(details, sizeof(details),
             "HCI 0x1001 status=0x00 | HCI=0x%02X rev=0x%04X | LMP=0x%02X subver=0x%04X | Manufacturer=0x%04X",
             static_cast<unsigned>(p[6]), le16(p + 7), static_cast<unsigned>(p[9]),
             le16(p + 12), le16(p + 10));
    return details;
}

// btrtl includes the legacy config only when the Realtek security-project key ID is zero.
bool verify_security_project(HANDLE device, WINUSB_INTERFACE_HANDLE usb, uint8_t pipe,
                             HANDLE event, std::string &report) {
    constexpr std::array<uint8_t, 5> security_project_register{0x10, 0xa4, 0xad, 0x00, 0xb0};
    const auto response = read_hci(0xfc61, device, usb, pipe, event,
                                   security_project_register.data(), security_project_register.size());
    if (response.result != HciReply::Result::Complete || response.size != 8 || response.bytes[5]) {
        report += hci_failure(0xfc61, response, false) + "\n";
        return false;
    }
    char details[112];
    snprintf(details, sizeof(details), "HCI 0xFC61 status=0x00 SEC_PROJ=0x%04X key_id=%u\n",
             le16(response.bytes.data() + 6), static_cast<unsigned>(response.bytes[6]));
    report += details;
    if (response.bytes[6] == 0) return true;
    report += "ABORT: this RAM experiment requires Realtek key ID 0 for the pinned legacy config\n";
    return false;
}

} // namespace

static std::string diagnose_a760_impl(bool korean, const std::string *expected_path, bool *rom_ok) {
    const auto tr = [korean](const char *en, const char *ko) { return korean ? ko : en; };
    const auto status = detect_dongle_driver(A760_VID, A760_PID);
    std::string report = "USB 0BDA:A760";
    if (status.mode == DongleDriverMode::NotPresent)
        return report + tr(" is not present.", " 장치를 찾을 수 없습니다.");

    report += tr(" | Driver service: ", " | 드라이버 서비스: ") + status.service;
    if (status.usb_revision) {
        char revision[16];
        snprintf(revision, sizeof(revision), " | REV_%04X", static_cast<unsigned>(status.usb_revision));
        report += revision;
    }
    report += '\n';
    if (status.mode != DongleDriverMode::WinUsbStream)
        return report + tr("PnP detection only. Raw HCI requires an existing WinUSB binding.",
                           "PnP 감지 결과입니다. 원시 HCI 진단은 이미 WinUSB가 연결된 장치에서만 사용할 수 있습니다.");

    std::string path;
    for (const auto &adapter : BtAdapterEnumerator::enumerate(true)) {
        if (!path.empty())
            return report + tr("Connect only one A760 for HCI diagnosis.", "HCI 진단에는 A760 한 대만 연결하세요.");
        path = adapter.device_path;
    }
    const auto unavailable = tr("Could not open the A760 Bluetooth interface exclusively.",
                                "A760 Bluetooth 인터페이스를 단독으로 열 수 없습니다.");
    if (path.empty()) return report + unavailable;
    if (expected_path && _stricmp(expected_path->c_str(), path.c_str()) != 0)
        return report + "ABORT: WinUSB A760 interface changed after verification";

    const HANDLE raw = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return report + unavailable;
    const FileHandle device(raw);
    WINUSB_INTERFACE_HANDLE handle;
    if (!WinUsb_Initialize(raw, &handle)) return report + unavailable;
    const std::unique_ptr<void, WinUsbCloser> usb(handle);

    if (!is_a760_descriptor(handle)) {
        return report + tr("HCI skipped: USB descriptor must identify 0BDA:A760 REV_0200.",
                           "USB 장치 설명자가 0BDA:A760 REV_0200과 일치할 때만 HCI 명령을 보냅니다.");
    }

    USB_INTERFACE_DESCRIPTOR iface{};
    if (!WinUsb_QueryInterfaceSettings(handle, 0, &iface) ||
        iface.bInterfaceClass != 0xe0 || iface.bInterfaceSubClass != 1 ||
        iface.bInterfaceProtocol != 1) {
        return report + unavailable;
    }
    UCHAR event_pipe = 0;
    for (UCHAR i = 0; i < iface.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (WinUsb_QueryPipe(handle, 0, i, &pipe) &&
            pipe.PipeType == UsbdPipeTypeInterrupt && (pipe.PipeId & 0x80)) {
            event_pipe = pipe.PipeId;
            break;
        }
    }
    const FileHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event_pipe || !event) return report + unavailable;

    constexpr uint16_t local_version = 0x1001;
    const auto version = read_hci(local_version, raw, handle, event_pipe, event.get());
    if (version.result != HciReply::Result::Complete || version.bytes[5] || version.size != 14)
        return report + hci_failure(local_version, version, korean);

    report += describe_version(version, korean) + "\n";
    char details[160];
    const bool expected_identity = matches_version(version, 0x000b, 0x8761);
    if (!expected_identity)
        return report + tr("ROM query skipped: expected the unpatched RTL8761BU identity.",
                           "ROM 조회 생략: 패치 전 RTL8761BU 식별값과 일치하지 않습니다.");

    // Same read-only Realtek vendor opcode used in BTstack phase 2 and Linux btrtl.
    constexpr uint16_t rom_version = 0xfc6d;
    const auto rom = read_hci(rom_version, raw, handle, event_pipe, event.get());
    if (rom.result != HciReply::Result::Complete || rom.bytes[5] || rom.size != 7)
        return report + hci_failure(rom_version, rom, korean);
    snprintf(details, sizeof(details), "HCI 0xFC6D | ROM=0x%02X",
             static_cast<unsigned>(rom.bytes[6]));
    report += details;
    if (rom.bytes[6] == 1)
        report += tr(" | RTL8761BU candidate, epatch chip ID 2.", " | RTL8761BU 후보, epatch 칩 ID 2.");
    else
        report += tr(" | ROM differs from the published A760 measurement.",
                     " | 공개 A760 실측의 ROM 버전과 다릅니다.");
    if (rom_ok && rom.bytes[6] == 1) {
        report += '\n';
        *rom_ok = verify_security_project(raw, handle, event_pipe, event.get(), report);
    }
    return report;
}

std::string diagnose_a760(bool korean) {
    return diagnose_a760_impl(korean, nullptr, nullptr);
}

bool verify_a760_rom(const std::string &path, std::string &report) {
    bool rom_ok = false;
    report += diagnose_a760_impl(false, &path, &rom_ok) + "\n";
    return rom_ok;
}

bool initialize_a760_ram(void *device, void *usb, uint8_t event_pipe,
                         const std::vector<uint8_t> &payload, std::string &report) {
    const auto log = [&report](const std::string &line) { report += line + "\n"; };
    if (!is_a760_descriptor(usb) || !(event_pipe & 0x80) || payload.size() != 30210) {
        log("ABORT: expected REV_0200, interrupt IN pipe and validated 30210-byte image");
        return false;
    }
    const FileHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        log("ABORT: CreateEvent failed, Win32=" + std::to_string(GetLastError()));
        return false;
    }
    const auto before = read_hci(0x1001, device, usb, event_pipe, event.get());
    log("Before: " + describe_version(before, false));
    if (!matches_version(before, 0x000b, 0x8761)) {
        log("ABORT: cold RTL8761BU identity mismatch; unplug/reconnect before retry");
        return false;
    }
    const auto rom = read_hci(0xfc6d, device, usb, event_pipe, event.get());
    if (rom.result != HciReply::Result::Complete || rom.bytes[5] || rom.size != 7) {
        log(hci_failure(0xfc6d, rom, false));
        return false;
    }
    log("HCI 0xFC6D status=0x00 ROM=" + std::to_string(rom.bytes[6]));
    if (rom.bytes[6] != 1) {
        log("ABORT: expected ROM=1 (chip ID 2)");
        return false;
    }

    if (!verify_security_project(device, usb, event_pipe, event.get(), report))
        return false;

    const auto deadline = GetTickCount64() + 35000;
    const size_t count = payload.size() / a760::fragment_size + 1;
    for (size_t i = 0; i < count; ++i) {
        if (GetTickCount64() >= deadline) {
            log("ABORT: total RAM patch timeout");
            return false;
        }
        const auto frag = a760::fragment(payload.size(), i);
        std::array<uint8_t, a760::fragment_size + 1> parameters{};
        parameters[0] = frag.index;
        std::copy_n(payload.data() + frag.offset, frag.length, parameters.data() + 1);
        const auto response = read_hci(0xfc20, device, usb, event_pipe, event.get(),
                                       parameters.data(), frag.length + 1);
        char line[192];
        snprintf(line, sizeof(line), "HCI 0xFC20 fragment=%zu/%zu payload=%zu tx=0x%02X rx=0x%02X status=0x%02X",
                 i + 1, count, frag.length, static_cast<unsigned>(frag.index),
                 static_cast<unsigned>(response.bytes[6]), static_cast<unsigned>(response.bytes[5]));
        log(line);
        if (response.result != HciReply::Result::Complete || response.bytes[5] || response.size != 7) {
            log(hci_failure(0xfc20, response, false));
            return false;
        }
        // Firmware may clear the end marker when echoing the final index.
        const auto echo = response.bytes[6];
        if (echo != frag.index && (i + 1 != count || echo != (frag.index & 0x7f))) {
            log("ABORT: fragment acknowledgement index mismatch");
            return false;
        }
    }
    const auto after = read_hci(0x1001, device, usb, event_pipe, event.get());
    log("After: " + describe_version(after, false));
    if (!matches_version(after, a760::patch_version >> 16, a760::patch_version & 0xffff)) {
        log("ABORT: expected patched firmware 0xDFC6D922");
        return false;
    }
    log("RAM patch verified; starting standard BTstack HCI initialization on the same USB handle");
    return true;
}
