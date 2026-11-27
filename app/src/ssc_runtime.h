// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// App-local tools and per-user encoder assets. Paths support Windows Unicode.
std::filesystem::path ssc_tools_dir();
std::filesystem::path ssc_native_dir();
std::wstring ssc_quote(const std::wstring &value);
std::wstring ssc_wsl_command();
std::string ssc_diagnose(bool native);
std::string ssc_self_test(bool native, uint32_t rate);
