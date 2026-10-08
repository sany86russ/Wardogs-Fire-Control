#include "wardogs/core.hpp"
#include "wardogs/ocr.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* name) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
bool automatic_evidence(const wardogs::OcrResult& result) {
    return result.map_axes_labeled && result.isolated_coordinate_pair &&
        result.coordinate_passes_agree && result.coordinate_glyph_count_matches &&
        !result.coordinate_boundary_clipped &&
        !wardogs::assess_ocr_result(result).requires_confirmation();
}
wardogs::Image crop(const wardogs::Image& source, int left, int top, int right, int bottom) {
    const int width = right - left, height = bottom - top;
    wardogs::Image result{width, height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3)};
    for (int y = 0; y < height; ++y)
        std::copy_n(source.bgr.data() + ((y + top) * source.width + left) * 3,
                    width * 3, result.bgr.data() + y * width * 3);
    return result;
}
void paste(wardogs::Image& destination, const wardogs::Image& source, int left, int top) {
    for (int y = 0; y < source.height; ++y)
        std::copy_n(source.bgr.data() + y * source.width * 3, source.width * 3,
                    destination.bgr.data() + ((y + top) * destination.width + left) * 3);
}
wardogs::Image scaled(const wardogs::Image& source, double factor) {
    const int width = static_cast<int>(std::lround(source.width * factor));
    const int height = static_cast<int>(std::lround(source.height * factor));
    wardogs::Image result{width, height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3)};
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const double sx = (x + 0.5) / factor - 0.5, sy = (y + 0.5) / factor - 0.5;
        const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, source.width - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, source.height - 1);
        const int x1 = std::min(x0 + 1, source.width - 1), y1 = std::min(y0 + 1, source.height - 1);
        const double fx = std::clamp(sx - x0, 0.0, 1.0), fy = std::clamp(sy - y0, 0.0, 1.0);
        for (int channel = 0; channel < 3; ++channel) {
            const auto pixel = [&](int xx, int yy) { return source.bgr[(yy * source.width + xx) * 3 + channel]; };
            const double a = pixel(x0, y0) * (1 - fx) + pixel(x1, y0) * fx;
            const double b = pixel(x0, y1) * (1 - fx) + pixel(x1, y1) * fx;
            result.bgr[(y * width + x) * 3 + channel] = static_cast<std::uint8_t>(std::lround(a * (1 - fy) + b * fy));
        }
    }
    return result;
}
wardogs::Image with_grid_edge(wardogs::Image image) {
    // The same full source text with a brighter map grid line crossing the
    // left crop edge. In X it also connects to the existing cursor bracket.
    for (int y = 0; y < image.height; ++y)
        std::fill_n(image.bgr.data() + static_cast<std::size_t>(y) * image.width * 3,
                    3, std::uint8_t{255});
    return image;
}
wardogs::Image covered(wardogs::Image image, wardogs::ImageRect bounds, std::uint8_t value) {
    for (int y = bounds.top; y < bounds.bottom; ++y)
        for (int x = bounds.left; x < bounds.right; ++x)
            std::fill_n(image.bgr.data() + (static_cast<std::size_t>(y) * image.width + x) * 3,
                        3, value);
    return image;
}
template<class Function> void rejected(Function operation, const char* name) {
    try { operation(); check(false, name); }
    catch (const std::invalid_argument&) { check(true, name); }
}
template<class Function> void not_automatic(Function operation, const char* name) {
    try { check(!automatic_evidence(operation()), name); }
    catch (const std::invalid_argument&) { check(true, name); }
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::cerr << "Expected model, X source crop, Y source crop\n"; return 2;
    }
    try {
        wardogs::RapidOcr ocr(argv[1]);
        const auto x = wardogs::load_image_file(argv[2]), y = wardogs::load_image_file(argv[3]);
        check(x.width == 140 && x.height == 58 && y.width == 152 && y.height == 68,
              "real source crops preserve exact original cursor-field geometry");
        const auto result = ocr.recognize_map_coordinates(x, y);
        check(wardogs::parse_ocr_coordinate(result.text) == wardogs::Point{97.79, 111.36},
              "real map X/Y labels survive instead of failing numeric-only parsing");
        check(wardogs::parse_ocr_coordinate(result.alternate_text) == wardogs::Point{97.79, 111.36},
              "independent crop pass keeps all repeated Y digits");
        check(automatic_evidence(result),
              "cursor-bracket fragment and partial PING caption do not corrupt full labeled coordinate evidence");
        check(!result.coordinate_is_chat_draft && !result.coordinate_bounds,
              "separate map-axis fields never masquerade as chat or one joint source rectangle");
        const auto grid_x = with_grid_edge(x), grid_y = with_grid_edge(y);
        for (const double scale : {0.75, 1.0, 1.5, 2.0}) {
            const auto grid_result = ocr.recognize_map_coordinates(scaled(grid_x, scale), scaled(grid_y, scale));
            if (!automatic_evidence(grid_result))
                std::wcerr << L"Grid map scale " << scale << L": " << grid_result.text << L" / "
                           << grid_result.alternate_text << L" labels=" << grid_result.map_axes_labeled
                           << L" glyphs=" << grid_result.coordinate_glyph_count_matches
                           << L" clipped=" << grid_result.coordinate_boundary_clipped << L'\n';
            check(wardogs::parse_ocr_coordinate(grid_result.text) == wardogs::Point{97.79, 111.36} &&
                      automatic_evidence(grid_result),
                  "bright map grid edge and joined cursor bracket do not masquerade as a cut-off coordinate glyph");
        }
        not_automatic([&] { return ocr.recognize_map_coordinates(crop(grid_x, 27, 0, grid_x.width, grid_x.height), grid_y); },
                      "map grid exclusion cannot restore a genuinely clipped X label");
        not_automatic([&] { return ocr.recognize_map_coordinates(crop(grid_x, 0, 0, 77, grid_x.height), grid_y); },
                      "map grid exclusion cannot hide a genuinely clipped fractional digit");
        rejected([&] { (void)ocr.recognize_map_coordinates(grid_y, grid_x); },
                 "map grid exclusion cannot convert a wrong-axis field into trusted coordinates");
        for (const double scale : {1.5, 2.0}) {
            const auto resized = ocr.recognize_map_coordinates(scaled(x, scale), scaled(y, scale));
            if (!automatic_evidence(resized))
                std::wcerr << L"Map scale " << scale << L": " << resized.text << L" / "
                           << resized.alternate_text << L" confidence=" << resized.confidence
                           << L" minimum=" << resized.minimum_confidence << L" labels="
                           << resized.map_axes_labeled << L" glyphs=" << resized.coordinate_glyph_count_matches
                           << L" clipped=" << resized.coordinate_boundary_clipped << L'\n';
            check(wardogs::parse_ocr_coordinate(resized.text) == wardogs::Point{97.79, 111.36} &&
                      automatic_evidence(resized),
                  "150/200-percent real map fields retain exact coordinates and all automatic guards");
        }
        const auto small_x = scaled(x, 0.75), small_y = scaled(y, 0.75);
        const auto small = ocr.recognize_map_coordinates(small_x, small_y);
        check(wardogs::parse_ocr_coordinate(small.text) == wardogs::Point{97.79, 111.36} &&
                  automatic_evidence(small),
              "tiny map Y and repeated digits recover complete source evidence through one bounded pixel replication");
        auto padded_x = x, padded_y = y;
        padded_x.bgr.assign(padded_x.bgr.size(), 20); padded_y.bgr.assign(padded_y.bgr.size(), 20);
        paste(padded_x, small_x, 0, 0); paste(padded_y, small_y, 0, 0);
        const auto padded_small = ocr.recognize_map_coordinates(padded_x, padded_y);
        check(wardogs::parse_ocr_coordinate(padded_small.text) == wardogs::Point{97.79, 111.36} &&
                  automatic_evidence(padded_small),
              "small text inside native-sized capture fields retains full axis and glyph guards");
        not_automatic([&] { return ocr.recognize_map_coordinates(crop(small_x, 20, 0, small_x.width, small_x.height), small_y); },
                      "small-font retry cannot restore a clipped X label from guessed axis order");
        not_automatic([&] { return ocr.recognize_map_coordinates(crop(small_x, 0, 0, 57, small_x.height), small_y); },
                      "small-font retry cannot accept a clipped fractional digit");
        rejected([&] { (void)ocr.recognize_map_coordinates(y, x); },
                 "swapping X/Y fields cannot silently assign the wrong axes");
        auto multiple = y;
        multiple.height = 140;
        multiple.bgr.assign(static_cast<std::size_t>(multiple.width) * multiple.height * 3, 20);
        paste(multiple, y, 0, 0); paste(multiple, y, 0, 72);
        rejected([&] { (void)ocr.recognize_map_coordinates(x, multiple); },
                 "a second complete numeric row cannot silently replace the current axis");
        auto incomplete = multiple;
        incomplete.bgr.assign(incomplete.bgr.size(), 20);
        paste(incomplete, y, 0, 0); paste(incomplete, crop(y, 16, 20, 69, 47), 16, 95);
        rejected([&] { (void)ocr.recognize_map_coordinates(x, incomplete); },
                 "an incomplete competing Y row cannot be discarded as a caption");
        not_automatic([&] { return ocr.recognize_map_coordinates(crop(x, 27, 0, x.width, x.height), y); },
                      "a clipped X label cannot become a trusted unlabeled coordinate");
        not_automatic([&] { return ocr.recognize_map_coordinates(crop(x, 0, 0, 77, x.height), y); },
                      "a clipped trailing coordinate digit cannot be automatically accepted");
        const auto bare_x = crop(x, 32, 19, 82, 39), bare_y = crop(y, 28, 22, 82, 45);
        const auto bare = ocr.recognize_map_coordinates(bare_x, bare_y);
        check(!bare.map_axes_labeled && !automatic_evidence(bare),
              "two bare decimals remain legacy manual evidence instead of arbitrary automatic game coordinates");
        const auto data = std::filesystem::path(argv[2]).parent_path();
        const auto fixture = [&](const wchar_t* name) { return wardogs::load_image_file((data / name).wstring()); };
        struct LiveCase { const wchar_t* x; const wchar_t* y; wardogs::Point expected; };
        const LiveCase live_cases[]{
            {L"map_live_cursor_lane_x.png", L"map_live_cursor_lane_y.png", {98.36, 108.82}},
            {L"map_live_player_overlay_x.png", L"map_live_player_overlay_y.png", {97.85, 109.12}},
            {L"map_live_player_overlay_retry_x.png", L"map_live_player_overlay_retry_y.png", {97.85, 109.12}},
            {L"map_live_ping_bracket_x.png", L"map_live_ping_bracket_y.png", {99.15, 108.04}}
        };
        for (const auto& live_case : live_cases) {
            const auto source_x = fixture(live_case.x), source_y = fixture(live_case.y);
            for (const double scale : {0.75, 1.0, 1.5, 2.0}) {
                const auto value = ocr.recognize_map_coordinates(scaled(source_x, scale), scaled(source_y, scale));
                if (!automatic_evidence(value))
                    std::wcerr << live_case.y << L" scale " << scale << L": " << value.text << L" / "
                               << value.alternate_text << L" glyphs=" << value.coordinate_glyph_count_matches
                               << L" clipped=" << value.coordinate_boundary_clipped << L'\n';
                check(wardogs::parse_ocr_coordinates(value.text) == std::vector<wardogs::Point>{live_case.expected} &&
                      wardogs::parse_ocr_coordinates(value.alternate_text) == std::vector<wardogs::Point>{live_case.expected} &&
                      automatic_evidence(value),
                      "actual map cursor line, player arrow and boundary ping bracket retain physical glyph proof at 75-200 percent");
            }
        }
        const auto overlay_x = fixture(L"map_live_player_overlay_x.png");
        const auto overlay_y = fixture(L"map_live_player_overlay_y.png");
        not_automatic([&] { return ocr.recognize_map_coordinates(overlay_x, crop(overlay_y, 24, 0, overlay_y.width, overlay_y.height)); },
                      "player-arrow baseline proof cannot restore a genuinely clipped Y label");
        not_automatic([&] { return ocr.recognize_map_coordinates(overlay_x, crop(overlay_y, 0, 0, 81, overlay_y.height)); },
                      "player-arrow baseline proof cannot conceal a cut fractional digit");
        for (const std::uint8_t value : {std::uint8_t{20}, std::uint8_t{255}})
            for (const wardogs::ImageRect bounds : {wardogs::ImageRect{37, 22, 50, 39}, wardogs::ImageRect{50, 22, 61, 39}})
                not_automatic([&] { return ocr.recognize_map_coordinates(overlay_x, covered(overlay_y, bounds, value)); },
                              "missing or solidly covered integer digit under player arrow cannot become trusted physical evidence");
        rejected([&] { (void)ocr.recognize_map_coordinates(overlay_y, overlay_x); },
                 "player-arrow recovery cannot convert swapped semantic axes into trusted coordinates");
        const auto bracket_x = fixture(L"map_live_ping_bracket_x.png");
        const auto bracket_y = fixture(L"map_live_ping_bracket_y.png");
        not_automatic([&] { return ocr.recognize_map_coordinates(bracket_x, crop(bracket_y, 24, 0, bracket_y.width, bracket_y.height)); },
                      "large ping bracket exclusion cannot recover a clipped semantic Y label");
        not_automatic([&] { return ocr.recognize_map_coordinates(bracket_x, crop(bracket_y, 0, 0, 86, bracket_y.height)); },
                      "large ping bracket exclusion cannot hide a clipped fractional digit");
        not_automatic([&] { return ocr.recognize_map_coordinates(x, covered(y, {11, 23, 13, 38}, 255)); },
                      "known cursor lane cannot discard an arbitrary full-font-height white stem");
        std::cout << "Map live OCR: " << checks << " checks, " << failures << " failures\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: live map OCR probe: " << error.what() << '\n'; return 1;
    }
}
