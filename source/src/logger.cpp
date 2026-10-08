#include "wardogs/logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace wardogs {
namespace {

struct LoggerState {
    std::mutex mutex;
    std::ofstream stream;
    std::filesystem::path path;
    std::filesystem::path previous_path;
    std::size_t bytes_written{};
    bool healthy{};
};

LoggerState& state() {
    static LoggerState value;
    return value;
}

const char* level_name(LogLevel level) {
    switch (level) {
    case LogLevel::debug: return "DEBUG";
    case LogLevel::info: return "INFO";
    case LogLevel::warning: return "WARN";
    case LogLevel::error: return "ERROR";
    }
    return "INFO";
}

// Messages may contain OCR/user text: never copy them to the Windows debugger
// when a file fails. Report only fixed diagnostic events there.
void report_failure(LoggerState& logger, const wchar_t* event) noexcept {
    logger.healthy = false;
    if (logger.stream.is_open()) logger.stream.close();
    OutputDebugStringW(event);
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &seconds);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::ostringstream text;
    text << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << milliseconds.count();
    return text.str();
}

std::string sanitize_message(std::string_view message) {
    constexpr std::string_view truncated = " [truncated]";
    constexpr std::size_t text_budget = max_log_message_bytes - truncated.size();
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(std::min(message.size(), max_log_message_bytes));
    std::size_t offset = 0;
    while (offset < message.size()) {
        const auto lead = static_cast<unsigned char>(message[offset]);
        std::size_t length = 1;
        std::uint32_t codepoint = lead;
        bool valid = true;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2; codepoint = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3; codepoint = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4; codepoint = lead & 0x07;
        } else if (lead >= 0x80) {
            valid = false;
        }
        if (length > message.size() - offset) valid = false;
        if (valid && length > 1) {
            for (std::size_t index = 1; index < length; ++index) {
                const auto byte = static_cast<unsigned char>(message[offset + index]);
                if ((byte & 0xC0) != 0x80) { valid = false; break; }
                codepoint = (codepoint << 6) | (byte & 0x3F);
            }
            const std::uint32_t minimum =
                length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
            if (codepoint < minimum || codepoint > 0x10FFFF ||
                (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                valid = false;
        }
        std::string escape;
        std::string_view piece;
        if (!valid) {
            length = 1;
            piece = "?";
        } else if (codepoint == '\r') piece = "\\r";
        else if (codepoint == '\n') piece = "\\n";
        else if (codepoint == '\t') piece = "\\t";
        else if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F) ||
                 codepoint == 0x2028 || codepoint == 0x2029) {
            escape = "\\u0000";
            for (int index = 0; index < 4; ++index)
                escape[5 - index] = hex[(codepoint >> (index * 4)) & 0xF];
            piece = escape;
        } else piece = message.substr(offset, length);
        if (piece.size() > text_budget - result.size()) break;
        result.append(piece);
        offset += length;
    }
    if (offset < message.size()) result.append(truncated);
    return result;
}

std::string make_line(LogLevel level, std::string_view message) {
    std::ostringstream line;
    line << '[' << timestamp() << "] [" << level_name(level)
         << "] [tid " << std::this_thread::get_id() << "] "
         << sanitize_message(message) << '\n';
    return line.str();
}

bool flush_locked(LoggerState& logger) noexcept {
    logger.stream.flush();
    if (logger.stream) return true;
    report_failure(logger, L"WARDOGS Fire Control: log.flush_failed; file logging disabled.\n");
    return false;
}

bool validate_previous_locked(LoggerState& logger) {
    std::error_code error;
    // The rotation destination belongs to this logger, but never replace a
    // directory that happens to have the same name as the expected log file.
    if (std::filesystem::is_directory(logger.previous_path, error)) {
        report_failure(logger, L"WARDOGS Fire Control: log.previous_is_directory; file logging disabled.\n");
        return false;
    }
    if (!error || error == std::errc::no_such_file_or_directory) return true;
    report_failure(logger, L"WARDOGS Fire Control: log.previous_status_failed; file logging disabled.\n");
    return false;
}

bool retain_previous_locked(LoggerState& logger) {
    if (!validate_previous_locked(logger)) return false;
    // Both files live in one directory. Atomic replacement preserves the
    // existing history if a sharing/permission failure prevents rotation.
    if (MoveFileExW(logger.path.c_str(), logger.previous_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    report_failure(logger, L"WARDOGS Fire Control: log.rotation_failed; file logging disabled.\n");
    return false;
}

bool rotate_locked(LoggerState& logger) {
    if (!flush_locked(logger)) return false;
    logger.stream.close();
    if (!logger.stream) {
        report_failure(logger, L"WARDOGS Fire Control: log.close_failed; file logging disabled.\n");
        return false;
    }
    if (!retain_previous_locked(logger)) return false;
    logger.stream.clear();
    logger.stream.open(logger.path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!logger.stream) {
        report_failure(logger, L"WARDOGS Fire Control: log.rotation_open_failed; file logging disabled.\n");
        return false;
    }
    logger.bytes_written = 0;
    const auto notice = make_line(LogLevel::info, "session.continued log.rotated previous=1");
    logger.stream.write(notice.data(), static_cast<std::streamsize>(notice.size()));
    if (!logger.stream) {
        report_failure(logger, L"WARDOGS Fire Control: log.write_failed; file logging disabled.\n");
        return false;
    }
    logger.bytes_written = notice.size();
    return true;
}

void write_locked(LoggerState& logger, LogLevel level,
                  std::string_view message) {
    if (!logger.healthy || !logger.stream.is_open()) return;
    const auto line = make_line(level, message);
    if (line.size() > max_session_log_bytes - logger.bytes_written &&
        !rotate_locked(logger)) return;
    logger.stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (!logger.stream) {
        report_failure(logger, L"WARDOGS Fire Control: log.write_failed; file logging disabled.\n");
        return;
    }
    logger.bytes_written += line.size();
    // Keep high-frequency capture diagnostics buffered, but publish completed
    // user actions immediately so a running session can be audited without
    // closing the game or losing the current correction history.
    const bool completed_action = message.starts_with("continuous.impact_recorded") ||
        message.starts_with("ocr.finished") || message.starts_with("terrain.selected") ||
        message.starts_with("terrain.solution") || message.starts_with("solution.l81");
    if (level == LogLevel::warning || level == LogLevel::error || completed_action)
        flush_locked(logger);
}

}  // namespace

bool initialize_session_log(const std::filesystem::path& path,
                            std::string_view version) noexcept {
    auto& logger = state();
    try {
        std::scoped_lock lock(logger.mutex);
        try {
            if (logger.stream.is_open()) {
                if (logger.healthy) flush_locked(logger);
                logger.stream.close();
                if (!logger.stream)
                    report_failure(logger, L"WARDOGS Fire Control: log.close_failed; prior session could not be fully closed.\n");
            }
            logger.stream.clear();
            logger.path.clear();
            logger.previous_path.clear();
            logger.bytes_written = 0;
            logger.healthy = false;
            if (path.empty() || path.filename().empty()) {
                report_failure(logger, L"WARDOGS Fire Control: log.invalid_path; file logging disabled.\n");
                return false;
            }
            if (path.has_parent_path())
                std::filesystem::create_directories(path.parent_path());
            logger.path = path;
            logger.previous_path = path.parent_path() /
                (path.stem().wstring() + L".previous" + path.extension().wstring());
            if (!validate_previous_locked(logger)) return false;
            std::error_code status_error;
            const bool prior_session = std::filesystem::exists(path, status_error);
            if (status_error || (prior_session &&
                    !std::filesystem::is_regular_file(path, status_error))) {
                report_failure(logger, L"WARDOGS Fire Control: log.invalid_existing_path; file logging disabled.\n");
                return false;
            }
            // A restart must retain the last battle's diagnostics. When only
            // previous exists, opening the new current file leaves it intact.
            if (prior_session && !retain_previous_locked(logger)) return false;
            logger.stream.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!logger.stream) {
                report_failure(logger, L"WARDOGS Fire Control: log.open_failed; file logging disabled.\n");
                return false;
            }
            logger.healthy = true;
            std::ostringstream message;
            message << "session.start version=" << sanitize_message(version)
                    << " pid=" << GetCurrentProcessId();
            write_locked(logger, LogLevel::info, message.str());
            if (!logger.healthy) return false;
            return flush_locked(logger);
        } catch (...) {
            report_failure(logger, L"WARDOGS Fire Control: log.initialize_failed; file logging disabled.\n");
            return false;
        }
    } catch (...) {
        OutputDebugStringW(L"WARDOGS Fire Control: log.synchronization_failed.\n");
        return false;
    }
}

void shutdown_session_log() noexcept {
    auto& logger = state();
    try {
        std::scoped_lock lock(logger.mutex);
        try {
            write_locked(logger, LogLevel::info, "session.end");
            if (logger.stream.is_open()) {
                if (logger.healthy && !flush_locked(logger)) return;
                logger.stream.close();
                if (!logger.stream)
                    report_failure(logger, L"WARDOGS Fire Control: log.close_failed; file logging disabled.\n");
            }
        } catch (...) {
            report_failure(logger, L"WARDOGS Fire Control: log.shutdown_failed; file logging disabled.\n");
        }
    } catch (...) {
        OutputDebugStringW(L"WARDOGS Fire Control: log.synchronization_failed.\n");
    }
}

void write_log(LogLevel level, std::string_view message) noexcept {
    auto& logger = state();
    try {
        std::scoped_lock lock(logger.mutex);
        try {
            write_locked(logger, level, message);
        } catch (...) {
            report_failure(logger, L"WARDOGS Fire Control: log.format_failed; file logging disabled.\n");
        }
    } catch (...) {
        OutputDebugStringW(L"WARDOGS Fire Control: log.synchronization_failed.\n");
    }
}

bool session_log_healthy() noexcept {
    auto& logger = state();
    try {
        std::scoped_lock lock(logger.mutex);
        return logger.healthy && logger.stream.is_open();
    } catch (...) {
        OutputDebugStringW(L"WARDOGS Fire Control: log.synchronization_failed.\n");
        return false;
    }
}

std::filesystem::path active_log_path() {
    auto& logger = state();
    std::scoped_lock lock(logger.mutex);
    return logger.path;
}

}  // namespace wardogs
