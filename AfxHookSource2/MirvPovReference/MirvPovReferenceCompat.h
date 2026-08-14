#pragma once

#include "../MirvPovContext.h"
#include "../../shared/AfxConsole.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace live_hud {

inline std::atomic<bool> g_hlae_pipeline_requested{false};

inline void set_hlae_pipeline_requested(bool requested) noexcept {
    g_hlae_pipeline_requested.store(requested, std::memory_order_release);
}

inline bool hlae_pipeline_requested() noexcept {
    return g_hlae_pipeline_requested.load(std::memory_order_acquire);
}

struct PeFingerprint {
    std::uint32_t size_of_image = 0;
    std::uint32_t time_date_stamp = 0;
};

inline bool fingerprint_matches(const PeFingerprint& actual,
                                std::uint32_t expected_size,
                                std::uint32_t expected_ts) {
    return expected_size != 0 && expected_ts != 0 &&
           actual.size_of_image == expected_size &&
           actual.time_date_stamp == expected_ts;
}

inline void log_line(std::string_view line) {
    const std::string copy(line);
    if (advancedfx::Message) {
        advancedfx::Message("[mirv_pov/reference] %s\n", copy.c_str());
    }
}

inline void log_kv(std::string_view key, std::string_view value) {
    const std::string key_copy(key);
    const std::string value_copy(value);
    if (advancedfx::Message) {
        advancedfx::Message("[mirv_pov/reference] %s=%s\n", key_copy.c_str(),
                            value_copy.c_str());
    }
}

inline std::filesystem::path temp_log_path() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    std::filesystem::path result = length ? std::filesystem::path(path)
                                          : std::filesystem::current_path();
    return result / L"mirv_pov_reference.log";
}

inline std::filesystem::path normalize_demo_path(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        return path;
    }
    return std::filesystem::weakly_canonical(absolute, error);
}

inline bool demo_path_ok(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    return _stricmp(path.extension().string().c_str(), ".dem") == 0;
}

} // namespace live_hud
