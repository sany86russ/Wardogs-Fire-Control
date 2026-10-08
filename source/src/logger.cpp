#include "wardogs/logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <limits>
#include <optional>
#include <vector>

namespace wardogs {
namespace {

struct LoggerState {
    std::mutex mutex;
    std::ofstream stream;
    std::filesystem::path path;
    std::filesystem::path previous_path;
    std::filesystem::path archive_path;
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

class FileHandle {
public:
    explicit FileHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~FileHandle() { if (valid()) CloseHandle(value_); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    [[nodiscard]] bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_;
};

bool safe_parent_directories(const std::filesystem::path& path) {
    auto parent = std::filesystem::absolute(path).lexically_normal().parent_path();
    while (!parent.empty()) {
        const DWORD attributes = GetFileAttributesW(parent.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        } else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                   (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    return true;
}

bool safe_log_handle(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(handle, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        info.nNumberOfLinks == 1 && info.nFileSizeHigh == 0 &&
        info.nFileSizeLow <= max_session_log_bytes;
}

bool validate_previous_locked(LoggerState& logger) {
    if (!safe_parent_directories(logger.previous_path)) {
        report_failure(logger, L"WARDOGS Fire Control: log.unsafe_parent; file logging disabled.\n");
        return false;
    }
    const DWORD attributes = GetFileAttributesW(logger.previous_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
        return true;
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return true;
    report_failure(logger, L"WARDOGS Fire Control: log.unsafe_previous; file logging disabled.\n");
    return false;
}

bool rename_handle(HANDLE handle, const std::filesystem::path& path, bool replace) {
    const auto filename = std::filesystem::absolute(path).lexically_normal().wstring();
    const auto bytes = filename.size() * sizeof(wchar_t);
    if (bytes > std::numeric_limits<DWORD>::max() - sizeof(FILE_RENAME_INFO)) return false;
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    // Windows 10 FileRenameInfoEx POSIX semantics keeps the pinned previous
    // handle valid while atomically replacing its directory entry. The old
    // handle remains our immutable source/rollback evidence until it closes.
    // FILE_RENAME_REPLACE_IF_EXISTS=1, FILE_RENAME_POSIX_SEMANTICS=2 (Microsoft).
    if (replace) info->Flags = 0x00000001 | 0x00000002;
    else info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(info->FileName, filename.data(), bytes);
    return SetFileInformationByHandle(handle, replace ? FileRenameInfoEx : FileRenameInfo, info,
        static_cast<DWORD>(storage.size())) != FALSE;
}

bool delete_handle(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(handle, FileDispositionInfo,
        &disposition, sizeof(disposition)) != FALSE;
}

struct ArchiveEntry {
    std::uint64_t sequence{};
    std::filesystem::path path;
};

std::optional<std::uint64_t> archive_sequence(const std::wstring& filename) {
    constexpr std::wstring_view prefix = L"session-";
    constexpr std::wstring_view suffix = L".log";
    if (filename.size() != prefix.size() + 20 + suffix.size() ||
        !filename.starts_with(prefix) || !filename.ends_with(suffix)) return std::nullopt;
    std::uint64_t result = 0;
    for (std::size_t index = prefix.size(); index < prefix.size() + 20; ++index) {
        if (filename[index] < L'0' || filename[index] > L'9') return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(filename[index] - L'0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return std::nullopt;
        result = result * 10 + digit;
    }
    return result ? std::optional{result} : std::nullopt;
}

std::wstring archive_name(std::uint64_t sequence) {
    std::wostringstream name;
    name << L"session-" << std::setfill(L'0') << std::setw(20) << sequence << L".log";
    return name.str();
}

// Keeps ownership of a newly created file until the rotation commits. Failure
// cleanup uses its native handle, never a name that could now belong to someone
// else. A failed cleanup is reported and no existing archive is removed.
class StagedArchive {
public:
    StagedArchive(LoggerState& logger, const std::filesystem::path& path)
        : logger_(logger), handle_(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~StagedArchive() {
        if (handle_.valid() && !keep_ && !delete_handle(handle_.get()))
            report_failure(logger_, L"WARDOGS Fire Control: log.archive_cleanup_failed; recovery file retained.\n");
    }
    [[nodiscard]] bool valid() const noexcept { return handle_.valid(); }
    [[nodiscard]] HANDLE get() const noexcept { return handle_.get(); }
    void keep() noexcept { keep_ = true; }
private:
    LoggerState& logger_;
    FileHandle handle_;
    bool keep_{};
};

bool retain_previous_locked(LoggerState& logger) {
    if (!validate_previous_locked(logger)) return false;
    if (!safe_parent_directories(logger.path)) {
        report_failure(logger, L"WARDOGS Fire Control: log.unsafe_parent; file logging disabled.\n");
        return false;
    }
    // DELETE access preflights rotation permission and sharing before creating
    // or pruning archives. Reads remain allowed; writes cannot race our copy.
    FileHandle current(CreateFileW(logger.path.c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!current.valid() || !safe_log_handle(current.get())) {
        report_failure(logger, L"WARDOGS Fire Control: log.unsafe_or_locked_current; file logging disabled.\n");
        return false;
    }
    const DWORD previous_attributes = GetFileAttributesW(logger.previous_path.c_str());
    if (previous_attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND &&
            rename_handle(current.get(), logger.previous_path, false)) return true;
        report_failure(logger, L"WARDOGS Fire Control: log.rotation_failed; file logging disabled.\n");
        return false;
    }
    FileHandle previous(CreateFileW(logger.previous_path.c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!previous.valid() || !safe_log_handle(previous.get())) {
        report_failure(logger, L"WARDOGS Fire Control: log.unsafe_or_locked_previous; file logging disabled.\n");
        return false;
    }
    if (!safe_parent_directories(logger.archive_path)) {
        report_failure(logger, L"WARDOGS Fire Control: log.unsafe_archive_parent; file logging disabled.\n");
        return false;
    }
    const DWORD archive_attributes = GetFileAttributesW(logger.archive_path.c_str());
    if (archive_attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND ||
            !CreateDirectoryW(logger.archive_path.c_str(), nullptr)) {
            report_failure(logger, L"WARDOGS Fire Control: log.archive_directory_failed; file logging disabled.\n");
            return false;
        }
    } else if (!(archive_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
               (archive_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        report_failure(logger, L"WARDOGS Fire Control: log.unsafe_archive_directory; file logging disabled.\n");
        return false;
    }
    std::vector<ArchiveEntry> entries;
    std::uint64_t highest_sequence = 0;
    for (const auto& entry : std::filesystem::directory_iterator(logger.archive_path)) {
        const auto sequence = archive_sequence(entry.path().filename().wstring());
        if (!sequence) continue;
        // Reserve recognizable names even when another owner put a directory
        // or a link there; these objects are never read, removed or overwritten.
        highest_sequence = std::max(highest_sequence, *sequence);
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            report_failure(logger, L"WARDOGS Fire Control: log.archive_status_failed; file logging disabled.\n");
            return false;
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        FileHandle file(CreateFileW(entry.path().c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file.valid()) {
            report_failure(logger, L"WARDOGS Fire Control: log.archive_status_failed; file logging disabled.\n");
            return false;
        }
        if (!safe_log_handle(file.get())) continue;
        if (entries.size() == max_session_log_archives) {
            report_failure(logger, L"WARDOGS Fire Control: log.archive_bounds_invalid; file logging disabled.\n");
            return false;
        }
        entries.push_back({*sequence, entry.path()});
    }
    if (entries.size() > max_session_log_archives ||
        highest_sequence == std::numeric_limits<std::uint64_t>::max()) {
        report_failure(logger, L"WARDOGS Fire Control: log.archive_bounds_invalid; file logging disabled.\n");
        return false;
    }
    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.sequence < rhs.sequence;
    });
    FileHandle oldest(entries.size() == max_session_log_archives
        ? CreateFileW(entries.front().path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)
        : INVALID_HANDLE_VALUE);
    if (entries.size() == max_session_log_archives &&
        (!oldest.valid() || !safe_log_handle(oldest.get()))) {
        report_failure(logger, L"WARDOGS Fire Control: log.archive_retention_blocked; file logging disabled.\n");
        return false;
    }
    const auto final_path = logger.archive_path / archive_name(highest_sequence + 1);
    const auto staging_path = std::filesystem::path(final_path.wstring() + L".partial");
    StagedArchive staged(logger, staging_path);
    if (!staged.valid()) {
        report_failure(logger, L"WARDOGS Fire Control: log.archive_create_failed; file logging disabled.\n");
        return false;
    }
    std::size_t copied = 0;
    char buffer[64 * 1024];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(previous.get(), buffer, sizeof(buffer), &read, nullptr)) {
            report_failure(logger, L"WARDOGS Fire Control: log.archive_read_failed; file logging disabled.\n");
            return false;
        }
        if (!read) break;
        if (read > max_session_log_bytes - copied) {
            report_failure(logger, L"WARDOGS Fire Control: log.archive_size_invalid; file logging disabled.\n");
            return false;
        }
        DWORD written = 0;
        if (!WriteFile(staged.get(), buffer, read, &written, nullptr) || written != read) {
            report_failure(logger, L"WARDOGS Fire Control: log.archive_write_failed; file logging disabled.\n");
            return false;
        }
        copied += read;
    }
    if (!FlushFileBuffers(staged.get()) || !rename_handle(staged.get(), final_path, false)) {
        report_failure(logger, L"WARDOGS Fire Control: log.archive_commit_failed; file logging disabled.\n");
        return false;
    }
    // Only now can the previous pathname be replaced: its complete bytes have
    // already been published atomically in the archive, with no overwrite.
    if (!rename_handle(current.get(), logger.previous_path, true)) {
        report_failure(logger, L"WARDOGS Fire Control: log.rotation_failed; file logging disabled.\n");
        return false;
    }
    if (oldest.valid() && !delete_handle(oldest.get())) {
        // Restore current and previous if pruning fails. If restoration itself
        // fails, keep the extra recovery archive rather than lose old history.
        const bool restored_current = rename_handle(current.get(), logger.path, false);
        const bool restored_previous = restored_current &&
            rename_handle(staged.get(), logger.previous_path, false);
        staged.keep();
        report_failure(logger, restored_previous
            ? L"WARDOGS Fire Control: log.archive_retention_failed; prior files restored, file logging disabled.\n"
            : L"WARDOGS Fire Control: log.archive_rollback_failed; recovery archive retained, file logging disabled.\n");
        return false;
    }
    staged.keep();
    return true;
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
        message.starts_with("impact.") || message.starts_with("shot.") ||
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
                if (logger.healthy && !flush_locked(logger)) return false;
                logger.stream.close();
                if (!logger.stream) {
                    report_failure(logger, L"WARDOGS Fire Control: log.close_failed; prior session could not be fully closed.\n");
                    return false;
                }
            }
            logger.stream.clear();
            logger.path.clear();
            logger.previous_path.clear();
            logger.archive_path.clear();
            logger.bytes_written = 0;
            logger.healthy = false;
            if (path.empty() || path.filename().empty()) {
                report_failure(logger, L"WARDOGS Fire Control: log.invalid_path; file logging disabled.\n");
                return false;
            }
            if (!safe_parent_directories(path)) {
                report_failure(logger, L"WARDOGS Fire Control: log.unsafe_parent; file logging disabled.\n");
                return false;
            }
            if (path.has_parent_path())
                std::filesystem::create_directories(path.parent_path());
            logger.path = std::filesystem::absolute(path).lexically_normal();
            logger.previous_path = logger.path.parent_path() /
                (path.stem().wstring() + L".previous" + path.extension().wstring());
            logger.archive_path = logger.path.parent_path() / (path.stem().wstring() + L".archive");
            if (!validate_previous_locked(logger)) return false;
            const DWORD current_attributes = GetFileAttributesW(logger.path.c_str());
            const bool prior_session = current_attributes != INVALID_FILE_ATTRIBUTES;
            if ((!prior_session && GetLastError() != ERROR_FILE_NOT_FOUND) ||
                (prior_session && (current_attributes &
                    (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))) {
                report_failure(logger, L"WARDOGS Fire Control: log.invalid_existing_path; file logging disabled.\n");
                return false;
            }
            // A restart must retain the last battle's diagnostics. When only
            // previous exists, opening the new current file leaves it intact.
            if (prior_session && !retain_previous_locked(logger)) return false;
            logger.stream.open(logger.path, std::ios::binary | std::ios::out | std::ios::trunc);
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

std::filesystem::path session_log_archive_directory() {
    auto& logger = state();
    std::scoped_lock lock(logger.mutex);
    return logger.archive_path;
}

}  // namespace wardogs
