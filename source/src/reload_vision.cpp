#include "wardogs/reload_vision.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace wardogs {
namespace {

constexpr std::uint8_t white = 1;
constexpr std::uint8_t green = 2;
constexpr std::uint8_t visited = 4;
constexpr int minimum_glyph = 7;
constexpr int maximum_glyph = 256;

struct Glyph {
    ImageRect bounds;
    ReloadArrow arrow;
    double score{};
    double x() const { return (bounds.left + bounds.right) * 0.5; }
    double y() const { return (bounds.top + bounds.bottom) * 0.5; }
    int extent() const { return std::max(bounds.right - bounds.left, bounds.bottom - bounds.top); }
};

std::uint8_t colour(const std::uint8_t* bgr) {
    const int b = bgr[0], g = bgr[1], r = bgr[2];
    // Keep the masks separate: green arrows can touch a neutral outline
    // through antialiasing and must not become part of the scenery component.
    if (g >= 95 && g * 4 > r * 5 && g * 4 > b * 5) return green;
    if (std::min({r, g, b}) >= 145 &&
        std::max({r, g, b}) - std::min({r, g, b}) <= 45) return white;
    return 0;
}

bool template_pixel(double x, double y, int direction, double head, double stem) {
    switch (direction) {
    case 1: { const double old_x = x; x = y; y = 1.0 - old_x; break; }
    case 2: x = 1.0 - x; y = 1.0 - y; break;
    case 3: { const double old_x = x; x = 1.0 - y; y = old_x; break; }
    default: break;
    }
    return std::abs(x - 0.5) <= (y < head ? y / (2.0 * head) : stem * 0.5);
}

std::optional<Glyph> classify(const std::vector<std::uint8_t>& mask, int stride,
                             ImageRect box, std::size_t pixels, std::uint8_t kind) {
    const int w = box.right - box.left, h = box.bottom - box.top;
    if (w < minimum_glyph || h < minimum_glyph ||
        w > maximum_glyph || h > maximum_glyph) return std::nullopt;
    const double occupancy = static_cast<double>(pixels) / (w * h);
    if (occupancy < 0.35 || occupancy > 0.78) return std::nullopt;
    const double aspect = static_cast<double>(w) / h;
    if (aspect < 0.55 || aspect > 1.85) return std::nullopt;
    std::array<double, 4> scores{};
    // Sampling a fixed lattice bounds the work for large HUD scales, and
    // normalizes all four directions independently of their pixel dimensions.
    constexpr int samples = 28;
    for (int direction = 0; direction < 4; ++direction) {
        if ((direction % 2 == 0 && aspect > 1.1) ||
            (direction % 2 == 1 && aspect < 0.9)) continue;
        for (const double head : {0.45, 0.5, 0.55}) {
            for (const double stem : {0.38, 0.44, 0.5}) {
                int intersection = 0, united = 0;
                for (int sy = 0; sy < samples; ++sy) {
                    const double v = (sy + 0.5) / samples;
                    const int iy = std::min(h - 1, static_cast<int>(v * h));
                    for (int sx = 0; sx < samples; ++sx) {
                        const double u = (sx + 0.5) / samples;
                        const int ix = std::min(w - 1, static_cast<int>(u * w));
                        const bool actual = (mask[static_cast<std::size_t>(box.top + iy) * stride + box.left + ix] & 3) == kind;
                        const bool expected = template_pixel(u, v, direction, head, stem);
                        intersection += actual && expected;
                        united += actual || expected;
                    }
                }
                if (united) scores[direction] = std::max(scores[direction],
                    static_cast<double>(intersection) / united);
            }
        }
    }
    const auto best = std::max_element(scores.begin(), scores.end());
    const auto direction = static_cast<int>(best - scores.begin());
    const double score = *best;
    scores[direction] = 0.0;
    const double runner_up = *std::max_element(scores.begin(), scores.end());
    // Sub-18px silhouettes lose entire edge pixels when the HUD is resampled.
    // Preserve a positive directional margin while tolerating that quantizing.
    const double minimum_margin = std::max(w, h) <= 18 ? 0.07 : 0.10;
    if (score < 0.72 || score - runner_up < minimum_margin) return std::nullopt;
    return Glyph{box, {static_cast<ReloadDirection>(direction), kind == green}, score};
}

bool same_row(const Glyph& first, const Glyph& second) {
    const int extent = std::max(first.extent(), second.extent());
    return std::abs(first.y() - second.y()) <= extent * 0.24 &&
        std::min(first.extent(), second.extent()) >= extent * 0.70;
}

void detach_joined_white(const std::vector<std::uint8_t>& mask, int width, int height,
                         std::vector<Glyph>& glyphs) {
    // A neutral arrow can touch a neutral outline or scenery by a narrow
    // antialiased bridge. Three independent silhouettes define a small strip
    // to inspect. Opening separates components and discards the bridge; the
    // remaining original foreground must pass the template score/margin.
    // No missing direction is inferred from its neighbors.
    if (glyphs.size() < 3 || glyphs.size() > 24) return;
    const auto independent = glyphs;
    std::vector<ImageRect> inspected;
    for (std::size_t a = 0; a < independent.size(); ++a) {
        for (std::size_t b = a + 1; b < independent.size(); ++b) {
            for (std::size_t c = b + 1; c < independent.size(); ++c) {
                std::array<const Glyph*, 3> seed{&independent[a], &independent[b], &independent[c]};
                std::sort(seed.begin(), seed.end(), [](const Glyph* x, const Glyph* y) { return x->x() < y->x(); });
                if (!same_row(*seed[0], *seed[1]) || !same_row(*seed[0], *seed[2])) continue;
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
                inspected.push_back(roi);
                if (inspected.size() > 12) return;
                const int rw = roi.right - roi.left, rh = roi.bottom - roi.top;
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
                    }
                }
            }
        }
    }
}

}  // namespace

static std::vector<ReloadObservation> find_reload_rows(const Image& image) {
    constexpr std::uint64_t maximum_pixels = 40'000'000;
    if (image.width < 1 || image.height < 1 || image.width > 16384 || image.height > 8192)
        return {};
    const auto pixel_count = static_cast<std::uint64_t>(image.width) * image.height;
    if (pixel_count > maximum_pixels || image.bgr.size() != pixel_count * 3)
        return {};
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(pixel_count));
    for (std::size_t i = 0; i < mask.size(); ++i) mask[i] = colour(image.bgr.data() + i * 3);

    std::vector<Glyph> glyphs;
    std::vector<int> stack;
    std::vector<int> component;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const int start = y * image.width + x;
            const auto kind = mask[start];
            if (!kind || (kind & visited)) continue;
            ImageRect box{x, y, x + 1, y + 1};
            std::size_t pixels = 0;
            stack.clear(); component.clear(); stack.push_back(start); mask[start] |= visited;
            while (!stack.empty()) {
                const int index = stack.back(); stack.pop_back();
                const int cx = index % image.width, cy = index / image.width;
                ++pixels;
                if (component.size() <= maximum_glyph * maximum_glyph) component.push_back(index);
                box.left = std::min(box.left, cx); box.right = std::max(box.right, cx + 1);
                box.top = std::min(box.top, cy); box.bottom = std::max(box.bottom, cy + 1);
                const auto push = [&](int next) {
                    if (mask[next] == kind) { mask[next] |= visited; stack.push_back(next); }
                };
                if (cx) push(index - 1);
                if (cx + 1 < image.width) push(index + 1);
                if (cy) push(index - image.width);
                if (cy + 1 < image.height) push(index + image.width);
            }
            // A glyph touching the image edge may be a cropped direction.
            if (box.left == 0 || box.top == 0 || box.right == image.width || box.bottom == image.height)
                continue;
            const int w = box.right - box.left, h = box.bottom - box.top;
            if (w < minimum_glyph || h < minimum_glyph || w > maximum_glyph || h > maximum_glyph ||
                component.size() > maximum_glyph * maximum_glyph) continue;
            // Similar-colored scenery inside a glyph's bounding box is a
            // DIFFERENT connected component. Sampling the global color mask
            // would incorrectly add those pixels to the arrow silhouette.
            std::vector<std::uint8_t> silhouette(static_cast<std::size_t>(w) * h);
            for (const int index : component)
                silhouette[static_cast<std::size_t>(index / image.width - box.top) * w + index % image.width - box.left] = kind;
            if (auto glyph = classify(silhouette, w, {0, 0, w, h}, pixels, kind)) {
                glyph->bounds = box;
                glyphs.push_back(*glyph);
                if (glyphs.size() > 512) return {};
            }
        }
    }
    detach_joined_white(mask, image.width, image.height, glyphs);
    if (glyphs.size() < 4) return {};
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
                if (result.size() > 32) return {};
            }
        }
    }
    return result;
}

std::optional<ReloadObservation> detect_reload_sequence(const Image& image) {
    const auto rows = find_reload_rows(image);
    if (rows.size() != 1) return std::nullopt;
    return rows.front();
}

}  // namespace wardogs
