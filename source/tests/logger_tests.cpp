#include "wardogs/logger.hpp"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()};
}

bool valid_utf8(const std::string& text) {
    return text.empty() || MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0) > 0;
}

std::vector<std::filesystem::path> archives(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> result;
    if (!std::filesystem::exists(directory)) return result;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        const auto name = file.path().filename().wstring();
        const DWORD attributes = GetFileAttributesW(file.path().c_str());
        if (name.starts_with(L"session-") && name.ends_with(L".log") && name.size() == 32 &&
            attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            result.push_back(file.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::map<std::filesystem::path, std::string> archive_bytes(const std::filesystem::path& directory) {
    std::map<std::filesystem::path, std::string> result;
    for (const auto& path : archives(directory)) result.emplace(path, read_file(path));
    return result;
}

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    if (!output) throw std::runtime_error("fixture file write failed");
}

// Junctions do not require Developer Mode or symbolic-link privileges on the
// Windows CI volume. Never launch a shell or follow the junction for cleanup.
bool create_junction(const std::filesystem::path& link,
                     const std::filesystem::path& target) {
    struct MountPointData {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        wchar_t path_buffer[1];
    };
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { RemoveDirectoryW(link.c_str()); return false; }
    const auto printable = std::filesystem::absolute(target).wstring();
    const auto substitute = L"\\??\\" + printable;
    const std::size_t path_bytes = (substitute.size() + printable.size() + 2) * sizeof(wchar_t);
    std::vector<unsigned char> buffer(offsetof(MountPointData, path_buffer) + path_bytes);
    auto* data = reinterpret_cast<MountPointData*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->data_length = static_cast<WORD>(buffer.size() - 8);
    data->substitute_offset = 0;
    data->substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data->print_length = static_cast<WORD>(printable.size() * sizeof(wchar_t));
    std::memcpy(data->path_buffer, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<unsigned char*>(data->path_buffer) + data->print_offset,
        printable.c_str(), (printable.size() + 1) * sizeof(wchar_t));
    DWORD returned = 0;
    const bool success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT,
        buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    if (!success) RemoveDirectoryW(link.c_str());
    return success;
}
}

int main() {
    int failures = 0;
    int checks = 0;
    const auto check = [&](bool condition, const char* message) {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    const auto directory = std::filesystem::temp_directory_path() /
        ("wardogs_logger_tests_" + std::to_string(GetCurrentProcessId()) + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto temporary_root = std::filesystem::absolute(std::filesystem::temp_directory_path());
    if (!std::filesystem::equivalent(directory.parent_path(), temporary_root) ||
        !directory.filename().wstring().starts_with(L"wardogs_logger_tests_") ||
        !std::filesystem::create_directory(directory)) {
        std::cerr << "FAIL: the uniquely owned temporary fixture cannot be created\n";
        return 1;
    }
    const auto path = directory / "latest.log";
    const auto previous = directory / "latest.previous.log";
    const auto archive_directory = directory / "latest.archive";

    wardogs::shutdown_session_log();
    check(!wardogs::session_log_healthy(), "no session has a writable logger");
    check(wardogs::initialize_session_log(path, "test-version") &&
              wardogs::session_log_healthy(), "a valid session reports healthy");
    std::string multiline = "Русский текст\r\nforged line\t";
    multiline.push_back('\0');
    multiline.push_back('\x01');
    multiline += "\xC2\x85\xE2\x80\xA8\xE2\x80\xA9";
    wardogs::log_info(multiline);
    wardogs::log_info(std::string("invalid=") + "\xC0\xAF\xED\xA0\x80\xF4\x90\x80\x80" + " конец");
    wardogs::log_warning("warning-flushed");
    const auto immediate = read_file(path);
    check(immediate.find("warning-flushed") != std::string::npos &&
              immediate.find("Русский текст") != std::string::npos,
          "a warning flushes preceding buffered diagnostics");
    wardogs::log_error("error-flushed");
    check(read_file(path).find("error-flushed") != std::string::npos,
          "an error is persisted before shutdown");
    wardogs::shutdown_session_log();
    auto contents = read_file(path);
    check(!wardogs::session_log_healthy(), "shutdown closes the active session");
    check(valid_utf8(contents) && contents.find("конец") != std::string::npos,
          "malformed input cannot invalidate UTF-8 or destroy later valid text");
    check(contents.find("Русский текст\\r\\nforged line\\t\\u0000\\u0001") !=
              std::string::npos &&
              contents.find("\\u0085\\u2028\\u2029") != std::string::npos,
          "newlines, controls and Unicode line separators are escaped");
    check(contents.find('\r') == std::string::npos &&
              contents.find('\0') == std::string::npos &&
              std::count(contents.begin(), contents.end(), '\n') == 6,
          "each diagnostic produces exactly one physical line");

    check(wardogs::initialize_session_log(path, "completed-actions"),
          "completed-action diagnostics open a session");
    check(read_file(previous) == contents,
          "opening a new session retains the complete preceding session");
    for (const auto* event : {"ocr.finished success=1", "continuous.impact_recorded count=5",
                              "shot.recorded number=2", "impact.guidance_accepted source=manual",
                              "terrain.selected map=ozeti", "terrain.solution height_delta_m=12",
                              "solution.l81 range_m=850 bearing_deg=236 available=1 mil=705"}) {
        wardogs::log_info(event);
        check(read_file(path).find(event) != std::string::npos,
              "a completed action is visible while the logger remains open");
    }
    wardogs::shutdown_session_log();
    check(wardogs::initialize_session_log(path, "bounded-message"),
          "a later run replaces the prior session");
    const auto first_archives = archives(archive_directory);
    check(first_archives.size() == 1 && read_file(first_archives.front()) == contents &&
              wardogs::session_log_archive_directory() == archive_directory,
          "three sessions retain the first complete UTF-8 session in a named archive");
    wardogs::log_info("oversized=" + std::string(50'000, 'A'));
    wardogs::log_info(std::string(5'000, '\xFF'));
    wardogs::shutdown_session_log();
    contents = read_file(path);
    std::istringstream lines{contents};
    std::string line;
    int truncated_lines = 0;
    while (std::getline(lines, line)) {
        if (line.find("[truncated]") != std::string::npos) {
            ++truncated_lines;
            const auto header_end = line.find("] ", line.find("[tid "));
            check(header_end != std::string::npos &&
                      line.size() - (header_end + 2) <= wardogs::max_log_message_bytes,
                  "escaped and long diagnostic messages obey their byte budget");
        }
    }
    check(truncated_lines == 2 && valid_utf8(contents),
          "long text and malformed text are truncated into valid single lines");

    check(wardogs::initialize_session_log(path, "rotation-test"),
          "the size-limited session opens");
    const std::string payload(3'800, 'X');
    const auto rotation_started = std::chrono::steady_clock::now();
    for (int index = 0; index < 3'500; ++index)
        wardogs::log_info("sequence=" + std::to_string(index) + " " + payload);
    wardogs::shutdown_session_log();
    const auto rotation_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - rotation_started).count();
    check(std::filesystem::is_regular_file(path) &&
              std::filesystem::is_regular_file(previous),
          "rotation retains current and previous same-session files");
    check(std::filesystem::file_size(path) <= wardogs::max_session_log_bytes &&
              std::filesystem::file_size(previous) <= wardogs::max_session_log_bytes,
          "multiple rotations never exceed either 4 MiB file cap");
    const auto rotated_archives = archives(archive_directory);
    check(rotated_archives.size() >= 4 &&
              rotated_archives.size() <= wardogs::max_session_log_archives,
          "multiple rotations archive displaced segments within the retention count");
    for (const auto& archive : rotated_archives)
        check(std::filesystem::file_size(archive) <= wardogs::max_session_log_bytes &&
                  valid_utf8(read_file(archive)),
              "every rotated archive respects the byte budget and UTF-8 contract");
    check(read_file(path).find("sequence=3499") != std::string::npos &&
              read_file(path).find("session.continued log.rotated previous=1") !=
                  std::string::npos &&
              read_file(path).find("session.end") != std::string::npos,
          "rotation preserves the latest complete records and lifecycle notice");
    const auto last_battle = read_file(path);
    check(wardogs::initialize_session_log(path, "new-session") &&
              read_file(previous) == last_battle,
          "a new session retains the last battle rather than deleting both log files");
    wardogs::shutdown_session_log();
    check(read_file(path).find("sequence=3499") == std::string::npos,
          "new runs do not mix retained records from prior sessions");

    const auto retained_battle = read_file(previous);
    std::filesystem::remove(path);
    check(wardogs::initialize_session_log(path, "missing-current") &&
              read_file(previous) == retained_battle,
          "an absent current file cannot erase the only retained battle log");
    wardogs::shutdown_session_log();
    const auto current_before_lock = read_file(path);
    const auto previous_before_lock = read_file(previous);
    const auto archives_before_lock = archive_bytes(archive_directory);
    for (const auto& locked_path : {path, previous}) {
        HANDLE locked = CreateFileW(locked_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(locked != INVALID_HANDLE_VALUE, "the startup retention fixture is locked");
        if (locked != INVALID_HANDLE_VALUE) {
            check(!wardogs::initialize_session_log(path, "locked-history") &&
                      !wardogs::session_log_healthy() &&
                      read_file(path) == current_before_lock &&
                      read_file(previous) == previous_before_lock &&
                      archive_bytes(archive_directory) == archives_before_lock,
                  "failed startup rotation preserves current, previous and every archive");
            CloseHandle(locked);
        }
    }

    check(wardogs::initialize_session_log(path, "open-session"),
          "startup retention recovers after the sharing lock is released");
    wardogs::log_info("buffered-before-reinitialize");
    check(wardogs::initialize_session_log(path, "reinitialized-session") &&
              read_file(previous).find("buffered-before-reinitialize") != std::string::npos,
          "reinitialization flushes and retains a still-open session");
    wardogs::shutdown_session_log();

    const auto bounded_directory = directory / "bounded";
    std::filesystem::create_directory(bounded_directory);
    const auto bounded_path = bounded_directory / "latest.log";
    const auto bounded_previous = bounded_directory / "latest.previous.log";
    const auto bounded_archives = bounded_directory / "latest.archive";
    for (int session = 0; session < 40; ++session) {
        check(wardogs::initialize_session_log(bounded_path, "retention-test"),
              "bounded archive sessions initialize");
        wardogs::log_warning("retained-session=" + std::to_string(session));
        wardogs::shutdown_session_log();
    }
    const auto bounded_files = archives(bounded_archives);
    check(bounded_files.size() == wardogs::max_session_log_archives &&
              bounded_files.front().filename() == L"session-00000000000000000007.log" &&
              bounded_files.back().filename() == L"session-00000000000000000038.log" &&
              read_file(bounded_files.front()).find("retained-session=6") != std::string::npos &&
              read_file(bounded_files.back()).find("retained-session=37") != std::string::npos,
          "retention deterministically keeps the latest 32 archives with unique monotonic names");
    if (bounded_files.size() != wardogs::max_session_log_archives) {
        std::filesystem::remove_all(directory);
        return 1;
    }
    std::uintmax_t archive_total_bytes = 0;
    for (const auto& file : bounded_files) archive_total_bytes += std::filesystem::file_size(file);
    check(archive_total_bytes <= wardogs::max_session_log_archives * wardogs::max_session_log_bytes,
          "completed managed archives stay inside the 128 MiB aggregate byte budget");
    const auto bounded_current_before = read_file(bounded_path);
    const auto bounded_previous_before = read_file(bounded_previous);
    const auto bounded_bytes_before = archive_bytes(bounded_archives);
    HANDLE locked_oldest = CreateFileW(bounded_files.front().c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked_oldest != INVALID_HANDLE_VALUE, "the oldest archive sharing fixture opens");
    if (locked_oldest != INVALID_HANDLE_VALUE) {
        check(!wardogs::initialize_session_log(bounded_path, "locked-retention") &&
                  !wardogs::session_log_healthy() &&
                  read_file(bounded_path) == bounded_current_before &&
                  read_file(bounded_previous) == bounded_previous_before &&
                  archive_bytes(bounded_archives) == bounded_bytes_before,
              "blocked archive retention preserves all existing history before a new commit");
        CloseHandle(locked_oldest);
    }
    check(SetFileAttributesW(bounded_files.front().c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
          "the read-only archive rollback fixture is prepared");
    check(!wardogs::initialize_session_log(bounded_path, "readonly-retention") &&
              !wardogs::session_log_healthy() &&
              read_file(bounded_path) == bounded_current_before &&
              read_file(bounded_previous) == bounded_previous_before &&
              archive_bytes(bounded_archives) == bounded_bytes_before,
          "a prune failure rolls current and previous back without losing any archive");
    check(SetFileAttributesW(bounded_files.front().c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE,
          "the read-only fixture is restored");
    write_file(bounded_archives / "unrelated.txt", "foreign file must survive");
    std::filesystem::create_directory(bounded_archives / "foreign-directory");
    write_file(bounded_archives / "foreign-directory" / "keep.txt", "foreign directory must survive");
    const auto foreign_named_directory = bounded_archives / L"session-00000000000000000100.log";
    std::filesystem::create_directory(foreign_named_directory);
    check(wardogs::initialize_session_log(bounded_path, "retention-recovered"),
          "archive retention recovers after obstructions disappear");
    wardogs::shutdown_session_log();
    const auto recovered_files = archives(bounded_archives);
    check(recovered_files.size() == wardogs::max_session_log_archives &&
              recovered_files.back().filename() == L"session-00000000000000000101.log" &&
              read_file(bounded_archives / "unrelated.txt") == "foreign file must survive" &&
              read_file(bounded_archives / "foreign-directory" / "keep.txt") ==
                  "foreign directory must survive" &&
              std::filesystem::is_directory(foreign_named_directory),
          "foreign files and directories are preserved and occupied names are never replaced");

    const auto blocked_directory = directory / "blocked-archive";
    std::filesystem::create_directory(blocked_directory);
    const auto blocked_path = blocked_directory / "latest.log";
    const auto blocked_previous = blocked_directory / "latest.previous.log";
    const auto blocked_archive = blocked_directory / "latest.archive";
    write_file(blocked_path, "current battle");
    write_file(blocked_previous, "previous battle");
    write_file(blocked_archive, "foreign obstruction");
    check(!wardogs::initialize_session_log(blocked_path, "blocked-directory") &&
              read_file(blocked_path) == "current battle" &&
              read_file(blocked_previous) == "previous battle" &&
              read_file(blocked_archive) == "foreign obstruction",
          "an archive pathname owned by a foreign file cannot erase history or the obstruction");
    std::filesystem::remove(blocked_archive);
    std::filesystem::create_directory(blocked_archive);
    const auto collision = blocked_archive / L"session-00000000000000000001.log.partial";
    write_file(collision, "unrecognized staging file");
    check(!wardogs::initialize_session_log(blocked_path, "staging-collision") &&
              read_file(blocked_path) == "current battle" &&
              read_file(blocked_previous) == "previous battle" &&
              read_file(collision) == "unrecognized staging file" && archives(blocked_archive).empty(),
          "a staging name collision never overwrites or cleans up another owner's file");
    std::filesystem::remove(collision);
    const auto foreign_target = directory / "foreign-target";
    std::filesystem::create_directory(foreign_target);
    write_file(foreign_target / "keep.txt", "reparse target survives");
    std::filesystem::remove(blocked_archive);
    const bool archive_junction = create_junction(blocked_archive, foreign_target);
    check(archive_junction, "the archive junction safety fixture can be created");
    if (archive_junction) {
        check(!wardogs::initialize_session_log(blocked_path, "reparse-archive") &&
                  read_file(blocked_path) == "current battle" &&
                  read_file(blocked_previous) == "previous battle" &&
                  read_file(foreign_target / "keep.txt") == "reparse target survives" &&
                  archives(foreign_target).empty(),
              "archive reparse points never redirect archive writes or cleanup");
        check(RemoveDirectoryW(blocked_archive.c_str()) != FALSE,
              "only the test junction itself is removed");
    }
    const auto parent_junction = directory / "parent-junction";
    const bool unsafe_parent = create_junction(parent_junction, foreign_target);
    check(unsafe_parent, "the parent junction safety fixture can be created");
    if (unsafe_parent) {
        check(!wardogs::initialize_session_log(parent_junction / "latest.log", "unsafe-parent") &&
                  !std::filesystem::exists(foreign_target / "latest.log") &&
                  read_file(foreign_target / "keep.txt") == "reparse target survives",
              "a reparse ancestor cannot redirect creation of a new session file");
        check(RemoveDirectoryW(parent_junction.c_str()) != FALSE,
              "only the parent test junction itself is removed");
    }
    const auto hardlinked_current = directory / "hardlinked-current.log";
    const bool current_hardlink = CreateHardLinkW(hardlinked_current.c_str(), blocked_path.c_str(), nullptr) != FALSE;
    check(current_hardlink, "the current hard-link safety fixture can be created");
    if (current_hardlink) {
        check(!wardogs::initialize_session_log(hardlinked_current, "hardlink-current") &&
                  read_file(blocked_path) == "current battle" &&
                  read_file(hardlinked_current) == "current battle",
              "a current file sharing a foreign hard link cannot be truncated or rotated");
        std::filesystem::remove(hardlinked_current);
    }
    const auto hardlinked_previous = directory / "foreign-previous-link.log";
    const bool previous_hardlink = CreateHardLinkW(hardlinked_previous.c_str(), blocked_previous.c_str(), nullptr) != FALSE;
    check(previous_hardlink, "the previous hard-link safety fixture can be created");
    if (previous_hardlink) {
        check(!wardogs::initialize_session_log(blocked_path, "hardlink-previous") &&
                  read_file(blocked_previous) == "previous battle" &&
                  read_file(hardlinked_previous) == "previous battle" &&
                  read_file(blocked_path) == "current battle",
              "a previous file sharing a foreign hard link cannot be replaced");
        std::filesystem::remove(hardlinked_previous);
    }
    write_file(blocked_previous, std::string(wardogs::max_session_log_bytes + 1, 'Z'));
    check(!wardogs::initialize_session_log(blocked_path, "oversized-previous") &&
              read_file(blocked_path) == "current battle" &&
              std::filesystem::file_size(blocked_previous) == wardogs::max_session_log_bytes + 1,
          "legacy oversized files remain intact and cannot create an oversized archive");

    // The directory was created uniquely by this process directly under the
    // verified temporary root. All reparse fixtures were removed by their own
    // native directory handles/names above; never recursively clean their targets.
    check(!std::filesystem::exists(parent_junction) &&
              !(GetFileAttributesW(blocked_archive.c_str()) != INVALID_FILE_ATTRIBUTES &&
                (GetFileAttributesW(blocked_archive.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)),
          "no fixture reparse point remains before owned temporary cleanup");
    const bool safe_cleanup = !std::filesystem::exists(parent_junction) &&
        !(GetFileAttributesW(blocked_archive.c_str()) != INVALID_FILE_ATTRIBUTES &&
          (GetFileAttributesW(blocked_archive.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT));

    check(wardogs::initialize_session_log(path, "concurrent-session"),
          "a concurrent session opens");
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
        workers.emplace_back([worker] {
            for (int index = 0; index < 100; ++index)
                wardogs::log_info("worker=" + std::to_string(worker) +
                                  " sequence=" + std::to_string(index));
        });
    for (auto& worker : workers) worker.join();
    wardogs::shutdown_session_log();
    contents = read_file(path);
    check(std::count(contents.begin(), contents.end(), '\n') == 402 &&
              valid_utf8(contents), "concurrent diagnostics remain complete records");

    // Reproduce a real filesystem rotation failure without filling the drive
    // or modifying permissions: another owner places a directory at the log
    // destination. The logger must preserve it and stop growing its own file.
    check(wardogs::initialize_session_log(path, "failed-rotation"),
          "a session opens before the injected filesystem obstruction");
    std::filesystem::remove(previous);
    std::filesystem::create_directory(previous);
    for (int index = 0; index < 1'200 && wardogs::session_log_healthy(); ++index)
        wardogs::log_info(payload);
    check(!wardogs::session_log_healthy() &&
              std::filesystem::is_directory(previous),
          "rotation failure is explicit and never removes a directory");
    const auto failed_size = std::filesystem::file_size(path);
    wardogs::log_error("must-not-grow-after-disk-failure");
    wardogs::shutdown_session_log();
    check(failed_size <= wardogs::max_session_log_bytes &&
              std::filesystem::file_size(path) == failed_size,
          "failed rotation stops disk growth and releases the file");
    check(!wardogs::initialize_session_log(path, "blocked-path"),
          "initialization also reports an obstructed previous-file path");
    std::filesystem::remove(previous);
    check(!wardogs::initialize_session_log(directory, "directory-path"),
          "opening a directory as the log file reports failure");
    check(std::filesystem::is_directory(directory),
          "failed initialization preserves the directory");
    check(wardogs::initialize_session_log(path, "recovered-session") &&
              wardogs::session_log_healthy(),
          "a new valid session resets an earlier failure");
    wardogs::shutdown_session_log();

    DWORD handles_before = 0, handles_after = 0;
    const BOOL counted_before = GetProcessHandleCount(GetCurrentProcess(), &handles_before);
    for (int session = 0; session < 64; ++session) {
        check(wardogs::initialize_session_log(path, "handle-test"),
              "repeated sessions open");
        wardogs::log_info("short diagnostic");
        wardogs::shutdown_session_log();
    }
    const BOOL counted_after = GetProcessHandleCount(GetCurrentProcess(), &handles_after);
    check(counted_before && counted_after && handles_after <= handles_before + 1,
          "repeated logging sessions do not accumulate Windows handles");
    std::filesystem::rename(path, directory / "closed.log");
    std::filesystem::remove(directory / "closed.log");
    std::filesystem::remove(previous);
    if (safe_cleanup) std::filesystem::remove_all(directory);
    if (failures) return 1;
    std::cout << "All logger resource, UTF-8, failure, rotation and lifecycle tests passed\n"
              << "checks=" << checks << " retained_archives=" << bounded_files.size() << '\n'
              << "rotation_workload records=3500 elapsed_ms=" << rotation_elapsed
              << " handles_before=" << handles_before << " handles_after=" << handles_after << '\n';
    return 0;
}
