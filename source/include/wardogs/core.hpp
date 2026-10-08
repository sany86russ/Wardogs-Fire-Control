#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wardogs {

struct Point {
    double x{};
    double y{};

    bool operator==(const Point&) const = default;
};

struct Shot {
    Point base;
    Point target;
    double dx{};
    double dy{};
    double distance{};  // Map coordinate units; multiply by 100 for metres.
    double angle{};
};

inline constexpr std::wstring_view default_ocr_coordinate_pattern =
    LR"(x\s*[:=]?\s*([-+]?\s*[0-9liI|Oo](?:[0-9liI|Oo\s]*[0-9liI|Oo])?\s*\.\s*[0-9liI|Oo]\s*[0-9liI|Oo](?:\s*[0-9liI|Oo])*)[\s,，;；:*&#.·]*y\s*[:=]?\s*([-+]?\s*[0-9liI|Oo](?:[0-9liI|Oo\s]*[0-9liI|Oo])?\s*\.\s*[0-9liI|Oo]\s*[0-9liI|Oo](?:\s*[0-9liI|Oo])*))";

Shot calculate_shot(Point base, Point target);
[[nodiscard]] double mortar_mil_for_distance(double distance_m);
Point parse_ocr_coordinate(std::wstring_view text,
                           std::wstring_view pattern = default_ocr_coordinate_pattern);
// All complete pairs in reading order; an empty result means no match.
// The single-pair API deliberately retains the last-pair behavior.
std::vector<Point> parse_ocr_coordinates(
    std::wstring_view text,
    std::wstring_view pattern = default_ocr_coordinate_pattern);
// Custom patterns use a deliberately restricted, non-branching grammar.
// Call during settings validation as well as before OCR recognition.
void validate_ocr_coordinate_pattern(std::wstring_view pattern);
// Repair only the known ANSI serialization of the built-in template.
// User templates are returned unchanged and retain restricted validation.
std::wstring normalize_ocr_coordinate_pattern(std::wstring_view pattern);
Point parse_manual_coordinate(std::wstring_view text);
std::wstring format_point(Point point);
std::wstring format_distance_meters(double distance);
std::wstring format_bearing(double angle);
std::wstring format_raw_distance(double distance);

}  // namespace wardogs
