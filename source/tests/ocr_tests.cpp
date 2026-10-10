#include "wardogs/core.hpp"
#include "wardogs/ocr.hpp"
#include "wardogs/ocr_preprocessing.hpp"
#include "ocr_preprocessing_reference.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
void rejects(const std::function<void()>& operation, const char* message) {
    try {
        operation();
        check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

void check_resampling_boundaries() {
    using namespace wardogs;
    for (const auto size : std::array<std::pair<int, int>, 10>{{
             {1, 1}, {2, 2}, {7, 19}, {226, 47}, {320, 48},
             {512, 97}, {3600, 48}, {4096, 48}, {3, 100}, {16384, 256}}}) {
        const auto varied = ocr_preprocessing_reference::pixels(size.first, size.second);
        for (const int crop : {0, size.second > 2 ? 1 : 0}) {
            if (48.0 * size.first / (size.second - 2 * crop) > 4096) continue;
            std::vector<float> reference, actual;
            int reference_width = 0, actual_width = 0;
            ocr_preprocessing_reference::tensor(varied, reference, reference_width, crop, crop);
            actual.assign(reference.size(), 17.0F);
            detail::prepare_ocr_tensor(varied, actual, actual_width, crop, crop, {});
            check(reference_width == actual_width && reference.size() == actual.size() &&
                      std::memcmp(reference.data(), actual.data(), reference.size() * sizeof(float)) == 0,
                  "optimized bilinear tensor is bit-identical across enlargement, shrinkage, crop and width limits");
        }
    }
    // Different BGR corners make an unintended upper/left-neighbour blend
    // observable independently from neural recognition or text geometry.
    const Image corners{2, 2, {0, 40, 80, 255, 90, 100,
                              80, 255, 30, 170, 30, 255}};
    std::vector<float> tensor;
    int width = 0;
    detail::prepare_ocr_tensor(corners, tensor, width, 0, 0, {});
    check(width == 320 && tensor.size() == 3 * 48 * 320,
          "tiny OCR image preserves the model's minimum tensor width and planar BGR shape");
    const auto sample = [&](int x, int y, int channel) {
        return tensor[static_cast<std::size_t>(channel) * 48 * width + y * width + x];
    };
    const auto normalized = [](double pixel) { return pixel / 127.5 - 1.0; };
    for (int channel = 0; channel < 3; ++channel) {
        check(std::abs(sample(0, 0, channel) - normalized(corners.bgr[channel])) < 0.00001,
              "bilinear enlargement preserves the first source pixel at the upper-left boundary");
        check(std::abs(sample(47, 0, channel) - normalized(corners.bgr[3 + channel])) < 0.00001,
              "bilinear enlargement preserves the upper-right source pixel");
        check(std::abs(sample(0, 47, channel) - normalized(corners.bgr[6 + channel])) < 0.00001,
              "bilinear enlargement preserves the lower-left source pixel");
        check(std::abs(sample(47, 47, channel) - normalized(corners.bgr[9 + channel])) < 0.00001,
              "bilinear enlargement preserves the lower-right source pixel");
        const double fraction = (23.5 * 2.0 / 48.0) - 0.5;
        const double top = corners.bgr[channel] * (1.0 - fraction) +
                           corners.bgr[3 + channel] * fraction;
        const double bottom = corners.bgr[6 + channel] * (1.0 - fraction) +
                              corners.bgr[9 + channel] * fraction;
        check(std::abs(sample(23, 23, channel) - normalized(top * (1.0 - fraction) +
                                                         bottom * fraction)) < 0.00001,
              "interior samples retain bilinear interpolation instead of nearest-neighbour copying");
        check(sample(48, 0, channel) == 0 && sample(319, 47, channel) == 0,
              "unused OCR tensor columns retain the model's zero padding");
    }
    Image cropped{2, 4, std::vector<std::uint8_t>(2 * 4 * 3, 255)};
    std::copy(corners.bgr.begin(), corners.bgr.end(), cropped.bgr.begin() + 6);
    std::vector<float> cropped_tensor;
    int cropped_width = 0;
    detail::prepare_ocr_tensor(cropped, cropped_tensor, cropped_width, 1, 1, {});
    check(cropped_width == width && cropped_tensor == tensor,
          "vertical tightening clamps to the selected source rows without blending cropped-away pixels");
    rejects([&] {
        detail::prepare_ocr_tensor(corners, cropped_tensor, cropped_width,
                                  std::numeric_limits<int>::max(),
                                  std::numeric_limits<int>::max(), {});
    }, "extreme crop margins are rejected before signed addition can overflow");
}

void check_com_lifetime(const wardogs::Image& golden,
                        const std::filesystem::path& missing) {
    std::jthread fresh([&] {
        try {
            const auto reloaded = wardogs::load_image_file(WARDOGS_TEST_IMAGE);
            check(reloaded.bgr == golden.bgr, "COM cleanup does not change decoded BGR bytes");
            auto switch_to_sta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            check(SUCCEEDED(switch_to_sta), "successful WIC load leaves no owned MTA initialization");
            if (SUCCEEDED(switch_to_sta)) CoUninitialize();
            try {
                (void)wardogs::load_image_file(missing);
                check(false, "missing image must report a loader error");
            } catch (const std::runtime_error&) {
            }
            switch_to_sta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            check(SUCCEEDED(switch_to_sta), "failed WIC load also leaves no owned MTA initialization");
            if (SUCCEEDED(switch_to_sta)) CoUninitialize();
        } catch (const std::exception&) {
            check(false, "fresh-thread WIC lifetime probe completes");
        }
    });
    fresh.join();

    std::jthread existing_mta([&] {
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        check(SUCCEEDED(initialized), "caller can initialize its own MTA");
        if (FAILED(initialized)) return;
        try {
            (void)wardogs::load_image_file(WARDOGS_TEST_IMAGE);
            try {
                (void)wardogs::load_image_file(missing);
                check(false, "missing image fails inside an existing MTA");
            } catch (const std::runtime_error&) {
            }
        } catch (const std::exception&) {
            check(false, "WIC can use an existing MTA");
        }
        CoUninitialize();
        const auto switch_to_sta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        check(SUCCEEDED(switch_to_sta), "S_FALSE initialization counts are balanced on success and failure");
        if (SUCCEEDED(switch_to_sta)) CoUninitialize();
    });
    existing_mta.join();

    std::jthread existing_sta([&] {
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        check(SUCCEEDED(initialized), "caller can initialize its own STA");
        if (FAILED(initialized)) return;
        try {
            const auto reloaded = wardogs::load_image_file(WARDOGS_TEST_IMAGE);
            check(reloaded.bgr == golden.bgr, "WIC retains exact pixels in a caller-owned STA");
            try {
                (void)wardogs::load_image_file(missing);
                check(false, "missing image fails inside an existing STA");
            } catch (const std::runtime_error&) {
            }
            APTTYPE type{};
            APTTYPEQUALIFIER qualifier{};
            check(SUCCEEDED(CoGetApartmentType(&type, &qualifier)) &&
                      (type == APTTYPE_STA || type == APTTYPE_MAINSTA),
                  "RPC_E_CHANGED_MODE never uninitializes the caller's STA");
        } catch (const std::exception&) {
            check(false, "WIC can use an existing STA");
        }
        CoUninitialize();
    });
    existing_sta.join();
}

wardogs::Image image_crop(const wardogs::Image& image, wardogs::ImageRect bounds) {
    const int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    wardogs::Image result{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 3)};
    for (int y = 0; y < h; ++y)
        std::copy_n(image.bgr.data() + ((y + bounds.top) * image.width + bounds.left) * 3,
                    w * 3, result.bgr.data() + y * w * 3);
    return result;
}
void image_paste(wardogs::Image& target, const wardogs::Image& source, int left, int top) {
    for (int y = 0; y < source.height; ++y)
        std::copy_n(source.bgr.data() + y * source.width * 3, source.width * 3,
                    target.bgr.data() + ((y + top) * target.width + left) * 3);
}
wardogs::Image image_scaled(const wardogs::Image& image, double factor) {
    const int w = static_cast<int>(std::lround(image.width * factor));
    const int h = static_cast<int>(std::lround(image.height * factor));
    wardogs::Image result{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 3)};
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const double sx = (x + 0.5) / factor - 0.5, sy = (y + 0.5) / factor - 0.5;
        const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, image.width - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, image.height - 1);
        const int x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
        const double fx = std::clamp(sx - x0, 0.0, 1.0), fy = std::clamp(sy - y0, 0.0, 1.0);
        for (int c = 0; c < 3; ++c) {
            const auto pixel = [&](int px, int py) { return image.bgr[(py * image.width + px) * 3 + c]; };
            const double top = pixel(x0, y0) * (1 - fx) + pixel(x1, y0) * fx;
            const double bottom = pixel(x0, y1) * (1 - fx) + pixel(x1, y1) * fx;
            result.bgr[(y * w + x) * 3 + c] = static_cast<std::uint8_t>(std::lround(top * (1 - fy) + bottom * fy));
        }
    }
    return result;
}
bool intact_evidence(const wardogs::OcrResult& result) {
    return result.isolated_coordinate_pair && result.coordinate_passes_agree &&
           result.coordinate_glyph_count_matches && !result.coordinate_boundary_clipped &&
           !wardogs::assess_ocr_result(result).requires_confirmation();
}
void check_live_chat_drafts(wardogs::RapidOcr& ocr, const wardogs::Image& old_history) {
    using namespace wardogs;
    const auto data = std::filesystem::path(WARDOGS_TEST_IMAGE).parent_path();
    // Original 2560x1440 game pixels, retaining only the channel plate,
    // coordinate draft and arrow. Cases include the blinking edit caret.
    for (int fixture = 1; fixture <= 3; ++fixture) {
        const auto draft = load_image_file(data / (L"chat_draft_live_" + std::to_wstring(fixture) + L".png"));
        const Point expected = fixture == 3 ? Point{98.66, 110.47} : Point{98.74, 111.85};
        check(draft.width == 490 && draft.height == 55,
              "live chat fixtures preserve source pixels without unrelated game or personal UI");
        const auto verify = [&](const Image& image) {
            const auto result = ocr.recognize_chat(image);
            check(parse_ocr_coordinates(result.text) == std::vector<Point>{expected} &&
                      parse_ocr_coordinates(result.alternate_text) == std::vector<Point>{expected} &&
                      intact_evidence(result) && result.coordinate_is_chat_draft,
                  "live drafts retain leading 9, repeated 1 and caret exclusion with complete automatic evidence");
        };
        verify(draft);
        Image search{1280, 720, std::vector<std::uint8_t>(1280 * 720 * 3, 20)};
        image_paste(search, old_history, 20, 15);
        image_paste(search, draft, 35, 255);
        verify(search);
        for (const double scale : {0.75, 1.5, 2.0}) verify(image_scaled(draft, scale));

        // Whole-strip inference includes the channel plate, arrow and margins;
        // its weak/inconsistent guesses must never qualify for automatic use.
        const auto whole_strip = ocr.recognize(draft);
        check(!intact_evidence(whole_strip) &&
                  assess_ocr_result(whole_strip).requires_confirmation(),
              "raw whole-strip guesses cannot bypass localized draft provenance or quality checks");
    }
}
void check_real_chat_background(wardogs::RapidOcr& ocr, const wardogs::Image& old_history) {
    using namespace wardogs;
    const auto data = std::filesystem::path(WARDOGS_TEST_IMAGE).parent_path();
    const auto scene = load_image_file(data / L"chat_live_industrial_background.png");
    check(scene.width == 1280 && scene.height == 720,
          "live industrial fixture preserves the exact automatic top-left capture quarter");
    rejects([&] { (void)find_chat_text_lines(scene); },
            "industrial background exceeds the unchanged general segmentation row budget");
    for (const double scale : {0.75, 1.0, 1.5, 2.0}) {
        const auto result = ocr.recognize_chat(image_scaled(scene, scale));
        if (!intact_evidence(result))
            std::wcerr << L"Industrial draft scale " << scale << L": " << result.text << L" / "
                       << result.alternate_text << L" confidence=" << result.confidence
                       << L" minimum=" << result.minimum_confidence << L" glyphs="
                       << result.coordinate_glyph_count_matches << L" clipped="
                       << result.coordinate_boundary_clipped << L'\n';
        check(parse_ocr_coordinates(result.text) == std::vector<Point>{{97.93, 109.55}} &&
                  parse_ocr_coordinates(result.alternate_text) == std::vector<Point>{{97.93, 109.55}} &&
                  result.coordinate_is_chat_draft && intact_evidence(result),
              "draft-first segmentation reads all source digits despite noisy scenery without relaxing automatic evidence");
    }
    for (const int erase_left : {274, 203}) {
        auto incomplete = scene;
        image_paste(incomplete, old_history, 70, 80);
        for (int y = 263; y < 296; ++y)
            for (int x = erase_left; x < 347; ++x)
                std::fill_n(incomplete.bgr.data() + (y * incomplete.width + x) * 3,
                            3, std::uint8_t{20});
        try {
            const auto result = ocr.recognize_chat(incomplete);
            check(parse_ocr_coordinates(result.text).empty() && !intact_evidence(result) &&
                      result.coordinate_is_chat_draft,
                  "an incomplete or empty active draft on noisy scenery never falls back to older chat coordinates");
        } catch (const std::invalid_argument&) { }
    }
    auto unrelated_pair = scene;
    for (int y = 263; y < 296; ++y)
        for (int x = 203; x < 347; ++x)
            std::fill_n(unrelated_pair.bgr.data() + (y * unrelated_pair.width + x) * 3,
                        3, std::uint8_t{20});
    // A genuine empty caption plate A is followed farther right by a solid
    // no-caption block B and a complete older pair C inside A's search band.
    // Only A may establish freshness; B must not adopt C as its draft.
    for (int y = 267; y < 292; ++y)
        for (int x = 230; x < 305; ++x)
            std::fill_n(unrelated_pair.bgr.data() + (y * unrelated_pair.width + x) * 3,
                        3, std::uint8_t{255});
    const auto old_draft = load_image_file(data / L"chat_draft_real_1.png");
    image_paste(unrelated_pair, image_crop(old_draft, {166, 12, 300, 38}), 315, 263);
    try {
        const auto unrelated = ocr.recognize_chat(unrelated_pair);
        check(!unrelated.coordinate_is_chat_draft && !intact_evidence(unrelated),
              "an unverified white block inside the active plate band cannot adopt older complete coordinates as a fresh draft");
    } catch (const std::invalid_argument&) { }
    Image unverified_plate{800, 160, std::vector<std::uint8_t>(800 * 160 * 3, 20)};
    for (int y = 55; y < 79; ++y)
        for (int x = 30; x < 150; ++x)
            std::fill_n(unverified_plate.bgr.data() + (y * unverified_plate.width + x) * 3,
                        3, std::uint8_t{255});
    image_paste(unverified_plate, image_crop(old_draft, {166, 12, 300, 38}), 160, 50);
    const auto legacy_white = ocr.recognize_chat(unverified_plate);
    check(parse_ocr_coordinates(legacy_white.text) == std::vector<Point>{{98.48, 110.35}} &&
              !legacy_white.coordinate_is_chat_draft,
          "legacy white coordinates beside a captionless bright block remain readable without fabricated draft freshness");
    const auto small_scene = image_scaled(scene, 0.75);
    try {
        const auto clipped = ocr.recognize_chat(image_crop(small_scene, {0, 0, 250, small_scene.height}));
        check(!intact_evidence(clipped),
              "a small draft with a fractional digit cut by the source capture edge cannot be applied automatically");
    } catch (const std::invalid_argument&) { }
}
void check_live_chat_channels(wardogs::RapidOcr& ocr) {
    using namespace wardogs;
    const auto data = std::filesystem::path(WARDOGS_TEST_IMAGE).parent_path();
    const auto noisy_scene = load_image_file(data / L"chat_live_industrial_background.png");
    for (const auto* channel : {L"all", L"team", L"local", L"vehicle"}) {
        const auto strip = load_image_file(data / (L"chat_channel_live_" + std::wstring(channel) + L".png"));
        check(strip.width == 485 && strip.height == 55,
              "all four live channel fixtures preserve the exact source strip without resampling");
        auto scene = noisy_scene;
        image_paste(scene, strip, 40, 255);
        const std::array<const Image*, 2> sources{&strip, &scene};
        for (const auto* source : sources)
            for (const double scale : {0.75, 1.0, 1.5, 2.0}) {
                const auto result = ocr.recognize_chat(image_scaled(*source, scale));
                if (!intact_evidence(result) || !result.coordinate_is_chat_draft)
                    std::wcerr << L"Chat channel " << channel << L" scale " << scale << L": "
                               << result.text << L" / " << result.alternate_text << L" confidence="
                               << result.confidence << L" minimum=" << result.minimum_confidence
                               << L" draft=" << result.coordinate_is_chat_draft << L" glyphs="
                               << result.coordinate_glyph_count_matches << L" clipped="
                               << result.coordinate_boundary_clipped << L'\n';
                check(parse_ocr_coordinates(result.text) == std::vector<Point>{{97.96, 109.61}} &&
                          parse_ocr_coordinates(result.alternate_text) == std::vector<Point>{{97.96, 109.61}} &&
                          result.coordinate_is_chat_draft && intact_evidence(result),
                      "short ALL and TEAM/LOCAL/VEHICLE captions preserve complete automatic draft evidence at every tested scale and noisy background");
            }
    }
}
void check_bright_scenery_draft(wardogs::RapidOcr& ocr, const wardogs::Image& old_history) {
    using namespace wardogs;
    const auto data = std::filesystem::path(WARDOGS_TEST_IMAGE).parent_path();
    // Privacy crop from a lossy CU screenshot of the live SPH-2 session.
    // Sky/tree component rectangles overlap the separate coordinate glyphs;
    // they must not erase text belonging to the verified channel caption.
    const auto scene = load_image_file(data / L"chat_live_bright_scenery.png");
    check(scene.width == 640 && scene.height == 400,
          "bright-scene fixture retains its privacy-cropped screenshot dimensions");
    const auto strip = image_crop(scene, {43, 255, 519, 310});
    Image quarter{1280, 720, std::vector<std::uint8_t>(1280 * 720 * 3, 20)};
    image_paste(quarter, scene, 0, 0);
    image_paste(quarter, old_history, 70, 80);
    const std::vector<Point> expected{{94.16, 110.55}};
    std::wstring first_text, first_alternate;
    const std::array<const Image*, 3> sources{&scene, &strip, &quarter};
    for (const auto* source : sources) {
        for (const double scale : {0.75, 1.0, 1.5, 2.0}) {
            const auto result = ocr.recognize_chat(image_scaled(*source, scale));
            check(parse_ocr_coordinates(result.text) == expected &&
                      parse_ocr_coordinates(result.alternate_text) == expected &&
                      result.coordinate_is_chat_draft && result.isolated_coordinate_pair &&
                      result.coordinate_passes_agree && result.coordinate_glyph_count_matches &&
                      !result.coordinate_boundary_clipped,
                  "verified bright-scene draft survives surrounding component bounds, scaling and older history");
            // Resampling a lossy screenshot can weaken the caret score at
            // 150%. Locator improvement must preserve the review gate rather
            // than promote that uncertain character to automatic evidence.
            const auto assessment = assess_ocr_result(result);
            if (scale == 1.5 && source != &strip)
                check(result.minimum_confidence < 0.65F && assessment.low_confidence &&
                          assessment.requires_confirmation() && !intact_evidence(result),
                      "the known weak resampled caret remains review-only despite the locator correction");
            if (scale == 1.0) {
                check(intact_evidence(result),
                      "the live native bright draft retains complete automatic evidence");
                if (first_text.empty()) {
                    first_text = result.text;
                    first_alternate = result.alternate_text;
                } else {
                    check(result.text == first_text && result.alternate_text == first_alternate,
                          "full bright scene, tight strip and automatic quarter retain identical source coordinates");
                }
            }
        }
    }
    for (const int erase_left : {272, 202}) {
        auto incomplete = quarter;
        for (int y = 263; y < 296; ++y)
            for (int x = erase_left; x < 341; ++x)
                std::fill_n(incomplete.bgr.data() + (y * incomplete.width + x) * 3,
                            3, std::uint8_t{20});
        try {
            const auto result = ocr.recognize_chat(incomplete);
            check(parse_ocr_coordinates(result.text).empty() &&
                      parse_ocr_coordinates(result.alternate_text).empty() && !intact_evidence(result),
                  "a missing Y or empty bright-scene draft cannot borrow a complete older coordinate pair");
        } catch (const std::invalid_argument&) { }
    }
    auto unverified = quarter;
    for (int y = 263; y < 296; ++y)
        for (int x = 54; x < 201; ++x)
            std::fill_n(unverified.bgr.data() + (y * unverified.width + x) * 3,
                        3, std::uint8_t{20});
    try {
        const auto result = ocr.recognize_chat(unverified);
        check(!result.coordinate_is_chat_draft,
              "bright scenery alone cannot establish freshness after its channel caption is removed");
    } catch (const std::invalid_argument&) { }
}
void check_real_draft_and_map(wardogs::RapidOcr& ocr, const wardogs::Image& old_history) {
    using namespace wardogs;
    const auto data = std::filesystem::path(WARDOGS_TEST_IMAGE).parent_path();
    const auto first = load_image_file(data / L"chat_draft_real_1.png");
    const auto second = load_image_file(data / L"chat_draft_real_2.png");
    check(first.width == 478 && first.height == 47 && second.width == 476 && second.height == 47,
          "real screenshots retain only original chat pixels without unrelated personal UI");
    for (const auto* draft : {&first, &second}) {
        const auto detected = find_chat_text_lines(*draft);
        check(detected.size() == 1 && detected[0].left > 150 && detected[0].right < 310,
              "channel plate and send arrow are excluded while the leading 9 remains in the crop");
        const auto result = ocr.recognize_chat(*draft);
        check(parse_ocr_coordinates(result.text) == std::vector<Point>{{98.48, 110.35}} &&
                  parse_ocr_coordinates(result.alternate_text) == std::vector<Point>{{98.48, 110.35}} &&
                  intact_evidence(result) && result.coordinate_is_chat_draft,
              "both user screenshot drafts retain every digit with strong independent crop agreement");
        check(result.coordinate_bounds && result.coordinate_bounds->left > 160 &&
                  result.coordinate_bounds->right < 300,
              "coordinate provenance describes source ink rather than the entire chat strip");
    }
    for (double scale : {0.75, 1.5, 2.0}) {
        const auto result = ocr.recognize_chat(image_scaled(first, scale));
        if (!intact_evidence(result))
            std::wcerr << L"Draft scale " << scale << L": " << result.text << L" / "
                       << result.alternate_text << L" confidence=" << result.confidence
                       << L" minimum=" << result.minimum_confidence << L" glyphs="
                       << result.coordinate_glyph_count_matches << L" clipped="
                       << result.coordinate_boundary_clipped << L'\n';
        check(parse_ocr_coordinate(result.text) == Point{98.48, 110.35} &&
                  intact_evidence(result) && result.coordinate_is_chat_draft,
              "75/150/200-percent draft geometry preserves labels, repeated digits, plate provenance and quality");
    }
    Image scene{900, 300, std::vector<std::uint8_t>(900 * 300 * 3, 20)};
    image_paste(scene, old_history, 20, 15);
    image_paste(scene, first, 25, 170);
    const auto current = ocr.recognize_chat(scene);
    check(parse_ocr_coordinates(current.text) == std::vector<Point>{{98.48, 110.35}} &&
              current.line_count == 1 && current.coordinate_is_chat_draft && intact_evidence(current),
          "a visually verified active draft is selected independently from an older full chat pair");
    auto incomplete = first;
    for (int y = 15; y < 38; ++y)
        for (int x = 245; x < 300; ++x)
            std::fill_n(incomplete.bgr.data() + (y * incomplete.width + x) * 3, 3, std::uint8_t{20});
    image_paste(scene, incomplete, 25, 170);
    try {
        const auto partial = ocr.recognize_chat(scene);
        check(!intact_evidence(partial) && parse_ocr_coordinates(partial.text).empty() &&
                  partial.coordinate_is_chat_draft,
              "an incomplete fresh draft cannot be silently rescued by stale full chat history");
    } catch (const std::invalid_argument&) { }

    Image x_field{140, 58, std::vector<std::uint8_t>(140 * 58 * 3, 20)};
    Image y_field{152, 68, std::vector<std::uint8_t>(152 * 68 * 3, 20)};
    image_paste(x_field, image_crop(first, {176, 18, 224, 31}), 30, 20);
    image_paste(y_field, image_crop(first, {245, 18, 294, 31}), 30, 20);
    const auto map = ocr.recognize_map_coordinates(x_field, y_field);
    check(parse_ocr_coordinate(map.text) == Point{98.48, 110.35} && intact_evidence(map) &&
              !map.coordinate_is_chat_draft && !map.coordinate_bounds,
          "independent verified cursor fields read both full decimals without chat or fabricated joint bounds");
    const auto native_x = load_image_file(data / L"map_tooltip_axis_x.png");
    const auto native_y = load_image_file(data / L"map_tooltip_axis_y.png");
    const auto native_map = ocr.recognize_map_coordinates(native_x, native_y);
    check(parse_ocr_coordinate(native_map.text) == Point{99.51, 111.81} && intact_evidence(native_map),
          "native fixture's Segoe UI map tooltip preserves repeated 9/1 digits in separate axis fields");
    auto negative_x = x_field;
    for (int y = 26; y < 28; ++y)
        for (int x = 19; x < 26; ++x)
            std::fill_n(negative_x.bgr.data() + (y * negative_x.width + x) * 3, 3, std::uint8_t{240});
    const auto negative_map = ocr.recognize_map_coordinates(negative_x, y_field);
    check(parse_ocr_coordinate(negative_map.text) == Point{-98.48, 110.35} &&
              negative_map.coordinate_glyph_count_matches,
          "a detached short minus sign cannot be dropped while localizing a numeric axis");
    for (double scale : {0.75, 1.5, 2.0}) {
        const auto result = ocr.recognize_map_coordinates(image_scaled(x_field, scale), image_scaled(y_field, scale));
        if (!intact_evidence(result))
            std::wcerr << L"Cursor scale " << scale << L": " << result.text << L" / "
                       << result.alternate_text << L" confidence=" << result.confidence
                       << L" minimum=" << result.minimum_confidence << L" glyphs="
                       << result.coordinate_glyph_count_matches << L" clipped="
                       << result.coordinate_boundary_clipped << L'\n';
        check(parse_ocr_coordinate(result.text) == Point{98.48, 110.35} && intact_evidence(result),
              "scaled cursor fields preserve both numeric axes and automatic-application evidence");
    }
    auto multiple_numbers = y_field;
    image_paste(multiple_numbers, image_crop(first, {245, 18, 294, 31}), 30, 45);
    rejects([&] { (void)ocr.recognize_map_coordinates(x_field, multiple_numbers); },
            "multiple plausible numbers in either cursor field cannot be silently selected");
    auto competing_partial = y_field;
    image_paste(competing_partial, image_crop(first, {245, 18, 282, 31}), 30, 45);
    rejects([&] { (void)ocr.recognize_map_coordinates(x_field, competing_partial); },
            "a second incomplete numeric row taints the axis instead of silently promoting the complete one");
    const auto clipped_x = image_crop(x_field, {32, 0, x_field.width, x_field.height});
    try {
        const auto clipped = ocr.recognize_map_coordinates(clipped_x, y_field);
        check(!intact_evidence(clipped) && clipped.coordinate_boundary_clipped,
              "a clipped leading numeric glyph prevents automatic application despite model confidence");
    } catch (const std::invalid_argument&) { }
    Image edge_fragment{140, 58, std::vector<std::uint8_t>(140 * 58 * 3, 20)};
    image_paste(edge_fragment, image_crop(first, {187, 18, 224, 31}), 5, 20);
    std::fill_n(edge_fragment.bgr.data() + (25 * edge_fragment.width) * 3, 3, std::uint8_t{240});
    const auto fragment = ocr.recognize_map_coordinates(edge_fragment, y_field);
    check(parse_ocr_coordinate(fragment.text) == Point{8.48, 110.35} &&
              fragment.coordinate_boundary_clipped && !intact_evidence(fragment),
          "a discarded tiny leading-glyph remnant still taints an otherwise confident clipped number");
    auto missing_fraction = y_field;
    for (int y = 0; y < missing_fraction.height; ++y)
        for (int x = 70; x < missing_fraction.width; ++x)
            std::fill_n(missing_fraction.bgr.data() + (y * missing_fraction.width + x) * 3, 3, std::uint8_t{20});
    rejects([&] { (void)ocr.recognize_map_coordinates(x_field, missing_fraction); },
            "a missing final fractional digit is refused rather than completed from the other axis or chat");
    auto dark = x_field;
    for (auto& pixel : dark.bgr) if (pixel > 70) pixel = 70;
    rejects([&] { (void)ocr.recognize_map_coordinates(dark, y_field); },
            "low-contrast cursor fields cannot invent a confident pair");
    Image sparse_plate{500, 250, std::vector<std::uint8_t>(500 * 250 * 3, 20)};
    image_paste(sparse_plate, image_crop(first, {166, 18, 294, 35}), 200, 150);
    for (int y = 145; y < 170; ++y)
        for (int x = 50; x < 190; ++x)
            if (y == 145 || y == 169 || x == 50 || x == 189)
                std::fill_n(sparse_plate.bgr.data() + (y * sparse_plate.width + x) * 3, 3, std::uint8_t{240});
    const auto sparse = ocr.recognize_chat(sparse_plate);
    check(parse_ocr_coordinate(sparse.text) == Point{98.48, 110.35} && !sparse.coordinate_is_chat_draft,
          "a sparse wide UI outline cannot masquerade as an active input channel plate");
    Image pepper{1024, 1024, std::vector<std::uint8_t>(1024 * 1024 * 3, 20)};
    for (int y = 0; y < pepper.height; y += 2)
        for (int x = 0; x < pepper.width; x += 2)
            std::fill_n(pepper.bgr.data() + (y * pepper.width + x) * 3, 3, std::uint8_t{240});
    rejects([&] { (void)find_chat_text_lines(pepper); },
            "component work has a fixed bound for pixel noise before any OCR inference");
}

void write_oversized_bmp(const std::filesystem::path& path) {
    // A valid 20-megapixel 24-bit BMP, stored without a large temporary vector.
    // Metadata decoding succeeds; the loader must reject it before pixel allocation.
    constexpr std::uint32_t width = 5000;
    constexpr std::uint32_t height = 4000;
    constexpr std::uint32_t pixel_bytes = width * height * 3;
    std::array<unsigned char, 54> header{};
    header[0] = 'B';
    header[1] = 'M';
    const auto write = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index)
            header[offset + index] = static_cast<unsigned char>(value >> (8 * index));
    };
    write(2, static_cast<std::uint32_t>(header.size()) + pixel_bytes);
    write(10, static_cast<std::uint32_t>(header.size()));
    write(14, 40);
    write(18, width);
    write(22, height);
    header[26] = 1;
    header[28] = 24;
    write(34, pixel_bytes);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(header.data()), header.size());
    stream.seekp(header.size() + pixel_bytes - 1);
    stream.put('\0');
    if (!stream) throw std::runtime_error("Cannot create oversized BMP regression fixture");
}
}  // namespace

int main() {
    using namespace wardogs;
    check_resampling_boundaries();

    // blank, a, b, space. Consecutive equal classes collapse; blank separates.
    const std::vector<std::wstring> characters{L"blank", L"a", L"b", L" "};
    const std::vector<std::size_t> winners{1, 1, 0, 1, 2, 2, 0};
    std::vector<float> probabilities(winners.size() * characters.size(), 0.01F);
    for (std::size_t t = 0; t < winners.size(); ++t) {
        probabilities[t * characters.size() + winners[t]] = 0.97F;
    }
    const auto decoded = decode_ctc(probabilities, winners.size(), characters.size(),
                                    characters);
    check(decoded.text == L"aab", "CTC removes duplicates and blank tokens");
    check(std::abs(decoded.confidence - 0.97F) < 0.001F,
          "CTC confidence averages selected character scores");
    check(std::abs(decoded.minimum_confidence - 0.97F) < 0.001F,
          "CTC retains the weakest selected character score");
    rejects([&] { (void)decode_ctc({}, std::numeric_limits<std::size_t>::max(),
                                  characters.size(), characters); },
            "CTC dimensions cannot overflow a multiplication");
    rejects([&] { (void)decode_ctc({0.1F, 0.2F}, 1, 4, characters); },
            "truncated CTC output is rejected before indexing");
    rejects([&] { (void)decode_ctc({0.1F, 1.1F, 0.0F, 0.0F}, 1, 4, characters); },
            "CTC does not expose impossible recognition confidence");
    rejects([&] { (void)decode_ctc({0.1F, 0.9F,
                                  std::numeric_limits<float>::quiet_NaN(), 0.0F},
                                  1, 4, characters); },
            "a non-winning NaN cannot produce falsely certain text");
    rejects([&] { (void)decode_ctc({0.1F, 0.9F, -0.1F, 0.0F}, 1, 4, characters); },
            "a non-winning invalid probability is rejected");
    auto weak_probabilities = probabilities;
    weak_probabilities[characters.size() * 4 + 2] = 0.40F;
    const auto weak = decode_ctc(weak_probabilities, winners.size(), characters.size(), characters);
    check(std::abs(weak.minimum_confidence - 0.40F) < 0.001F,
          "a weak emitted glyph is preserved even when the other characters are strong");
    const auto blank = decode_ctc({0.99F, 0.0F, 0.0F, 0.0F}, 1, 4, characters);
    check(blank.text.empty() && blank.confidence == 0 && blank.minimum_confidence == 0,
          "all-blank output has no invented confidence");

    const auto unique = assess_ocr_result({L"x114.51, y191.81", 0.99F, 0.95F});
    check(unique.selected == Point{114.51, 191.81} && unique.candidates.size() == 1 &&
              unique.confidence_available && !unique.requires_confirmation(),
          "one strong coordinate remains directly usable");
    const auto history = assess_ocr_result({L"old x80.00,y80.00\nnew x84.00,y83.00", 0.99F, 0.95F});
    check(history.selected == Point{84, 83} && history.candidates.size() == 2 &&
              history.match_count == 2 && history.ambiguous && history.requires_confirmation(),
          "different old chat coordinates require confirmation without changing last-match selection");
    const auto duplicate = assess_ocr_result({L"x114.51,y191.81 xll4.5l,yI9l.8l", 0.99F, 0.95F});
    check(duplicate.match_count == 2 && duplicate.candidates.size() == 1 &&
              !duplicate.ambiguous && !duplicate.requires_confirmation(),
          "duplicate repaired coordinates do not create artificial ambiguity");
    const auto disagreement = assess_ocr_result({L"x99.67,y111.06", 0.99F, 0.95F,
                                                L"x99.67,y11.06"});
    check(disagreement.pass_disagreement && disagreement.ambiguous &&
              disagreement.requires_confirmation(),
          "conflicting repeated-digit passes are never hidden by their mean confidence");
    const auto agreement = assess_ocr_result({L"x99.67,y111.06", 0.99F, 0.95F,
                                             L"chat x99.67, y111.06"});
    check(!agreement.pass_disagreement && !agreement.requires_confirmation(),
          "irrelevant text differences do not imply coordinate disagreement");
    const auto weak_coordinate = assess_ocr_result({L"x99.67,y111.06", 0.96F, 0.40F});
    check(weak_coordinate.low_confidence && weak_coordinate.requires_confirmation(),
          "high mean confidence cannot conceal a weak digit");
    const auto unscored = assess_ocr_result({L"x114.51,y191.81", 0.0F});
    check(!unscored.confidence_available && !unscored.low_confidence &&
              !unscored.requires_confirmation(),
          "Windows OCR does not fabricate a score or reject a unique pair solely for no score");
    const auto multiple_lines = assess_ocr_result({L"chat\nx114.51,y191.81", 0.0F, 0.0F, {}, 2});
    check(multiple_lines.multiple_lines && multiple_lines.requires_confirmation(),
          "a multi-line Windows capture requires a user check even if one pair survived");
    check(!assess_ocr_result({L"Ammo: 120 Range: 500", 0.99F, 0.98F}).selected,
          "unrelated game numbers are never accepted as coordinates");
    // The default grammar used to backtrack through long overlapping digit
    // and whitespace repetitions before finding that no decimal exists.
    const auto parser_start = std::chrono::steady_clock::now();
    check(parse_ocr_coordinates(L"x" + std::wstring(8000, L'1') + L" y2.00").empty(),
          "long incomplete OCR numbers fail without invoking a backtracking regex");
    check(std::chrono::steady_clock::now() - parser_start < std::chrono::seconds(1),
          "malformed default OCR text is scanned within a bounded time");
    check(parse_ocr_coordinate(L"x + 1 2 . 3 4 ; y - 5 6 . 7 8") == Point{12.34, -56.78},
          "linear default parsing preserves signs and spaced glyphs");

    const auto image = load_image_file(WARDOGS_TEST_IMAGE);
    check(image.width == 226 && image.height == 47,
          "WIC loads a golden screenshot in physical pixels");
    check(image.bgr.size() == static_cast<std::size_t>(image.width * image.height * 3),
          "decoded screenshot is packed BGR");
    const auto temporary = std::filesystem::temp_directory_path() /
        (L"wardogs-wic-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    check_com_lifetime(image, temporary);
    const auto oversized = temporary.wstring() + L".bmp";
    write_oversized_bmp(oversized);
    rejects([&] { (void)load_image_file(oversized); },
            "real 20-megapixel BMP is rejected before allocating the BGR output");
    std::error_code cleanup_error;
    std::filesystem::remove(oversized, cleanup_error);
    check(!cleanup_error, "temporary oversized-image fixture is removed");

    RapidOcr ocr(WARDOGS_TEST_MODEL);
    rejects([&] { (void)ocr.recognize({0, 47, {}}); },
            "empty image is rejected before model inference");
    rejects([&] { (void)ocr.recognize({std::numeric_limits<int>::max(), 47, {}}); },
            "image size arithmetic cannot overflow before validation");
    rejects([&] { (void)ocr.recognize({226, 47, {}}); },
            "missing pixel data is rejected");
    rejects([&] { (void)ocr.recognize({4096, 1, std::vector<std::uint8_t>(4096 * 3)}); },
            "extremely wide capture cannot create a huge inference tensor");
    std::stop_source cancelled;
    cancelled.request_stop();
    try {
        (void)ocr.recognize(image, cancelled.get_token());
        check(false, "recognition honours cancellation before inference");
    } catch (const std::runtime_error&) {
    }
    check(ocr.character_count() == 18710,
          "model metadata supplies the full CTC character table");
    const auto result = ocr.recognize(image);
    check(result.text == L"x114.51, y191.81",
          "recognition-only model reads the game coordinate line");
    check(result.confidence > 0.95F, "golden screenshot confidence stays high");
    check(parse_ocr_coordinate(result.text) == Point{114.51, 191.81},
          "OCR result feeds the coordinate parser");
    check(assess_ocr_result(result).selected == Point{114.51, 191.81} &&
              !assess_ocr_result(result).requires_confirmation(),
          "the golden fixture passes the review gate without invented accuracy claims");

    const auto make_chat = [&](int count, bool white_text) {
        Image chat{800, std::max(160, count * 60), {}};
        chat.bgr.assign(static_cast<std::size_t>(chat.width) * chat.height * 3, 20);
        for (int row = 0; row < count; ++row)
            for (int y = 0; y < image.height; ++y)
                for (int x = 0; x < image.width; ++x) {
                    const auto input = static_cast<std::size_t>((y * image.width + x) * 3);
                    const auto output = static_cast<std::size_t>(
                        (((row * 60 + 5 + y) * chat.width) + 20 + x) * 3);
                    if (!white_text) std::copy_n(image.bgr.data() + input, 3, chat.bgr.data() + output);
                    else if (image.bgr[input + 1] >= image.bgr[input + 2] + 35 &&
                             image.bgr[input + 1] >= image.bgr[input] - 15)
                        std::fill_n(chat.bgr.data() + output, 3, std::uint8_t{240});
                }
        return chat;
    };
    const auto chat = make_chat(2, false);
    const auto chat_lines = find_chat_text_lines(chat);
    check(chat_lines.size() == 2 && chat_lines[0].top < chat_lines[1].top &&
              chat_lines[0].right - chat_lines[0].left < image.width,
          "green text is segmented in reading order while pale background blocks are excluded");
    const auto chat_result = ocr.recognize_chat(chat);
    check(chat_result.line_count == 2 &&
              parse_ocr_coordinates(chat_result.text) == std::vector<Point>{{114.51, 191.81}, {114.51, 191.81}} &&
              assess_ocr_result(chat_result).requires_confirmation(),
          "a large chat region recognizes separate coordinate rows and preserves review evidence");
    const auto white_chat = make_chat(2, true);
    check(find_chat_text_lines(white_chat).size() == 2,
          "bright neutral draft text is found on a dark chat background");
    const auto white_result = ocr.recognize_chat(white_chat);
    check(parse_ocr_coordinates(white_result.text) ==
              std::vector<Point>{{114.51, 191.81}, {114.51, 191.81}} &&
              assess_ocr_result(white_result).requires_confirmation(),
          "automatic OCR reads white draft coordinates as well as green marked coordinates");
    auto low_contrast = white_chat;
    for (auto& pixel : low_contrast.bgr) if (pixel == 240) pixel = 70;
    rejects([&] { (void)ocr.recognize_chat(low_contrast); },
            "low-contrast auto-search refuses a result instead of inventing a confident coordinate");
    rejects([&] { (void)find_chat_text_lines(make_chat(9, false)); },
            "noisy chat search refuses more than eight candidate lines instead of dropping history");
    rejects([&] { (void)ocr.recognize_chat({800, 200, std::vector<std::uint8_t>(800 * 200 * 3, 20)}); },
            "empty auto-search fails explicitly without spending model work on the whole scene");
    rejects([&] { (void)find_chat_text_lines({800, 200, {}}); },
            "automatic line detection validates BGR data before scanning pixels");

    Image doubled{image.width * 2, image.height * 2,
                  std::vector<std::uint8_t>(image.bgr.size() * 4)};
    for (int y = 0; y < doubled.height; ++y)
        for (int x = 0; x < doubled.width; ++x)
            std::copy_n(image.bgr.data() + ((y / 2) * image.width + x / 2) * 3,
                        3, doubled.bgr.data() + (y * doubled.width + x) * 3);
    check(parse_ocr_coordinate(ocr.recognize(doubled).text) == Point{114.51, 191.81},
          "200-percent physical-pixel scaling preserves the coordinate values");

    check_real_draft_and_map(ocr, image);
    check_live_chat_drafts(ocr, image);
    check_real_chat_background(ocr, image);
    check_live_chat_channels(ocr);
    check_bright_scenery_draft(ocr, image);

    std::exception_ptr concurrent_failures[2];
    OcrResult concurrent_results[2];
    std::jthread concurrent_first([&] {
        try { concurrent_results[0] = ocr.recognize(image); }
        catch (...) { concurrent_failures[0] = std::current_exception(); }
    });
    std::jthread concurrent_second([&] {
        try { concurrent_results[1] = ocr.recognize(doubled); }
        catch (...) { concurrent_failures[1] = std::current_exception(); }
    });
    concurrent_first.join();
    concurrent_second.join();
    for (int index = 0; index < 2; ++index) {
        if (concurrent_failures[index]) std::rethrow_exception(concurrent_failures[index]);
        check(parse_ocr_coordinate(concurrent_results[index].text) == Point{114.51, 191.81},
              "concurrent callers cannot overwrite the reusable ONNX input buffer");
    }
    std::stop_source active_stop;
    std::promise<void> active_started;
    std::exception_ptr active_failure;
    const Image wide_blank{4096, 96, std::vector<std::uint8_t>(4096 * 96 * 3, 255)};
    std::jthread active([&] {
        active_started.set_value();
        try { (void)ocr.recognize(wide_blank, active_stop.get_token()); }
        catch (...) { active_failure = std::current_exception(); }
    });
    active_started.get_future().wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto cancellation_start = std::chrono::steady_clock::now();
    active_stop.request_stop();
    active.join();
    check(std::chrono::steady_clock::now() - cancellation_start < std::chrono::seconds(1),
          "active ONNX cancellation does not wait for the next normal inference");
    if (active_failure) {
        try { std::rethrow_exception(active_failure); }
        catch (const std::runtime_error& error) {
            check(std::string_view(error.what()) == "Распознавание отменено",
                  "interrupted ONNX reports a clear cancellation reason");
        }
    }
    std::cout << "ONNX active cancellation: "
              << (active_failure ? "cancelled" : "completed before stop") << '\n';
    check(parse_ocr_coordinate(ocr.recognize(image).text) == Point{114.51, 191.81},
          "new RunOptions recover immediately after an active ONNX cancellation");

    const auto extra_text_image = load_image_file(WARDOGS_TEST_EXTRA_TEXT_IMAGE);
    check(extra_text_image.width == 390 && extra_text_image.height == 58,
          "extra-text regression screenshot loads at its original size");
    const auto extra_text = ocr.recognize(extra_text_image);
    try {
        const auto extra_point = parse_ocr_coordinate(extra_text.text);
        check(std::abs(extra_point.x - 120.32) < 0.001 &&
                  std::abs(extra_point.y - 84.21) < 0.001,
              "extra multilingual text does not change the coordinate values");
    } catch (const std::invalid_argument&) {
        check(false, "direct OCR output keeps an embedded coordinate parseable");
    }

    const auto repeated_image = load_image_file(WARDOGS_TEST_REPEATED_IMAGE);
    check(repeated_image.width == 150 && repeated_image.height == 43,
          "repeated-digit regression keeps the loose vertical crop");
    const auto repeated = ocr.recognize(repeated_image);
    check(repeated.text.find(L"x99.67") != std::wstring::npos,
          "loose vertical crop preserves the x coordinate");
    check(repeated.text.find(L"y111.06") != std::wstring::npos,
          "recognition preserves three repeated 1 digits");
    try {
        check(parse_ocr_coordinate(repeated.text) == Point{99.67, 111.06},
              "repeated-digit OCR output remains parseable");
    } catch (const std::invalid_argument&) {
        check(false, "repeated-digit OCR output contains a complete coordinate pair");
    }
    auto mixed_chat = chat;
    std::fill(mixed_chat.bgr.begin() + mixed_chat.width * 60 * 3,
              mixed_chat.bgr.end(), std::uint8_t{20});
    for (int y = 0; y < repeated_image.height; ++y)
        std::copy_n(repeated_image.bgr.data() + y * repeated_image.width * 3,
                    repeated_image.width * 3,
                    mixed_chat.bgr.data() + ((y + 65) * mixed_chat.width + 20) * 3);
    const auto mixed_result = ocr.recognize_chat(mixed_chat);
    const auto mixed_assessment = assess_ocr_result(mixed_result);
    check(parse_ocr_coordinates(mixed_result.text) ==
              std::vector<Point>{{114.51, 191.81}, {99.67, 111.06}} &&
              mixed_assessment.selected == Point{99.67, 111.06} &&
              mixed_assessment.candidates.size() == 2 && mixed_assessment.requires_confirmation(),
          "different chat history pairs survive line recognition and cannot be silently auto-applied");

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All OCR tests passed\n";
    return 0;
}
