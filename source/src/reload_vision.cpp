#include "wardogs/reload_vision.hpp"
#include "reload_pixel_mask.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <emmintrin.h>
#include <limits>
#include <vector>

namespace wardogs {

std::uint8_t reload_pixel_class(const std::uint8_t* bgr) noexcept {
    return detail::reload_pixel_class_exact(bgr);
}

namespace {

constexpr std::uint8_t white = 1;
constexpr std::uint8_t green = 2;
constexpr std::uint8_t visited = 4;
constexpr int minimum_glyph = 7;
constexpr int maximum_glyph = 256;
constexpr std::size_t maximum_glyphs = 512;

struct Glyph {
    ImageRect bounds;
    ReloadArrow arrow;
    double score{};
    double x() const { return (bounds.left + bounds.right) * 0.5; }
    double y() const { return (bounds.top + bounds.bottom) * 0.5; }
    int extent() const { return std::max(bounds.right - bounds.left, bounds.bottom - bounds.top); }
};

bool template_pixel(double x, double y, int direction, double head, double stem) {
    switch (direction) {
    case 1: { const double old_x = x; x = y; y = 1.0 - old_x; break; }
    case 2: x = 1.0 - x; y = 1.0 - y; break;
    case 3: { const double old_x = x; x = 1.0 - y; y = old_x; break; }
    default: break;
    }
    return std::abs(x - 0.5) <= (y < head ? y / (2.0 * head) : stem * 0.5);
}

constexpr int template_samples = 28;
constexpr std::size_t template_words = (template_samples * template_samples + 63) / 64;
using ShapeBits = std::array<std::uint64_t, template_words>;
using DirectionTemplates = std::array<std::array<ShapeBits, 9>, 4>;

const DirectionTemplates& direction_templates() {
    // These normalized silhouettes are independent of capture size and colour.
    // Build them once instead of rebuilding 36 templates for every component.
    static const auto shapes = [] {
        DirectionTemplates result{};
        for (int direction = 0; direction < 4; ++direction) {
            std::size_t variant = 0;
            for (const double head : {0.45, 0.5, 0.55}) {
                for (const double stem : {0.38, 0.44, 0.5}) {
                    auto& bits = result[direction][variant++];
                    for (int y = 0; y < template_samples; ++y) {
                        for (int x = 0; x < template_samples; ++x) {
                            const auto index = static_cast<std::size_t>(y * template_samples + x);
                            if (template_pixel((x + 0.5) / template_samples,
                                    (y + 0.5) / template_samples, direction, head, stem))
                                bits[index / 64] |= std::uint64_t{1} << (index % 64);
                        }
                    }
                }
            }
        }
        return result;
    }();
    return shapes;
}

std::optional<Glyph> classify(const std::vector<std::uint8_t>& mask, int stride,
                             ImageRect box, std::size_t pixels, std::uint8_t kind, bool candidate_only = false) {
    const int w = box.right - box.left, h = box.bottom - box.top;
    if (w < minimum_glyph || h < minimum_glyph ||
        w > maximum_glyph || h > maximum_glyph) return std::nullopt;
    const double occupancy = static_cast<double>(pixels) / (w * h);
    if (occupancy < (candidate_only ? 0.1 : 0.35) || occupancy > (candidate_only ? 1.0 : 0.78)) return std::nullopt;
    const double aspect = static_cast<double>(w) / h;
    if (aspect < (candidate_only ? 0.4 : 0.55) || aspect > (candidate_only ? 2.5 : 1.85)) return std::nullopt;
    ShapeBits actual{};
    for (int y = 0; y < template_samples; ++y) {
        const int iy = std::min(h - 1, static_cast<int>((y + 0.5) / template_samples * h));
        for (int x = 0; x < template_samples; ++x) {
            const int ix = std::min(w - 1, static_cast<int>((x + 0.5) / template_samples * w));
            if ((mask[static_cast<std::size_t>(box.top + iy) * stride + box.left + ix] & 3) != kind) continue;
            const auto index = static_cast<std::size_t>(y * template_samples + x);
            actual[index / 64] |= std::uint64_t{1} << (index % 64);
        }
    }
    std::array<double, 4> scores{};
    const auto& templates = direction_templates();
    for (int direction = 0; direction < 4; ++direction) {
        if ((direction % 2 == 0 && aspect > 1.1) ||
            (direction % 2 == 1 && aspect < 0.9)) continue;
        for (const auto& expected : templates[direction]) {
            int intersection = 0, united = 0;
            for (std::size_t i = 0; i < template_words; ++i) {
                intersection += std::popcount(actual[i] & expected[i]);
                united += std::popcount(actual[i] | expected[i]);
            }
            if (united) scores[direction] = std::max(scores[direction],
                static_cast<double>(intersection) / united);
        }
    }
    const auto best = std::max_element(scores.begin(), scores.end());
    const auto direction = static_cast<int>(best - scores.begin());
    const double score = *best;
    scores[direction] = 0.0;
    const double runner_up = *std::max_element(scores.begin(), scores.end());
    const double minimum_margin = candidate_only ? ((aspect < 0.8 || aspect > 1.25) ? 0.0 : 0.025) : (std::max(w, h) <= 18 ? 0.07 : 0.10);
    if (score < (candidate_only ? 0.45 : 0.72) || score - runner_up < minimum_margin) return std::nullopt;
    return Glyph{box, {static_cast<ReloadDirection>(direction), kind == green}, score};
}

struct Span { int y, left, right; };
struct Component {
    ImageRect bounds;
    std::size_t pixels{};
    std::uint8_t kind{};
    std::vector<Span> spans;
    bool oversized{};
};

int next_mask_pixel(const std::uint8_t* row, int begin, int end, std::uint8_t kind) {
    const auto expected = _mm_set1_epi8(static_cast<char>(kind));
    const auto whites = _mm_set1_epi8(white), greens = _mm_set1_epi8(green);
    for (; begin + 16 <= end; begin += 16) {
        const auto pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + begin));
        const auto matches = kind ? _mm_cmpeq_epi8(pixels, expected)
            : _mm_or_si128(_mm_cmpeq_epi8(pixels, whites), _mm_cmpeq_epi8(pixels, greens));
        const auto bits = static_cast<unsigned long>(_mm_movemask_epi8(matches));
        if (!bits) continue;
        unsigned long offset{};
        _BitScanForward(&offset, bits);
        return begin + static_cast<int>(offset);
    }
    for (; begin < end; ++begin)
        if (kind ? row[begin] == kind : (row[begin] == white || row[begin] == green)) return begin;
    return end;
}

template<class Visitor>
bool scan_components(std::vector<std::uint8_t>& mask, int width, int height, Visitor visit) {
    std::vector<Span> pending;
    Component component;
    for (int y = 0; y < height; ++y) {
        const auto* outer_row = mask.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            x = next_mask_pixel(outer_row, x, width, 0);
            if (x == width) break;
            const auto kind = outer_row[x];
            component.bounds = {x, y, x + 1, y + 1};
            component.pixels = 0; component.kind = kind;
            component.spans.clear(); component.oversized = false;
            pending.clear();
            const auto add_span = [&](int px, int py) {
                auto* row = mask.data() + static_cast<std::size_t>(py) * width;
                int left = px, right = px + 1;
                const auto expected = _mm_set1_epi8(static_cast<char>(kind));
                while (left >= 16 && _mm_movemask_epi8(_mm_cmpeq_epi8(
                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + left - 16)), expected)) == 0xffff)
                    left -= 16;
                while (left > 0 && row[left - 1] == kind) --left;
                while (right + 16 <= width && _mm_movemask_epi8(_mm_cmpeq_epi8(
                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + right)), expected)) == 0xffff)
                    right += 16;
                while (right < width && row[right] == kind) ++right;
                std::fill(row + left, row + right, static_cast<std::uint8_t>(kind | visited));
                component.bounds.left = std::min(component.bounds.left, left);
                component.bounds.right = std::max(component.bounds.right, right);
                component.bounds.top = std::min(component.bounds.top, py);
                component.bounds.bottom = std::max(component.bounds.bottom, py + 1);
                component.pixels += static_cast<std::size_t>(right - left);
                if (!component.oversized && (component.pixels > maximum_glyph * maximum_glyph ||
                        component.bounds.right - component.bounds.left > maximum_glyph ||
                        component.bounds.bottom - component.bounds.top > maximum_glyph)) {
                    component.oversized = true;
                    component.spans.clear();
                }
                const Span span{py, left, right};
                if (!component.oversized) component.spans.push_back(span);
                pending.push_back(span);
                return right;
            };
            add_span(x, y);
            while (!pending.empty()) {
                const auto span = pending.back(); pending.pop_back();
                for (const int adjacent : {span.y - 1, span.y + 1}) {
                    if (adjacent < 0 || adjacent >= height) continue;
                    auto* row = mask.data() + static_cast<std::size_t>(adjacent) * width;
                    for (int px = span.left; px < span.right; ) {
                        px = next_mask_pixel(row, px, span.right, kind);
                        if (px < span.right) px = add_span(px, adjacent);
                    }
                }
            }
            if (!visit(component)) return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> silhouette(const Component& component, int radius = 0) {
    const auto& box = component.bounds;
    const int width = box.right - box.left + radius * 2;
    const int height = box.bottom - box.top + radius * 2;
    std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height);
    for (const auto& span : component.spans) {
        for (int dy = -radius; dy <= radius; ++dy) {
            auto* row = result.data() + static_cast<std::size_t>(span.y - box.top + radius + dy) * width;
            std::fill(row + span.left - box.left, row + span.right - box.left + radius * 2, component.kind);
        }
    }
    return result;
}

bool same_row(const Glyph& first, const Glyph& second) {
    const int extent = std::max(first.extent(), second.extent());
    return std::abs(first.y() - second.y()) <= extent * 0.24 &&
        std::min(first.extent(), second.extent()) >= extent * 0.70;
}

int brightness(const Image& image, int x, int y) {
    const auto* value = image.bgr.data() + (static_cast<std::size_t>(y) * image.width + x) * 3;
    return (29 * value[0] + 150 * value[1] + 77 * value[2]) >> 8;
}

bool supported_outline(const Image& image, const std::vector<std::uint8_t>& shape,
                       ImageRect box, std::uint8_t kind, int radius) {
    // This is independent evidence from the original pixels: most of the
    // reconstructed boundary must have a darker visible outline/background.
    // A broad relaxed-colour mask alone is never sufficient for acceptance.
    const int width = box.right - box.left, height = box.bottom - box.top;
    int boundary = 0, supported = 0;
    constexpr std::array<std::array<int, 2>, 4> directions{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (shape[static_cast<std::size_t>(y) * width + x] != kind) continue;
            const int inside = brightness(image, box.left + x, box.top + y);
            for (const auto& direction : directions) {
                const int nx = x + direction[0], ny = y + direction[1];
                if (nx >= 0 && nx < width && ny >= 0 && ny < height &&
                    shape[static_cast<std::size_t>(ny) * width + nx] == kind) continue;
                ++boundary;
                int outside = 255;
                for (int distance = 1; distance <= radius + 2; ++distance) {
                    const int px = box.left + x + direction[0] * distance;
                    const int py = box.top + y + direction[1] * distance;
                    if (px < 0 || py < 0 || px >= image.width || py >= image.height) break;
                    outside = std::min(outside, brightness(image, px, py));
                }
                if (inside - outside >= 32) ++supported;
            }
        }
    }
    return boundary >= minimum_glyph * 2 && supported * 5 >= boundary * 3;
}

std::vector<std::uint8_t> erode_white(const std::vector<std::uint8_t>& mask,
                                    int width, int height, int radius) {
    std::vector<std::uint8_t> horizontal(mask.size());
    std::vector<std::uint8_t> core(mask.size());
    if (width <= radius * 2 || height <= radius * 2) return core;
    // Class 1 (with or without the visited bit) is exactly the low bit. SSE2
    // is baseline on x64. Separable erosion is linear in pixels, with no
    // per-pixel flood stack and no (2r+1)^2 neighbourhood loops.
    const auto white_bit = _mm_set1_epi8(1);
    for (int y = 0; y < height; ++y) {
        const auto* source = mask.data() + static_cast<std::size_t>(y) * width;
        auto* target = horizontal.data() + static_cast<std::size_t>(y) * width;
        int x = radius;
        for (; x + 16 <= width - radius; x += 16) {
            auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + x - radius));
            // An all-zero factor makes the entire erosion block zero. The
            // destination was zero-initialized, so no remaining loads/stores
            // are needed for dark/background-only blocks.
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(value, white_bit), _mm_setzero_si128())) == 0xffff)
                continue;
            for (int dx = 1 - radius; dx <= radius; ++dx)
                value = _mm_and_si128(value, _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + x + dx)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(target + x), _mm_and_si128(value, white_bit));
        }
        for (; x < width - radius; ++x) {
            std::uint8_t value = 1;
            for (int dx = -radius; dx <= radius; ++dx) value &= source[x + dx];
            target[x] = value & 1;
        }
    }
    for (int y = radius; y < height - radius; ++y) {
        auto* target = core.data() + static_cast<std::size_t>(y) * width;
        int x = 0;
        for (; x + 16 <= width; x += 16) {
            auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(horizontal.data() +
                static_cast<std::size_t>(y - radius) * width + x));
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(value, _mm_setzero_si128())) == 0xffff) continue;
            for (int dy = 1 - radius; dy <= radius; ++dy)
                value = _mm_and_si128(value, _mm_loadu_si128(reinterpret_cast<const __m128i*>(horizontal.data() +
                    static_cast<std::size_t>(y + dy) * width + x)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(target + x), value);
        }
        for (; x < width; ++x) {
            std::uint8_t value = 1;
            for (int dy = -radius; dy <= radius; ++dy)
                value &= horizontal[static_cast<std::size_t>(y + dy) * width + x];
            target[x] = value;
        }
    }
    return core;
}

bool append_contour_glyph(const Glyph& glyph, std::vector<Glyph>& glyphs,
                          ReloadVisionDiagnostics* diagnostics) {
    // Multiple scales may recover the same physical contour. Keep one reading
    // when they agree; preserve conflicting directions for row ambiguity.
    const auto duplicate = std::find_if(glyphs.begin(), glyphs.end(), [&](const Glyph& other) {
        const int extent = std::max(glyph.extent(), other.extent());
        return glyph.arrow == other.arrow &&
            std::abs(glyph.x() - other.x()) < extent * 0.30 &&
            std::abs(glyph.y() - other.y()) < extent * 0.30 &&
            std::min(glyph.extent(), other.extent()) >= extent * 0.65;
    });
    if (duplicate != glyphs.end()) return true;
    glyphs.push_back(glyph);
    if (diagnostics) ++diagnostics->contour_glyphs;
    return glyphs.size() <= maximum_glyphs;
}

std::optional<Glyph> refine_small_contour(const Image& image,
    const std::vector<std::uint8_t>& mask, const Component& component) {
    // A 1px opening can remove a tiny arrow's complete pointed tip. Grow only
    // ORIGINAL foreground reachable from that actual core, for four bounded
    // steps. Unrelated light scenery inside the box is never sampled as fill.
    constexpr int padding = 4;
    const ImageRect area{std::max(0, component.bounds.left - padding),
        std::max(0, component.bounds.top - padding),
        std::min(image.width, component.bounds.right + padding),
        std::min(image.height, component.bounds.bottom + padding)};
    const int width = area.right - area.left, height = area.bottom - area.top;
    std::vector<std::uint8_t> connected(static_cast<std::size_t>(width) * height);
    for (const auto& span : component.spans)
        std::fill(connected.begin() + static_cast<std::ptrdiff_t>(span.y - area.top) * width + span.left - area.left,
                  connected.begin() + static_cast<std::ptrdiff_t>(span.y - area.top) * width + span.right - area.left,
                  component.kind);
    std::array<std::optional<Glyph>, 4> readings;
    for (int step = 0; step < padding; ++step) {
        auto grown = connected;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * width + x;
            if (connected[index] || (mask[static_cast<std::size_t>(area.top + y) * image.width + area.left + x] & 3) != component.kind)
                continue;
            if ((x && connected[index - 1]) || (x + 1 < width && connected[index + 1]) ||
                (y && connected[index - width]) || (y + 1 < height && connected[index + width])) grown[index] = component.kind;
        }
        connected.swap(grown);
        ImageRect extent{width, height, 0, 0};
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            if (!connected[static_cast<std::size_t>(y) * width + x]) continue;
            extent.left = std::min(extent.left, x); extent.right = std::max(extent.right, x + 1);
            extent.top = std::min(extent.top, y); extent.bottom = std::max(extent.bottom, y + 1);
        }
        // A notch may also grow a short spur. Compare a fixed set of boundary
        // trims, retaining independent directional evidence across every fit.
        for (int variant = 0; variant < 9; ++variant) {
            auto trimmed = extent;
            const int amount = variant ? (variant - 1) % 2 + 1 : 0;
            if (variant == 1 || variant == 2) trimmed.left += amount;
            if (variant == 3 || variant == 4) trimmed.right -= amount;
            if (variant == 5 || variant == 6) trimmed.top += amount;
            if (variant == 7 || variant == 8) trimmed.bottom -= amount;
            const int w = trimmed.right - trimmed.left, h = trimmed.bottom - trimmed.top;
            if (w < minimum_glyph || h < minimum_glyph) continue;
            std::vector<std::uint8_t> shape(static_cast<std::size_t>(w) * h);
            std::size_t pixels = 0;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const auto value = connected[static_cast<std::size_t>(trimmed.top + y) * width + trimmed.left + x];
                shape[static_cast<std::size_t>(y) * w + x] = value;
                pixels += value != 0;
            }
            auto glyph = classify(shape, w, {0, 0, w, h}, pixels, component.kind);
            const ImageRect box{area.left + trimmed.left, area.top + trimmed.top,
                area.left + trimmed.right, area.top + trimmed.bottom};
            if (!glyph || box.left <= 0 || box.top <= 0 || box.right >= image.width || box.bottom >= image.height ||
                !supported_outline(image, shape, box, component.kind, 0)) continue;
            glyph->bounds = box;
            auto& reading = readings[static_cast<int>(glyph->arrow.direction)];
            if (!reading || glyph->score > reading->score) reading = *glyph;
        }
    }
    std::optional<Glyph> best;
    double runner_up = 0;
    for (const auto& reading : readings) {
        if (!reading) continue;
        if (!best || reading->score > best->score) {
            if (best) runner_up = best->score;
            best = reading;
        } else runner_up = std::max(runner_up, reading->score);
    }
    if (best && best->score - runner_up < (best->extent() <= 18 ? 0.07 : 0.10)) return std::nullopt;
    return best;
}

struct SmallContourCore { Component component; Glyph estimate; };

std::vector<bool> row_supported_cores(const std::vector<SmallContourCore>& cores,
                                     const std::vector<Glyph>& glyphs) {
    // Refinement work is selected by the four-cell geometry, not by the count
    // of tiny text/icons elsewhere in the capture. In particular, a longer
    // text line is not arbitrarily shortened to four candidate directions.
    struct Seed { Glyph glyph; std::size_t core; };
    constexpr auto known = std::numeric_limits<std::size_t>::max();
    std::vector<Seed> seeds;
    for (const auto& glyph : glyphs) seeds.push_back({glyph, known});
    for (std::size_t i = 0; i < cores.size(); ++i) seeds.push_back({cores[i].estimate, i});
    std::sort(seeds.begin(), seeds.end(), [](const Seed& a, const Seed& b) { return a.glyph.x() < b.glyph.x(); });
    const auto coarse_row = [](const Glyph& first, const Glyph& second) {
        const int extent = std::max(first.extent(), second.extent());
        return std::abs(first.y() - second.y()) <= extent * 0.55 &&
            std::min(first.extent(), second.extent()) >= extent * 0.45;
    };
    std::vector<bool> selected(cores.size());
    for (std::size_t a = 0; a < seeds.size(); ++a) {
        for (std::size_t b = a + 1; b < seeds.size(); ++b) {
            const auto& first = seeds[a].glyph;
            const double step = seeds[b].glyph.x() - first.x();
            if (step > first.extent() * 5.0) break;
            if (step < first.extent() * 0.65 || !coarse_row(first, seeds[b].glyph)) continue;
            std::array<const Seed*, 4> row{&seeds[a], &seeds[b], nullptr, nullptr};
            bool valid = true;
            for (int item = 2; item < 4; ++item) {
                const double target = first.x() + item * step;
                auto candidate = std::lower_bound(seeds.begin(), seeds.end(), target - step * 0.30,
                    [](const Seed& seed, double x) { return seed.glyph.x() < x; });
                for (; candidate != seeds.end() && candidate->glyph.x() <= target + step * 0.30; ++candidate) {
                    if (!coarse_row(first, candidate->glyph)) continue;
                    if (row[item]) { valid = false; break; }
                    row[item] = &*candidate;
                }
            }
            if (!valid || !row[2] || !row[3]) continue;
            if (valid) for (const auto* seed : row) if (seed->core != known) selected[seed->core] = true;
        }
    }
    return selected;
}

std::vector<ReloadObservation> rows_from_glyphs(const Image& image,
    std::vector<Glyph>& glyphs, ReloadVisionDiagnostics* diagnostics);

enum class ContourRecoveryStatus { complete, glyph_limit, work_limit, ambiguous };

ContourRecoveryStatus recover_contrast_contours(const Image& image, std::vector<std::uint8_t>& mask,
                               std::vector<Glyph>& glyphs, ReloadVisionDiagnostics* diagnostics) {
    // This separate candidate mask admits dim neutral interiors, but each
    // accepted component must additionally pass original-image outline support.
    // Exact white/green classification and primary matching stay unchanged.
    constexpr std::size_t maximum_candidates = 4096;
    constexpr std::size_t maximum_candidate_pixels = 4'000'000;
    constexpr std::size_t maximum_small_refinements = 256;
    std::size_t candidates = 0, candidate_pixels = 0, small_refinements = 0;
    bool limited = false;
    std::vector<ReloadObservation> canonical_rows;
    const auto remember_rows = [&] {
        const auto rows = rows_from_glyphs(image, glyphs, nullptr);
        for (const auto& row : rows) {
            const bool duplicate = std::any_of(canonical_rows.begin(), canonical_rows.end(), [&](const ReloadObservation& previous) {
                const int tolerance = std::max(2, (row.bounds.bottom - row.bounds.top) / 6);
                return row.arrows == previous.arrows && std::abs(row.bounds.left - previous.bounds.left) <= tolerance &&
                    std::abs(row.bounds.right - previous.bounds.right) <= tolerance &&
                    std::abs(row.bounds.top - previous.bounds.top) <= tolerance &&
                    std::abs(row.bounds.bottom - previous.bounds.bottom) <= tolerance;
            });
            if (!duplicate) canonical_rows.push_back(row);
        }
        return canonical_rows.size() > 1;
    };
    if (remember_rows()) return ContourRecoveryStatus::ambiguous;
    std::vector<std::uint8_t> next_mask(mask.size());
    bool previous_mask_processed = false;
    std::size_t previous_start_glyphs = 0;
    const auto read_level = [&](int threshold) -> ContourRecoveryStatus {
        if (threshold == 145) detail::build_reload_pixel_mask(image.bgr.data(), next_mask.size(), next_mask.data());
        else detail::build_reload_contour_candidate_mask(image.bgr.data(), next_mask.size(), next_mask.data(),
            static_cast<std::uint8_t>(threshold));
        // Different colour cuts frequently produce the exact same mask. Reuse
        // its completed result only when the glyph context also stayed stable:
        // new glyphs could otherwise enable an earlier tiny-core row fit.
        if (previous_mask_processed && glyphs.size() == previous_start_glyphs && next_mask == mask)
            return ContourRecoveryStatus::complete;
        mask.swap(next_mask);
        previous_mask_processed = true;
        previous_start_glyphs = glyphs.size();
        for (const int radius : {0, 1, 2, 4}) {
            if (diagnostics) ++diagnostics->contour_passes;
            auto core = radius ? erode_white(mask, image.width, image.height, radius) : mask;
            std::vector<SmallContourCore> small_cores;
            const bool complete = scan_components(core, image.width, image.height, [&](const Component& component) {
                auto box = component.bounds;
                if (component.oversized || box.left <= radius || box.top <= radius ||
                    box.right >= image.width - radius || box.bottom >= image.height - radius) return true;
                box = {box.left - radius, box.top - radius, box.right + radius, box.bottom + radius};
                const int w = box.right - box.left, h = box.bottom - box.top;
                if (w < minimum_glyph || h < minimum_glyph || w > maximum_glyph || h > maximum_glyph ||
                    (radius && std::max(w, h) < radius * 12)) return true;
                const auto shape = silhouette(component, radius);
                const auto pixels = static_cast<std::size_t>(std::count(shape.begin(), shape.end(), component.kind));
                const auto area = static_cast<std::size_t>(w) * h;
                const double occupancy = static_cast<double>(pixels) / area;
                const double aspect = static_cast<double>(w) / h;
                const bool tiny_refinement = radius == 1 && std::max(w, h) <= 20;
                // A tiny eroded core can be rectangular after losing its entire
                // triangle tip. Only the final ORIGINAL-pixel fit uses the strict
                // occupancy/direction thresholds; this is merely a work prefilter.
                if (!tiny_refinement && (occupancy < 0.35 || occupancy > 0.78 ||
                        aspect < 0.55 || aspect > 1.85)) return true;
                const bool known = tiny_refinement && std::any_of(glyphs.begin(), glyphs.end(), [&](const Glyph& other) {
                    const int extent = std::max(std::max(w, h), other.extent());
                    return std::abs((box.left + box.right) * 0.5 - other.x()) < extent * 0.25 &&
                        std::abs((box.top + box.bottom) * 0.5 - other.y()) < extent * 0.25 &&
                        std::min(std::max(w, h), other.extent()) >= extent * 0.65;
                });
                auto glyph = classify(shape, w, {0, 0, w, h}, pixels, component.kind);
                // Candidate-only matching never accepts an arrow. The tiny
                // candidates must form a four-cell row before original-pixel fits.
                const auto coarse = tiny_refinement && !known && !glyph
                    ? classify(shape, w, {0, 0, w, h}, pixels, component.kind, true) : std::nullopt;
                if (!glyph && !coarse) return true;
                if (++candidates > maximum_candidates || area > maximum_candidate_pixels - candidate_pixels) {
                    limited = true;
                    return false;
                }
                candidate_pixels += area;
                if (coarse) {
                    auto estimate = *coarse;
                    estimate.bounds = box;
                    small_cores.push_back({component, estimate});
                    if (small_cores.size() > maximum_glyphs) { limited = true; return false; }
                }
                if (!glyph || !supported_outline(image, shape, box, component.kind, radius)) return true;
                glyph->bounds = box;
                return append_contour_glyph(*glyph, glyphs, diagnostics);
            });
            if (!complete) {
                if (limited && diagnostics) diagnostics->recovery_budget_exhausted = true;
                return limited ? ContourRecoveryStatus::work_limit : ContourRecoveryStatus::glyph_limit;
            }
            const auto selected = row_supported_cores(small_cores, glyphs);
            for (std::size_t i = 0; i < small_cores.size(); ++i) {
                if (!selected[i]) continue;
                if (++small_refinements > maximum_small_refinements) {
                    if (diagnostics) diagnostics->recovery_budget_exhausted = true;
                    return ContourRecoveryStatus::work_limit;
                }
                if (const auto refined = refine_small_contour(image, mask, small_cores[i].component))
                    if (!append_contour_glyph(*refined, glyphs, diagnostics)) return ContourRecoveryStatus::glyph_limit;
            }
            if (remember_rows()) return ContourRecoveryStatus::ambiguous;
        }
        return ContourRecoveryStatus::complete;
    };
    for (const int threshold : {145, 64}) {
        const auto status = read_level(threshold);
        if (status != ContourRecoveryStatus::complete) return status;
    }
    // Additional levels are a fallback on the SAME pixels. Ordinary white HUDs
    // pay for only the two base masks. A contrast of >=32 has an intermediate
    // cut in this fixed 32-level grid, including dim fills and grey outlines.
    if (canonical_rows.empty()) {
        for (const int threshold : {96, 128, 160, 192, 224}) {
            const auto status = read_level(threshold);
            if (status != ContourRecoveryStatus::complete) return status;
        }
    }
    return ContourRecoveryStatus::complete;
}

void detach_joined_white(const std::vector<std::uint8_t>& mask, int width, int height,
                         std::vector<Glyph>& glyphs, ReloadVisionDiagnostics* diagnostics) {
    // A neutral arrow can touch a neutral outline or scenery by a narrow
    // antialiased bridge. Three independent silhouettes define a small strip
    // to inspect. Opening separates components and discards the bridge; the
    // remaining original foreground must pass the template score/margin.
    // No missing direction is inferred from its neighbors.
    if (glyphs.size() < 3) return;
    auto independent = glyphs;
    std::sort(independent.begin(), independent.end(), [](const Glyph& a, const Glyph& b) { return a.x() < b.x(); });
    // Search only a third silhouette at a plausible local spacing. Unrelated
    // glyphs elsewhere no longer disable recovery, or create a cubic scan.
    // These explicit budgets bound cluttered frames independently of scene size.
    constexpr std::size_t maximum_seed_candidates = 4096;
    constexpr std::size_t maximum_rois = 32;
    constexpr std::size_t maximum_roi_pixels = 4'000'000;
    std::size_t seed_candidates = 0, roi_pixels = 0;
    std::vector<ImageRect> inspected;
    const auto exhausted = [&] {
        if (diagnostics) diagnostics->recovery_budget_exhausted = true;
    };
    for (std::size_t a = 0; a < independent.size(); ++a) {
        for (std::size_t b = a + 1; b < independent.size(); ++b) {
            const double gap = independent[b].x() - independent[a].x();
            // same_row permits up to a 1/0.70 extent ratio; a valid pair can
            // span at most 2.15 * 3 times that larger extent.
            if (gap > independent[a].extent() * 10.0) break;
            if (gap < std::max(independent[a].extent(), independent[b].extent()) ||
                !same_row(independent[a], independent[b])) continue;
            const std::array<std::array<double, 2>, 3> gaps{{
                {gap / 2.15, gap / 1.85}, {gap / 1.15, gap * 1.15}, {gap * 1.85, gap * 2.15}}};
            for (const auto& range : gaps) {
                const auto begin = std::lower_bound(independent.begin() + static_cast<std::ptrdiff_t>(b + 1),
                    independent.end(), independent[b].x() + range[0],
                    [](const Glyph& glyph, double x) { return glyph.x() < x; });
                for (auto third = begin; third != independent.end() && third->x() <= independent[b].x() + range[1]; ++third) {
                    if (++seed_candidates > maximum_seed_candidates) { exhausted(); return; }
                    std::array<const Glyph*, 3> seed{&independent[a], &independent[b], &*third};
                    if (!same_row(*seed[0], *seed[2])) continue;
                    const double first_gap = seed[1]->x() - seed[0]->x();
                    const double last_gap = seed[2]->x() - seed[1]->x();
                    const double step = std::min(first_gap, last_gap);
                    const int extent = std::max({seed[0]->extent(), seed[1]->extent(), seed[2]->extent()});
                    if (step < extent || step > extent * 3.0 ||
                        std::abs(std::max(first_gap, last_gap) / step -
                                 std::round(std::max(first_gap, last_gap) / step)) > 0.15 ||
                        std::max(first_gap, last_gap) > step * 2.15) continue;
                    const double center_y = (seed[0]->y() + seed[1]->y() + seed[2]->y()) / 3.0;
                    ImageRect roi{std::max(0, static_cast<int>(seed[0]->x() - step - extent)),
                        std::max(0, static_cast<int>(center_y - extent)),
                        std::min(width, static_cast<int>(seed[2]->x() + step + extent + 1)),
                        std::min(height, static_cast<int>(center_y + extent + 1))};
                    if (std::any_of(inspected.begin(), inspected.end(), [&](const ImageRect& previous) {
                        return std::abs(previous.left - roi.left) < extent / 2 &&
                            std::abs(previous.top - roi.top) < extent / 2 &&
                            std::abs(previous.right - roi.right) < extent / 2;
                    })) continue;
                    const int rw = roi.right - roi.left, rh = roi.bottom - roi.top;
                    const auto area = static_cast<std::size_t>(rw) * rh;
                    if (inspected.size() >= maximum_rois || area > maximum_roi_pixels - roi_pixels) {
                        exhausted(); return;
                    }
                    inspected.push_back(roi);
                    roi_pixels += area;
                    if (diagnostics) ++diagnostics->joined_recovery_rois;
                    const int radius = std::clamp(static_cast<int>(std::round(extent / 22.0)), 1, 6);
                    std::vector<std::uint8_t> eroded(static_cast<std::size_t>(rw) * rh);
                    for (int y = radius; y + radius < rh; ++y) {
                        for (int x = radius; x + radius < rw; ++x) {
                            bool solid = true;
                            for (int dy = -radius; dy <= radius && solid; ++dy)
                                for (int dx = -radius; dx <= radius; ++dx)
                                    if ((mask[static_cast<std::size_t>(roi.top + y + dy) * width + roi.left + x + dx] & 3) != white) {
                                        solid = false; break;
                                    }
                            eroded[static_cast<std::size_t>(y) * rw + x] = solid ? white : 0;
                        }
                    }
                    std::vector<int> stack;
                    std::vector<int> core;
                    for (int y = 0; y < rh; ++y) {
                        for (int x = 0; x < rw; ++x) {
                            const int start = y * rw + x;
                            if (eroded[start] != white) continue;
                            ImageRect box{x, y, x + 1, y + 1};
                            stack.clear(); stack.push_back(start); eroded[start] |= visited;
                            core.clear();
                            while (!stack.empty()) {
                                const int index = stack.back(); stack.pop_back();
                                if (core.size() <= maximum_glyph * maximum_glyph) core.push_back(index);
                                const int px = index % rw, py = index / rw;
                                box.left = std::min(box.left, px); box.right = std::max(box.right, px + 1);
                                box.top = std::min(box.top, py); box.bottom = std::max(box.bottom, py + 1);
                                const auto push = [&](int next) {
                                    if (eroded[next] == white) { eroded[next] |= visited; stack.push_back(next); }
                                };
                                if (px) push(index - 1);
                                if (px + 1 < rw) push(index + 1);
                                if (py) push(index - rw);
                                if (py + 1 < rh) push(index + rw);
                            }
                            if (box.left <= radius || box.top <= radius ||
                                box.right >= rw - radius || box.bottom >= rh - radius ||
                                core.size() > maximum_glyph * maximum_glyph) continue;
                            box = {box.left + roi.left - radius, box.top + roi.top - radius,
                                   box.right + roi.left + radius, box.bottom + roi.top + radius};
                            const int gw = box.right - box.left, gh = box.bottom - box.top;
                            if (gw > maximum_glyph || gh > maximum_glyph) continue;
                            std::vector<std::uint8_t> opening(static_cast<std::size_t>(gw) * gh);
                            std::size_t pixels = 0;
                            for (const int index : core) {
                                const int px = index % rw + roi.left - box.left;
                                const int py = index / rw + roi.top - box.top;
                                for (int dy = -radius; dy <= radius; ++dy) {
                                    for (int dx = -radius; dx <= radius; ++dx) {
                                        auto& pixel = opening[static_cast<std::size_t>(py + dy) * gw + px + dx];
                                        if (!pixel) { pixel = white; ++pixels; }
                                    }
                                }
                            }
                            auto glyph = classify(opening, gw, {0, 0, gw, gh}, pixels, white);
                            if (glyph) glyph->bounds = box;
                            if (!glyph || !same_row(*seed[0], *glyph)) continue;
                            if (std::any_of(glyphs.begin(), glyphs.end(), [&](const Glyph& existing) {
                                return std::abs(existing.x() - glyph->x()) < extent * 0.35 &&
                                    std::abs(existing.y() - glyph->y()) < extent * 0.35;
                            })) continue;
                            glyphs.push_back(*glyph);
                            if (glyphs.size() > maximum_glyphs) return;
                        }
                    }
                }
            }
        }
    }
}

std::vector<ReloadObservation> rows_from_glyphs(const Image& image,
    std::vector<Glyph>& glyphs, ReloadVisionDiagnostics* diagnostics) {
    if (glyphs.size() < 4) {
        if (diagnostics) diagnostics->failure = ReloadDetectionFailure::not_enough_glyphs;
        return {};
    }
    if (diagnostics) diagnostics->failure = ReloadDetectionFailure::no_row;
    std::sort(glyphs.begin(), glyphs.end(), [](const Glyph& a, const Glyph& b) { return a.x() < b.x(); });
    std::vector<ReloadObservation> result;
    for (std::size_t first = 0; first < glyphs.size(); ++first) {
        const auto& origin = glyphs[first];
        for (std::size_t second = first + 1; second < glyphs.size(); ++second) {
            const auto& next = glyphs[second];
            const double step = next.x() - origin.x();
            if (step > origin.extent() * 3.0) break;
            if (step < origin.extent() * 1.0 || !same_row(origin, next)) continue;
            std::array<const Glyph*, 4> row{&origin, &next, nullptr, nullptr};
            bool ambiguous = false;
            for (int item = 2; item < 4; ++item) {
                for (const auto& candidate : glyphs) {
                    if (!same_row(origin, candidate) ||
                        std::abs(candidate.x() - (origin.x() + item * step)) > step * 0.18) continue;
                    if (row[item]) { ambiguous = true; break; }
                    row[item] = &candidate;
                }
            }
            if (ambiguous || !row[2] || !row[3]) continue;
            bool pending = false, valid = true;
            for (const auto* glyph : row) {
                if (!glyph->arrow.accepted) pending = true;
                else if (pending) valid = false;
            }
            // Do not silently pick four out of a longer same-line sequence.
            for (const auto& glyph : glyphs) {
                if (!same_row(origin, glyph)) continue;
                if (std::find(row.begin(), row.end(), &glyph) == row.end() &&
                    glyph.x() >= origin.x() - step * 0.75 &&
                    glyph.x() <= row[3]->x() + step * 0.75) valid = false;
                if (std::abs(glyph.x() - (origin.x() - step)) < step * 0.25 ||
                    std::abs(glyph.x() - (row[3]->x() + step)) < step * 0.25) valid = false;
            }
            if (!valid) continue;
            const int margin = std::max(2, origin.extent() / 5);
            ImageRect bounds{row[0]->bounds.left, row[0]->bounds.top,
                             row[3]->bounds.right, row[0]->bounds.bottom};
            double confidence = 1.0;
            std::array<ReloadArrow, 4> arrows;
            for (std::size_t i = 0; i < row.size(); ++i) {
                bounds.top = std::min(bounds.top, row[i]->bounds.top);
                bounds.bottom = std::max(bounds.bottom, row[i]->bounds.bottom);
                arrows[i] = row[i]->arrow;
                confidence = std::min(confidence, row[i]->score);
            }
            bounds = {std::max(0, bounds.left - margin), std::max(0, bounds.top - margin),
                      std::min(image.width, bounds.right + margin), std::min(image.height, bounds.bottom + margin)};
            const bool duplicate = std::any_of(result.begin(), result.end(), [&](const auto& old) {
                return std::abs(old.bounds.left - bounds.left) <= 2 && std::abs(old.bounds.top - bounds.top) <= 2 &&
                    std::abs(old.bounds.right - bounds.right) <= 2 && std::abs(old.bounds.bottom - bounds.bottom) <= 2 &&
                    old.arrows == arrows;
            });
            if (!duplicate) {
                result.push_back(ReloadObservation{bounds, arrows, confidence});
                if (result.size() > 32) {
                    if (diagnostics) {
                        diagnostics->rows = result.size();
                        diagnostics->failure = ReloadDetectionFailure::ambiguous_rows;
                    }
                    return result;
                }
            }
        }
    }
    if (diagnostics) {
        diagnostics->rows = result.size();
        diagnostics->failure = result.empty() ? ReloadDetectionFailure::no_row
            : result.size() > 1 ? ReloadDetectionFailure::ambiguous_rows : ReloadDetectionFailure::none;
    }
    return result;
}

}  // namespace

static std::vector<ReloadObservation> find_reload_rows(const Image& image, ReloadVisionDiagnostics* diagnostics) {
    constexpr std::uint64_t maximum_pixels = 40'000'000;
    if (image.width < 1 || image.height < 1 || image.width > 16384 || image.height > 8192) {
        if (diagnostics) diagnostics->failure = ReloadDetectionFailure::invalid_image;
        return {};
    }
    const auto pixel_count = static_cast<std::uint64_t>(image.width) * image.height;
    if (pixel_count > maximum_pixels || image.bgr.size() != pixel_count * 3) {
        if (diagnostics) diagnostics->failure = ReloadDetectionFailure::invalid_image;
        return {};
    }
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(pixel_count));
    detail::build_reload_pixel_mask(image.bgr.data(), mask.size(), mask.data());

    std::vector<Glyph> glyphs;
    const auto record_glyphs = [&] {
        if (!diagnostics) return;
        diagnostics->glyphs = glyphs.size();
        diagnostics->glyph_bounds.clear();
        diagnostics->glyph_bounds.reserve(std::min(glyphs.size(), maximum_glyphs));
        for (std::size_t i = 0; i < std::min(glyphs.size(), maximum_glyphs); ++i)
            diagnostics->glyph_bounds.push_back(glyphs[i].bounds);
    };
    const bool scanned = scan_components(mask, image.width, image.height, [&](const Component& component) {
        if (diagnostics) ++diagnostics->components;
        const auto& box = component.bounds;
        if (component.oversized || box.left == 0 || box.top == 0 ||
            box.right == image.width || box.bottom == image.height) return true;
        const int w = box.right - box.left, h = box.bottom - box.top;
        if (w < minimum_glyph || h < minimum_glyph) return true;
        const auto shape = silhouette(component);
        if (auto glyph = classify(shape, w, {0, 0, w, h}, component.pixels, component.kind)) {
            glyph->bounds = box;
            glyphs.push_back(*glyph);
            if (glyphs.size() > maximum_glyphs) return false;
        }
        return true;
    });
    if (!scanned) {
        if (diagnostics) diagnostics->failure = ReloadDetectionFailure::glyph_limit;
        record_glyphs();
        return {};
    }
    detach_joined_white(mask, image.width, image.height, glyphs, diagnostics);
    const auto contour_status = recover_contrast_contours(image, mask, glyphs, diagnostics);
    if (contour_status != ContourRecoveryStatus::complete) {
        if (diagnostics) {
            diagnostics->failure = contour_status == ContourRecoveryStatus::glyph_limit
                ? ReloadDetectionFailure::glyph_limit : contour_status == ContourRecoveryStatus::ambiguous
                    ? ReloadDetectionFailure::ambiguous_rows : ReloadDetectionFailure::no_row;
            if (contour_status == ContourRecoveryStatus::ambiguous) diagnostics->rows = 2;
        }
        // Partial work must not silently pick one row before ambiguity checks
        // have completed. The explicit diagnostic flag identifies work limits.
        record_glyphs();
        return {};
    }
    record_glyphs();
    if (glyphs.size() > maximum_glyphs) {
        if (diagnostics) diagnostics->failure = ReloadDetectionFailure::glyph_limit;
        return {};
    }
    return rows_from_glyphs(image, glyphs, diagnostics);
}

const char* reload_detection_failure_name(ReloadDetectionFailure failure) noexcept {
    switch (failure) {
    case ReloadDetectionFailure::none: return "none";
    case ReloadDetectionFailure::invalid_image: return "invalid_image";
    case ReloadDetectionFailure::glyph_limit: return "glyph_limit";
    case ReloadDetectionFailure::not_enough_glyphs: return "not_enough_glyphs";
    case ReloadDetectionFailure::no_row: return "no_row";
    case ReloadDetectionFailure::ambiguous_rows: return "ambiguous_rows";
    }
    return "unknown";
}

std::optional<ReloadObservation> detect_reload_sequence(const Image& image, ReloadVisionDiagnostics* diagnostics) {
    if (diagnostics) *diagnostics = {};
    const auto rows = find_reload_rows(image, diagnostics);
    if (rows.size() != 1) return std::nullopt;
    return rows.front();
}

}  // namespace wardogs
