#include "wardogs/core.hpp"
#include "wardogs/hotkeys.hpp"
#include "wardogs/logger.hpp"
#include "wardogs/settings.hpp"

#include <array>
#include <cmath>
#include <clocale>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void close(double actual, double expected, const char* message) {
    check(std::abs(actual - expected) < 1e-8, message);
}

void rejects(const std::function<void()>& action, const char* message) {
    try {
        action();
        check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

void rejects_with_message(const std::function<void()>& action,
                          std::string_view expected, const char* message) {
    try {
        action();
        check(false, message);
    } catch (const std::invalid_argument& error) {
        check(error.what() == expected, message);
    }
}

}  // namespace

int main() {
    using wardogs::Point;

    const std::vector<std::pair<Point, double>> bearings{
        {{0, 10}, 0}, {{10, 0}, 90}, {{0, -10}, 180}, {{-10, 0}, 270},
    };
    for (const auto& [target, expected] : bearings) {
        close(wardogs::calculate_shot({0, 0}, target).angle, expected,
              "bearing is clockwise from positive Y");
    }
    const auto shot = wardogs::calculate_shot({0, 0}, {3, 4});
    close(shot.distance, 5, "distance uses Euclidean length");
    close(shot.angle, 36.86989764584402, "north-zero diagonal bearing");
    close(wardogs::calculate_shot({0, 0}, {-0.0, -0.0}).angle, 0,
          "coincident points use a canonical bearing even with signed zero");
    for (double x : {-0.0, -1e-15}) {
        const auto north = wardogs::calculate_shot({0, 0}, {x, 10});
        check(north.angle >= 0.0 && north.angle < 360.0 && !std::signbit(north.angle),
              "rounded north retains a canonical bearing inside the half-open circle");
    }
    for (int degree = 0; degree < 360; degree += 3) {
        const double radians = degree * std::acos(-1.0) / 180.0;
        const Point base{60, 70};
        const Point target{60 + 17.25 * std::sin(radians),
                           70 + 17.25 * std::cos(radians)};
        const auto forward = wardogs::calculate_shot(base, target);
        const auto reverse = wardogs::calculate_shot(target, base);
        close(forward.distance, 17.25, "distance is independent of bearing");
        close(reverse.distance, forward.distance, "distance is symmetric");
        close(std::abs(std::remainder(reverse.angle - forward.angle, 360)), 180.0,
              "reversing a nonzero target changes bearing by half a turn");
        close(std::remainder(forward.angle - degree, 360), 0,
              "bearing recovers a full-circle reference direction");
    }
    rejects([] { (void)wardogs::calculate_shot(
        {0, 0}, {std::numeric_limits<double>::quiet_NaN(), 1}); },
        "NaN cannot become an aim bearing");
    rejects([] { (void)wardogs::calculate_shot(
        {std::numeric_limits<double>::max(), 0},
        {-std::numeric_limits<double>::max(), 0}); },
        "coordinate subtraction overflow is reported");
    rejects([] { (void)wardogs::calculate_shot({0, 0}, {1e307, 1e307}); },
            "finite distance cannot overflow when converted to game metres");
    close(wardogs::mortar_mil_for_distance(132.0), 850.0,
          "mortar table keeps the upstream near endpoint");
    close(wardogs::mortar_mil_for_distance(684.0), 150.0,
          "mortar table keeps the upstream far endpoint");
    for (double base_coordinate : {50.0, 98.54, 110.33}) {
        for (double endpoint : {132.0, 684.0}) {
            const auto endpoint_shot = wardogs::calculate_shot(
                {base_coordinate, base_coordinate},
                {base_coordinate, base_coordinate + endpoint / 100.0});
            try {
                close(wardogs::mortar_mil_for_distance(endpoint_shot.distance * 100.0),
                      endpoint == 132.0 ? 850.0 : 150.0,
                      "decimal map coordinates preserve exact L81 range endpoints");
            } catch (const std::invalid_argument&) {
                check(false, "an exact L81 endpoint cannot be rejected by coordinate roundoff");
            }
        }
    }
    for (double range : {132.0, 136.0, 208.0, 340.0, 600.0, 684.0}) {
        const double reference_mil = wardogs::mortar_mil_for_distance(range);
        for (int degree = 0; degree < 360; degree += 3) {
            const double radians = degree * std::acos(-1.0) / 180.0;
            const Point base{98.54, 110.33};
            const Point target{base.x + range / 100.0 * std::sin(radians),
                               base.y + range / 100.0 * std::cos(radians)};
            try {
                close(wardogs::mortar_mil_for_distance(
                          wardogs::calculate_shot(base, target).distance * 100.0),
                      reference_mil,
                      "L81 sight settings are invariant under map translation and bearing");
            } catch (const std::invalid_argument&) {
                check(false, "rotation of a valid L81 shot cannot move it beyond the firing table");
            }
        }
    }
    rejects([] { (void)wardogs::mortar_mil_for_distance(132.0 - 1e-8); },
            "L81 endpoint allowance cannot admit a genuinely shorter range");
    rejects([] { (void)wardogs::mortar_mil_for_distance(684.0 + 1e-8); },
            "L81 endpoint allowance cannot extend the supported maximum range");
    close(wardogs::mortar_mil_for_distance(136.0), 845.0,
          "mortar MIL is linearly interpolated between upstream samples");
    close(wardogs::mortar_mil_for_distance(339.0), 650.0,
          "L81 retains the lower neighbor of the live screenshot range");
    close(wardogs::mortar_mil_for_distance(348.0), 640.0,
          "L81 retains the upper neighbor of the live screenshot range");
    close(wardogs::mortar_mil_for_distance(340.0), 648.8888888888889,
          "L81 interpolates an intermediate range between table samples");
    close(wardogs::mortar_mil_for_distance(600.0), 315.0,
          "L81 angular sight scale need not exceed the number of metres");
    const auto screenshot_shot = wardogs::calculate_shot({98.54, 110.33}, {101.72, 109.13});
    close(screenshot_shot.distance * 100.0, 339.8882169184445,
          "live screenshot coordinates retain the unrounded range in metres");
    close(screenshot_shot.angle, 110.67442476087398,
          "live screenshot coordinates recover the displayed bearing");
    close(wardogs::mortar_mil_for_distance(screenshot_shot.distance * 100.0), 649.0130923128394,
          "live screenshot MIL uses full range before display rounding");
    rejects([] { (void)wardogs::mortar_mil_for_distance(131.9); },
            "mortar table rejects targets below the upstream minimum range");
    rejects([] { (void)wardogs::mortar_mil_for_distance(684.1); },
            "mortar table rejects targets beyond the upstream maximum range");
    rejects([] { (void)wardogs::mortar_mil_for_distance(
        std::numeric_limits<double>::quiet_NaN()); },
        "non-finite mortar distance cannot produce an endpoint solution");
    rejects([] { (void)wardogs::mortar_mil_for_distance(
        std::numeric_limits<double>::infinity()); }, "positive infinity cannot produce mortar MIL");
    rejects([] { (void)wardogs::mortar_mil_for_distance(
        -std::numeric_limits<double>::infinity()); }, "negative infinity cannot produce mortar MIL");
    double previous_mortar_mil = wardogs::mortar_mil_for_distance(132);
    for (double metres = 132; metres <= 684; metres += 0.25) {
        const double mil = wardogs::mortar_mil_for_distance(metres);
        check(std::isfinite(mil) && mil <= previous_mortar_mil && mil >= 150 && mil <= 850,
              "retained L81 interpolation is finite, monotone and within table limits");
        previous_mortar_mil = mil;
    }

    const std::vector<std::pair<std::wstring, Point>> ocr_cases{
        {L"x12.34, y56.78", {12.34, 56.78}},
        {L"旧 x1.00, y2.00\n新 x30.00, y40.00", {30, 40}},
        {L"x1 1.32, y54.1 3", {11.32, 54.13}},
        {L"x114.51, yl 91.81", {114.51, 191.81}},
        {L"xI14.51, yO91.81", {114.51, 91.81}},
        {L"xli4.51& YI 91.81", {114.51, 191.81}},
        {L"x13.11, y14.21|", {13.11, 14.21}},
        {L"x13.11, y14.2|", {13.11, 14.21}},
        {L"x101.33, y112.554", {101.33, 112.55}},
        {L"x101.3399, y112.5588", {101.33, 112.55}},
        {L"x13.11, y14.21|3", {13.11, 14.21}},
    };
    for (const auto& [text, expected] : ocr_cases) {
        check(wardogs::parse_ocr_coordinate(text) == expected,
              "OCR parser repairs spaces and glyph confusions");
    }
    rejects([] { wardogs::parse_ocr_coordinate(L"x121 51, y131.81"); },
            "OCR rejects a missing decimal point");
    rejects([] { wardogs::parse_ocr_coordinate(L"x12.3, y123.22"); },
            "OCR still requires two fractional digits");
    check(wardogs::parse_ocr_coordinate(L"A 12.34 B 56.78", LR"(A\s+([\d.]+)\s+B\s+([\d.]+))") ==
              Point{12.34, 56.78}, "custom coordinate pattern remains supported");
    check(wardogs::parse_ocr_coordinate(L"A 98.76 B 54.32", LR"(A\s+([\d.]+)\s+B\s+([\d.]+))") ==
              Point{98.76, 54.32}, "cached custom regex reads new text each time");
    rejects([] { (void)wardogs::parse_ocr_coordinate(L"x1.00 y2.00", L"["); },
            "invalid custom regex is reported");
    rejects([] { (void)wardogs::parse_ocr_coordinate(L"x1.00 y2.00", L"x.*y.*"); },
            "a matching custom regex must provide coordinate capture groups");

    check(wardogs::parse_ocr_coordinates(L"x1.00 y2.00\nx30.00 y4o.00") ==
              std::vector<Point>{{1, 2}, {30, 40}},
          "all OCR coordinate pairs retain reading order and glyph repair");
    check(wardogs::parse_ocr_coordinates(L"no complete coordinates").empty(),
          "all-pairs OCR parser distinguishes no match from a malformed number");
    check(wardogs::parse_ocr_coordinates(L"A 12.34 B 56.78 A 90.12 B 34.56",
          LR"(A\s+([\d.]+)\s+B\s+([\d.]+))") ==
          std::vector<Point>{{12.34, 56.78}, {90.12, 34.56}},
          "all-pairs OCR parser supports the cached user pattern");
    rejects([] { (void)wardogs::parse_ocr_coordinates(L"x1.00 y2.00", L"["); },
            "all-pairs OCR parser rejects an invalid regex");
    rejects([] { (void)wardogs::parse_ocr_coordinates(L"x1.00 y2.00", L"x.*y.*"); },
            "all-pairs OCR matches must capture both numbers");
    rejects([] { (void)wardogs::parse_ocr_coordinates(std::wstring(8193, L'x')); },
            "all-pairs OCR text is bounded before scanning");
    for (const auto* pattern : {L"((x+)+)y", L"x([0-9]+)([0-9]+)",
             L"x(a|aa)+([0-9]+)", L"x(?=1)([0-9]+)y([0-9]+)",
             L"x([0-9]+)y(\\1)", L"x(.*)y(.*)",
             L"x(A+)a(a+)", L"x([0-9]{100000000})y([0-9]+)",
             L"A([a-z]+)Z([0-9]+)", L"^x*Xx*Xx*XA([0-9.]+)B([0-9.]+)"}) {
        rejects([&] { wardogs::validate_ocr_coordinate_pattern(pattern); },
                "unsafe custom patterns are rejected before a backtracking search");
    }
    wardogs::validate_ocr_coordinate_pattern(wardogs::default_ocr_coordinate_pattern);
    std::wstring ansi_default{wardogs::default_ocr_coordinate_pattern};
    for (auto& character : ansi_default) {
        if (character == L'，') character = L',';
        if (character == L'；') character = L';';
    }
    check(ansi_default != wardogs::default_ocr_coordinate_pattern &&
              wardogs::normalize_ocr_coordinate_pattern(ansi_default) ==
                  wardogs::default_ocr_coordinate_pattern,
          "the exact ANSI-damaged built-in template is repaired");
    wardogs::validate_ocr_coordinate_pattern(ansi_default);
    for (const auto& [text, expected] : ocr_cases) {
        check(wardogs::parse_ocr_coordinate(text, ansi_default) == expected,
              "the ANSI built-in alias uses the bounded default OCR scanner");
    }
    check(wardogs::parse_ocr_coordinates(L"x1.00 y2.00\nx30.00 y4o.00", ansi_default) ==
              std::vector<Point>{{1, 2}, {30, 40}},
          "the ANSI alias retains every complete coordinate pair");
    rejects([&] { (void)wardogs::parse_ocr_coordinates(std::wstring(8193, L'x'), ansi_default); },
            "the ANSI alias retains the default scanner's input bound");
    auto changed_default = ansi_default;
    changed_default.front() = L'A';
    check(wardogs::normalize_ocr_coordinate_pattern(changed_default) == changed_default,
          "a changed user expression is not silently replaced with the default");
    rejects([&] { wardogs::validate_ocr_coordinate_pattern(changed_default); },
            "changing the ANSI alias cannot bypass restricted pattern validation");
    constexpr auto custom_numeric = LR"(^([0-9]{1,4}\.\d{2}),([0-9]{1,4}\.\d{2})$)";
    check(wardogs::normalize_ocr_coordinate_pattern(custom_numeric) == custom_numeric,
          "safe custom templates remain unchanged during normalization");
    wardogs::validate_ocr_coordinate_pattern(LR"(^([0-9]{1,4}\.\d{2}),([0-9]{1,4}\.\d{2})$)");
    check(wardogs::parse_ocr_coordinate(L"12.34,56.78",
              LR"(^([0-9]{1,4}\.\d{2}),([0-9]{1,4}\.\d{2})$)") == Point{12.34, 56.78},
          "anchored numeric templates with small bounded counts remain supported");
    check(wardogs::parse_ocr_coordinate(L"A12.34B56.78", LR"(A([\d.]+)B([\d.]+))") ==
              Point{12.34, 56.78},
          "case-folded disjoint literal labels preserve custom coordinate templates");

    check(wardogs::parse_manual_coordinate(L"12.34 56.78") == Point{12.34, 56.78},
          "manual parser accepts a whitespace pair");
    check(wardogs::parse_manual_coordinate(L"x1, y2") == Point{1, 2},
          "manual parser accepts labelled integers");
    check(wardogs::parse_manual_coordinate(L"x1.239, y2.999") == Point{1.239, 2.999},
          "manual input keeps its full precision");
    rejects([] { wardogs::parse_manual_coordinate(L"12 34 56"); },
            "manual parser rejects three values");
    rejects([] { (void)wardogs::parse_manual_coordinate(std::wstring(400, L'9') + L" 2"); },
            "oversized manual numbers are reported as a coordinate error");
    check(wardogs::parse_manual_coordinate(L"x+12.345678 y-56.789012") ==
              Point{12.345678, -56.789012},
          "signed manual decimals retain their full precision");
    const std::string original_numeric_locale{std::setlocale(LC_NUMERIC, nullptr)};
    const char* russian_locale = std::setlocale(LC_NUMERIC, "Russian_Russia.1251");
    check(russian_locale != nullptr, "Windows Russian decimal locale is available for regression");
    if (russian_locale) {
        try {
            check(wardogs::parse_manual_coordinate(L"x12.34 y56.78") == Point{12.34, 56.78},
                  "manual decimal dots are independent of the process numeric locale");
            check(wardogs::parse_ocr_coordinate(L"x12.34 y56.78") == Point{12.34, 56.78},
                  "OCR decimal dots are independent of the process numeric locale");
        } catch (...) {
            check(false, "coordinates parse under the Russian decimal locale");
        }
    }
    std::setlocale(LC_NUMERIC, original_numeric_locale.c_str());

    check(wardogs::format_point({12, 34.5}) == L"x12, y34.5",
          "point formatting trims trailing zeroes");
    check(wardogs::format_distance_meters(5) == L"500 м",
          "game units convert to metres");
    check(wardogs::format_bearing(36.86989765) == L"36.9° NE",
          "bearing includes an eight-way compass direction");
    check(wardogs::format_bearing(359.96) == L"0.0° N",
          "bearing display wraps rounded north");
    check(wardogs::format_point({-0.0, -0.00001}) == L"x0, y0" &&
              wardogs::format_bearing(-0.0) == L"0.0° N" &&
              wardogs::format_distance_meters(-0.0) == L"0 м",
          "rounded and signed zeros display consistently without a negative sign");
    check(wardogs::format_raw_distance(5) ==
              L"Расстояние на карте: 5.0000 ед. · 1 ед. = 100 м",
          "raw distance remains available as secondary text");

    const auto f8 = wardogs::parse_hotkey(L"f8");
    check(f8.virtual_key == VK_F8 && f8.modifiers == MOD_NOREPEAT &&
              f8.display == L"F8",
          "function-key hotkeys are normalized");
    const auto chord = wardogs::parse_hotkey(L"Ctrl + Alt + q");
    check(chord.virtual_key == 'Q' &&
              chord.modifiers == (MOD_CONTROL | MOD_ALT | MOD_NOREPEAT) &&
              chord.display == L"Ctrl+Alt+Q",
          "modifier chords are normalized");
    rejects_with_message([] { wardogs::parse_hotkey(L""); },
                         "Горячая клавиша не может быть пустой",
                         "an empty hotkey reports a clear error");
    rejects([] { wardogs::parse_hotkey(L"Ctrl+Alt"); },
                         "a hotkey requires a non-modifier key");
    rejects([] { wardogs::parse_hotkey(L"F25"); },
            "unsupported function keys are rejected");

    const wardogs::AppSettings default_settings;
    check(default_settings.quick_target_hotkey == L"Alt+V",
          "quick target capture has an independent default hotkey");
    check(default_settings.pinned_card.unlock_hotkey == L"Ctrl+Alt+Q",
          "the pinned card has a default global unlock hotkey");
    const std::array unique_hotkeys{
        wardogs::parse_hotkey(L"F8"), wardogs::parse_hotkey(L"F9"),
        wardogs::parse_hotkey(L"F10"), wardogs::parse_hotkey(L"F11"),
        wardogs::parse_hotkey(L"Ctrl+Alt+Q")};
    try {
        wardogs::validate_unique_hotkeys(unique_hotkeys);
    } catch (const std::invalid_argument&) {
        check(false, "five distinct hotkeys are accepted");
    }
    const std::array duplicate_hotkeys{
        wardogs::parse_hotkey(L"F8"), wardogs::parse_hotkey(L"F9"),
        wardogs::parse_hotkey(L"F10"), wardogs::parse_hotkey(L"F11"),
        wardogs::parse_hotkey(L"F8")};
    rejects_with_message(
        [&] { wardogs::validate_unique_hotkeys(duplicate_hotkeys); },
        "Горячие клавиши не должны повторяться", "a duplicate quick target hotkey is rejected");

    wardogs::HotkeyMatcher matcher{unique_hotkeys};
    check(matcher.handle_key_event(VK_F8, true, 0) == 0,
          "a matching key-down selects the first hotkey");
    check(!matcher.handle_key_event(VK_F8, true, 0),
          "holding a hotkey does not trigger repeatedly");
    check(!matcher.handle_key_event(VK_F8, false, 0),
          "key-up only rearms the hotkey");
    check(matcher.handle_key_event(VK_F8, true, 0) == 0,
          "a hotkey triggers again after it is released");
    check(!matcher.handle_key_event(VK_F9, true, MOD_CONTROL),
          "extra modifiers do not trigger an unmodified hotkey");
    matcher.handle_key_event(VK_F9, false, MOD_CONTROL);

    const std::array modifier_hotkeys{wardogs::parse_hotkey(L"F8"),
                                      wardogs::parse_hotkey(L"Ctrl+F8")};
    wardogs::HotkeyMatcher modifier_matcher{modifier_hotkeys};
    check(modifier_matcher.handle_key_event(VK_F8, true, MOD_CONTROL) == 1,
          "the same key can coexist with a distinct modifier combination");
    modifier_matcher.handle_key_event(VK_F8, false, MOD_CONTROL);
    check(modifier_matcher.handle_key_event(VK_F8, true, 0) == 0,
          "the unmodified form remains independently available");

    const auto log_path =
        std::filesystem::temp_directory_path() / "wardogs_session_logger_test.log";
    {
        std::ofstream stale(log_path);
        stale << "previous session marker";
    }
    check(wardogs::initialize_session_log(log_path, "test-version"),
          "session logger opens a local file");
    wardogs::log_info("hotkey diagnostic marker");
    wardogs::shutdown_session_log();
    std::ifstream first_log(log_path);
    const std::string first_contents((std::istreambuf_iterator<char>(first_log)),
                                     std::istreambuf_iterator<char>());
    check(first_contents.find("previous session marker") == std::string::npos,
          "starting the logger replaces the previous run");
    check(first_contents.find("session.start version=test-version") !=
              std::string::npos &&
              first_contents.find("hotkey diagnostic marker") != std::string::npos &&
              first_contents.find("session.end") != std::string::npos,
          "session log contains lifecycle and diagnostic entries");
    first_log.close();
    check(wardogs::initialize_session_log(log_path, "next-version"),
          "session logger can start a later run");
    wardogs::shutdown_session_log();
    std::ifstream second_log(log_path);
    const std::string second_contents((std::istreambuf_iterator<char>(second_log)),
                                      std::istreambuf_iterator<char>());
    check(second_contents.find("hotkey diagnostic marker") == std::string::npos &&
              second_contents.find("version=next-version") != std::string::npos,
          "a later run replaces all earlier diagnostic lines");
    second_log.close();
    std::error_code remove_error;
    std::filesystem::remove(log_path, remove_error);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
