#pragma once

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wardogs {

enum class LogLevel { debug, info, warning, error };

inline constexpr std::size_t max_session_log_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t max_log_message_bytes = 4096;
inline constexpr std::size_t max_session_log_archives = 32;

// Retains the former current log as <stem>.previous<extension> on startup.
// Before replacing previous, atomically archives it in <stem>.archive using
// monotonic session-<20 digit sequence>.log names. Retains 32 managed archives,
// each <= 4 MiB, plus current and previous. One bounded staging/recovery file
// may exist during a transaction or after an unrecoverable filesystem error.
// Unrecognized files, directories, hard links and reparse points are untouched.
bool initialize_session_log(const std::filesystem::path& path,
                            std::string_view version) noexcept;
void shutdown_session_log() noexcept;
void write_log(LogLevel level, std::string_view message) noexcept;
// False when there is no open session or a write/rotation/flush has failed.
// Failures also emit fixed OutputDebugString events without message contents.
[[nodiscard]] bool session_log_healthy() noexcept;
[[nodiscard]] std::filesystem::path active_log_path();
[[nodiscard]] std::filesystem::path session_log_archive_directory();

inline void log_debug(std::string_view message) noexcept {
    write_log(LogLevel::debug, message);
}
inline void log_info(std::string_view message) noexcept {
    write_log(LogLevel::info, message);
}
inline void log_warning(std::string_view message) noexcept {
    write_log(LogLevel::warning, message);
}
inline void log_error(std::string_view message) noexcept {
    write_log(LogLevel::error, message);
}

}  // namespace wardogs
