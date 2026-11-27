// SPDX-License-Identifier: MIT
#include "a760_diagnostics.h"

#include "bt_adapter_enum.h"
#include "driver_mode.h"

#include <windows.h>
#include <winusb.h>

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
                  UCHAR pipe, HANDLE event) {
    HciReply reply;
    UCHAR command[] = {static_cast<UCHAR>(opcode), static_cast<UCHAR>(opcode >> 8), 0};
    WINUSB_SETUP_PACKET setup{0x20, 0, 0, 0, sizeof(command)};
    ResetEvent(event);
    OVERLAPPED io{};
    io.hEvent = event;
    ULONG sent = 0;
    if (!finish_transfer(WinUsb_ControlTransfer(usb, setup, command, sizeof(command), &sent, &io),
                         device, usb, io, sent, 1000)) {
        reply.result = HciReply::Result::SendFailed;
        reply.error = GetLastError();
        return reply;
    }
    if (sent != sizeof(command)) {
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

} // namespace

std::string diagnose_a760(bool korean) {
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
    const HANDLE raw = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return report + unavailable;
    const FileHandle device(raw);
    WINUSB_INTERFACE_HANDLE handle;
    if (!WinUsb_Initialize(raw, &handle)) return report + unavailable;
    const std::unique_ptr<void, WinUsbCloser> usb(handle);

    USB_DEVICE_DESCRIPTOR descriptor{};
    ULONG size = 0;
    if (!WinUsb_GetDescriptor(handle, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                              reinterpret_cast<PUCHAR>(&descriptor), sizeof(descriptor), &size) ||
        size != sizeof(descriptor) || descriptor.idVendor != A760_VID ||
        descriptor.idProduct != A760_PID || descriptor.bcdDevice != A760_REV) {
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

    const auto *reply = version.bytes.data();
    const auto manufacturer = le16(reply + 10);
    const auto revision = le16(reply + 7);
    const auto subversion = le16(reply + 12);
    char details[160];
    snprintf(details, sizeof(details),
             "HCI=0x%02X rev=0x%04X | LMP=0x%02X subver=0x%04X | Manufacturer=0x%04X",
             static_cast<unsigned>(reply[6]), revision, static_cast<unsigned>(reply[9]),
             subversion, manufacturer);
    report += details;
    report += '\n';

    const bool expected_identity = reply[6] == 0x0a && revision == 0x000b &&
                                   reply[9] == 0x0a && subversion == 0x8761 && manufacturer == 0x005d;
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
    return report + tr("\nFirmware load unavailable: no approved Windows WinUSB firmware/config pair."
                       "\nInitialization, discovery, pairing and streaming are gated.",
                       "\n펌웨어 로드 대기: Windows WinUSB에서 검증된 펌웨어·설정 조합이 없습니다."
                       "\n초기화·검색·페어링·스트리밍은 검증 후 활성화됩니다.");
}
