#include "wardogs/vehicle_ballistics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <numbers>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wardogs {
namespace {

using TableEntry = std::pair<double, double>;

// SPH-2 flat-ground observations from https://wardogs.t0ki.cn/js/data.js,
// retrieved 2026-09-11. Values are treated as sight milliradians.
constexpr std::array<TableEntry, 59> low_table{{
    TableEntry{1181, 20}, {1232, 30}, {1283, 40}, {1334, 50}, {1384, 60},
    {1433, 70}, {1482, 80}, {1529, 90}, {1576, 100}, {1622, 110},
    {1666, 120}, {1709, 130}, {1751, 140}, {1792, 150}, {1832, 160},
    {1870, 170}, {1907, 180}, {1944, 190}, {1979, 200}, {2014, 210},
    {2046, 220}, {2079, 230}, {2110, 240}, {2139, 250}, {2168, 260},
    {2196, 270}, {2223, 280}, {2249, 290}, {2273, 300}, {2296, 310},
    {2319, 320}, {2341, 330}, {2362, 340}, {2383, 350}, {2403, 360},
    {2422, 370}, {2439, 380}, {2456, 390}, {2471, 400}, {2485, 410},
    {2499, 420}, {2513, 430}, {2526, 440}, {2538, 450}, {2550, 460},
    {2561, 470}, {2570, 480}, {2579, 490}, {2586, 500}, {2593, 510},
    {2599, 520}, {2605, 530}, {2610, 540}, {2615, 550}, {2620, 560},
    {2623, 570}, {2626, 580}, {2628, 590}, {2629, 600},
}};

constexpr std::array<TableEntry, 80> high_table{{
    TableEntry{2629, 610}, {2629, 620}, {2628, 630}, {2626, 640},
    {2624, 650}, {2621, 660}, {2617, 670}, {2613, 680}, {2609, 690},
    {2604, 700}, {2599, 710}, {2592, 720}, {2584, 730}, {2576, 740},
    {2567, 750}, {2557, 760}, {2546, 770}, {2536, 780}, {2524, 790},
    {2513, 800}, {2501, 810}, {2488, 820}, {2474, 830}, {2460, 840},
    {2444, 850}, {2429, 860}, {2412, 870}, {2395, 880}, {2378, 890},
    {2360, 900}, {2342, 910}, {2323, 920}, {2303, 930}, {2282, 940},
    {2261, 950}, {2239, 960}, {2217, 970}, {2194, 980}, {2171, 990},
    {2147, 1000}, {2123, 1010}, {2098, 1020}, {2072, 1030},
    {2046, 1040}, {2019, 1050}, {1991, 1060}, {1963, 1070},
    {1934, 1080}, {1905, 1090}, {1875, 1100}, {1844, 1110},
    {1813, 1120}, {1782, 1130}, {1750, 1140}, {1717, 1150},
    {1684, 1160}, {1650, 1170}, {1616, 1180}, {1582, 1190},
    {1547, 1200}, {1512, 1210}, {1475, 1220}, {1438, 1230},
    {1401, 1240}, {1363, 1250}, {1324, 1260}, {1285, 1270},
    {1245, 1280}, {1205, 1290}, {1165, 1300}, {1124, 1310},
    {1083, 1320}, {1041, 1330}, {999, 1340}, {956, 1350}, {913, 1360},
    {869, 1370}, {825, 1380}, {780, 1390}, {735, 1400},
}};

std::span<const TableEntry> table_for(Arc arc) {
    switch (arc) {
        case Arc::low: return low_table;
        case Arc::high: return high_table;
    }
    throw std::invalid_argument("Выберите поддерживаемую траекторию SPH-2");
}

double interpolate(double value, std::span<const TableEntry> pairs,
                   bool input_is_distance, const char* label) {
    const auto input = [input_is_distance](const TableEntry& pair) {
        return input_is_distance ? pair.first : pair.second;
    };
    const auto output = [input_is_distance](const TableEntry& pair) {
        return input_is_distance ? pair.second : pair.first;
    };
    if (!std::isfinite(value) || value < input(pairs.front()) || value > input(pairs.back())) {
        throw std::invalid_argument(label);
    }
    // First equal entry preserves the observed high-arc maximum's duplicate
    // range semantics (2629 m -> 610 MIL), without scanning the entire table.
    const auto right = std::lower_bound(pairs.begin(), pairs.end(), value,
        [&](const TableEntry& entry, double wanted) { return input(entry) < wanted; });
    if (right == pairs.begin()) return output(*right);
    const auto& left = *std::prev(right);
    const double fraction = (value - input(left)) / (input(*right) - input(left));
    return output(left) + fraction * (output(*right) - output(left));
}

double dot(Vector3 first, Vector3 second) {
    return first[0] * second[0] + first[1] * second[1] + first[2] * second[2];
}

Vector3 cross(Vector3 first, Vector3 second) {
    return {first[1] * second[2] - first[2] * second[1],
            first[2] * second[0] - first[0] * second[2],
            first[0] * second[1] - first[1] * second[0]};
}

Vector3 normalize(Vector3 vector) {
    const double squared_length = dot(vector, vector);
    if (std::isfinite(squared_length) &&
        squared_length >= std::numeric_limits<double>::min()) {
        const double length = std::sqrt(squared_length);
        for (double& value : vector) value /= length;
        return vector;
    }
    const double scale = std::max({std::abs(vector[0]), std::abs(vector[1]),
                                   std::abs(vector[2])});
    if (!std::isfinite(scale) || scale == 0.0 ||
        !std::all_of(vector.begin(), vector.end(), [](double value) { return std::isfinite(value); })) {
        throw std::invalid_argument("Направления двух выстрелов не дают устойчивую калибровку");
    }
    // Direction observations have no meaningful magnitude. Scaling first
    // avoids overflow/underflow for otherwise valid finite directions.
    for (double& value : vector) value /= scale;
    const double length = std::sqrt(dot(vector, vector));
    for (double& value : vector) value /= length;
    return vector;
}

Matrix3 transpose(const Matrix3& matrix) {
    Matrix3 result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            result[row][column] = matrix[column][row];
    return result;
}

Vector3 matrix_vector(const Matrix3& matrix, Vector3 vector) {
    return {dot(matrix[0], vector), dot(matrix[1], vector),
            dot(matrix[2], vector)};
}

Vector3 checked_matrix_vector(const Matrix3& matrix, Vector3 vector) {
    for (double value : vector)
        if (!std::isfinite(value))
            throw std::invalid_argument("Направление должно содержать конечные числа");
    const auto result = matrix_vector(matrix, vector);
    for (double value : result)
        if (!std::isfinite(value))
            throw std::invalid_argument("Направление слишком большое для поворота платформы");
    return result;
}

Matrix3 matrix_multiply(const Matrix3& first, const Matrix3& second) {
    const auto columns = transpose(second);
    Matrix3 result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            result[row][column] = dot(first[row], columns[column]);
    return result;
}

Matrix3 pair_basis(Vector3 first, Vector3 second) {
    Vector3 sum{}, difference{};
    for (std::size_t index = 0; index < 3; ++index) {
        sum[index] = first[index] + second[index];
        difference[index] = first[index] - second[index];
    }
    if (std::hypot(sum[0], sum[1], sum[2]) < 1e-12 ||
        std::hypot(difference[0], difference[1], difference[2]) < 1e-12)
        throw std::invalid_argument("Направления двух выстрелов не дают устойчивую калибровку");
    const auto bisector = normalize(sum);
    difference = normalize(difference);
    const auto normal = normalize(cross(bisector, difference));
    return transpose(Matrix3{bisector, difference, normal});
}

std::pair<double, double> shot_geometry(Point base, Point point,
                                        const char* label) {
    const double dx = point.x - base.x;
    const double dy = point.y - base.y;
    const double distance_m = std::hypot(dx, dy) * 100.0;
    if (!std::isfinite(distance_m) || distance_m <= 0.0)
        throw std::invalid_argument(label);
    double bearing = std::atan2(dx, dy) * 180.0 / std::numbers::pi;
    if (bearing < 0.0) bearing += 360.0;
    if (bearing == 0.0 || bearing >= 360.0) bearing = 0.0;
    return {distance_m, bearing};
}

double bearing_separation(double first, double second) {
    double value = std::fmod(second - first + 540.0, 360.0) - 180.0;
    return std::abs(value);
}

double height_delta(Point base, Point point, const HeightLookup& lookup) {
    if (!lookup) return 0.0;
    const auto base_height = lookup(base);
    const auto point_height = lookup(point);
    if (!base_height) throw std::invalid_argument("Орудие находится за границами данных рельефа");
    if (!point_height) throw std::invalid_argument("Цель или попадание находится за границами данных рельефа");
    if (!std::isfinite(*base_height) || !std::isfinite(*point_height) ||
        !std::isfinite(*point_height - *base_height))
        throw std::invalid_argument("Высоты орудия и цели должны быть конечными числами");
    return *point_height - *base_height;
}

double trajectory_elevation(double horizontal_distance_m,
                            double height_delta_m, Arc arc) {
    (void)table_for(arc);
    if (!std::isfinite(horizontal_distance_m) || !std::isfinite(height_delta_m) ||
        horizontal_distance_m <= 0.0)
        throw std::invalid_argument("Горизонтальная дальность должна быть положительной, а высота — конечной");
    const double distance = horizontal_distance_m / sph2_maximum_range_m;
    const double height = height_delta_m / sph2_maximum_range_m;
    const double discriminant = 1.0 - distance * distance - 2.0 * height;
    constexpr double tolerance = 1e-7 /
        (sph2_maximum_range_m * sph2_maximum_range_m);
    if (!std::isfinite(discriminant) || discriminant < -tolerance)
        throw std::invalid_argument("Цель находится за пределами расчётной траектории");
    const double root = std::sqrt(std::max(0.0, discriminant));
    // Rationalizing 1-root keeps the low branch accurate near zero distance.
    // This is the same retained game approximation, not a new trajectory model.
    const double tangent = arc == Arc::low
        ? (distance + 2.0 * height / distance) / (1.0 + root)
        : (1.0 + root) / distance;
    if (tangent <= 0.0 || !std::isfinite(tangent))
        throw std::invalid_argument("Выбранная траектория не поддерживает такую разницу высот");
    return std::atan(tangent);
}

double flat_range_for_elevation(double elevation, Arc arc) {
    constexpr double tolerance = 1e-9;
    const double boundary = std::numbers::pi / 4.0;
    const bool valid = arc == Arc::low
        ? elevation >= -tolerance && elevation <= boundary + tolerance
        : elevation >= boundary - tolerance &&
              elevation <= std::numbers::pi / 2.0 + tolerance;
    if (!valid || !std::isfinite(elevation))
        throw std::invalid_argument("Наклон платформы выводит траекторию за поддерживаемые пределы");
    const double range = std::clamp(
        sph2_maximum_range_m * std::sin(2.0 * elevation), 0.0,
        sph2_maximum_range_m);
    // Solving a height-aware trajectory and converting its direction back to
    // sight range can move an exact supported endpoint by several ULPs. In
    // particular, a downhill shot at the 20-MIL low endpoint previously became
    // 1180.9999999999998 m and was rejected by the strict table lookup. Snap
    // only internally calculated roundoff; user-supplied ranges remain strict.
    // Preserve the high table's first duplicate-maximum semantics as well.
    constexpr double range_roundoff_m = 32.0 *
        std::numeric_limits<double>::epsilon() * sph2_maximum_range_m;
    const auto table = table_for(arc);
    const double minimum = arc == Arc::low
        ? table.front().first : table.back().first;
    for (const double endpoint : {minimum, sph2_maximum_range_m}) {
        if (std::abs(range - endpoint) <= range_roundoff_m) return endpoint;
    }
    return range;
}

double equivalent_flat_range(double horizontal_distance_m,
                             double height_delta_m, Arc arc) {
    return flat_range_for_elevation(
        trajectory_elevation(horizontal_distance_m, height_delta_m, arc), arc);
}

Vector3 direction_from_bearing_and_elevation(double bearing_deg,
                                              double elevation) {
    if (!std::isfinite(bearing_deg) || !std::isfinite(elevation))
        throw std::invalid_argument("Азимут и угол возвышения должны быть конечными числами");
    const double bearing = std::fmod(bearing_deg, 360.0) * std::numbers::pi / 180.0;
    const double horizontal = std::cos(elevation);
    return {horizontal * std::sin(bearing), horizontal * std::cos(bearing),
            std::sin(elevation)};
}

Vector3 solve_3x3(Matrix3 matrix, Vector3 right) {
    double augmented[3][4]{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column)
            augmented[row][column] = matrix[row][column];
        augmented[row][3] = right[row];
    }
    for (std::size_t column = 0; column < 3; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 3; ++row)
            if (std::abs(augmented[row][column]) >
                std::abs(augmented[pivot][column]))
                pivot = row;
        if (!std::isfinite(augmented[pivot][column]) ||
            std::abs(augmented[pivot][column]) < 1e-12)
            throw std::invalid_argument("Недостаточно разных направлений для уточнения калибровки");
        if (pivot != column)
            for (std::size_t item = column; item < 4; ++item)
                std::swap(augmented[column][item], augmented[pivot][item]);
        const double divisor = augmented[column][column];
        for (std::size_t item = column; item < 4; ++item)
            augmented[column][item] /= divisor;
        for (std::size_t row = 0; row < 3; ++row) {
            if (row == column) continue;
            const double factor = augmented[row][column];
            for (std::size_t item = column; item < 4; ++item)
                augmented[row][item] -= factor * augmented[column][item];
        }
    }
    return {augmented[0][3], augmented[1][3], augmented[2][3]};
}

Matrix3 rotation_from_vector(Vector3 vector) {
    const double angle = std::sqrt(dot(vector, vector));
    if (angle < 1e-14) return identity_rotation();
    for (double& value : vector) value /= angle;
    const auto [x, y, z] = vector;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const double complement = 1.0 - cosine;
    return {{{complement * x * x + cosine,
              complement * x * y - sine * z,
              complement * x * z + sine * y},
             {complement * y * x + sine * z,
              complement * y * y + cosine,
              complement * y * z - sine * x},
             {complement * z * x - sine * y,
              complement * z * y + sine * x,
              complement * z * z + cosine}}};
}

void accumulate_rotation_equation(Matrix3& normal, Vector3& right,
                                  Vector3 predicted, Vector3 observed,
                                  double weight) {
    if (weight <= 0.0) return;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            normal[row][column] += weight *
                ((row == column ? 1.0 : 0.0) -
                 predicted[row] * predicted[column]);
        }
    }
    const auto residual = cross(predicted, observed);
    for (std::size_t index = 0; index < 3; ++index)
        right[index] += weight * residual[index];
}

}  // namespace

Matrix3 identity_rotation() {
    return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

void validate_platform_calibration(const PlatformCalibration& calibration) {
    constexpr double tolerance = 1e-8;
    if (!std::isfinite(calibration.pair_angle_residual_deg) ||
        calibration.pair_angle_residual_deg < 0.0 ||
        calibration.pair_angle_residual_deg > 180.0)
        throw std::invalid_argument("Ошибка исходной калибровки должна быть конечной и от 0° до 180°");
    for (const auto& row : calibration.rotation)
        for (double value : row)
            if (!std::isfinite(value) || std::abs(value) > 1.0 + tolerance)
                throw std::invalid_argument("Калибровка платформы содержит неверную матрицу поворота");
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t other = row; other < 3; ++other) {
            const double expected = row == other ? 1.0 : 0.0;
            if (std::abs(dot(calibration.rotation[row], calibration.rotation[other]) - expected) > tolerance)
                throw std::invalid_argument("Калибровка платформы содержит неверную матрицу поворота");
        }
    }
    if (std::abs(dot(calibration.rotation[0], cross(calibration.rotation[1],
                                                   calibration.rotation[2])) - 1.0) > tolerance)
        throw std::invalid_argument("Калибровка платформы содержит отражение вместо поворота");
}

Vector3 PlatformCalibration::local_to_world(Vector3 direction) const {
    validate_platform_calibration(*this);
    return checked_matrix_vector(rotation, direction);
}

Vector3 PlatformCalibration::world_to_local(Vector3 direction) const {
    validate_platform_calibration(*this);
    return checked_matrix_vector(transpose(rotation), direction);
}

Vector3 direction_from_bearing_and_mil(double bearing_deg, double mil) {
    if (!std::isfinite(bearing_deg) || !std::isfinite(mil))
        throw std::invalid_argument("Азимут и значение MIL должны быть конечными числами");
    return direction_from_bearing_and_elevation(bearing_deg, mil / 1000.0);
}

Vector3 firing_direction(double bearing_deg, double mil, Arc arc) {
    const double equivalent_range = sph2_distance_for_mil(mil, arc);
    return direction_from_bearing_and_elevation(
        bearing_deg, trajectory_elevation(equivalent_range, 0.0, arc));
}

Vector3 impact_direction(Point base, Point impact, Arc arc,
                         double height_delta_m) {
    const auto geometry = shot_geometry(base, impact, "Попадание должно отличаться от позиции орудия");
    return direction_from_bearing_and_elevation(
        geometry.second,
        trajectory_elevation(geometry.first, height_delta_m, arc));
}

double sph2_mil_for_distance(double distance_m, Arc arc) {
    auto pairs = table_for(arc);
    if (arc == Arc::high) {
        static const auto ordered = [] {
            auto values = high_table;
            std::stable_sort(values.begin(), values.end(),
                         [](const auto& left, const auto& right) {
                             return left.first < right.first;
                         });
            return values;
        }();
        return interpolate(distance_m, ordered, true, "Дальность вне поддерживаемой таблицы SPH-2");
    }
    return interpolate(distance_m, pairs, true, "Дальность вне поддерживаемой таблицы SPH-2");
}

double sph2_world_mil_for_distance(double distance_m, Arc arc) {
    (void)table_for(arc);
    if (!std::isfinite(distance_m) || distance_m < 0.0 || distance_m > sph2_maximum_range_m)
        throw std::invalid_argument("Дальность вне поддерживаемой таблицы SPH-2");
    if (arc == Arc::low) {
        static const auto extended = [] {
            std::array<TableEntry, low_table.size() + 1> values{};
            values[0] = {0.0, 0.0};
            std::copy(low_table.begin(), low_table.end(), values.begin() + 1);
            return values;
        }();
        return interpolate(distance_m, extended, true, "Дальность вне поддерживаемой таблицы SPH-2");
    }
    static const auto extended = [] {
        std::array<TableEntry, high_table.size() + 1> values{};
        values[0] = {0.0, std::numbers::pi * 500.0};
        std::copy(high_table.begin(), high_table.end(), values.begin() + 1);
        std::stable_sort(values.begin(), values.end(),
                     [](const auto& left, const auto& right) {
                         return left.first < right.first;
                     });
        return values;
    }();
    return interpolate(distance_m, extended, true, "Дальность вне поддерживаемой таблицы SPH-2");
}

double sph2_distance_for_mil(double mil, Arc arc) {
    auto pairs = table_for(arc);
    return interpolate(mil, pairs, false, "Значение MIL вне поддерживаемой таблицы SPH-2");
}

double sph2_mil_for_trajectory(double horizontal_distance_m,
                               double height_delta_m, Arc arc,
                               bool extend_to_physical_endpoint) {
    if (!std::isfinite(height_delta_m))
        throw std::invalid_argument("Высотная поправка должна быть конечным числом");
    if (std::abs(height_delta_m) < 1e-9) {
        return extend_to_physical_endpoint
            ? sph2_world_mil_for_distance(horizontal_distance_m, arc)
            : sph2_mil_for_distance(horizontal_distance_m, arc);
    }
    const double equivalent =
        equivalent_flat_range(horizontal_distance_m, height_delta_m, arc);
    return extend_to_physical_endpoint
        ? sph2_world_mil_for_distance(equivalent, arc)
        : sph2_mil_for_distance(equivalent, arc);
}

double calibration_aim_separation_deg(Point base, Point first, Point second) {
    const auto first_aim = shot_geometry(base, first, "Первая цель должна отличаться от позиции орудия");
    const auto second_aim = shot_geometry(base, second, "Вторая цель должна отличаться от позиции орудия");
    return bearing_separation(first_aim.second, second_aim.second);
}

bool valid_calibration_aim_separation(double separation_deg) {
    // The endpoints are inclusive. atan2 and wrapping can move an exact
    // 30/150-degree pair by a few ULPs, especially across north.
    constexpr double separation_roundoff_deg = 1e-10;
    return std::isfinite(separation_deg) &&
        separation_deg >= minimum_calibration_separation_deg - separation_roundoff_deg &&
        separation_deg <= maximum_calibration_separation_deg + separation_roundoff_deg;
}

PlatformCalibration calibrate_platform(Point base, const CalibrationShot& first,
                                       const CalibrationShot& second,
                                       const HeightLookup& height_lookup) {
    const auto first_aim = shot_geometry(base, first.aim_point, "Первая цель должна отличаться от позиции орудия");
    const auto second_aim = shot_geometry(base, second.aim_point, "Вторая цель должна отличаться от позиции орудия");
    const double separation = calibration_aim_separation_deg(base, first.aim_point, second.aim_point);
    if (!valid_calibration_aim_separation(separation))
        throw std::invalid_argument("Разница азимутов двух целей должна быть от 30° до 150°");
    const auto first_impact =
        shot_geometry(base, first.impact_point, "Первое попадание должно отличаться от позиции орудия");
    const auto second_impact =
        shot_geometry(base, second.impact_point, "Второе попадание должно отличаться от позиции орудия");
    const double first_aim_height = height_delta(base, first.aim_point, height_lookup);
    const double second_aim_height = height_delta(base, second.aim_point, height_lookup);
    // Calibration must describe nominal shots the supported sight could
    // actually request. Observed impacts retain the wider physical domain:
    // an undershoot below the sight-table minimum is useful evidence.
    const auto check_nominal_shot = [](double range_m, double height_m,
                                       Arc arc, const char* label) {
        try {
            (void)sph2_mil_for_trajectory(range_m, height_m, arc);
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument(std::string(label) + ": " + error.what());
        }
    };
    check_nominal_shot(first_aim.first, first_aim_height, first.arc,
                       "Цель первого выстрела");
    check_nominal_shot(second_aim.first, second_aim_height, second.arc,
                       "Цель второго выстрела");
    const std::array nominal{
        direction_from_bearing_and_elevation(
            first_aim.second,
            trajectory_elevation(
                first_aim.first,
                first_aim_height, first.arc)),
        direction_from_bearing_and_elevation(
            second_aim.second,
            trajectory_elevation(
                second_aim.first,
                second_aim_height, second.arc))};
    const std::array observed{
        direction_from_bearing_and_elevation(
            first_impact.second,
            trajectory_elevation(
                first_impact.first,
                height_delta(base, first.impact_point, height_lookup),
                first.arc)),
        direction_from_bearing_and_elevation(
            second_impact.second,
            trajectory_elevation(
                second_impact.first,
                height_delta(base, second.impact_point, height_lookup),
                second.arc))};
    const double nominal_angle =
        std::acos(std::clamp(dot(nominal[0], nominal[1]), -1.0, 1.0));
    const double observed_angle =
        std::acos(std::clamp(dot(observed[0], observed[1]), -1.0, 1.0));
    const PlatformCalibration result{matrix_multiply(pair_basis(observed[0], observed[1]),
                            transpose(pair_basis(nominal[0], nominal[1]))),
            std::abs(observed_angle - nominal_angle) * 180.0 /
                std::numbers::pi};
    validate_platform_calibration(result);
    // Both bases remain proper rotations even when their pair angles differ.
    // Reject an incompatible initial pair rather than publishing a model that
    // already cannot explain the recorded impacts. Only accommodate roundoff
    // at the inclusive policy boundary; this is not a dispersion estimate.
    constexpr double residual_roundoff_deg = 1e-10;
    if (result.pair_angle_residual_deg >
        maximum_initial_calibration_residual_deg + residual_roundoff_deg) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2)
                << "Попадания не согласуются с общей поправкой: расхождение "
                << result.pair_angle_residual_deg << "° (допустимо до "
                << maximum_initial_calibration_residual_deg
                << "°). Проверьте азимут и MIL обоих выстрелов; после перемещения орудия повторите пристрелку.";
        throw std::invalid_argument(message.str());
    }
    return result;
}

PlatformCalibration refine_platform_calibration(
    const PlatformCalibration& prior,
    std::span<const DirectionObservation> observations,
    double prior_weight) {
    validate_platform_calibration(prior);
    if (!std::isfinite(prior_weight) || prior_weight <= 0.0)
        throw std::invalid_argument("Вес исходной калибровки должен быть конечным положительным числом");
    double weight_scale = prior_weight;
    std::vector<DirectionObservation> directions;
    directions.reserve(observations.size());
    for (const auto& observation : observations) {
        if (!std::isfinite(observation.weight) || observation.weight < 0.0)
            throw std::invalid_argument("Вес уточнения калибровки должен быть конечным неотрицательным числом");
        weight_scale = std::max(weight_scale, observation.weight);
        directions.push_back({normalize(observation.local_direction),
                              normalize(observation.world_direction),
                              observation.weight});
    }
    // Only relative weights matter. Normalize even moderate common scales:
    // otherwise the solver's pivot tolerance can reject a well-conditioned
    // system solely because all valid weights happen to be small.
    prior_weight /= weight_scale;
    for (auto& direction : directions) direction.weight /= weight_scale;
    Matrix3 rotation = prior.rotation;
    constexpr double robust_scale_rad = 7.0 * std::numbers::pi / 180.0;
    constexpr double maximum_step_rad = 2.0 * std::numbers::pi / 180.0;
    for (int iteration = 0; iteration < 24; ++iteration) {
        Matrix3 normal{};
        Vector3 right{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            Vector3 local{};
            local[axis] = 1.0;
            accumulate_rotation_equation(
                normal, right, matrix_vector(rotation, local),
                matrix_vector(prior.rotation, local), prior_weight);
        }
        for (const auto& observation : directions) {
            const auto predicted = matrix_vector(
                rotation, observation.local_direction);
            const auto observed = observation.world_direction;
            const double angle = std::acos(std::clamp(
                dot(predicted, observed), -1.0, 1.0));
            const double scaled = angle / robust_scale_rad;
            const double robust = 1.0 /
                (1.0 + scaled * scaled * scaled * scaled);
            accumulate_rotation_equation(
                normal, right, predicted, observed,
                observation.weight * robust);
        }
        auto step = solve_3x3(normal, right);
        double length = std::hypot(step[0], step[1], step[2]);
        if (!std::isfinite(length))
            throw std::invalid_argument("Направления выстрелов не дают конечное уточнение калибровки");
        if (length < 1e-10) break;
        if (length > maximum_step_rad) {
            for (double& value : step) value *= maximum_step_rad / length;
            length = maximum_step_rad;
        }
        rotation = matrix_multiply(rotation_from_vector(step), rotation);
        if (length < 1e-8) break;
    }
    const PlatformCalibration result{rotation, prior.pair_angle_residual_deg};
    validate_platform_calibration(result);
    return result;
}

FiringAngles required_firing_angles(Point base, Point point,
                                    const PlatformCalibration& calibration,
                                    Arc arc, double height_delta_m) {
    const auto geometry = shot_geometry(base, point, "Цель должна отличаться от позиции орудия");
    const double desired_elevation = trajectory_elevation(
        geometry.first, height_delta_m, arc);
    const auto desired_world = direction_from_bearing_and_elevation(
        geometry.second, desired_elevation);
    const auto corrected = calibration.world_to_local(desired_world);
    double bearing = std::atan2(corrected[0], corrected[1]) * 180.0 /
                     std::numbers::pi;
    if (bearing < 0.0) bearing += 360.0;
    if (bearing == 0.0 || bearing >= 360.0) bearing = 0.0;
    const double local_elevation =
        std::atan2(corrected[2], std::hypot(corrected[0], corrected[1]));
    const double equivalent_range = flat_range_for_elevation(local_elevation, arc);
    const double mil = sph2_world_mil_for_distance(equivalent_range, arc);
    return {bearing, mil};
}

CorrectedSolution corrected_solution(Point base, Point target,
                                     const PlatformCalibration& calibration,
                                     Arc arc, double height_delta_m) {
    const auto firing = required_firing_angles(
        base, target, calibration, arc, height_delta_m);
    return {arc, firing.bearing_deg,
            sph2_distance_for_mil(firing.mil, arc), firing.mil};
}

}  // namespace wardogs
