#include "wardogs/core.hpp"
#include "wardogs/ocr.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
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
struct Neighborhood {
    wardogs::Image image;
    wardogs::MapOcrSearchLayout layout;
};
Neighborhood neighborhood(const wardogs::Image& x, const wardogs::Image& y,
                          double scale = 1.0, int shift_x = 0, int shift_y = 0) {
    const auto pixels = [scale](int value) { return static_cast<int>(std::lround(value * scale)); };
    Neighborhood result;
    result.image.width = pixels(480); result.image.height = pixels(352);
    result.image.bgr.assign(static_cast<std::size_t>(result.image.width) * result.image.height * 3, 20);
    const int xx = pixels(212), xy = pixels(178), yx = pixels(180), yy = pixels(104);
    result.layout = {{xx, xy, xx + x.width, xy + x.height},
                     {yx, yy, yx + y.width, yy + y.height}, pixels(192), pixels(224), scale};
    // Retained native crop pixels are composed into a synthetic neighborhood;
    // this tests displacement, never claims an unobserved full game screenshot.
    paste(result.image, x, xx + shift_x, xy + shift_y);
    paste(result.image, y, yx + shift_x, yy + shift_y);
    return result;
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
        const auto normal_neighborhood = neighborhood(x, y);
        const auto neighborhood_result = ocr.recognize_map_neighborhood(
            normal_neighborhood.image, normal_neighborhood.layout);
        check(wardogs::parse_ocr_coordinate(neighborhood_result.text) == wardogs::Point{97.79, 111.36} &&
                  automatic_evidence(neighborhood_result),
              "one screenshot neighborhood retains the real complete semantic axes");
        check(neighborhood_result.map_x_bounds && neighborhood_result.map_y_bounds &&
                  !neighborhood_result.coordinate_bounds && !neighborhood_result.coordinate_is_chat_draft,
              "both map glyph bounds refer to the same screenshot without claiming chat provenance");
        auto native_font_layout = normal_neighborhood.layout;
        native_font_layout.x_prior = {219, 163, 405, 240};
        native_font_layout.y_prior = {176, 64, 379, 155};
        native_font_layout.scale = 4.0 / 3.0;
        const auto native_font_read = ocr.recognize_map_neighborhood(normal_neighborhood.image, native_font_layout);
        check(wardogs::parse_ocr_coordinate(native_font_read.text) == wardogs::Point{97.79, 111.36} &&
                  automatic_evidence(native_font_read),
              "native font pixels remain readable when 1440p preferred geometry uses a different client scale");
        auto shortened_digit = neighborhood(x, covered(y, {33, 23, 37, 30}, 20));
        shortened_digit.layout.cursor_x = 216;
        shortened_digit.layout.x_prior = {236, 178, 376, 236};
        shortened_digit.layout.y_prior = {204, 104, 356, 172};
        not_automatic([&] {
            const auto value = ocr.recognize_map_neighborhood(shortened_digit.image, shortened_digit.layout);
            if (automatic_evidence(value)) std::wcerr << L"Damaged cursor-lane digit=" << value.text << L'\n';
            return value;
        }, "a shortened real integer at the cursor lane cannot be discarded before glyph proof");
        wardogs::Image signed_x{x.width + 10, x.height, {}};
        signed_x.bgr.assign(static_cast<std::size_t>(signed_x.width) * signed_x.height * 3, 20);
        paste(signed_x, crop(x, 0, 0, 33, x.height), 0, 0);
        paste(signed_x, crop(x, 33, 0, x.width, x.height), 43, 0);
        // A retained native horizontal stroke is inserted as a synthetic minus.
        // A missed sign may cause refusal, never an automatic absolute value.
        paste(signed_x, crop(y, 60, 23, 68, 24), 34, 30);
        const auto signed_patch = neighborhood(signed_x, y);
        try {
            const auto signed_read = ocr.recognize_map_neighborhood(signed_patch.image, signed_patch.layout);
            check(!automatic_evidence(signed_read) ||
                      wardogs::parse_ocr_coordinate(signed_read.text) == wardogs::Point{-97.79, 111.36},
                  "a signed map axis cannot silently become a trusted positive absolute value");
        } catch (const std::invalid_argument&) {
            check(true, "a signed map axis cannot silently become a trusted positive absolute value");
        }
        for (const auto shift : {std::pair{80, 90}, std::pair{-140, 60}, std::pair{60, -65}}) {
            const auto moved = neighborhood(x, y, 1.0, shift.first, shift.second);
            const auto read = ocr.recognize_map_neighborhood(moved.image, moved.layout);
            check(wardogs::parse_ocr_coordinate(read.text) == wardogs::Point{97.79, 111.36} &&
                      automatic_evidence(read),
                  "displaced full X/Y labels are found beyond the preferred field without guessing numbers");
        }
        auto duplicate_neighborhood = normal_neighborhood;
        paste(duplicate_neighborhood.image, x, 40, 25);
        rejected([&] { ocr.recognize_map_neighborhood(duplicate_neighborhood.image, duplicate_neighborhood.layout); },
                 "preferred coordinates cannot hide another complete X row in the same neighborhood");
        auto incomplete_axis = normal_neighborhood;
        paste(incomplete_axis.image, crop(y, 16, 20, 55, 47), 40, 25);
        rejected([&] { ocr.recognize_map_neighborhood(incomplete_axis.image, incomplete_axis.layout); },
                 "an incomplete labeled axis cannot be discarded in favor of an easy preferred coordinate");
        auto unrelated_number = normal_neighborhood;
        paste(unrelated_number.image, crop(x, 32, 19, 82, 39), 35, 25);
        const auto unrelated_read = ocr.recognize_map_neighborhood(unrelated_number.image, unrelated_number.layout);
        check(wardogs::parse_ocr_coordinate(unrelated_read.text) == wardogs::Point{97.79, 111.36} &&
                  automatic_evidence(unrelated_read),
              "unlabeled nearby decimal is not a coordinate axis or a reason to replace labeled X/Y");
        auto missing_y = normal_neighborhood;
        missing_y.image = covered(missing_y.image, missing_y.layout.y_prior, 20);
        rejected([&] { ocr.recognize_map_neighborhood(missing_y.image, missing_y.layout); },
                 "neighborhood never fills a missing Y from another number or the prior position");
        auto damaged_y = neighborhood(x, covered(y, {50, 22, 61, 39}, 255));
        not_automatic([&] { return ocr.recognize_map_neighborhood(damaged_y.image, damaged_y.layout); },
                      "covered Y digit remains unsafe in the neighborhood semantic extractor");
        auto invalid_layout = normal_neighborhood.layout;
        invalid_layout.x_prior.left = -1;
        rejected([&] { ocr.recognize_map_neighborhood(normal_neighborhood.image, invalid_layout); },
                 "invalid neighborhood geometry cannot start inference");
        std::stop_source canceled;
        canceled.request_stop();
        try {
            ocr.recognize_map_neighborhood(normal_neighborhood.image, normal_neighborhood.layout, canceled.get_token());
            check(false, "canceled neighborhood request cannot return coordinate evidence");
        } catch (const std::runtime_error&) {
            check(true, "canceled neighborhood request cannot return coordinate evidence");
        }
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
            for (const double factor : {0.75, 1.0, 1.5, 2.0}) {
                const auto patch = neighborhood(scaled(source_x, factor), scaled(source_y, factor),
                                                source_x.width / 140.0 * factor);
                const auto searched = ocr.recognize_map_neighborhood(patch.image, patch.layout);
                if (!automatic_evidence(searched))
                    std::wcerr << L"Neighborhood " << live_case.y << L" scale " << factor << L": "
                               << searched.text << L" / " << searched.alternate_text << L" glyphs="
                               << searched.coordinate_glyph_count_matches << L" clipped="
                               << searched.coordinate_boundary_clipped << L'\n';
                check(wardogs::parse_ocr_coordinate(searched.text) == live_case.expected && automatic_evidence(searched),
                      "retained native map pixels survive scaled single-frame semantic neighborhood search");
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
        for (const auto& changed_y : {crop(y, 0, 0, 77, y.height),
                                      covered(y, {11, 23, 13, 38}, 255),
                                      covered(overlay_y, {37, 22, 50, 39}, 20),
                                      covered(overlay_y, {50, 22, 61, 39}, 255)}) {
            const auto damaged = neighborhood(x, changed_y);
            not_automatic([&] { return ocr.recognize_map_neighborhood(damaged.image, damaged.layout); },
                          "neighborhood cannot drop a full-height stem, recover a missing label or fill a hidden digit");
        }
        auto clipped_search = normal_neighborhood;
        clipped_search.image.bgr.assign(clipped_search.image.bgr.size(), 20);
        paste(clipped_search.image, x, 60, 178);
        paste(clipped_search.image, crop(y, 24, 0, y.width, y.height), 0, 104);
        clipped_search.layout = {{60, 178, 200, 236}, {0, 104, 128, 172}, 12, 224, 1.0};
        not_automatic([&] { return ocr.recognize_map_neighborhood(clipped_search.image, clipped_search.layout); },
                      "a Y stub cut by the actual screenshot boundary cannot become complete semantic evidence");
        for (const int digit_x : {35, 44, 53}) {
            auto joined_grid = normal_neighborhood;
            const int column = joined_grid.layout.y_prior.left + digit_x;
            // A one-pixel bright grid preserves the visible digit strokes.
            // Correct complete recovery is allowed; erased/covered digits in
            // the separate tests above must still remain nonautomatic.
            joined_grid.image = covered(joined_grid.image, {column, 0, column + 1, joined_grid.image.height}, 255);
            try {
                const auto value = ocr.recognize_map_neighborhood(joined_grid.image, joined_grid.layout);
                check(!automatic_evidence(value) ||
                          wardogs::parse_ocr_coordinate(value.text) == wardogs::Point{97.79, 111.36},
                      "a bright grid touching a digit cannot create a wrong trusted coordinate");
            } catch (const std::invalid_argument&) {
                check(true, "a bright grid touching a digit cannot create a wrong trusted coordinate");
            }
        }
        std::cout << "Map live OCR: " << checks << " checks, " << failures << " failures\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: live map OCR probe: " << error.what() << '\n'; return 1;
    }
}
