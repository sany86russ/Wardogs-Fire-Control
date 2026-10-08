#pragma once

#include "wardogs/core.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wardogs {

struct Image {
    int width{};
    int height{};
    std::vector<std::uint8_t> bgr;
};

struct ImageRect {
    int left{};
    int top{};
    int right{};
    int bottom{};
};

struct MapOcrSearchLayout {
    ImageRect x_prior{};
    ImageRect y_prior{};
    int cursor_x{};
    int cursor_y{};
    double scale{1.0};
};

// Detects plausible green/bright-neutral chat lines, never game coordinates.
// Refuses noisy scenes with more than eight lines instead of dropping history.
std::vector<ImageRect> find_chat_text_lines(const Image& image, std::stop_token stop = {});

struct OcrResult {
    std::wstring text;
    float confidence{};
    // Model character scores are diagnostics, not calibrated probabilities
    // that an entire coordinate is correct. Windows OCR supplies neither score.
    float minimum_confidence{};
    std::wstring alternate_text;
    std::size_t line_count{};
    // Physical capture pixels. These default false for ordinary/Windows OCR;
    // callers may automatically apply only complete, unique isolated evidence.
    bool isolated_coordinate_pair{};
    bool coordinate_boundary_clipped{};
    bool coordinate_passes_agree{};
    bool coordinate_glyph_count_matches{};
    std::optional<ImageRect> coordinate_bounds;
    // An active input channel plate immediately precedes this white draft.
    // Isolation alone cannot establish freshness of a chat-history message.
    bool coordinate_is_chat_draft{};
    // Both cursor fields explicitly contain their expected X/Y axis label.
    // Bare numbers stay readable for legacy/manual review, never automatic use.
    bool map_axes_labeled{};
    // Both bounds refer to the same neighborhood screenshot. Ordinary OCR
    // and the legacy independently captured fields leave them unavailable.
    std::optional<ImageRect> map_x_bounds;
    std::optional<ImageRect> map_y_bounds;
};

struct OcrCoordinateAssessment {
    std::optional<Point> selected;
    std::vector<Point> candidates;
    std::size_t match_count{};
    bool confidence_available{};
    bool low_confidence{};
    bool ambiguous{};
    bool pass_disagreement{};
    bool multiple_lines{};

    [[nodiscard]] bool requires_confirmation() const noexcept {
        return low_confidence || ambiguous || pass_disagreement || multiple_lines;
    }
};

// Preserves last-match selection for compatibility, while exposing the full
// evidence needed to confirm a capture before changing game coordinates.
OcrCoordinateAssessment assess_ocr_result(
    const OcrResult& result,
    std::wstring_view pattern = default_ocr_coordinate_pattern);

Image load_image_file(const std::filesystem::path& path);

OcrResult decode_ctc(const std::vector<float>& probabilities,
                     std::size_t time_steps,
                     std::size_t class_count,
                     const std::vector<std::wstring>& characters);

class RapidOcr {
public:
    explicit RapidOcr(const std::filesystem::path& model_path);
    ~RapidOcr();
    RapidOcr(RapidOcr&&) noexcept;
    RapidOcr& operator=(RapidOcr&&) noexcept;
    RapidOcr(const RapidOcr&) = delete;
    RapidOcr& operator=(const RapidOcr&) = delete;

    OcrResult recognize(const Image& image, std::stop_token stop = {}) const;
    // Each detected line is recognized separately with bounded CPU/memory.
    // Unrelated UI labels are excluded; provenance records intact glyphs and
    // independent crop agreement. Multiple coordinate rows remain visible.
    OcrResult recognize_chat(const Image& image, std::stop_token stop = {}) const;
    // The caller captures the two verified cursor-tooltip axis fields. Neither
    // field is inferred from chat or from the other axis on a recognition error.
    OcrResult recognize_map_coordinates(const Image& x_field, const Image& y_field,
                                       std::stop_token stop = {}) const;
    // Searches the complete bounded neighborhood, retaining every competing
    // semantic axis. A preferred field cannot hide another valid X/Y row.
    OcrResult recognize_map_neighborhood(const Image& image,
                                        const MapOcrSearchLayout& layout,
                                        std::stop_token stop = {}) const;
    [[nodiscard]] std::size_t character_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs
