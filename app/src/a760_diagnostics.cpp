// SPDX-License-Identifier: MIT
#include "a760_diagnostics.h"

#include "bt_adapter_enum.h"
#include "driver_mode.h"

#include <windows.h>
#include <winusb.h>

#include <cstdio>
#include <memory>

namespace {
struct FileCloser {
    void operator()(void* handle) const { CloseHandle(handle); }
};
struct WinUsbCloser {
    void operator()(void* handle) const { WinUsb_Free(handle); }
};
using FileHandle = std::unique_ptr<void, FileCloser>;

bool finish_transfer(BOOL completed, HANDLE device, WINUSB_INTERFACE_HANDLE usb,
                     OVERLAPPED& io, ULONG& size, DWORD timeout)
{
    if (completed) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(io.hEvent, timeout) == WAIT_OBJECT_0)
        return WinUsb_GetOverlappedResult(usb, &io, &size, FALSE) != FALSE;

    // Drain the cancelled I/O before its OVERLAPPED and buffer leave scope.
    CancelIoEx(device, &io);
    WinUsb_GetOverlappedResult(usb, &io, &size, TRUE);
    return false;
}

unsigned le16(const UCHAR* data)
{
    return data[0] | (static_cast<unsigned>(data[1]) << 8);
}
}

std::string diagnose_a760(bool korean)
{
    const auto tr = [korean](const char* en, const char* ko) { return korean ? ko : en; };
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
    for (const auto& adapter : BtAdapterEnumerator::enumerate(true)) {
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

    UCHAR command[] = {0x01, 0x10, 0x00};
    WINUSB_SETUP_PACKET setup{0x20, 0, 0, 0, sizeof(command)};
    OVERLAPPED io{};
    io.hEvent = event.get();
    if (!finish_transfer(WinUsb_ControlTransfer(handle, setup, command, sizeof(command), &size, &io),
                         raw, handle, io, size, 1000) || size != sizeof(command)) {
        return report + tr("Could not send HCI command 0x1001.", "HCI 명령 0x1001 전송에 실패했습니다.");
    }

    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (auto now = GetTickCount64(); now < deadline; now = GetTickCount64()) {
        UCHAR reply[257]{};
        ResetEvent(event.get());
        io = {};
        io.hEvent = event.get();
        if (!finish_transfer(WinUsb_ReadPipe(handle, event_pipe, reply, sizeof(reply), &size, &io),
                             raw, handle, io, size, static_cast<DWORD>(deadline - now))) break;
        if (size < 6 || size != 2u + reply[1] || reply[0] != 0x0e || le16(reply + 3) != 0x1001)
            continue;
        if (reply[5]) {
            char result[32];
            snprintf(result, sizeof(result), "0x%02X", static_cast<unsigned>(reply[5]));
            return report + tr("HCI status: ", "HCI 상태: ") + result;
        }
        if (size != 14) break;
        char version[160];
        snprintf(version, sizeof(version),
                 "HCI=0x%02X rev=0x%04X | LMP=0x%02X subver=0x%04X | Manufacturer=0x%04X",
                 static_cast<unsigned>(reply[6]), le16(reply + 7), static_cast<unsigned>(reply[9]),
                 le16(reply + 12), le16(reply + 10));
        return report + version;
    }
    return report + tr("No valid HCI 0x1001 response.", "유효한 HCI 0x1001 응답을 받지 못했습니다.");
}
