#include "wardogs/sph_hud_ocr.hpp"

#include <Windows.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/base.h>

namespace {

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("OCR text is too large for UTF-8 conversion");
    const auto length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
                                       nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("OCR text is not valid UTF-16");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
                            result.data(), size, nullptr, nullptr) != size)
        throw std::runtime_error("Cannot encode OCR text as UTF-8");
    return result;
}

std::string json_string(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result(1, '"');
    for (const unsigned char ch : text) {
        switch (ch) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    result += "\\u00";
                    result += hex[ch >> 4]; result += hex[ch & 0x0f];
                } else result += static_cast<char>(ch);
        }
    }
    result += '"';
    return result;
}

void json_number(std::ostream& output, double number) {
    if (std::isfinite(number)) output << number;
    else output << "null";
}

void json_instrument(std::ostream& output, const wardogs::SphHudInstrumentDiagnostics& instrument) {
    output << "{\"attempted_regions\":" << instrument.attempted_regions
              << ",\"tokens\":" << instrument.tokens
              << ",\"admission_rejections\":" << instrument.admission_rejections
              << ",\"unanchored_tokens\":" << instrument.unanchored_tokens
              << ",\"budget_exhausted\":" << instrument.budget_exhausted
              << ",\"geometry_rejections\":" << instrument.geometry_rejections << '}';
}

void json_tokens(std::ostream& output, const std::vector<wardogs::SphHudToken>& tokens) {
    output << '[';
    bool first = true;
    for (const auto& token : tokens) {
        if (!first) output << ',';
        first = false;
        const auto& raw = token.reading;
        const bool confidence_available =
            (std::isfinite(raw.confidence) && raw.confidence > 0.0F && raw.confidence <= 1.0F) ||
            (std::isfinite(raw.minimum_confidence) && raw.minimum_confidence > 0.0F && raw.minimum_confidence <= 1.0F);
        output << "{\"text\":" << json_string(utf8(raw.text))
                  << ",\"alternate\":" << json_string(utf8(raw.alternate_text))
                  << ",\"confidence_available\":" << confidence_available
                  << ",\"minimum_confidence\":";
        json_number(output, raw.minimum_confidence);
        output << ",\"confidence\":"; json_number(output, raw.confidence);
        output << ",\"line_count\":" << raw.line_count
                  << ",\"word_geometry_verified\":" << raw.word_geometry_verified
                  << ",\"bounds\":[" << token.bounds.left << ',' << token.bounds.top << ','
                  << token.bounds.right << ',' << token.bounds.bottom << ']'
                  << ",\"bounds_source\":" << static_cast<int>(token.bounds_source)
                  << ",\"observed_glyph_count\":" << token.observed_glyph_count
                  << ",\"geometry_verified\":" << token.geometry_verified << '}';
    }
    output << ']';
}

void json_rect(std::ostream& output, wardogs::ImageRect rect) {
    output << '[' << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << ']';
}

void json_instrument_regions(std::ostream& output, const wardogs::SphHudScaleLayout& layout) {
    output << "{\"bounds\":"; json_rect(output, layout.labels);
    output << ",\"header\":"; json_rect(output, layout.header);
    output << ",\"labels\":[";
    bool first = true;
    for (const auto rect : layout.label_regions) {
        if (!first) output << ',';
        first = false;
        json_rect(output, rect);
    }
    output << "]}";
}

void json_scale(std::ostream& output, const std::optional<wardogs::SphHudScaleReading>& scale) {
    if (!scale) { output << "null"; return; }
    output << "{\"value\":"; json_number(output, scale->value);
    output << ",\"uncertainty\":"; json_number(output, scale->uncertainty);
    output << ",\"units_per_pixel\":"; json_number(output, scale->units_per_pixel);
    output << ",\"labels\":[";
    bool first = true;
    for (const auto& label : scale->labels) {
        if (!first) output << ',';
        first = false;
        output << "{\"value\":"; json_number(output, label.value);
        output << ",\"tick_x\":"; json_number(output, label.tick.x);
        output << ",\"tick_y\":"; json_number(output, label.tick.y);
        output << ",\"anchor_offset_px\":"; json_number(output, label.anchor_offset_px);
        output << '}';
    }
    output << "]}";
}

void json_reading(std::ostream& output, const wardogs::SphHudFrame& frame, const wardogs::SphHudReading& reading,
                  const wardogs::SphHudReading& supplemental, double elapsed_ms,
                  std::wstring_view language) {
    output.imbue(std::locale::classic());
    output << std::setprecision(17) << std::boolalpha
              << "{\"accepted\":" << reading.accepted()
              << ",\"refusal\":" << static_cast<int>(reading.refusal)
              << ",\"refusal_name\":" << json_string(wardogs::sph_hud_refusal_name(reading.refusal))
              << ",\"layout\":" << static_cast<int>(frame.layout.source)
              << ",\"layout_name\":" << json_string(wardogs::sph_hud_layout_source_name(frame.layout.source))
              << ",\"stage\":" << json_string(wardogs::sph_hud_recognition_stage_name(frame.diagnostics.stage))
              << ",\"tokens\":" << frame.tokens.size()
              << ",\"provider\":" << json_string(frame.provider)
              << ",\"ocr_language\":" << json_string(utf8(language.empty() ? L"en-US" : language))
              << ",\"elapsed_ms\":";
    json_number(output, elapsed_ms);
    output << ",\"pose\":";
    if (reading.accepted()) {
        const auto& pose = *reading.pose;
        output << "{\"bearing_deg\":"; json_number(output, pose.bearing_deg);
        output << ",\"sight_mil\":"; json_number(output, pose.sight_mil);
        output << ",\"bearing_uncertainty_deg\":"; json_number(output, pose.bearing_uncertainty_deg);
        output << ",\"mil_uncertainty\":"; json_number(output, pose.mil_uncertainty);
        output << '}';
    } else output << "null";
    output << ",\"layout_geometry\":{\"reticle_x\":";
    json_number(output, frame.layout.reticle.x);
    output << ",\"reticle_y\":"; json_number(output, frame.layout.reticle.y);
    output << ",\"reticle_uncertainty_px\":"; json_number(output, frame.layout.reticle_uncertainty_px);
    output << ",\"tick_uncertainty_px\":"; json_number(output, frame.layout.tick_uncertainty_px);
    output << "},\"instrument_regions\":{\"mil\":"; json_instrument_regions(output, frame.layout.mil_scale);
    output << ",\"range\":"; json_instrument_regions(output, frame.layout.range_scale);
    output << ",\"compass\":"; json_instrument_regions(output, frame.layout.compass);
    output << "},\"scale_readings\":{\"mil\":"; json_scale(output, reading.mil_scale);
    output << ",\"range\":"; json_scale(output, reading.range_scale);
    output << ",\"compass\":"; json_scale(output, reading.compass);
    output << '}';
    const auto& ammunition = reading.ammunition ? reading.ammunition : supplemental.ammunition;
    output << ",\"ammunition\":";
    if (ammunition)
        output << "{\"count\":" << ammunition->count
                  << ",\"kind\":" << static_cast<int>(ammunition->kind) << '}';
    else output << "null";
    const auto& stabilized = reading.stabilized ? reading.stabilized : supplemental.stabilized;
    output << ",\"stabilized\":";
    if (stabilized) output << *stabilized;
    else output << "null";
    const auto& geometry = frame.diagnostics.geometry;
    output << ",\"geometry_diagnostics\":{\"stage\":"
           << json_string(wardogs::sph_hud_layout_stage_name(geometry.stage))
           << ",\"reticle_candidates\":" << geometry.reticle_candidates
           << ",\"left_ticks\":" << geometry.left_ticks << ",\"right_ticks\":" << geometry.right_ticks
           << ",\"compass_strong_columns\":" << geometry.compass_strong_columns
           << ",\"compass_faint_columns\":" << geometry.compass_faint_columns
           << ",\"compass_grid_ticks\":" << geometry.compass_grid_ticks
           << ",\"reticle_uncertainty_px\":";
    json_number(output, geometry.reticle_uncertainty_px);
    output << ",\"compass_baseline_contrast\":"; json_number(output, geometry.compass_baseline_contrast);
    output << ",\"compass_tick_uncertainty_px\":"; json_number(output, geometry.compass_tick_uncertainty_px);
    output << ",\"compass_faint_rescue\":" << geometry.compass_faint_rescue
           << ",\"compass_weak_baseline\":" << geometry.compass_weak_baseline
           << ",\"compass_grid_ambiguous\":" << geometry.compass_grid_ambiguous << '}';
    output << ",\"diagnostics\":{\"mil\":"; json_instrument(output, frame.diagnostics.mil);
    output << ",\"range\":"; json_instrument(output, frame.diagnostics.range);
    output << ",\"compass\":"; json_instrument(output, frame.diagnostics.compass);
    output << ",\"asl\":"; json_instrument(output, frame.diagnostics.asl);
    output << ",\"stabilization\":"; json_instrument(output, frame.diagnostics.stabilization);
    output << ",\"ammunition\":"; json_instrument(output, frame.diagnostics.ammunition);
    output << ",\"weapon\":"; json_instrument(output, frame.diagnostics.weapon);
    output << "},\"token_details\":"; json_tokens(output, frame.tokens);
    output << ",\"unanchored_compass_token_details\":";
    json_tokens(output, frame.unanchored_compass_tokens);
    output << "}\n";
}

int usage() {
    std::wcerr << L"usage: sph_hud_probe [--json] [--repeat=1..20] [--ocr-language=ru-RU] <saved-image.png> [recognition-model.onnx]\n";
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool json{};
    bool repeat_specified{};
    unsigned repeat_count{1};
    std::wstring language;
    std::vector<std::wstring> paths;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--json") {
            if (json) return usage();
            json = true;
        } else if (argument.starts_with(L"--repeat=")) {
            if (repeat_specified) return usage();
            repeat_specified = true;
            const auto count = argument.substr(std::wstring_view(L"--repeat=").size());
            if (count.empty()) return usage();
            repeat_count = 0;
            for (const auto ch : count) {
                if (ch < L'0' || ch > L'9') return usage();
                repeat_count = repeat_count * 10 + static_cast<unsigned>(ch - L'0');
                if (repeat_count > 20) return usage();
            }
            if (repeat_count == 0) return usage();
        } else if (argument.starts_with(L"--ocr-language=")) {
            if (!language.empty()) return usage();
            language = argument.substr(std::wstring_view(L"--ocr-language=").size());
            if (language.empty()) return usage();
        } else if (argument.starts_with(L"--")) return usage();
        else paths.emplace_back(argument);
    }
    if (paths.empty() || paths.size() > 2) return usage();
    try {
        const auto image = wardogs::load_image_file(paths.front());
        auto ocr = language.empty() ? wardogs::WindowsOcr{} : wardogs::WindowsOcr{language};
        std::unique_ptr<wardogs::WindowsOcr> localized_metadata;
        if (ocr.language_tag() != L"ru-RU" && ocr.language_tag() != L"ru" &&
            wardogs::WindowsOcr::is_language_supported(L"ru-RU"))
            localized_metadata = std::make_unique<wardogs::WindowsOcr>(L"ru-RU");
        std::unique_ptr<wardogs::RapidOcr> recognition;
        if (paths.size() == 2) recognition = std::make_unique<wardogs::RapidOcr>(paths.back());
        std::ostringstream document;
        if (repeat_count > 1) {
            if (json) document << "{\"retained_image_repeats\":true,\"repeat_count\":" << repeat_count
                << ",\"frame_id\":1,\"independent_frame_count\":1,\"repeat_observations\":[";
            else std::cout << "retained_image_repeats=1 repeat_count=" << repeat_count
                << " frame_id=1 independent_frame_count=1\n";
        }
        for (unsigned repeat = 0; repeat < repeat_count; ++repeat) {
            // Retained-image repeats reuse these engines and the exact stored image.
            // They never acquire or manufacture independent physical frames.
            const auto started = std::chrono::steady_clock::now();
            const auto frame = wardogs::recognize_sph_hud_frame(
                image, ocr, 1, {}, recognition.get(), nullptr, localized_metadata.get());
            const auto reading = wardogs::read_sph_hud(frame);
            const auto elapsed_ms = std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now() - started).count();
            const auto supplemental = wardogs::read_sph_hud_supplemental(frame);
            if (json) {
                if (repeat != 0) document << ',';
                json_reading(document, frame, reading, supplemental, elapsed_ms, ocr.language_tag());
                continue;
            }
            if (repeat_count > 1) std::cout << "repeat_index=" << repeat + 1 << '\n';
            std::cout << std::setprecision(17) << "accepted=" << reading.accepted()
                      << " refusal=" << static_cast<int>(reading.refusal)
                      << " layout=" << static_cast<int>(frame.layout.source)
                      << " tokens=" << frame.tokens.size()
                      << " elapsed_ms=" << elapsed_ms << '\n';
            if (reading.pose) std::cout << "bearing=" << reading.pose->bearing_deg
                << " mil=" << reading.pose->sight_mil << " bearing_uncertainty=" << reading.pose->bearing_uncertainty_deg
                << " mil_uncertainty=" << reading.pose->mil_uncertainty << '\n';
            if (reading.ammunition) std::cout << "shell_count=" << reading.ammunition->count
                << " shell_channel=" << static_cast<int>(reading.ammunition->kind) << '\n';
            if (reading.stabilized) std::cout << "stabilized=" << *reading.stabilized << '\n';
            if (supplemental.ammunition) std::cout << "partial_shell_count=" << supplemental.ammunition->count
                << " partial_shell_channel=" << static_cast<int>(supplemental.ammunition->kind) << '\n';
            if (supplemental.stabilized) std::cout << "partial_stabilized=" << *supplemental.stabilized << '\n';
            if (frame.layout.source == wardogs::SphHudLayoutSource::optic_pixel_geometry) {
                const auto rect = frame.layout.compass.labels;
                const auto candidates = wardogs::extract_sph_hud_compass_word_regions(image, rect);
                std::cout << "compass_word_roi=" << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom
                          << " ink_groups=" << candidates.size() << '\n';
                for (const auto candidate : candidates)
                    std::cout << "compass_ink_union=" << candidate.left << ',' << candidate.top << ','
                              << candidate.right << ',' << candidate.bottom << '\n';
            }
            for (const auto& token : frame.tokens) std::wcout << L"token=" << token.reading.text
                << L" alternate=" << token.reading.alternate_text << L" bounds=" << token.bounds.left << L','
                << token.bounds.top << L',' << token.bounds.right << L',' << token.bounds.bottom
                << L" bounds_source=" << static_cast<int>(token.bounds_source)
                << L" geometry_verified=" << token.geometry_verified << '\n';
        }
        if (json) {
            if (repeat_count > 1) document << "]}\n";
            std::cout << document.str();
        }
        return 0; // Refusal is an expected diagnostic outcome, not a crash.
    } catch (const winrt::hresult_error& error) {
        std::wcerr << L"Windows OCR failed: " << error.message().c_str() << '\n'; return 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
