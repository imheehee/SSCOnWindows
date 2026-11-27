/*
 * Driver state checks for guided USB Bluetooth driver changes.
 * SPDX-License-Identifier: MIT
 */

#include "driver_switch.h"

#include "driver_mode.h"

DongleSwitchReport get_dongle_driver_instructions(uint16_t vid, uint16_t pid, bool enable_winusb, bool korean) {
    const auto tr = [korean](const char *en, const char *ko) { return korean ? ko : en; };
    const auto status = detect_dongle_driver(vid, pid);
    if (status.mode == DongleDriverMode::NotPresent) {
        return {DongleSwitchResult::NotPresent, tr("The USB Bluetooth adapter was not detected.", "USB Bluetooth 어댑터를 찾을 수 없습니다.")};
    }

    const auto requested = enable_winusb ? DongleDriverMode::WinUsbStream
                                         : DongleDriverMode::BthUsb;
    if (status.mode == requested) {
        return {DongleSwitchResult::NoChange,
                enable_winusb ? tr("WinUSB is active.", "WinUSB가 활성화되어 있습니다.")
                              : tr("BTHUSB is active.", "BTHUSB가 활성화되어 있습니다.")};
    }

    const std::string instructions = enable_winusb
                                         ? tr("In Device Manager, install a trusted signed WinUSB package for this device, then refresh.",
                                              "장치 관리자에서 이 장치용 서명된 WinUSB 패키지를 설치한 뒤 새로 고침을 누르세요.")
                                         : tr("In Device Manager, roll back this device's driver or select the original signed Bluetooth driver, then refresh.",
                                              "장치 관리자에서 이 장치의 드라이버를 롤백하거나 기존 서명된 Bluetooth 드라이버를 선택한 뒤 새로 고침을 누르세요.");
    return {DongleSwitchResult::ManualActionRequired,
            instructions + tr(" Device instance: ", " 장치 인스턴스: ") + status.instance_id};
}
