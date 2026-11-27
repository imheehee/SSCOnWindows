/*
 * Dongle driver-mode detection — implementation.
 *
 * Uses SetupAPI to enumerate device nodes (not interfaces, so it also sees
 * BTHUSB devices that have no WinUSB DeviceInterface) and matches hardware
 * IDs like "USB\VID_2357&PID_0604&..." against the requested VID:PID.
 *
 * SPDX-License-Identifier: MIT
 */

#include "driver_mode.h"

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>

#include <cctype>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {
std::string multi_sz_first(const char *data, DWORD len) {
    /* First non-empty string of a REG_MULTI_SZ */
    for (DWORD off = 0; off + 1 < len;) {
        const char *s = data + off;
        size_t n = strnlen(s, len - off);
        if (n == 0) break;
        if (n > 0 && _stricmp(s, "ROOT\\*PNP0X01") != 0) {
            return std::string(s, n);
        }
        off += (DWORD)n + 1;
    }
    return {};
}

bool hwid_matches(const char *hwid, uint16_t vid, uint16_t pid) {
    char prefix[32];
    const int length = snprintf(prefix, sizeof(prefix), "USB\\VID_%04X&PID_%04X",
                                static_cast<unsigned>(vid), static_cast<unsigned>(pid));
    return _strnicmp(hwid, prefix, length) == 0 &&
           (hwid[length] == '\0' || hwid[length] == '&');
}
} // namespace

DongleDriverStatus detect_dongle_driver(uint16_t vid, uint16_t pid) {
    DongleDriverStatus st;

    HDEVINFO devs = SetupDiGetClassDevsA(nullptr, nullptr, nullptr,
                                         DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return st;

    SP_DEVINFO_DATA dev = {};
    dev.cbSize = sizeof(dev);

    for (DWORD idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev); idx++) {
        /* Hardware IDs: REG_MULTI_SZ list of "USB\VID_....&PID_....&..." */
        DWORD reg_type = 0;
        DWORD len = 0;
        /* Size query: intentionally FAILS with ERROR_INSUFFICIENT_BUFFER
           (return FALSE); the required byte count is still written to len. */
        SetupDiGetDeviceRegistryPropertyA(devs, &dev, SPDRP_HARDWAREID,
                                          &reg_type, nullptr, 0, &len);
        if (len == 0) continue;

        std::vector<char> buf(len);
        if (!SetupDiGetDeviceRegistryPropertyA(devs, &dev, SPDRP_HARDWAREID,
                                               &reg_type, (PBYTE)buf.data(), len, &len))
            continue;
        if (reg_type != REG_MULTI_SZ) continue;

        bool matched = false;
        for (DWORD off = 0; off + 1 < len;) {
            const char *s = buf.data() + off;
            size_t n = strnlen(s, len - off);
            if (n == 0) break;
            if (hwid_matches(s, vid, pid)) {
                matched = true;
                const char* revision = std::strstr(s, "&REV_");
                if (revision && std::strlen(revision + 5) >= 4) {
                    unsigned value = 0;
                    if (sscanf_s(revision + 5, "%4x", &value) == 1)
                        st.usb_revision = static_cast<uint16_t>(value);
                }
            }
            off += (DWORD)n + 1;
        }
        if (!matched) continue;

        /* PnP instance ID (e.g. "USB\VID_2357&PID_0604\0001") — used by the
           driver-toggle automation to remove/restart the exact devnode. */
        DWORD ilen = 0;
        SetupDiGetDeviceInstanceIdA(devs, &dev, nullptr, 0, &ilen);
        if (ilen > 0) {
            std::vector<char> inst(ilen);
            if (SetupDiGetDeviceInstanceIdA(devs, &dev, inst.data(), ilen, &ilen))
                st.instance_id = multi_sz_first(inst.data(), ilen);
        }

        /* Friendly name (if any) */
        DWORD nlen = 0;
        SetupDiGetDeviceRegistryPropertyA(devs, &dev, SPDRP_FRIENDLYNAME,
                                          nullptr, nullptr, 0, &nlen);
        if (nlen > 0) {
            std::vector<char> name(nlen);
            if (SetupDiGetDeviceRegistryPropertyA(devs, &dev, SPDRP_FRIENDLYNAME,
                                                  nullptr, (PBYTE)name.data(), nlen, &nlen))
                st.device_name = multi_sz_first(name.data(), nlen);
        }
        if (st.device_name.empty()) {
            nlen = 0;
            SetupDiGetDeviceRegistryPropertyA(devs, &dev, SPDRP_DEVICEDESC,
                                              nullptr, nullptr, 0, &nlen);
            if (nlen > 0) {
                std::vector<char> name(nlen);
                if (SetupDiGetDeviceRegistryPropertyA(devs, &dev, SPDRP_DEVICEDESC,
                                                      nullptr, (PBYTE)name.data(), nlen, &nlen))
                    st.device_name = multi_sz_first(name.data(), nlen);
            }
        }

        /* Driver service name */
        nlen = 0;
        SetupDiGetDeviceRegistryPropertyA(devs, &dev, SPDRP_SERVICE,
                                          nullptr, nullptr, 0, &nlen);
        if (nlen > 0) {
            std::vector<char> svc(nlen);
            if (SetupDiGetDeviceRegistryPropertyA(devs, &dev, SPDRP_SERVICE,
                                                  nullptr, (PBYTE)svc.data(), nlen, &nlen)) {
                st.service = multi_sz_first(svc.data(), nlen);
            }
        }

        if (_stricmp(st.service.c_str(), "WinUSB") == 0 ||
            _stricmp(st.service.c_str(), "WinUsb") == 0) {
            st.mode = DongleDriverMode::WinUsbStream;
        } else if (_stricmp(st.service.c_str(), "BTHUSB") == 0) {
            st.mode = DongleDriverMode::BthUsb;
        } else {
            st.mode = DongleDriverMode::Other;
        }

        break;
    }

    SetupDiDestroyDeviceInfoList(devs);
    return st;
}
