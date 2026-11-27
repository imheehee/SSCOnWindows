// SPDX-License-Identifier: MIT
#include "ssc_runtime.h"
#include "ssc_daemon_backend.h"

#include <windows.h>
#include <array>
#include <cstdlib>

std::filesystem::path ssc_tools_dir() {
    std::array<wchar_t, 32768> executable{};
    const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!count || count == executable.size()) return {};
    auto parent = std::filesystem::path(executable.data()).parent_path();
    for (int i = 0; i < 6 && !parent.empty(); ++i) {
        std::error_code error;
        const auto tools = parent / L"tools";
        if (std::filesystem::is_regular_file(tools / L"ssc_setup.ps1", error)) return tools;
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    return {};
}

std::filesystem::path ssc_native_dir() {
    const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
    return local && *local ? std::filesystem::path(local) / L"SSCOnWindows" / L"encoder" : std::filesystem::path{};
}

std::wstring ssc_quote(const std::wstring &value) {
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(slashes * (c == L'"' ? 2 : 1) + (c == L'"' ? 1 : 0), L'\\');
        slashes = 0;
        quoted += c;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

std::wstring ssc_wsl_command() {
    const wchar_t *distro = _wgetenv(L"SSC_WSL_DISTRO");
    return distro && *distro ? L"wsl.exe --distribution " + ssc_quote(distro) : L"wsl.exe";
}

std::string ssc_diagnose(bool native) {
    DaemonSscBackend backend(native);
    return backend.diagnose();
}

std::string ssc_self_test(bool native, uint32_t rate) {
    DaemonSscBackend backend(native);
    const bool ok = backend.init(rate, 2, 192000);
    const auto result = backend.last_error();
    backend.shutdown();
    return ok ? "SSC_SELF_TEST_OK" : result;
}
