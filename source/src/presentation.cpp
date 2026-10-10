#include "wardogs/core.hpp"
#include "wardogs/presentation.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace wardogs {
namespace {

std::wstring trimmed(double value) {
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(4) << value;
    std::wstring result = stream.str();
    while (!result.empty() && result.back() == L'0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == L'.') {
        result.pop_back();
    }
    if (result == L"-0") result = L"0";
    return result;
}

}  // namespace

double displayed_bearing_deg(double bearing_deg) {
    if (!std::isfinite(bearing_deg))
        throw std::invalid_argument("Азимут должен быть конечным числом");
    double normalized = std::fmod(bearing_deg, 360.0);
    if (normalized < 0.0) normalized += 360.0;
    const double rounded = std::round(normalized * 10.0) / 10.0;
    return rounded == 0.0 || rounded >= 360.0 ? 0.0 : rounded;
}

DisplayedFiringCommand displayed_firing_command(
    double bearing_deg, double mil, Arc arc) {
    const double bearing = displayed_bearing_deg(bearing_deg);
    (void)sph2_distance_for_mil(mil, arc);
    const double commanded_mil = std::round(mil);
    const double table_distance = sph2_distance_for_mil(commanded_mil, arc);
    return {arc, bearing, commanded_mil, table_distance};
}

DisplayedFiringCommand displayed_firing_command(const CorrectedSolution& solution) {
    return displayed_firing_command(solution.bearing_deg, solution.mil, solution.arc);
}

std::wstring format_point(Point point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return L"—";
    return L"x" + trimmed(point.x) + L", y" + trimmed(point.y);
}

std::wstring format_distance_meters(double distance) {
    if (!std::isfinite(distance) || distance < 0 || !std::isfinite(distance * 100.0))
        return L"—";
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(0) << (distance == 0.0 ? 0.0 : distance * 100.0) << L" м";
    return stream.str();
}

std::wstring format_bearing(double angle) {
    if (!std::isfinite(angle)) return L"—";
    static constexpr std::array<std::wstring_view, 8> directions{
        L"N", L"NE", L"E", L"SE", L"S", L"SW", L"W", L"NW"};
    const double rounded = displayed_bearing_deg(angle);
    const auto sector = static_cast<std::size_t>(
        std::floor((rounded + 22.5) / 45.0)) % directions.size();
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(1) << rounded << L"° "
           << directions[sector];
    return stream.str();
}

std::wstring format_raw_distance(double distance) {
    if (!std::isfinite(distance) || distance < 0) return L"—";
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << L"Расстояние на карте: " << std::fixed << std::setprecision(4) << (distance == 0.0 ? 0.0 : distance)
           << L" ед. · 1 ед. = 100 м";
    return stream.str();
}

}  // namespace wardogs
