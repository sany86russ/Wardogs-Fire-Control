#include "wardogs/ocr.hpp"
#include "wardogs/ocr_preprocessing.hpp"
#include "wardogs/logger.hpp"

#include <onnxruntime_cxx_api.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace wardogs {
namespace {

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("Метаданные OCR-модели содержат неверный UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::vector<std::wstring> split_characters(std::string_view value) {
    std::vector<std::wstring> result;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find('\n', begin);
        auto line = value.substr(begin, end == std::string_view::npos
                                           ? value.size() - begin
                                           : end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        result.push_back(utf8_to_wide(line));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    result.push_back(L" ");
    result.insert(result.begin(), L"blank");
    return result;
}

void validate_ocr_image(const Image& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.width > 16384 || image.height > 16384 ||
        static_cast<std::size_t>(image.width) * image.height > 16'000'000 ||
        image.bgr.size() != static_cast<std::size_t>(image.width) * image.height * 3) {
        throw std::invalid_argument("Изображение OCR должно содержать корректные BGR-пиксели и не превышать 16 млн пикселей");
    }
}

}  // namespace
namespace detail {

// Kept outside the anonymous namespace so the resampling boundary can be
// checked exactly without depending on an OCR model's probabilistic output.
void prepare_ocr_tensor(const Image& image, std::vector<float>& tensor, int& tensor_width,
                        int crop_top, int crop_bottom, std::stop_token stop) {
    validate_ocr_image(image);
    if (crop_top < 0 || crop_bottom < 0 || crop_top >= image.height ||
        crop_bottom >= image.height - crop_top) {
        throw std::invalid_argument("Неверные границы обрезки OCR-изображения");
    }
    constexpr int target_height = 48;
    constexpr int minimum_width = 320;
    const int source_height = image.height - crop_top - crop_bottom;
    const double ratio = static_cast<double>(image.width) / source_height;
    if (target_height * ratio > 4096)
        throw std::invalid_argument("Область OCR слишком широкая: выделите только строку координат");
    tensor_width = std::max(minimum_width,
                            static_cast<int>(target_height * std::max(320.0 / 48.0, ratio)));
    const int resized_width = std::min(
        tensor_width, static_cast<int>(std::ceil(target_height * ratio)));
    tensor.resize(static_cast<std::size_t>(3 * target_height * tensor_width));

    const double scale_x = static_cast<double>(image.width) / resized_width;
    const double scale_y = static_cast<double>(source_height) / target_height;
    const std::size_t plane = static_cast<std::size_t>(target_height * tensor_width);
    // Horizontal sampling is identical for all 48 rows. Compute each clamped
    // coordinate once; retain the exact bilinear order and float conversion.
    struct SampleColumn { int left; int right; double fraction; };
    std::array<SampleColumn, 4096> columns;
    for (int x = 0; x < resized_width; ++x) {
        const double source_x = std::clamp((x + 0.5) * scale_x - 0.5, 0.0,
                                          static_cast<double>(image.width - 1));
        const int x0 = static_cast<int>(std::floor(source_x));
        columns[x] = {x0 * 3, std::min(x0 + 1, image.width - 1) * 3, source_x - x0};
    }
    for (int y = 0; y < target_height; ++y) {
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        // Clamp the sampling coordinate before calculating its fraction.
        // Clamping only y0/x0 blends the first source pixel with its neighbour
        // at a negative coordinate, eroding thin strokes on the upper/left edge.
        const double source_y = std::clamp(crop_top + (y + 0.5) * scale_y - 0.5,
                                          static_cast<double>(crop_top),
                                          static_cast<double>(image.height - crop_bottom - 1));
        const int y0 = static_cast<int>(std::floor(source_y));
        const int y1 = std::min(y0 + 1, image.height - crop_bottom - 1);
        const double fy = source_y - y0;
        const auto* top_row = image.bgr.data() + static_cast<std::size_t>(y0) * image.width * 3;
        const auto* bottom_row = image.bgr.data() + static_cast<std::size_t>(y1) * image.width * 3;
        for (int x = 0; x < resized_width; ++x) {
            const auto& column = columns[x];
            const double fx = column.fraction;
            for (int channel = 0; channel < 3; ++channel) {
                const double top = top_row[column.left + channel] * (1.0 - fx) +
                                   top_row[column.right + channel] * fx;
                const double bottom = bottom_row[column.left + channel] * (1.0 - fx) +
                                      bottom_row[column.right + channel] * fx;
                const double pixel = top * (1.0 - fy) + bottom * fy;
                tensor[static_cast<std::size_t>(channel) * plane +
                       static_cast<std::size_t>(y * tensor_width + x)] =
                    static_cast<float>(pixel / 127.5 - 1.0);
            }
        }
        // All active samples were overwritten. Clear only the right padding,
        // including columns left behind when a retained buffer's crop narrows.
        for (int channel = 0; channel < 3; ++channel)
            std::fill_n(tensor.data() + static_cast<std::size_t>(channel) * plane +
                            static_cast<std::size_t>(y * tensor_width + resized_width),
                        tensor_width - resized_width, 0.0F);
    }
}

}  // namespace detail
namespace {

OcrResult decode_ctc_view(std::span<const float> probabilities,
                          std::size_t time_steps,
                          std::size_t class_count,
                          const std::vector<std::wstring>& characters) {
    if (class_count == 0 || characters.size() != class_count ||
        time_steps > std::numeric_limits<std::size_t>::max() / class_count ||
        probabilities.size() != time_steps * class_count) {
        throw std::invalid_argument("Неверные размеры CTC-результата OCR");
    }
    OcrResult result;
    float confidence_sum = 0.0F;
    float minimum_confidence = 1.0F;
    std::size_t selected = 0;
    std::size_t previous = class_count;
    for (std::size_t step = 0; step < time_steps; ++step) {
        const auto begin = probabilities.begin() +
                           static_cast<std::ptrdiff_t>(step * class_count);
        std::size_t index = 0;
        float winner = -1.0F;
        for (std::size_t character = 0; character < class_count; ++character) {
            const float score = begin[static_cast<std::ptrdiff_t>(character)];
            // Invalid non-winning logits must not be hidden by max_element:
            // a NaN can otherwise leave a plausible, falsely certain winner.
            // ONNX FLOAT is IEEE 754 binary32. All valid [0, 1] values have
            // ordered nonnegative bits; also accept mathematical zero -0.0.
            // One integer comparison avoids three floating checks per class.
            const auto bits = std::bit_cast<std::uint32_t>(score);
            if (bits > 0x3f800000U && bits != 0x80000000U)
                throw std::invalid_argument("OCR-модель вернула неверную уверенность распознавания");
            if (score > winner) {
                winner = score;
                index = character;
            }
        }
        if (index != previous && index != 0) {
            result.text += characters[index];
            confidence_sum += winner;
            minimum_confidence = std::min(minimum_confidence, winner);
            ++selected;
        }
        previous = index;
    }
    result.confidence = selected ? confidence_sum / static_cast<float>(selected) : 0.0F;
    result.minimum_confidence = selected ? minimum_confidence : 0.0F;
    return result;
}

struct InkComponent {
    int left{}, top{}, right{}, bottom{}, area{};
    std::size_t parent{};
};
struct TextLine {
    ImageRect ink;
    int font_height{};
    std::size_t glyphs{};
    bool clipped{};
    bool draft{};
};

// Run-length connected components avoid a full-size visited bitmap/flood queue.
// Both masks have a fixed 65k-run limit; noisy scenes stop before inference.
std::vector<InkComponent> ink_components(const Image& image, bool neutral,
                                          std::stop_token stop, bool dark = false) {
    struct Run { int left, right; std::size_t component; };
    std::vector<InkComponent> nodes;
    std::vector<Run> previous, current;
    const auto root = [&](std::size_t node) {
        while (nodes[node].parent != node) {
            nodes[node].parent = nodes[nodes[node].parent].parent;
            node = nodes[node].parent;
        }
        return node;
    };
    const auto is_ink = [&](int x, int y) {
        const auto offset = static_cast<std::size_t>((y * image.width + x) * 3);
        const int b = image.bgr[offset], g = image.bgr[offset + 1], r = image.bgr[offset + 2];
        if (dark) return std::max({b, g, r}) <= 100;
        return neutral ? std::min({b, g, r}) >= 160 &&
                             std::max({b, g, r}) - std::min({b, g, r}) <= 40
                       : g >= 90 && g >= r + 35 && g >= b - 15;
    };
    for (int y = 0; y < image.height; ++y) {
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        current.clear();
        std::size_t prior = 0;
        for (int x = 0; x < image.width;) {
            if (!is_ink(x, y)) { ++x; continue; }
            const int left = x++;
            while (x < image.width && is_ink(x, y)) ++x;
            const int right = x;
            if (nodes.size() == 65'536)
                throw std::invalid_argument("Слишком много деталей для безопасного поиска координат");
            std::size_t node = nodes.size();
            nodes.push_back({left, y, right, y + 1, right - left, node});
            while (prior < previous.size() && previous[prior].right < left) ++prior;
            for (std::size_t i = prior; i < previous.size() && previous[i].left <= right; ++i) {
                const auto other = root(previous[i].component);
                node = root(node);
                if (node == other) continue;
                // Earlier root wins: each pixel's area is counted exactly once.
                const auto destination = std::min(node, other), source = std::max(node, other);
                auto& target = nodes[destination];
                const auto& incoming = nodes[source];
                target.left = std::min(target.left, incoming.left);
                target.top = std::min(target.top, incoming.top);
                target.right = std::max(target.right, incoming.right);
                target.bottom = std::max(target.bottom, incoming.bottom);
                target.area += incoming.area;
                nodes[source].parent = destination;
                node = destination;
            }
            current.push_back({left, right, node});
        }
        previous.swap(current);
    }
    std::vector<InkComponent> components;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].parent == i) components.push_back(nodes[i]);
    return components;
}

bool has_channel_caption(const Image& image, const InkComponent& plate,
                         std::stop_token stop) {
    const int width = plate.right - plate.left, height = plate.bottom - plate.top;
    Image caption{width, height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3)};
    for (int y = 0; y < height; ++y) {
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        std::copy_n(image.bgr.data() +
            (static_cast<std::size_t>(plate.top + y) * image.width + plate.left) * 3,
            static_cast<std::size_t>(width) * 3,
            caption.bgr.data() + static_cast<std::size_t>(y) * width * 3);
    }
    std::vector<InkComponent> letters;
    for (const auto& piece : ink_components(caption, false, stop, true)) {
        const int w = piece.right - piece.left, h = piece.bottom - piece.top;
        // A channel plate encloses dark caption letters. Pale terrain behind
        // green chat text and solid bright scenery have no such caption.
        if (piece.left > 0 && piece.right < width && piece.top > 0 && piece.bottom < height &&
            h >= std::max(5, height / 3) && h <= height * 4 / 5 && w <= h * 2 && piece.area >= 6)
            letters.push_back(piece);
        if (letters.size() > 64) return false;
    }
    return std::any_of(letters.begin(), letters.end(), [&](const auto& anchor) {
        return std::count_if(letters.begin(), letters.end(), [&](const auto& piece) {
            const int overlap = std::min(anchor.bottom, piece.bottom) - std::max(anchor.top, piece.top);
            return overlap * 2 >= std::min(anchor.bottom - anchor.top, piece.bottom - piece.top);
        }) >= 3;
    });
}

std::vector<TextLine> locate_text_lines(const Image& image, std::stop_token stop,
                                      wchar_t map_axis = 0, bool draft_only = false,
                                      bool* active_draft_found = nullptr,
                                      std::size_t maximum_lines = 8,
                                      const MapOcrSearchLayout* map_layout = nullptr) {
    validate_ocr_image(image);
    const bool map_search = map_layout != nullptr;
    std::vector<TextLine> found;
    for (const bool neutral : {false, true}) {
        if (draft_only && !neutral) continue;
        const auto components = ink_components(image, neutral, stop);
        std::vector<InkComponent> blocks, glyphs, punctuation;
        for (const auto& component : components) {
            const int w = component.right - component.left, h = component.bottom - component.top;
            if (w > 128 || h > 128 ||
                (h >= 7 && w >= h * 3 && component.area * 2 > w * h))
                blocks.push_back(component);
        }
        if (blocks.size() > 256)
            throw std::invalid_argument("Слишком много фоновых деталей для поиска координат");
        const auto in_background_block = [&](const InkComponent& component) {
            return std::any_of(blocks.begin(), blocks.end(), [&](const auto& block) {
                // Bright scenery can surround separate draft glyphs without
                // sharing their ink. Once an enclosed channel caption binds
                // this draft, exclude the connected background itself, not
                // unrelated text merely inside its bounding rectangle.
                if (draft_only || map_search) return component.parent == block.parent;
                return component.left >= block.left && component.right <= block.right &&
                       component.top >= block.top && component.bottom <= block.bottom;
            });
        };
        std::vector<InkComponent> channel_plates;
        if (draft_only) {
            std::size_t caption_candidates = 0;
            // Short channel captions such as "ВСЕ" are narrower than a
            // background block. Classify their filled plate independently;
            // only the enclosed caption glyphs establish this exception.
            for (const auto& plate : components) {
                const int width = plate.right - plate.left, height = plate.bottom - plate.top;
                if (height < 7 || height > 128 || width < height ||
                    plate.area * 2 <= width * height || plate.left == 0 || plate.top == 0 ||
                    plate.right == image.width || plate.bottom == image.height) continue;
                if (++caption_candidates > 256)
                    throw std::invalid_argument("Слишком много деталей для поиска плашки канала чата");
                if (has_channel_caption(image, plate, stop)) {
                    channel_plates.push_back(plate);
                    if (channel_plates.size() > 8)
                        throw std::invalid_argument("Найдено слишком много возможных полей ввода чата");
                }
            }
            if (channel_plates.empty()) continue;
            if (active_draft_found) *active_draft_found = true;
        }
        const auto near_channel_plate = [&](const InkComponent& component) {
            if (!draft_only) return true;
            return std::any_of(channel_plates.begin(), channel_plates.end(), [&](const auto& plate) {
                const int height = plate.bottom - plate.top;
                const int overlap = std::min(plate.bottom, component.bottom) -
                                    std::max(plate.top, component.top);
                return component.left >= plate.right && component.right <= plate.right + height * 16 &&
                       overlap > 0;
            });
        };
        for (const auto& component : components) {
            if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
            const int w = component.right - component.left, h = component.bottom - component.top;
            // White islands inside a channel plate are part of that plate,
            // not detached letters to concatenate with the coordinate draft.
            const bool in_channel_plate = draft_only &&
                std::any_of(channel_plates.begin(), channel_plates.end(), [&](const auto& plate) {
                    return component.left >= plate.left && component.right <= plate.right &&
                           component.top >= plate.top && component.bottom <= plate.bottom;
                });
            if (in_background_block(component) || in_channel_plate || w > 128 || h > 128 ||
                !near_channel_plate(component)) continue;
            if (h >= 5 && w <= h * 3 && component.area >= 6) glyphs.push_back(component);
            else if (h < 5 && w <= 16) punctuation.push_back(component);
        }
        if (glyphs.size() > 2048 || punctuation.size() > 4096)
            throw std::invalid_argument("Слишком много символов для безопасного поиска координат");
        std::sort(glyphs.begin(), glyphs.end(), [](const auto& a, const auto& b) {
            return a.top + a.bottom < b.top + b.bottom;
        });
        std::vector<std::vector<InkComponent>> rows;
        for (const auto& glyph : glyphs) {
            auto matching = rows.end();
            for (auto row = rows.begin(); row != rows.end(); ++row) {
                const auto& anchor = row->front();
                int top = anchor.top, bottom = anchor.bottom;
                // A split upper loop can lead a tiny map or localized draft
                // row. Other digits establish its full baseline; use that
                // span so detached X/Y parts join the same complete row.
                if (map_axis || draft_only) for (const auto& part : *row) {
                    top = std::min(top, part.top); bottom = std::max(bottom, part.bottom);
                }
                const int overlap = std::min(bottom, glyph.bottom) - std::max(top, glyph.top);
                if (overlap * 2 >= std::min(bottom - top, glyph.bottom - glyph.top)) {
                    matching = row; break;
                }
            }
            if (matching == rows.end()) {
                if (rows.size() == maximum_lines * 2)
                    throw std::invalid_argument("Слишком много строк для безопасного поиска координат");
                rows.push_back({glyph});
            }
            else matching->push_back(glyph);
        }
        for (auto& row : rows) {
            std::vector<int> heights;
            for (const auto& glyph : row) heights.push_back(glyph.bottom - glyph.top);
            std::sort(heights.begin(), heights.end());
            const int font = heights[heights.size() / 2];
            const auto has_prefix_glyph = [&](const InkComponent& component) {
                return map_search && std::any_of(row.begin(), row.end(), [&](const auto& previous) {
                    return previous.right <= component.left &&
                           component.left - previous.right <= font * 2 &&
                           previous.bottom - previous.top >= std::max(5, font * 3 / 4);
                });
            };
            const auto map_cursor_lane = [&](const InkComponent& component) {
                // Y captures include the vertical cursor line at offset 12
                // in their 152-pixel field. Only that known narrow lane may
                // be removed; an arbitrary short white glyph stays evidence.
                const int lane = map_search ? map_layout->cursor_x :
                    static_cast<int>(std::lround(image.width * 12.0 / 152.0));
                // In a displaced tooltip the cursor can cross a real integer.
                // Never count away an interior fragment after an axis/glyph.
                if (has_prefix_glyph(component)) return false;
                return (map_search || (map_axis == L'y' &&
                       std::abs(image.width * 68 - image.height * 152) <= 152)) &&
                       component.right - component.left <= std::max(3, font / 5) &&
                       ((component.bottom - component.top) * 4 <= font * 3 ||
                        (component.bottom - component.top) * 2 >= font * 3) &&
                       component.left >= lane - std::max(1, font / 8) &&
                       component.right <= lane + std::max(2, font / 8 + 1);
            };
            std::vector<InkComponent> map_ping_brackets;
            if (map_axis) for (const auto& component : row) {
                const int width = component.right - component.left, height = component.bottom - component.top;
                const bool cursor_bracket = map_search &&
                    !has_prefix_glyph(component) &&
                    component.left >= map_layout->cursor_x - 32 * map_layout->scale &&
                    component.right <= map_layout->cursor_x + 8 * map_layout->scale;
                if ((component.left == 0 || cursor_bracket) && height > font * 2 &&
                    width <= font * 3 / 2 && component.area * 3 <= width * height &&
                       std::count_if(row.begin(), row.end(), [&](const auto& glyph) {
                           return glyph.left >= component.right &&
                                  std::abs(glyph.bottom - glyph.top - font) <= std::max(1, font / 8);
                       }) >= 3)
                    map_ping_brackets.push_back(component);
            }
            const auto map_ping_bracket = [&](const InkComponent& component) {
                return std::any_of(map_ping_brackets.begin(), map_ping_brackets.end(), [&](const auto& bracket) {
                    return component.left >= bracket.left && component.right <= bracket.right &&
                           component.top >= bracket.top && component.bottom <= bracket.bottom;
                });
            };
            const auto map_grid_edge = [&](const InkComponent& component) {
                // A map grid line can join the cursor bracket at the crop
                // edge. It spans past the glyph baseline and reaches a
                // vertical crop boundary; its wider bracket does not make
                // it a letter or evidence that a coordinate was cut off.
                return map_ping_bracket(component) ||
                       (map_axis && component.bottom - component.top > font * 3 / 2 &&
                       component.right - component.left <= std::max(3, font / 2) &&
                       (component.left == 0 || component.right == image.width) &&
                       (component.top == 0 || component.bottom == image.height));
            };
            // A terminal edit caret is taller than the text, very narrow and
            // should never become an extra coordinate digit or crop margin.
            std::erase_if(row, [&](const auto& glyph) {
                return map_grid_edge(glyph) || map_cursor_lane(glyph) ||
                       (!map_search && glyph.bottom - glyph.top > font * 3 / 2 &&
                        glyph.right - glyph.left <= std::max(3, font / 6));
            });
            std::sort(row.begin(), row.end(), [](const auto& a, const auto& b) { return a.left < b.left; });
            for (std::size_t begin = 0; begin < row.size();) {
                std::size_t end = begin + 1;
                int right = row[begin].right;
                // Map neighborhoods contain nearby independent captions and
                // cursor bracket fragments. A whole font-height gap separates
                // those runs; ordinary within-coordinate spacing stays intact.
                while (end < row.size() && row[end].left - right <= font * (map_search ? 1 : 2)) {
                    right = std::max(right, row[end].right); ++end;
                }
                ImageRect bounds{row[begin].left, row[begin].top, right, row[begin].bottom};
                std::vector<InkComponent> pieces(row.begin() + static_cast<std::ptrdiff_t>(begin),
                                                 row.begin() + static_cast<std::ptrdiff_t>(end));
                for (const auto& piece : pieces) {
                    bounds.top = std::min(bounds.top, piece.top);
                    bounds.bottom = std::max(bounds.bottom, piece.bottom);
                }
                for (const auto& piece : punctuation)
                    if (piece.right >= bounds.left - font / 2 && piece.left <= bounds.right + font / 2 &&
                        piece.top >= bounds.top + font / 3 && piece.bottom <= bounds.bottom + font / 3) {
                        pieces.push_back(piece);
                        bounds.left = std::min(bounds.left, piece.left);
                        bounds.right = std::max(bounds.right, piece.right);
                        bounds.bottom = std::max(bounds.bottom, piece.bottom);
                    }
                if (end - begin >= (map_search ? 2U : 3U) &&
                    bounds.right - bounds.left >= font * (map_search ? 1 : 2)) {
                    // Detached comma tails/dots at the same x belong to one
                    // glyph. Disjoint horizontal glyph columns stay distinct.
                    std::sort(pieces.begin(), pieces.end(), [](const auto& a, const auto& b) {
                        return a.left < b.left;
                    });
                    std::size_t count = 0;
                    int previous_right = -1;
                    for (const auto& piece : pieces) {
                        if (piece.left >= previous_right) ++count;
                        previous_right = std::max(previous_right, piece.right);
                    }
                    // A player icon above the row can connect otherwise
                    // separated Y/digit components. Three unaffected glyphs
                    // must establish a common font baseline before counting
                    // their independently visible lower strokes. Missing
                    // glyphs leave a wide gap; a solid covering patch cannot
                    // become physical proof for an unseen digit.
                    if (map_axis) {
                        std::vector<int> baselines;
                        for (const auto& piece : pieces) {
                            const int w = piece.right - piece.left, h = piece.bottom - piece.top;
                            if (std::abs(h - font) <= std::max(1, font / 8) &&
                                piece.area * 10 < w * h * 9)
                                baselines.push_back(piece.bottom);
                        }
                        if (baselines.size() >= 3) {
                            std::sort(baselines.begin(), baselines.end());
                            const int baseline = baselines[baselines.size() / 2];
                            const bool overlay = std::any_of(pieces.begin(), pieces.end(), [&](const auto& piece) {
                                return piece.top < baseline - font * 3 / 2 &&
                                       piece.bottom >= baseline && piece.right - piece.left >= font;
                            });
                            const bool aligned = std::all_of(baselines.begin(), baselines.end(), [&](int bottom) {
                                return std::abs(bottom - baseline) <= std::max(1, font / 8);
                            });
                            if (overlay && aligned) {
                                const int top = std::max(bounds.top, baseline - font / 2);
                                const int bottom = std::min(bounds.bottom, baseline + font / 3);
                                std::size_t body_count = 0;
                                int prior_end = bounds.left;
                                bool proof = true;
                                for (int x = bounds.left; x < bounds.right;) {
                                    const auto ink = [&](int xx, int yy) {
                                        const auto offset = (static_cast<std::size_t>(yy) * image.width + xx) * 3;
                                        const int b = image.bgr[offset], g = image.bgr[offset + 1], r = image.bgr[offset + 2];
                                        return neutral ? std::min({b, g, r}) >= 160 &&
                                            std::max({b, g, r}) - std::min({b, g, r}) <= 40 :
                                            g >= 90 && g >= r + 35 && g >= b - 15;
                                    };
                                    const auto column = [&](int xx) {
                                        for (int yy = top; yy < bottom; ++yy) if (ink(xx, yy)) return true;
                                        return false;
                                    };
                                    if (!column(x)) { ++x; continue; }
                                    const int left = x++;
                                    while (x < bounds.right && column(x)) ++x;
                                    int ink_top = bottom, ink_bottom = top, area = 0;
                                    for (int xx = left; xx < x; ++xx) for (int yy = top; yy < bottom; ++yy)
                                        if (ink(xx, yy)) {
                                            ++area; ink_top = std::min(ink_top, yy); ink_bottom = std::max(ink_bottom, yy + 1);
                                        }
                                    proof &= (!body_count || left - prior_end <= std::max(2, font * 2 / 3)) &&
                                             !(x - left >= font / 3 &&
                                               area * 10 >= (x - left) * (ink_bottom - ink_top) * 9);
                                    ++body_count; prior_end = x;
                                }
                                if (proof) count = body_count;
                            }
                        }
                    }
                    const auto& draft_plates = draft_only ? channel_plates : blocks;
                    const bool draft = neutral && std::any_of(draft_plates.begin(), draft_plates.end(), [&](const auto& plate) {
                        const int height = plate.bottom - plate.top;
                        const int width = plate.right - plate.left;
                        const int overlap = std::min(plate.bottom, bounds.bottom) - std::max(plate.top, bounds.top);
                        return (draft_only || (width >= height * 3 && width >= font * 4)) &&
                               plate.area * 2 > width * height &&
                               plate.right <= bounds.left && bounds.left - plate.right <= font * 2 &&
                               height >= font && height <= font * 3 &&
                               overlap >= font / 2 && plate.left > 0 && plate.top > 0 &&
                               plate.bottom < image.height;
                    });
                    if (draft_only && !draft) { begin = end; continue; }
                    const bool boundary_fragment = std::any_of(components.begin(), components.end(), [&](const auto& piece) {
                        // Background components excluded from the coordinate
                        // row cannot prove a clipped map glyph. Keep narrow
                        // edge fragments of real letters and all direct ink
                        // boundary checks below, including fractional digits.
                        if (map_axis && (map_grid_edge(piece) || in_background_block(piece) ||
                                         piece.right - piece.left > 128 || piece.bottom - piece.top > 128))
                            return false;
                        const int overlap = std::min(bounds.bottom, piece.bottom) - std::max(bounds.top, piece.top);
                        return overlap > 0 &&
                            ((piece.left == 0 && bounds.left <= font * 2) ||
                             (piece.right == image.width && image.width - bounds.right <= font * 2));
                    });
                    found.push_back({bounds, font, count,
                        bounds.left == 0 || bounds.top == 0 || bounds.right == image.width ||
                        bounds.bottom == image.height || boundary_fragment, draft});
                    if (found.size() > maximum_lines * 2)
                        throw std::invalid_argument("Слишком много строк для безопасного поиска координат");
                }
                begin = end;
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.ink.top != b.ink.top ? a.ink.top < b.ink.top : a.ink.left < b.ink.left;
    });
    // Mixed green/white antialiasing may produce the same line in both masks.
    std::vector<TextLine> unique;
    for (const auto& line : found) {
        const auto duplicate = std::find_if(unique.begin(), unique.end(), [&](const auto& previous) {
            const int x = std::min(previous.ink.right, line.ink.right) - std::max(previous.ink.left, line.ink.left);
            const int y = std::min(previous.ink.bottom, line.ink.bottom) - std::max(previous.ink.top, line.ink.top);
            return x > 0 && y > 0 && x * 2 >= std::min(previous.ink.right - previous.ink.left,
                                                       line.ink.right - line.ink.left) &&
                   y * 2 >= std::min(previous.ink.bottom - previous.ink.top,
                                      line.ink.bottom - line.ink.top);
        });
        if (duplicate == unique.end()) unique.push_back(line);
        else duplicate->clipped |= line.clipped;
    }
    if (unique.size() > maximum_lines)
        throw std::invalid_argument("В области слишком много строк для безопасного поиска координат");
    return unique;
}

ImageRect padded_bounds(const Image& image, const TextLine& line, int padding) {
    return {std::max(0, line.ink.left - padding), std::max(0, line.ink.top - padding),
            std::min(image.width, line.ink.right + padding), std::min(image.height, line.ink.bottom + padding)};
}
Image crop_text_line(const Image& image, const TextLine& line, int padding) {
    const auto bounds = padded_bounds(image, line, padding);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    Image cropped{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3)};
    for (int y = 0; y < height; ++y) {
        const auto row = image.bgr.data() + static_cast<std::size_t>((bounds.top + y) * image.width * 3);
        // Margin uses the adjacent background, never neighbouring UI ink.
        // A blinking caret immediately after the last digit must not enter a
        // second crop and be mistaken for a trailing '1' or separator.
        const int background_x = std::max(0, line.ink.left - padding);
        for (int x = 0; x < width; ++x)
            std::copy_n(row + background_x * 3, 3,
                        cropped.bgr.data() + static_cast<std::size_t>((y * width + x) * 3));
        std::copy_n(row + line.ink.left * 3,
                    static_cast<std::size_t>(line.ink.right - line.ink.left) * 3,
                    cropped.bgr.data() + static_cast<std::size_t>((y * width + line.ink.left - bounds.left) * 3));
    }
    return cropped;
}
std::size_t visible_characters(std::wstring_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](wchar_t c) {
        return c != L' ' && c != L'\n' && c != L'\r' && c != L'\t';
    }));
}
std::optional<std::wstring> complete_decimal(std::wstring_view text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.remove_suffix(1);
    if (text.empty()) return std::nullopt;
    std::size_t i = text.front() == L'-' || text.front() == L'+' ? 1 : 0;
    const std::size_t digits = i;
    while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') ++i;
    if (i == digits || i + 3 != text.size() || text[i] != L'.' ||
        text[i + 1] < L'0' || text[i + 1] > L'9' || text[i + 2] < L'0' || text[i + 2] > L'9')
        return std::nullopt;
    return std::wstring(text);
}

struct MapAxisValue { std::wstring number; bool labeled{}; };
std::optional<MapAxisValue> parse_map_axis(std::wstring_view text, wchar_t expected) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.remove_suffix(1);
    bool labeled = false;
    if (!text.empty()) {
        const bool x = text.front() == L'x' || text.front() == L'X' ||
                       text.front() == L'х' || text.front() == L'Х';
        const bool y = text.front() == L'y' || text.front() == L'Y' ||
                       text.front() == L'у' || text.front() == L'У';
        if (x || y) {
            if ((expected == L'x' && !x) || (expected == L'y' && !y)) return std::nullopt;
            labeled = true;
            text.remove_prefix(1);
        }
    }
    const auto number = complete_decimal(text);
    if (!number) return std::nullopt;
    return MapAxisValue{*number, labeled};
}

bool map_axis_prefix(std::wstring_view text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    if (text.empty()) return false;
    const wchar_t first = text.front();
    if (first != L'x' && first != L'X' && first != L'х' && first != L'Х' &&
        first != L'y' && first != L'Y' && first != L'у' && first != L'У') return false;
    text.remove_prefix(1);
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    if (text.empty()) return false;
    const auto numeric_glyph = [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || c == L'l' || c == L'I' || c == L'i' ||
               c == L'O' || c == L'o' || c == L'|' || c == L'.' || c == L',' ||
               c == L'-' || c == L'+' || c == L' ' || c == L'\t';
    };
    // Confusable OCR strokes such as "ylll" stay damaged numeric evidence.
    // They are never substituted into a value. YARD/YORK remain ordinary words.
    return numeric_glyph(text.front()) &&
        (std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }) ||
         std::all_of(text.begin(), text.end(), numeric_glyph));
}

OcrResult recognize_text_line(const RapidOcr& ocr, const Image& image,
                             const TextLine& line, bool numeric, std::stop_token stop) {
    const int base = std::max({4, line.font_height / 3,
                               (20 - (line.ink.bottom - line.ink.top) + 1) / 2});
    auto result = ocr.recognize(crop_text_line(image, line, base), stop);
    const auto agrees = [&](const OcrResult& value) {
        if (visible_characters(value.text) != line.glyphs ||
            visible_characters(value.alternate_text) != line.glyphs) return false;
        if (numeric) {
            const auto first = complete_decimal(value.text), second = complete_decimal(value.alternate_text);
            return first && first == second;
        }
        const auto first = parse_ocr_coordinates(value.text), second = parse_ocr_coordinates(value.alternate_text);
        return first.size() == 1 && first == second;
    };
    if (agrees(result) && result.confidence >= 0.90F && result.minimum_confidence >= 0.65F)
        return result;
    // One bounded retry changes only the background margin. Low-resolution
    // antialiasing can make one otherwise intact crop weak; thresholds and
    // the requirement for two agreeing full-glyph passes remain unchanged.
    const int retry_padding = std::max(base + 1, line.font_height / 2);
    auto retry = ocr.recognize(crop_text_line(image, line, retry_padding), stop);
    if (!agrees(retry) || (agrees(result) && retry.minimum_confidence <= result.minimum_confidence))
        return result;
    const auto conflicting = [&](std::wstring_view text) {
        if (numeric) {
            const auto number = complete_decimal(text), selected = complete_decimal(retry.text);
            return number && selected && number != selected;
        }
        const auto points = parse_ocr_coordinates(text), selected = parse_ocr_coordinates(retry.text);
        return !points.empty() && !selected.empty() && points != selected;
    };
    // A different valid value from any earlier pass survives as disagreement.
    if (conflicting(result.text)) retry.alternate_text = result.text;
    else if (conflicting(result.alternate_text)) retry.alternate_text = result.alternate_text;
    return retry;
}

}  // namespace

std::vector<ImageRect> find_chat_text_lines(const Image& image, std::stop_token stop) {
    std::vector<ImageRect> bounds;
    for (const auto& line : locate_text_lines(image, stop))
        bounds.push_back(padded_bounds(image, line, std::max(4, line.font_height / 3)));
    return bounds;
}

OcrResult decode_ctc(const std::vector<float>& probabilities,
                     std::size_t time_steps,
                     std::size_t class_count,
                     const std::vector<std::wstring>& characters) {
    return decode_ctc_view(probabilities, time_steps, class_count, characters);
}

OcrCoordinateAssessment assess_ocr_result(const OcrResult& result,
                                         std::wstring_view pattern) {
    OcrCoordinateAssessment assessment;
    const auto primary = parse_ocr_coordinates(result.text, pattern);
    const auto alternate = parse_ocr_coordinates(result.alternate_text, pattern);
    assessment.match_count = primary.size();
    if (!primary.empty()) assessment.selected = primary.back();
    const auto append_unique = [&](const std::vector<Point>& points) {
        for (const auto point : points)
            if (std::find(assessment.candidates.begin(), assessment.candidates.end(), point) ==
                assessment.candidates.end())
                assessment.candidates.push_back(point);
    };
    append_unique(primary);
    append_unique(alternate);
    assessment.ambiguous = assessment.candidates.size() > 1;
    assessment.pass_disagreement = !primary.empty() && !alternate.empty() &&
                                   primary.back() != alternate.back();
    assessment.multiple_lines = result.line_count > 1;
    assessment.confidence_available = std::isfinite(result.confidence) &&
                                      result.confidence > 0.0F && result.confidence <= 1.0F;
    // Conservative review thresholds, not an accuracy claim. A weak digit can
    // be masked by a high average across an otherwise easy chat line.
    assessment.low_confidence = !std::isfinite(result.confidence) ||
        !std::isfinite(result.minimum_confidence) || result.confidence < 0.0F ||
        result.confidence > 1.0F || result.minimum_confidence < 0.0F ||
        result.minimum_confidence > 1.0F ||
        (assessment.confidence_available &&
         (result.confidence < 0.90F ||
          (result.minimum_confidence > 0.0F && result.minimum_confidence < 0.65F)));
    return assessment;
}

struct RapidOcr::Impl {
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "wardogs"};
    Ort::SessionOptions options;
    Ort::Session session{nullptr};
    std::vector<std::wstring> characters;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<float> input;
    std::timed_mutex recognition_mutex;

    explicit Impl(const std::filesystem::path& model_path) {
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session = Ort::Session(environment, model_path.c_str(), options);
        Ort::AllocatorWithDefaultOptions allocator;
        auto metadata = session.GetModelMetadata();
        auto encoded = metadata.LookupCustomMetadataMapAllocated("character", allocator);
        if (!encoded) {
            throw std::runtime_error("OCR-модель не содержит таблицу символов");
        }
        characters = split_characters(encoded.get());
    }
};

RapidOcr::RapidOcr(const std::filesystem::path& model_path)
    : impl_(std::make_unique<Impl>(model_path)) {}

RapidOcr::~RapidOcr() = default;
RapidOcr::RapidOcr(RapidOcr&&) noexcept = default;
RapidOcr& RapidOcr::operator=(RapidOcr&&) noexcept = default;

std::size_t RapidOcr::character_count() const { return impl_->characters.size(); }

OcrResult RapidOcr::recognize(const Image& image, std::stop_token stop) const {
    if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
    // A session is retained for the lifetime of the recognizer. Serialize calls
    // so its input buffer can be reused without corrupting another caller.
    std::unique_lock lock(impl_->recognition_mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(10)))
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
    const auto infer = [this, &image, stop](int crop_top, int crop_bottom) {
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        int width = 0;
        detail::prepare_ocr_tensor(image, impl_->input, width, crop_top, crop_bottom, stop);
        const std::array<std::int64_t, 4> shape{1, 3, 48, width};
        auto tensor = Ort::Value::CreateTensor<float>(impl_->memory,
                                                       impl_->input.data(), impl_->input.size(),
                                                       shape.data(), shape.size());
        constexpr std::array<const char*, 1> input_names{"x"};
        constexpr std::array<const char*, 1> output_names{"fetch_name_0"};
        Ort::RunOptions run_options;
        std::atomic_bool cancellation_failed{};
        std::stop_callback cancel_run(stop, [&]() noexcept {
            // The C API reports an error instead of throwing from the stop
            // callback. SetTerminate is the runtime's supported Run interrupt.
            if (auto* status = Ort::GetApi().RunOptionsSetTerminate(run_options)) {
                cancellation_failed = true;
                Ort::GetApi().ReleaseStatus(status);
            }
        });
        std::vector<Ort::Value> outputs;
        try {
            outputs = impl_->session.Run(run_options, input_names.data(),
                                          &tensor, 1, output_names.data(), 1);
        } catch (const Ort::Exception&) {
            if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
            throw;
        }
        if (cancellation_failed)
            throw std::runtime_error("ONNX Runtime не смог отменить распознавание");
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("OCR-модель вернула неверный тип результата");
        const auto output_shape = info.GetShape();
        if (output_shape.size() != 3 || output_shape[0] != 1 ||
            output_shape[1] <= 0 || output_shape[2] <= 0) {
            throw std::runtime_error("OCR-модель вернула неожиданный размер результата");
        }
        const auto steps = static_cast<std::size_t>(output_shape[1]);
        const auto classes = static_cast<std::size_t>(output_shape[2]);
        const float* values = outputs[0].GetTensorData<float>();
        return decode_ctc_view(std::span<const float>{values, info.GetElementCount()},
                               steps, classes, impl_->characters);
    };

    OcrResult original = infer(0, 0);
    if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
    if (image.height < 20) return original;

    // Recognition-only models expect a tightly cropped text line. Preserve the
    // user's exact region, but also try removing a small amount of vertical
    // margin so repeated narrow glyphs receive enough horizontal time steps.
    const int trim = std::clamp(static_cast<int>(std::lround(image.height * 0.10)),
                                1, (image.height - 12) / 2);
    OcrResult tightened = infer(trim, trim);
    const bool original_parseable = !parse_ocr_coordinates(original.text).empty();
    const bool tightened_parseable = !parse_ocr_coordinates(tightened.text).empty();
    const float agreed_confidence = std::min(original.confidence, tightened.confidence);
    const float agreed_minimum = std::min(original.minimum_confidence, tightened.minimum_confidence);
    // Prefer a complete game coordinate over unrelated text with a higher
    // character score, but retain both passes for ambiguity review by the UI.
    if ((tightened_parseable && !original_parseable) ||
        (tightened_parseable == original_parseable &&
         tightened.confidence > original.confidence)) {
        tightened.alternate_text = std::move(original.text);
        tightened.confidence = agreed_confidence;
        tightened.minimum_confidence = agreed_minimum;
        return tightened;
    }
    original.alternate_text = std::move(tightened.text);
    // Evidence for automatic application must include the weaker crop pass;
    // selecting a strong pass must not hide uncertainty in the other one.
    original.confidence = agreed_confidence;
    original.minimum_confidence = agreed_minimum;
    return original;
}

OcrResult RapidOcr::recognize_chat(const Image& image, std::stop_token stop) const {
    // Segment the active draft before considering the game scene behind the
    // chat. Bright scenery must not exhaust the row budget before this row
    // can establish freshness. Limits and full-glyph evidence stay unchanged.
    bool active_draft_found = false;
    std::vector<TextLine> lines;
    try {
        lines = locate_text_lines(image, stop, false, true, &active_draft_found);
        if (!active_draft_found) {
            lines = locate_text_lines(image, stop);
            // Legacy history remains readable, but generic bright-background
            // geometry cannot establish freshness without a verified caption.
            for (auto& line : lines) line.draft = false;
        }
    } catch (const std::exception& error) {
        if (!stop.stop_requested()) {
            std::ostringstream failure;
            failure << "ocr.chat_localization_failed active_plate=" << active_draft_found
                    << " error=" << error.what();
            log_warning(failure.str());
        }
        throw;
    }
    {
        std::ostringstream evidence;
        evidence << "ocr.chat_lines width=" << image.width << " height=" << image.height
                 << " active_plate=" << active_draft_found << " lines=" << lines.size();
        log_info(evidence.str());
    }
    // A detected input plate with empty/partial text blocks old chat history,
    // including cases too short to form a three-glyph TextLine.
    if (lines.empty())
        throw std::invalid_argument("Не найдена полная строка координат. Отметьте координаты на карте ещё раз.");
    struct RecognizedLine { OcrResult result; TextLine line; };
    std::vector<RecognizedLine> recognized_lines;
    for (const auto& line : lines) {
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        auto recognized = recognize_text_line(*this, image, line, false, stop);
        recognized.coordinate_bounds = line.ink;
        recognized.coordinate_boundary_clipped = line.clipped;
        recognized.coordinate_is_chat_draft = line.draft;
        recognized.coordinate_glyph_count_matches =
            visible_characters(recognized.text) == line.glyphs &&
            visible_characters(recognized.alternate_text) == line.glyphs;
        const auto primary = parse_ocr_coordinates(recognized.text);
        const auto alternate = parse_ocr_coordinates(recognized.alternate_text);
        recognized.coordinate_passes_agree = primary.size() == 1 && alternate == primary;
        // UI channel labels/arrows do not contribute scores or line count to
        // a complete coordinate. Keep incomplete labeled rows for X/Y popups.
        const auto coordinate_like = [](std::wstring_view text) {
            return text.find_first_of(L"xXхХyYуУ") != std::wstring_view::npos &&
                   std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        };
        if (!primary.empty() || !alternate.empty() || line.draft ||
            coordinate_like(recognized.text) || coordinate_like(recognized.alternate_text))
            recognized_lines.push_back({std::move(recognized), line});
    }
    if (recognized_lines.empty())
        throw std::invalid_argument("На изображении не найдены полные координаты X и Y");
    OcrResult combined;
    combined.minimum_confidence = 1.0F;
    combined.coordinate_glyph_count_matches = true;
    combined.coordinate_is_chat_draft = true;
    std::size_t scored_characters = 0;
    // An active input plate establishes draft provenance independently from
    // OCR. If its row is incomplete, old history must never rescue that row.
    const bool has_draft = std::any_of(recognized_lines.begin(), recognized_lines.end(), [](const auto& row) {
        return row.line.draft;
    });
    const bool complete_rows = std::any_of(recognized_lines.begin(), recognized_lines.end(), [](const auto& row) {
        return !parse_ocr_coordinates(row.result.text).empty() ||
               !parse_ocr_coordinates(row.result.alternate_text).empty();
    });
    for (const auto& row : recognized_lines) {
        const auto& recognized = row.result;
        if (has_draft && !row.line.draft) continue;
        if (!combined.text.empty()) { combined.text += L'\n'; combined.alternate_text += L'\n'; }
        combined.text += recognized.text;
        combined.alternate_text += recognized.alternate_text;
        ++combined.line_count;
        combined.coordinate_boundary_clipped |= recognized.coordinate_boundary_clipped;
        combined.coordinate_glyph_count_matches &= recognized.coordinate_glyph_count_matches;
        combined.coordinate_is_chat_draft &= recognized.coordinate_is_chat_draft;
        if (!combined.coordinate_bounds) combined.coordinate_bounds = row.line.ink;
        else {
            auto& bounds = *combined.coordinate_bounds;
            bounds.left = std::min(bounds.left, row.line.ink.left);
            bounds.top = std::min(bounds.top, row.line.ink.top);
            bounds.right = std::max(bounds.right, row.line.ink.right);
            bounds.bottom = std::max(bounds.bottom, row.line.ink.bottom);
        }
        const auto characters = visible_characters(recognized.text);
        combined.confidence += recognized.confidence * static_cast<float>(characters);
        scored_characters += characters;
        combined.minimum_confidence = std::min(combined.minimum_confidence, recognized.minimum_confidence);
    }
    if (scored_characters) combined.confidence /= static_cast<float>(scored_characters);
    else combined.minimum_confidence = 0.0F;
    const auto primary = parse_ocr_coordinates(combined.text);
    const auto alternate = parse_ocr_coordinates(combined.alternate_text);
    combined.coordinate_passes_agree = primary.size() == 1 && alternate == primary;
    // At most two adjacent rows can be a split X/Y popup. History with two
    // complete pairs never qualifies, even if those pairs happen to be equal.
    bool adjacent = combined.line_count == 1;
    if (!complete_rows && recognized_lines.size() == 2) {
        const auto& first = recognized_lines[0].line;
        const auto& second = recognized_lines[1].line;
        adjacent = second.ink.top - first.ink.bottom <= std::max(first.font_height, second.font_height) * 2 &&
                   std::abs(first.ink.left - second.ink.left) <= std::max(first.font_height, second.font_height) * 2;
    }
    combined.isolated_coordinate_pair = adjacent && primary.size() == 1;
    return combined;
}

OcrResult RapidOcr::recognize_map_coordinates(const Image& x_field, const Image& y_field,
                                            std::stop_token stop) const {
    const auto parse_axis = parse_map_axis;
    const auto recognize_axis = [&](auto&& self, const Image& source, wchar_t expected,
                                    bool magnified) -> OcrResult {
        validate_ocr_image(source);
        Image image = source;
        if (magnified) {
            // This retry is limited to small cursor fields. Pixel replication
            // preserves all gaps/colours while raising thin glyph fragments
            // above the existing component-size limit, without lowering it.
            image.width *= 2; image.height *= 2;
            image.bgr.resize(static_cast<std::size_t>(image.width) * image.height * 3);
            for (int y = 0; y < image.height; ++y) {
                if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
                for (int x = 0; x < image.width; ++x)
                    std::copy_n(source.bgr.data() +
                        (static_cast<std::size_t>(y / 2) * source.width + x / 2) * 3,
                        3, image.bgr.data() + (static_cast<std::size_t>(y) * image.width + x) * 3);
            }
        }
        bool edge_fragment_removed = false;
        // A map cursor bracket extends a few pixels into the X rectangle.
        // Its narrow boundary fragment must not join the coordinate's glyphs.
        // Removing a genuine clipped axis label can only leave a bare number:
        // that number lacks semantic labels and cannot be applied automatically.
        const int edge_width = std::max(4, image.height / 12);
        for (const auto& component : ink_components(image, true, stop)) {
            const int width = component.right - component.left;
            const int height = component.bottom - component.top;
            if (width > edge_width || height > image.height / 2 ||
                (component.left != 0 && component.right != image.width)) continue;
            edge_fragment_removed = true;
            for (int y = component.top; y < component.bottom; ++y)
                for (int x = component.left; x < component.right; ++x)
                    std::fill_n(image.bgr.data() + (static_cast<std::size_t>(y) * image.width + x) * 3,
                                3, std::uint8_t{20});
        }
        const auto lines = locate_text_lines(image, stop, expected);
        if (lines.empty()) throw std::invalid_argument("В поле карты не найдены все цифры координаты");
        std::optional<OcrResult> selected;
        int selected_font = 0;
        bool competing = false;
        for (const auto& line : lines) {
            if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
            const int padding = std::max({4, line.font_height / 3,
                                         (20 - (line.ink.bottom - line.ink.top) + 1) / 2});
            auto result = recognize(crop_text_line(image, line, padding), stop);
            const auto agrees = [&](const OcrResult& candidate) {
                const auto first = parse_axis(candidate.text, expected);
                const auto second = parse_axis(candidate.alternate_text, expected);
                return first && second && first->number == second->number &&
                       first->labeled == second->labeled &&
                       visible_characters(candidate.text) == line.glyphs &&
                       visible_characters(candidate.alternate_text) == line.glyphs;
            };
            if (!agrees(result) || result.confidence < 0.90F || result.minimum_confidence < 0.65F) {
                auto retry = recognize(crop_text_line(image, line,
                    std::max(padding + 1, line.font_height / 2)), stop);
                if (agrees(retry) && (!agrees(result) || retry.minimum_confidence > result.minimum_confidence)) {
                    // Preserve any earlier different valid value as disagreement.
                    const auto chosen = parse_axis(retry.text, expected);
                    for (const auto* previous : {&result.text, &result.alternate_text}) {
                        const auto value = parse_axis(*previous, expected);
                        if (value && chosen && (value->number != chosen->number ||
                                               value->labeled != chosen->labeled)) {
                            retry.alternate_text = *previous;
                            break;
                        }
                    }
                    result = std::move(retry);
                }
            }
            const auto number = parse_axis(result.text, expected);
            const auto alternate = parse_axis(result.alternate_text, expected);
            if (!number && !alternate) {
                // A nearby PING caption contains no numbers and is not an axis.
                // Every incomplete numeric row or wrong axis remains competing
                // evidence; it cannot be silently discarded in favour of a row.
                const auto has_digit = [](std::wstring_view text) {
                    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
                };
                competing |= has_digit(result.text) || has_digit(result.alternate_text);
                continue;
            }
            if (selected) throw std::invalid_argument("В поле карты найдены несколько числовых строк");
            result.coordinate_passes_agree = number && alternate &&
                number->number == alternate->number && number->labeled == alternate->labeled;
            result.coordinate_glyph_count_matches =
                visible_characters(result.text) == line.glyphs &&
                visible_characters(result.alternate_text) == line.glyphs;
            result.coordinate_bounds = line.ink;
            result.map_axes_labeled = number && alternate && number->labeled && alternate->labeled;
            result.coordinate_boundary_clipped = line.clipped ||
                (edge_fragment_removed && !result.map_axes_labeled);
            if (number) result.text = number->number;
            if (alternate) result.alternate_text = alternate->number;
            selected = std::move(result);
            selected_font = line.font_height;
        }
        if (competing)
            throw std::invalid_argument("В поле карты есть неполная координата или другая ось. Повторите отметку.");
        if (!selected || !complete_decimal(selected->text))
            throw std::invalid_argument("Не удалось прочитать полное число с двумя дробными цифрами на карте");
        if (!magnified && selected_font < 12 && source.width <= 2048 && source.height <= 512 &&
            (!selected->map_axes_labeled || !selected->coordinate_glyph_count_matches ||
             !selected->coordinate_passes_agree)) {
            auto enlarged = self(self, source, expected, true);
            // Re-measuring the same pixels cannot replace a different full
            // decimal reported by an earlier pass, even at higher confidence.
            if (enlarged.text != selected->text) {
                enlarged.alternate_text = selected->text;
                enlarged.coordinate_passes_agree = false;
            } else if (complete_decimal(selected->alternate_text) &&
                       enlarged.text != selected->alternate_text) {
                enlarged.alternate_text = selected->alternate_text;
                enlarged.coordinate_passes_agree = false;
            }
            return enlarged;
        }
        {
            std::ostringstream evidence;
            evidence << "ocr.map_axis_pass axis=" << static_cast<char>(expected)
                     << " width=" << source.width << " height=" << source.height
                     << " magnified=" << magnified << " font=" << selected_font
                     << " confidence=" << selected->confidence
                     << " minimum_confidence=" << selected->minimum_confidence
                     << " labeled=" << selected->map_axes_labeled
                     << " passes_agree=" << selected->coordinate_passes_agree
                     << " glyph_count_matches=" << selected->coordinate_glyph_count_matches
                     << " clipped=" << selected->coordinate_boundary_clipped;
            log_info(evidence.str());
        }
        return *selected;
    };
    const auto read_axis = [&](const Image& source, wchar_t expected) {
        try {
            return recognize_axis(recognize_axis, source, expected, false);
        } catch (const std::exception& error) {
            if (!stop.stop_requested()) {
                std::ostringstream failure;
                failure << "ocr.map_axis_failed axis=" << static_cast<char>(expected)
                        << " error=" << error.what();
                log_warning(failure.str());
            }
            throw;
        }
    };
    const auto x = read_axis(x_field, L'x');
    const auto y = read_axis(y_field, L'y');
    OcrResult result;
    result.text = L"x" + x.text + L",y" + y.text;
    result.alternate_text = L"x" + x.alternate_text + L",y" + y.alternate_text;
    const auto x_count = visible_characters(x.text), y_count = visible_characters(y.text);
    result.confidence = (x.confidence * static_cast<float>(x_count) +
                         y.confidence * static_cast<float>(y_count)) / static_cast<float>(x_count + y_count);
    result.minimum_confidence = std::min(x.minimum_confidence, y.minimum_confidence);
    result.line_count = 1;
    result.isolated_coordinate_pair = parse_ocr_coordinates(result.text).size() == 1;
    result.coordinate_boundary_clipped = x.coordinate_boundary_clipped || y.coordinate_boundary_clipped;
    result.coordinate_passes_agree = x.coordinate_passes_agree && y.coordinate_passes_agree;
    result.coordinate_glyph_count_matches = x.coordinate_glyph_count_matches && y.coordinate_glyph_count_matches;
    result.map_axes_labeled = x.map_axes_labeled && y.map_axes_labeled;
    // The two rectangles are in independent axis-capture coordinate spaces;
    // combining them into one misleading bounding box would lose provenance.
    return result;
}

OcrResult RapidOcr::recognize_map_neighborhood(const Image& image,
                                              const MapOcrSearchLayout& layout,
                                              std::stop_token stop) const {
    if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
    validate_ocr_image(image);
    const auto valid_rect = [&](const ImageRect& rect) {
        return rect.left >= 0 && rect.top >= 0 && rect.right <= image.width &&
               rect.bottom <= image.height && rect.right > rect.left && rect.bottom > rect.top;
    };
    if (image.width > 4096 || image.height > 4096 ||
        static_cast<std::size_t>(image.width) * image.height > 4'000'000 ||
        !valid_rect(layout.x_prior) || !valid_rect(layout.y_prior) ||
        layout.cursor_x < 0 || layout.cursor_y < 0 ||
        layout.cursor_x >= image.width || layout.cursor_y >= image.height ||
        !std::isfinite(layout.scale) || layout.scale <= 0.0 || layout.scale > 16.0)
        throw std::invalid_argument("Неверная геометрия области поиска координат карты.");

    struct AxisEvidence { OcrResult result; TextLine line; };
    std::optional<AxisEvidence> x, y;
    // Finding every line precedes selection: an easy preferred field must not
    // hide a second intact semantic coordinate elsewhere in this screenshot.
    auto lines = locate_text_lines(image, stop, L'x', false, nullptr, 24, &layout);
    std::erase_if(lines, [&](const auto& line) {
        return line.glyphs < 2 || line.glyphs > 24 ||
               line.font_height < 5 || line.font_height > 64 * layout.scale ||
               line.ink.right - line.ink.left > 240 * layout.scale;
    });
    if (lines.size() > 12)
        throw std::invalid_argument("Вокруг отметки слишком много подписей. Переместите курсор и повторите отметку.");
    const auto overlaps = [](const ImageRect& a, const ImageRect& b) {
        return std::min(a.right, b.right) > std::max(a.left, b.left) &&
               std::min(a.bottom, b.bottom) > std::max(a.top, b.top);
    };
    // Prefer the known layout for latency only. All remaining rows still run
    // before a coordinate is returned, including captions at displaced labels.
    std::stable_sort(lines.begin(), lines.end(), [&](const auto& a, const auto& b) {
        const bool a_prior = overlaps(a.ink, layout.x_prior) || overlaps(a.ink, layout.y_prior);
        const bool b_prior = overlaps(b.ink, layout.x_prior) || overlaps(b.ink, layout.y_prior);
        return a_prior && !b_prior;
    });
    for (const auto& line : lines) {
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        const int padding = std::max({4, line.font_height / 3,
                                     (20 - (line.ink.bottom - line.ink.top) + 1) / 2});
        auto result = recognize(crop_text_line(image, line, padding), stop);
        const auto labeled_axis = [](const OcrResult& value) -> wchar_t {
            wchar_t found = 0;
            for (const wchar_t axis : {L'x', L'y'}) {
                const auto primary = parse_map_axis(value.text, axis);
                const auto alternate = parse_map_axis(value.alternate_text, axis);
                if ((primary && primary->labeled) || (alternate && alternate->labeled)) {
                    if (found) throw std::invalid_argument("Подпись карты содержит противоречащие оси координат.");
                    found = axis;
                }
            }
            return found;
        };
        const wchar_t axis = labeled_axis(result);
        if (!axis) {
            // Buildings, player names and PING are not numeric axes. A row
            // beginning with X/Y and digits remains damaged competing evidence.
            if (map_axis_prefix(result.text) || map_axis_prefix(result.alternate_text))
                throw std::invalid_argument("В области карты найдена неполная подпись X/Y. Повторите отметку.");
            continue;
        }
        const auto agrees = [&](const OcrResult& value) {
            const auto primary = parse_map_axis(value.text, axis);
            const auto alternate = parse_map_axis(value.alternate_text, axis);
            return primary && alternate && primary->labeled && alternate->labeled &&
                   primary->number == alternate->number &&
                   visible_characters(value.text) == line.glyphs &&
                   visible_characters(value.alternate_text) == line.glyphs;
        };
        if (!agrees(result) || result.confidence < 0.90F || result.minimum_confidence < 0.65F) {
            auto retry = recognize(crop_text_line(image, line,
                std::max(padding + 1, line.font_height / 2)), stop);
            (void)labeled_axis(retry);
            if (agrees(retry) && (!agrees(result) || retry.minimum_confidence > result.minimum_confidence)) {
                const auto chosen = parse_map_axis(retry.text, axis);
                for (const auto* previous : {&result.text, &result.alternate_text}) {
                    const auto value = parse_map_axis(*previous, axis);
                    if (value && chosen && value->number != chosen->number) {
                        retry.alternate_text = *previous;
                        break;
                    }
                }
                result = std::move(retry);
            }
        }
        const auto primary = parse_map_axis(result.text, axis);
        const auto alternate = parse_map_axis(result.alternate_text, axis);
        if (!primary || !primary->labeled)
            throw std::invalid_argument("Не удалось прочитать полную подпись X/Y с двумя дробными цифрами.");
        auto& selected = axis == L'x' ? x : y;
        if (selected)
            throw std::invalid_argument("Рядом с отметкой найдены несколько подписей одной оси. Повторите отметку.");
        result.coordinate_passes_agree = primary && alternate && alternate->labeled &&
                                        primary->number == alternate->number;
        result.coordinate_glyph_count_matches = visible_characters(result.text) == line.glyphs &&
                                                visible_characters(result.alternate_text) == line.glyphs;
        result.map_axes_labeled = primary->labeled && alternate && alternate->labeled;
        result.coordinate_boundary_clipped = line.clipped;
        result.text = primary->number;
        if (alternate) result.alternate_text = alternate->number;
        selected = AxisEvidence{std::move(result), line};
    }
    if (!x || !y)
        throw std::invalid_argument("Не найдены обе полные подписи X/Y рядом с отметкой. Уберите всплывающую подпись и повторите отметку.");
    const auto center_x = [](const auto& r) { return (r.left + r.right) / 2.0; };
    const auto center_y = [](const auto& r) { return (r.top + r.bottom) / 2.0; };
    if (overlaps(x->line.ink, y->line.ink) ||
        std::abs(center_x(x->line.ink) - center_x(y->line.ink)) > 200 * layout.scale ||
        std::abs(center_y(x->line.ink) - center_y(y->line.ink)) > 240 * layout.scale ||
        std::min(x->line.font_height, y->line.font_height) * 2 <
            std::max(x->line.font_height, y->line.font_height))
        throw std::invalid_argument("Подписи X/Y не образуют одну пару координат рядом с отметкой.");
    OcrResult result;
    result.text = L"x" + x->result.text + L",y" + y->result.text;
    result.alternate_text = L"x" + x->result.alternate_text + L",y" + y->result.alternate_text;
    const auto x_count = visible_characters(x->result.text), y_count = visible_characters(y->result.text);
    result.confidence = (x->result.confidence * static_cast<float>(x_count) +
                         y->result.confidence * static_cast<float>(y_count)) /
                        static_cast<float>(x_count + y_count);
    result.minimum_confidence = std::min(x->result.minimum_confidence, y->result.minimum_confidence);
    result.line_count = 1;
    result.map_axes_labeled = x->result.map_axes_labeled && y->result.map_axes_labeled;
    result.isolated_coordinate_pair = parse_ocr_coordinates(result.text).size() == 1;
    result.coordinate_passes_agree = x->result.coordinate_passes_agree && y->result.coordinate_passes_agree;
    result.coordinate_glyph_count_matches = x->result.coordinate_glyph_count_matches &&
                                             y->result.coordinate_glyph_count_matches;
    result.coordinate_boundary_clipped = x->result.coordinate_boundary_clipped ||
                                          y->result.coordinate_boundary_clipped;
    result.map_x_bounds = x->line.ink;
    result.map_y_bounds = y->line.ink;
    std::ostringstream evidence;
    evidence << "ocr.map_neighborhood width=" << image.width << " height=" << image.height
             << " candidate_lines=" << lines.size() << " confidence=" << result.confidence
             << " minimum_confidence=" << result.minimum_confidence
             << " labeled=" << result.map_axes_labeled << " passes_agree=" << result.coordinate_passes_agree
             << " glyph_count_matches=" << result.coordinate_glyph_count_matches
             << " clipped=" << result.coordinate_boundary_clipped
             << " x_bounds=" << result.map_x_bounds->left << ',' << result.map_x_bounds->top << ','
             << result.map_x_bounds->right << ',' << result.map_x_bounds->bottom
             << " y_bounds=" << result.map_y_bounds->left << ',' << result.map_y_bounds->top << ','
             << result.map_y_bounds->right << ',' << result.map_y_bounds->bottom;
    log_info(evidence.str());
    return result;
}

}  // namespace wardogs
