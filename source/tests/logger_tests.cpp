#include "wardogs/logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
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
}

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    const auto directory = std::filesystem::temp_directory_path() /
        ("wardogs_logger_tests_" + std::to_string(GetCurrentProcessId()) + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "latest.log";
    const auto previous = directory / "latest.previous.log";

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
                              "terrain.selected map=ozeti", "terrain.solution height_delta_m=12",
                              "solution.l81 range_m=850 bearing_deg=236 available=1 mil=705"}) {
        wardogs::log_info(event);
        check(read_file(path).find(event) != std::string::npos,
              "a completed action is visible while the logger remains open");
    }
    wardogs::shutdown_session_log();
    check(wardogs::initialize_session_log(path, "bounded-message"),
          "a later run replaces the prior session");
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
    check(std::distance(std::filesystem::directory_iterator(directory),
                        std::filesystem::directory_iterator{}) == 2,
          "rotation does not create unlimited archives");
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
    for (const auto& locked_path : {path, previous}) {
        HANDLE locked = CreateFileW(locked_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(locked != INVALID_HANDLE_VALUE, "the startup retention fixture is locked");
        if (locked != INVALID_HANDLE_VALUE) {
            check(!wardogs::initialize_session_log(path, "locked-history") &&
                      !wardogs::session_log_healthy() &&
                      read_file(path) == current_before_lock &&
                      read_file(previous) == previous_before_lock,
                  "failed startup rotation preserves both current and previous bytes");
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
    std::filesystem::remove(directory);
    if (failures) return 1;
    std::cout << "All logger resource, UTF-8, failure, rotation and lifecycle tests passed\n"
              << "rotation_workload records=3500 elapsed_ms=" << rotation_elapsed
              << " handles_before=" << handles_before << " handles_after=" << handles_after << '\n';
    return 0;
}
