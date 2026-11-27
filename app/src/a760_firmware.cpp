// SPDX-License-Identifier: MIT
#include "a760_firmware.h"
#include "a760_diagnostics.h"

#include "bt_adapter_enum.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <array>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace {
constexpr wchar_t upstream[] = L"/kernel-firmware/linux-firmware/-/raw/a2dcb924f0897368b0b4f305cbf9808cdda90ce0/rtl_bt/";
struct FirmwareFile {
    const wchar_t *name;
    size_t size;
    const char *hash;
};
constexpr FirmwareFile firmware{L"rtl8761bu_fw.bin", 44484,
                                "1d7a9597349ad89344fa16c1913d3e39e9a12e966e417ca16871bc79bbe59edb"};
constexpr FirmwareFile config{L"rtl8761bu_config.bin", 6,
                              "6c28a3f07c6a30ed208c4b64862a23f02b7d93543ea980edd24df16bab45095f"};

using HttpHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

[[noreturn]] void http_error(const char *operation) {
    const auto code = GetLastError();
    throw std::runtime_error(std::string(operation) + " (Win32=" + std::to_string(code) + ")");
}

std::vector<uint8_t> download(const FirmwareFile &file) {
    HttpHandle session(WinHttpOpen(L"SSCOnWindows/0.2 A760", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
                       WinHttpCloseHandle);
    if (!session || !WinHttpSetTimeouts(session.get(), 6000, 6000, 6000, 6000))
        http_error("WinHTTP session");
    HttpHandle connection(WinHttpConnect(session.get(), L"gitlab.com", INTERNET_DEFAULT_HTTPS_PORT, 0),
                          WinHttpCloseHandle);
    if (!connection) http_error("WinHTTP connect");
    const std::wstring path = std::wstring(upstream) + file.name;
    HttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
                       WinHttpCloseHandle);
    if (!request) http_error("WinHTTP request");
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        http_error("WinHTTP GET");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        http_error("WinHTTP status");
    if (status != 200) throw std::runtime_error("firmware HTTP status " + std::to_string(status));
    std::vector<uint8_t> data(file.size + 1);
    size_t used = 0;
    const auto deadline = GetTickCount64() + 15000;
    while (used < data.size()) {
        DWORD received = 0;
        if (GetTickCount64() >= deadline) throw std::runtime_error("firmware download timed out");
        if (!WinHttpReadData(request.get(), data.data() + used,
                             static_cast<DWORD>(data.size() - used), &received))
            http_error("WinHTTP read");
        if (!received) break;
        used += received;
    }
    data.resize(used);
    return data;
}

std::vector<uint8_t> read_local(const std::filesystem::path &directory, const FirmwareFile &file) {
    std::ifstream input(directory / file.name, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open one of the two firmware files");
    std::vector<uint8_t> data(file.size + 1);
    input.read(reinterpret_cast<char *>(data.data()), data.size());
    if (input.bad()) throw std::runtime_error("firmware file read failed");
    data.resize(static_cast<size_t>(input.gcount()));
    return data;
}

std::vector<uint8_t> checked_file(const FirmwareFile &file, const std::filesystem::path &directory,
                                  std::string &report) {
    auto data = directory.empty() ? download(file) : read_local(directory, file);
    if (data.size() != file.size)
        throw std::runtime_error("firmware length mismatch (received " + std::to_string(data.size()) + ")");
    std::array<UCHAR, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, data.data(),
                   static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("SHA-256 calculation failed");
    std::string hex;
    for (const auto byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    report += "SHA-256 " + hex + " (" + std::to_string(data.size()) + " bytes)\n";
    if (hex != file.hash) throw std::runtime_error("firmware SHA-256 mismatch");
    return data;
}
} // namespace

A760Firmware A760Firmware::prepare(const std::filesystem::path &directory, std::string &report) {
    A760Firmware result;
    const auto adapters = BtAdapterEnumerator::enumerate(true);
    if (adapters.size() != 1)
        throw std::runtime_error("expected exactly one WinUSB A760 Bluetooth interface");
    result.device_path_ = adapters.front().device_path;
    report = "linux-firmware a2dcb924f0897368b0b4f305cbf9808cdda90ce0\nrtl8761bu_fw.bin: ";
    const auto fw = checked_file(firmware, directory, report);
    report += "rtl8761bu_config.bin: ";
    const auto cfg = checked_file(config, directory, report);
    result.payload_ = a760::build_payload(fw, cfg);
    if (result.payload_.size() != 30210)
        throw std::runtime_error("epatch version/project/chip, payload length or config mismatch");
    report += "Realtech epatch | project=14 | ROM=1 required | chip=2 | config records=0\n";
    report += "RAM payload: 30210 bytes, expected HCI firmware 0xDFC6D922\n";
    if (!verify_a760_rom(result.device_path_, report))
        throw std::runtime_error("A760 cold HCI identity, ROM or security-project check failed");
    return result;
}
