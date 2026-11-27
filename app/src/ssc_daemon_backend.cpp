/*
 * Daemon SSC Encode Backend
 *
 * SPDX-License-Identifier: MIT
 */

#include "ssc_daemon_backend.h"
#include "console_style.h"
#include "ssc_runtime.h"

#include <cstdio>
#include <algorithm>
#include <array>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <ws2tcpip.h>
#include <winsock2.h>
#endif

namespace {

constexpr int kDaemonPort = 20248;
constexpr uint32_t kFrameSamples = 864;
constexpr uint32_t kMaxEncodeBytes = 4096;
/* Wire-protocol magic (LE bytes 'S''H''C''D') the client sends as the
 * frame_samples header to make the daemon exit cleanly. */
constexpr uint32_t kCmdShutdown = 0x44434853u;

} // namespace

DaemonSscBackend::DaemonSscBackend(bool native)
    : native_(native) {}

DaemonSscBackend::~DaemonSscBackend() {
    shutdown();
}

namespace {
std::string log_tail(const std::filesystem::path &path) {
    if (path.empty()) return {};
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto end = static_cast<std::streamoff>(input.tellg());
    if (end <= 0) return {};
    input.seekg(end > 4096 ? end - 4096 : 0);
    std::string output{std::istreambuf_iterator<char>(input), {}};
    // Windows launcher failures can be UTF-16; keep a readable ASCII fallback.
    if (output.find('\0') != std::string::npos)
        output.erase(std::remove(output.begin(), output.end(), '\0'), output.end());
    if (output.find_first_not_of(" \r\n\t") == std::string::npos) return {};
    // Diagnostics are forwarded to UTF-8 GUI strings and logs.
    for (auto &c : output)
        if (static_cast<unsigned char>(c) >= 128) c = '?';
    return output;
}

bool launch(std::wstring command, void *&process, void *&job, std::filesystem::path &log) {
    wchar_t directory[MAX_PATH]{}, file[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"ssc", 0, file)) return false;
    log = file;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE output = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, 0, nullptr);
    const HANDLE owned_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION child{};
    bool ok = input != INVALID_HANDLE_VALUE && owned_job &&
              SetInformationJobObject(owned_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
              CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &child);
    if (ok) {
        ok = AssignProcessToJobObject(owned_job, child.hProcess) && ResumeThread(child.hThread) != DWORD(-1);
        CloseHandle(child.hThread);
        if (!ok) {
            TerminateProcess(child.hProcess, 1);
            CloseHandle(child.hProcess);
        }
    }
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    CloseHandle(output);
    if (!ok) {
        if (owned_job) CloseHandle(owned_job);
        return false;
    }
    process = child.hProcess;
    job = owned_job;
    return true;
}

std::string run_check(const std::wstring &command, const char *failure) {
    void *process = nullptr, *job = nullptr;
    std::filesystem::path log;
    if (!launch(command, process, job, log)) {
        if (!log.empty()) DeleteFileW(log.c_str());
        return failure;
    }
    DWORD code = 1;
    if (WaitForSingleObject(process, 12000) == WAIT_OBJECT_0) GetExitCodeProcess(process, &code);
    CloseHandle(job);
    WaitForSingleObject(process, 1000);
    CloseHandle(process);
    const auto output = log_tail(log);
    DeleteFileW(log.c_str());
    if (output.find("SSC_DEP_") != std::string::npos) return output;
    if (code == 0 && output.find("SSC_CHECK_OK") != std::string::npos) return {};
    return std::string(failure) + "\n" + output;
}

std::string session_token() {
    std::array<UCHAR, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    for (const auto byte : random) {
        token += hex[byte >> 4];
        token += hex[byte & 15];
    }
    return token;
}

bool receive_exact(SOCKET socket, void *buffer, size_t size) {
    auto *out = static_cast<char *>(buffer);
    while (size) {
        const int n = recv(socket, out, static_cast<int>(size), 0);
        if (n <= 0) return false;
        out += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool send_exact(SOCKET socket, const void *buffer, size_t size) {
    const auto *data = static_cast<const char *>(buffer);
    while (size) {
        const int n = send(socket, data, static_cast<int>(size), 0);
        if (n <= 0) return false;
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool connect_to_daemon(SOCKET sock, const sockaddr_in &address) {
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) return false;
    if (connect(sock, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        fd_set write{}, error{};
        FD_SET(sock, &write);
        FD_SET(sock, &error);
        timeval timeout{0, 350000};
        if (select(0, nullptr, &write, &error, &timeout) <= 0 || !FD_ISSET(sock, &write) ||
            FD_ISSET(sock, &error)) return false;
    }
    mode = 0;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
}

bool port_available() {
    const auto sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kDaemonPort);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL exclusive = TRUE;
    const bool available = setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                                      reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) == 0 &&
                           bind(sock, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0;
    closesocket(sock);
    return available;
}
} // namespace

std::string DaemonSscBackend::preflight() {
    if (native_) {
        const auto installed = ssc_native_dir();
        const wchar_t *override_path = _wgetenv(L"SSC_DAEMON_PY");
        const auto script = override_path && *override_path ? std::filesystem::path(override_path)
                                                            : installed / L"sscblobd.py";
        const auto root = script.parent_path();
        std::error_code error;
        std::string report;
        if (root.empty() || !std::filesystem::is_regular_file(root / L"rootfs/blob/libScalable_Encoder.so", error))
            report += "SSC_DEP_BLOB: rootfs/blob/libScalable_Encoder.so\n";
        for (const auto *file : {L"sscblobd.py", L"qiling_patches.py", L"rootfs/helper/ssc_blob_helper",
                                 L"rootfs/shims/libc.so", L"rootfs/shims/libdl.so", L"rootfs/shims/liblog.so",
                                 L"rootfs/shims/libm.so", L"rootfs/lib/ld-linux-aarch64.so.1",
                                 L"rootfs/lib/aarch64-linux-gnu/libc.so.6", L"rootfs/lib/aarch64-linux-gnu/libgcc_s.so.1"}) {
            if (!std::filesystem::is_regular_file(root / file, error)) {
                report += "SSC_DEP_NATIVE_ASSETS: rootfs/helper, rootfs/shims, rootfs/lib\n";
                break;
            }
        }
        const auto py = run_check(L"py -3.14 -c \"import importlib.util; print('SSC_CHECK_OK' if importlib.util.find_spec('qiling') else 'SSC_DEP_QILING')\"",
                                  "SSC_DEP_PYTHON: Python 3.14 and py launcher");
        if (!py.empty()) report += py;
        if (!report.empty()) return report;
        return run_check(L"py -3.14 " + ssc_quote(script.wstring()) + L" --check-managed",
                         "SSC_DEP_NATIVE_ASSETS: Qiling import or old daemon");
    }
    return run_check(ssc_wsl_command() + L" --exec bash -c " + ssc_quote(L"case $(uname -r) in *WSL2*|*wsl2*) ;; *) echo SSC_DEP_WSL2; exit 1;; esac; "
                                                                         L"test -s ~/ssc/blob/libScalable_Encoder.so || echo SSC_DEP_BLOB; "
                                                                         L"command -v qemu-aarch64 >/dev/null && test -r /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 || echo SSC_DEP_QEMU; "
                                                                         L"~/ssc/openssc/build_blob/sscblobd --check-managed 2>/dev/null | grep -q SSC_CHECK_OK && "
                                                                         L"test -x ~/ssc/openssc/build_blob/ssc_blob_helper && "
                                                                         L"test -s ~/ssc/openssc/build_blob/libc.so && test -s ~/ssc/openssc/build_blob/liblog.so || echo SSC_DEP_WSL_ASSETS; "
                                                                         L"echo SSC_CHECK_OK"),
                     "SSC_DEP_WSL2: wsl.exe or selected Linux distribution");
}

std::string DaemonSscBackend::diagnose() {
    std::string report = preflight();
    if (report.empty()) report = "SSC_CHECK_OK";
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return report + "\nSSC_DEP_SOCKET";
    report += port_available() ? "\nSSC_PORT_20248_FREE" : "\nSSC_DEP_PORT: 127.0.0.1:20248";
    WSACleanup();
    return report;
}

std::wstring DaemonSscBackend::daemon_command(const std::string &token) const {
    const std::wstring wide_token(token.begin(), token.end());
    const auto args = std::to_wstring(kDaemonPort) + L" " + std::to_wstring(sample_rate_) + L" " +
                      std::to_wstring(channels_) + L" " + std::to_wstring(bitrate_bps_);
    if (native_) {
        const wchar_t *override_path = _wgetenv(L"SSC_DAEMON_PY");
        const auto script = override_path && *override_path ? std::filesystem::path(override_path)
                                                            : ssc_native_dir() / L"sscblobd.py";
        return L"py -3.14 -u " + ssc_quote(script.wstring()) + L" " + args + L" --managed " + wide_token;
    }
    return ssc_wsl_command() + L" --exec bash -c " + ssc_quote(L"cd ~/ssc/openssc/build_blob && export SSCENC_BLOB_SO=~/ssc/blob/libScalable_Encoder.so SSC_SHIM_DIR=\"$PWD\" && exec ./sscblobd " + args + L" \"$PWD/ssc_blob_helper\" --managed " + wide_token);
}

bool DaemonSscBackend::start_daemon() {
    shutdown();
    last_error_ = preflight();
    if (!last_error_.empty()) return false;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        last_error_ = "SSC_DEP_SOCKET";
        return false;
    }
    winsock_started_ = true;
    if (!port_available()) {
        last_error_ = "SSC_DEP_PORT: 127.0.0.1:20248";
        shutdown();
        return false;
    }
    const auto token = session_token();
    if (token.empty() || !launch(daemon_command(token), process_, job_, daemon_log_)) {
        last_error_ = "SSC_DEP_START: daemon process";
        shutdown();
        return false;
    }
    // Wait for the child-owned listener, then verify an actual encoded PCM frame.
    for (int attempt = 0; attempt < 70; ++attempt) {
        if (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) break;
        if (log_tail(daemon_log_).find("SSC_MANAGED_READY") == std::string::npos) {
            Sleep(100);
            continue;
        }
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == INVALID_SOCKET) break;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kDaemonPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect_to_daemon(sock_, addr)) {
            int one = 1;
            DWORD timeout = 8000;
            setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
            setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
            setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
            std::array<char, 32> peer{};
            if (!receive_exact(sock_, peer.data(), peer.size()) ||
                !std::equal(peer.begin(), peer.end(), token.begin(), token.end())) {
                last_error_ = "SSC_DEP_PORT: session ownership check failed at TCP 20248";
                shutdown();
                return false;
            }
            connected_ = true;
            std::array<int32_t, kFrameSamples * 2> pcm{};
            for (size_t i = 0; i < pcm.size(); ++i)
                pcm[i] = (static_cast<int32_t>((i / 2) % 44) - 22) * 100000;
            std::array<uint8_t, kMaxEncodeBytes> frame{};
            size_t encoded = 0;
            if (channels_ <= 2 && encode(pcm.data(), kFrameSamples, channels_, frame.data(), frame.size(), &encoded) && encoded)
                return true;
            last_error_ = "SSC_DEP_ENCODE: helper did not return an SSC frame\n" + log_tail(daemon_log_);
            shutdown();
            return false;
        }
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        Sleep(100);
    }
    last_error_ = "SSC_DEP_CONNECT: TCP 127.0.0.1:20248\n" + log_tail(daemon_log_);
    shutdown();
    return false;
}

bool DaemonSscBackend::init(uint32_t sample_rate, uint32_t channels,
                            uint32_t bitrate_bps) {
    sample_rate_ = sample_rate;
    channels_ = channels;
    bitrate_bps_ = bitrate_bps;
    return start_daemon();
}

bool DaemonSscBackend::recover() {
#ifdef _WIN32
    connected_ = false;
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    fprintf(stderr, "SSC: daemon lost, restarting\n");
    return start_daemon();
#else
    return false;
#endif
}

bool DaemonSscBackend::encode(const int32_t *pcm, size_t frames, size_t channels,
                              uint8_t *out, size_t out_cap, size_t *out_size) {
    if (!out_size) return false;
    *out_size = 0;
    if (!connected() || !pcm) return false;

    uint32_t expect_bytes = static_cast<uint32_t>(frames) * static_cast<uint32_t>(channels) * sizeof(int32_t);
    if (frames == 0 || channels == 0) return false;

#ifdef _WIN32
    LARGE_INTEGER sq, eq, sfq, hq, fq, frq;
    QueryPerformanceFrequency(&frq);
    QueryPerformanceCounter(&sq);

    /* frame_samples LE + 32-bit interleaved PCM (24-bit, 2^29 scale) */
    uint32_t frame_samples = static_cast<uint32_t>(frames);
    if (!send_exact(sock_, &frame_samples, sizeof(frame_samples)) ||
        !send_exact(sock_, pcm, expect_bytes)) return false;
    QueryPerformanceCounter(&eq);

    int32_t ret = 0;
    if (!receive_exact(sock_, &ret, sizeof(ret))) return false;
    QueryPerformanceCounter(&sfq);
    if (ret <= 0 || ret > static_cast<int32_t>(kMaxEncodeBytes) ||
        static_cast<size_t>(ret) > out_cap || !receive_exact(sock_, out, static_cast<size_t>(ret)))
        return false;
    QueryPerformanceCounter(&fq);

    static double max_total = 0.0;
    static uint32_t diag_count = 0;
    static DWORD diag_tick = 0;
    diag_count++;
    double t_send = (double)(eq.QuadPart - sq.QuadPart) * 1000.0 / (double)frq.QuadPart;
    double t_hdr = (double)(sfq.QuadPart - eq.QuadPart) * 1000.0 / (double)frq.QuadPart;
    double t_data = (double)(fq.QuadPart - sfq.QuadPart) * 1000.0 / (double)frq.QuadPart;
    double t_tot = (double)(fq.QuadPart - sq.QuadPart) * 1000.0 / (double)frq.QuadPart;
    if (t_tot > max_total) max_total = t_tot;
    if (diag_tick == 0) diag_tick = GetTickCount();
    if (GetTickCount() - diag_tick >= 2000) {
        cstyle::fprint(stderr, cstyle::Tag::Data, "SSC:     ");
        fprintf(stderr, "enc%u t_send=%.2fms t_hdr=%.2fms t_data=%.2fms total=",
                diag_count, t_send, t_hdr, t_data);
        if (t_tot > 15.0)
            cstyle::fprint(stderr, cstyle::Tag::Warn, "%.2fms", t_tot);
        else
            fprintf(stderr, "%.2fms", t_tot);
        fprintf(stderr, " max=%.2fms ret=", max_total);
        if (ret <= 0)
            cstyle::fprint(stderr, cstyle::Tag::Error, "%d", ret);
        else
            fprintf(stderr, "%d", ret);
        fprintf(stderr, "\n");
        diag_count = 0;
        diag_tick = GetTickCount();
        max_total = 0.0;
    }

    *out_size = static_cast<size_t>(ret);
    return true;
#else
    (void)pcm;
    (void)frames;
    (void)channels;
    (void)out;
    (void)out_cap;
    return false;
#endif
}

void DaemonSscBackend::request_shutdown() {
#ifdef _WIN32
    if (sock_ != INVALID_SOCKET && connected_) {
        uint32_t magic = kCmdShutdown;
        (void)send_exact(sock_, &magic, sizeof(magic));
    }
#endif
}

void DaemonSscBackend::shutdown() {
#ifdef _WIN32
    request_shutdown();
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (process_) WaitForSingleObject(process_, 1500);
    if (job_) CloseHandle(job_);
    if (process_) CloseHandle(process_);
    job_ = process_ = nullptr;
    if (!daemon_log_.empty()) DeleteFileW(daemon_log_.c_str());
    daemon_log_.clear();
    if (winsock_started_) WSACleanup();
    winsock_started_ = false;
#endif
    connected_ = false;
}