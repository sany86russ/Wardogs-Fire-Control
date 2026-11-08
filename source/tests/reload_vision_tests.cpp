#include "wardogs/reload_vision.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
using namespace wardogs;
int checks{}, failures{};
void check(bool value, const char* name) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

Image solid(int w, int h, std::array<std::uint8_t, 3> colour = {235, 235, 235}) {
    Image result{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 3)};
    for (std::size_t i = 0; i < result.bgr.size(); i += 3)
        std::copy(colour.begin(), colour.end(), result.bgr.begin() + static_cast<std::ptrdiff_t>(i));
    return result;
}

void put(Image& image, int x, int y, std::array<std::uint8_t, 3> colour) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) return;
    const auto i = (static_cast<std::size_t>(y) * image.width + x) * 3;
    std::copy(colour.begin(), colour.end(), image.bgr.begin() + static_cast<std::ptrdiff_t>(i));
}

Image resized(const Image& source, double scale) {
    Image result = solid(static_cast<int>(std::round(source.width * scale)),
                         static_cast<int>(std::round(source.height * scale)));
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            const auto i = (static_cast<std::size_t>(std::min(source.height - 1, static_cast<int>(y / scale))) * source.width +
                            std::min(source.width - 1, static_cast<int>(x / scale))) * 3;
            put(result, x, y, {source.bgr[i], source.bgr[i + 1], source.bgr[i + 2]});
        }
    }
    return result;
}

void paste(Image& destination, const Image& source, int left, int top) {
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const auto i = (static_cast<std::size_t>(y) * source.width + x) * 3;
            put(destination, left + x, top + y, {source.bgr[i], source.bgr[i + 1], source.bgr[i + 2]});
        }
    }
}

Image cropped(const Image& source, ImageRect rect) {
    auto result = solid(rect.right - rect.left, rect.bottom - rect.top);
    for (int y = 0; y < result.height; ++y)
        for (int x = 0; x < result.width; ++x) {
            const auto i = (static_cast<std::size_t>(rect.top + y) * source.width + rect.left + x) * 3;
            put(result, x, y, {source.bgr[i], source.bgr[i + 1], source.bgr[i + 2]});
        }
    return result;
}

bool expected(const std::optional<ReloadObservation>& observation,
              const std::array<ReloadArrow, 4>& arrows) {
    return observation && observation->arrows == arrows && observation->confidence >= 0.72;
}

constexpr std::array<ReloadArrow, 4> real_arrows{{
    {ReloadDirection::up, true}, {ReloadDirection::right, true},
    {ReloadDirection::down, false}, {ReloadDirection::down, false}}};

Image real_prefix(const Image& original, int prefix) {
    Image result = original;
    constexpr std::array<ImageRect, 4> boxes{{{127, 183, 145, 205}, {160, 184, 182, 201},
                                             {199, 181, 216, 202}, {234, 181, 251, 203}}};
    for (int arrow_index = 0; arrow_index < 4; ++arrow_index) {
        const auto& box = boxes[arrow_index];
        for (int y = box.top; y < box.bottom; ++y) {
            for (int x = box.left; x < box.right; ++x) {
                const auto i = (static_cast<std::size_t>(y) * original.width + x) * 3;
                const int b = original.bgr[i], g = original.bgr[i + 1], r = original.bgr[i + 2];
                const bool body = arrow_index < 2
                    ? g >= 80 && g > r * 1.2 && g > b * 1.3
                    : std::min({r, g, b}) >= 145 && std::max({r, g, b}) - std::min({r, g, b}) <= 45;
                if (body) put(result, x, y, arrow_index < prefix
                    ? std::array<std::uint8_t, 3>{35, 200, 105}
                    : std::array<std::uint8_t, 3>{215, 215, 215});
            }
        }
    }
    return result;
}

// The synthetic glyph is deliberately defined independently of the detector's
// normalized template: rasterize a polygon in its original upright orientation.
void arrow(Image& image, int cx, int cy, int size, ReloadArrow value) {
    const std::array<std::array<double, 2>, 7> polygon{{
        {0, -11}, {9, 0}, {4, 0}, {4, 11}, {-4, 11}, {-4, 0}, {-9, 0}}};
    const double scale = size / 22.0;
    for (int dy = -size; dy <= size; ++dy) {
        for (int dx = -size; dx <= size; ++dx) {
            double px = (dx + 0.5) / scale, py = (dy + 0.5) / scale;
            switch (value.direction) {
            case ReloadDirection::right: { const double old = px; px = py; py = -old; break; }
            case ReloadDirection::down: px = -px; py = -py; break;
            case ReloadDirection::left: { const double old = px; px = -py; py = old; break; }
            default: break;
            }
            bool inside = false;
            for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
                const auto& a = polygon[i]; const auto& b = polygon[j];
                if ((a[1] > py) != (b[1] > py) &&
                    px < (b[0] - a[0]) * (py - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
            }
            if (inside) put(image, cx + dx, cy + dy, value.accepted
                ? std::array<std::uint8_t, 3>{35, 200, 105}
                : std::array<std::uint8_t, 3>{215, 215, 215});
        }
    }
}

void band(Image& image, int x, int y, int size,
          const std::array<ReloadArrow, 4>& values, bool plate = true) {
    const int step = size * 8 / 5;
    if (plate) {
        for (int yy = y - size; yy <= y + size; ++yy)
            for (int xx = x - size; xx <= x + 3 * step + size; ++xx)
                put(image, xx, yy, {14, 14, 14});
    }
    for (int i = 0; i < 4; ++i) arrow(image, x + i * step, y, size, values[i]);
}

constexpr std::array<ReloadArrow, 4> live_arrows{{
    {ReloadDirection::left, false}, {ReloadDirection::right, false},
    {ReloadDirection::up, false}, {ReloadDirection::down, false}}};

Image replace_outside(Image source, ImageRect keep, int mode) {
    std::mt19937 random(20261108);
    for (int y = 0; y < source.height; ++y) for (int x = 0; x < source.width; ++x) {
        if (x >= keep.left && x < keep.right && y >= keep.top && y < keep.bottom) continue;
        std::array<std::uint8_t, 3> color;
        switch (mode) {
        case 0: color = {255, 255, 255}; break;
        case 1: color = {0, 0, 0}; break;
        case 2: color = {110, 160, 210}; break;
        default: color = {static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(random()),
                         static_cast<std::uint8_t>(random())}; break;
        }
        put(source, x, y, color);
    }
    return source;
}

void fixture_regressions() {
    const auto directory = std::filesystem::path(WARDOGS_TEST_SOURCE_DIR) / L"tests/fixtures/reload";
    const auto original = real_prefix(load_image_file(directory / L"user-sph2-arrow-crop.png"), 0);
    const auto live = load_image_file(directory / L"live-sph2-neutral-arrows.png");
    const auto strip = load_image_file(directory / L"reload-panel-down-up-up-left.png");
    const auto progress = load_image_file(directory / L"live-panel-progress-0.png");
    auto original_arrows = real_arrows;
    for (auto& value : original_arrows) value.accepted = false;
    constexpr std::array<ReloadArrow, 4> strip_arrows{{{ReloadDirection::down, false},
        {ReloadDirection::up, false}, {ReloadDirection::up, false}, {ReloadDirection::left, false}}};
    constexpr std::array<ReloadArrow, 4> progress_arrows{{{ReloadDirection::left, false},
        {ReloadDirection::down, false}, {ReloadDirection::right, false}, {ReloadDirection::right, false}}};
    const std::array fixtures{original, live, strip, progress};
    const std::array directions{original_arrows, live_arrows, strip_arrows, progress_arrows};
    const std::array regions{ImageRect{117, 175, 261, 211}, ImageRect{70, 228, 270, 276},
        ImageRect{5, 5, 206, 53}, ImageRect{9, 8, 212, 57}};
    for (std::size_t fixture = 0; fixture < fixtures.size(); ++fixture) {
        check(expected(detect_reload_sequence(fixtures[fixture]), directions[fixture]),
              "one real image supplies all four white arrow directions");
        for (int background = 0; background < 4; ++background)
            check(expected(detect_reload_sequence(replace_outside(fixtures[fixture], regions[fixture], background)), directions[fixture]),
                  "white, black, sky and random surrounding pixels do not determine arrow directions");
        for (const double scale : {0.75, 1.0, 1.25, 1.4, 1.5, 2.0, 3.0}) {
            const auto result = detect_reload_sequence(resized(fixtures[fixture], scale));
            if (!expected(result, directions[fixture])) std::cerr << "Glyph fixture=" << fixture << " scale=" << scale << '\n';
            check(expected(result, directions[fixture]), "real arrow shapes remain readable from 0.75 through 3x HUD scale");
        }
        check(expected(detect_reload_sequence(cropped(fixtures[fixture], regions[fixture])), directions[fixture]),
              "a manually selected arrow-only rectangle supplies all four directions");
    }
    for (const auto size : {std::array{1280, 720}, std::array{1920, 1080},
                           std::array{2560, 1440}, std::array{3840, 2160}, std::array{7680, 2160}}) {
        auto scene = solid(size[0], size[1]);
        paste(scene, strip, size[0] / 2 - strip.width / 2, size[1] / 2);
        const auto start = std::chrono::steady_clock::now();
        const auto result = detect_reload_sequence(scene);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        check(expected(result, strip_arrows), "full-client acquisition finds four arrows at HD, 4K and ultrawide resolutions");
        std::cout << "Full arrow search " << size[0] << 'x' << size[1] << ": " << elapsed << " ms\n";
    }
    auto ambiguous = solid(1000, 600);
    paste(ambiguous, strip, 100, 100); paste(ambiguous, strip, 600, 350);
    check(!detect_reload_sequence(ambiguous), "two valid arrow rows cannot be chosen arbitrarily");
}

void outlined_synthetic() {
    constexpr std::array<ReloadArrow, 4> values{{{ReloadDirection::up, false}, {ReloadDirection::right, false},
        {ReloadDirection::down, false}, {ReloadDirection::left, false}}};
    for (const auto background : {std::array<std::uint8_t, 3>{10, 10, 10}, {220, 220, 220}, {90, 130, 170}}) {
        auto scene = solid(800, 400, background);
        for (int index = 0; index < 4; ++index) {
            auto glyph = solid(90, 90, {0, 0, 0});
            arrow(glyph, 45, 45, 30, values[index]);
            // An explicit two-pixel dark outline separates a white UI glyph
            // from light-gray scene pixels; no detector template is reused.
            for (int y = 0; y < glyph.height; ++y) for (int x = 0; x < glyph.width; ++x) {
                const auto i = (static_cast<std::size_t>(y) * glyph.width + x) * 3;
                if (glyph.bgr[i] != 215) continue;
                for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx)
                    put(scene, 200 + index * 48 + x - 45 + dx, 180 + y - 45 + dy, {10, 10, 10});
            }
            for (int y = 0; y < glyph.height; ++y) for (int x = 0; x < glyph.width; ++x) {
                const auto i = (static_cast<std::size_t>(y) * glyph.width + x) * 3;
                if (glyph.bgr[i] == 215) put(scene, 200 + index * 48 + x - 45, 180 + y - 45, {215, 215, 215});
            }
        }
        check(expected(detect_reload_sequence(scene), values),
              "four independently outlined arrows work on light, dark and colored interiors without a frame");
    }

}

void synthetic_scenes() {
    constexpr std::array<ReloadArrow, 4> all_directions{{
        {ReloadDirection::left, false}, {ReloadDirection::down, false},
        {ReloadDirection::up, false}, {ReloadDirection::right, false}}};
    for (int size : {10, 14, 22, 35, 70, 130}) {
        auto scene = solid(1500, 500);
        band(scene, 250, 200, size, all_directions);
        const auto reading = detect_reload_sequence(scene);
        check(expected(reading, all_directions), "independent polygon rasterizer exercises all four directions and HUD scales");
    }
    for (int prefix = 0; prefix <= 4; ++prefix) {
        auto values = all_directions;
        for (int i = 0; i < prefix; ++i) values[i].accepted = true;
        auto scene = solid(800, 400);
        band(scene, 200, 180, 30, values);
        check(expected(detect_reload_sequence(scene), values), "all accepted-prefix lengths are preserved including completion");
    }
    {
        auto scene = solid(800, 400);
        auto values = all_directions; values[2].accepted = true;
        band(scene, 200, 180, 30, values);
        check(!detect_reload_sequence(scene), "green accepted arrow after a pending arrow is invalid");
    }
    {
        auto scene = solid(800, 400);
        band(scene, 200, 180, 30, all_directions, false);
        check(!detect_reload_sequence(scene), "unoutlined white glyphs merged into a bright background are not guessed");
    }
    {
        auto scene = solid(800, 400, {14, 14, 14});
        band(scene, 200, 180, 30, all_directions);
        arrow(scene, 200 + 4 * 48, 180, 30, {ReloadDirection::up, false});
        check(!detect_reload_sequence(scene), "longer same-line arrow rows cannot be shortened into a four-step prompt");
    }
    {
        auto scene = solid(800, 400, {14, 14, 14});
        band(scene, 200, 180, 30, all_directions);
        arrow(scene, 200 + 72, 180, 30, {ReloadDirection::up, false});
        check(!detect_reload_sequence(scene), "a fifth arrow inserted between aligned slots cannot be ignored");
    }
    {
        auto scene = solid(800, 400, {14, 14, 14});
        for (int i = 0; i < 4; ++i)
            arrow(scene, 150 + i * (i + 3) * 15, 180, 30, all_directions[i]);
        check(!detect_reload_sequence(scene), "irregularly spaced arrows do not form a reload strip");
    }
    {
        // O/I/X-style neutral icons have similar dimensions/occupancy but no
        // unambiguous arrow silhouette. They must fail before row grouping.
        auto scene = solid(800, 400, {14, 14, 14});
        for (int item = 0; item < 4; ++item) {
            for (int y = -15; y <= 15; ++y) {
                for (int x = -12; x <= 12; ++x) {
                    bool ink = false;
                    if (item == 0) ink = std::abs(x) >= 8 || std::abs(y) >= 11;
                    if (item == 1) ink = std::abs(x) <= 3 || std::abs(y) >= 11;
                    if (item == 2) ink = std::abs(x - y * 0.7) <= 3 || std::abs(x + y * 0.7) <= 3;
                    if (item == 3) ink = std::abs(y) <= 3 || std::abs(x) <= 3;
                    if (ink) put(scene, 200 + item * 48 + x, 180 + y, {215, 215, 215});
                }
            }
        }
        check(!detect_reload_sequence(scene), "text-like O/I/X and cross icons are rejected on a dark panel");
    }
    {
        auto scene = solid(800, 400);
        band(scene, 200, 180, 30, all_directions);
        band(scene, 200, 300, 30, all_directions);
        check(!detect_reload_sequence(scene), "two independently rasterized dark panels are ambiguous");
    }
    {
        auto scene = solid(800, 400);
        band(scene, 8, 180, 30, all_directions);
        check(!detect_reload_sequence(scene), "partially clipped first direction rejects the whole row");
    }
    {
        auto scene = solid(800, 400, {20, 20, 20});
        std::mt19937 random(20261108);
        for (int i = 0; i < 15000; ++i)
            put(scene, static_cast<int>(random() % 800), static_cast<int>(random() % 400), {220, 220, 220});
        check(!detect_reload_sequence(scene), "random high-contrast scene does not create a reload sequence");
    }
    check(!detect_reload_sequence(solid(800, 400)), "blank bright frame has no arrows");
    check(!detect_reload_sequence(solid(800, 400, {0, 0, 0})), "blank dark frame has no arrows");
}

void malformed() {
    check(!detect_reload_sequence(Image{}), "empty frame rejected");
    check(!detect_reload_sequence(Image{-1, 10, {}}), "negative dimensions rejected");
    check(!detect_reload_sequence(Image{10, 10, {1, 2, 3}}), "truncated pixel storage rejected");
    check(!detect_reload_sequence(Image{1, 1, {1, 2, 3, 4}}), "extra pixel storage rejected");
    check(!detect_reload_sequence(Image{16385, 1, {}}), "excessive width rejected before allocation");
    check(!detect_reload_sequence(Image{1, 8193, {}}), "excessive height rejected before allocation");
    check(!detect_reload_sequence(Image{8192, 8192, {}}), "pixel budget rejected before allocation");
}
}  // namespace

int main() {
    try { fixture_regressions(); outlined_synthetic(); synthetic_scenes(); malformed(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << checks << " reload vision checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
