#include "wardogs/sph_hud.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <string_view>
#include <utility>

namespace wardogs {
namespace {

enum class ScaleKind { mil, range, compass };

bool finite_point(Point point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool valid_rect(ImageRect rect, int width, int height) {
    return rect.left >= 0 && rect.top >= 0 && rect.right > rect.left &&
        rect.bottom > rect.top && rect.right <= width && rect.bottom <= height;
}

bool contains(ImageRect outer, ImageRect inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
        inner.right <= outer.right && inner.bottom <= outer.bottom;
}

std::wstring normalized(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    std::wstring value(text);
    for (auto& ch : value) {
        ch = static_cast<wchar_t>(std::towlower(ch));
        if (ch == L'\u2212') ch = L'-';
    }
    return value;
}

struct Number {
    double value{};
    std::wstring unit;
};

bool digit(wchar_t ch) { return ch >= L'0' && ch <= L'9'; }

std::optional<Number> number(std::wstring_view raw) {
    const auto text = normalized(raw);
    if (text.empty() || text.size() > 40) return std::nullopt;
    std::size_t end{};
    if (text.front() == L'-' || text.front() == L'+') ++end;
    const auto begin = end;
    while (end < text.size() && (digit(text[end]) || text[end] == L',' ||
                               text[end] == L'.')) ++end;
    if (end == begin || !digit(text[begin]) || !digit(text[end - 1])) return std::nullopt;
    auto numeric = text.substr(begin, end - begin);
    const auto unit = normalized(std::wstring_view(text).substr(end));
    // Comma groups of exactly three digits are the game's thousands format.
    // Other single commas may be decimal separators; mixed formats are refused.
    if (numeric.find(L',') != std::wstring::npos) {
        if (numeric.find(L'.') != std::wstring::npos) return std::nullopt;
        const auto first = numeric.find(L',');
        bool thousands = first >= 1 && first <= 3;
        for (auto pos = first; pos != std::wstring::npos;) {
            const auto next = numeric.find(L',', pos + 1);
            if ((next == std::wstring::npos ? numeric.size() : next) - pos != 4)
                thousands = false;
            pos = next;
        }
        if (thousands) {
            numeric.erase(std::remove(numeric.begin(), numeric.end(), L','), numeric.end());
        } else {
            if (numeric.find(L',', first + 1) != std::wstring::npos ||
                numeric.size() - first > 3) return std::nullopt;
            numeric[first] = L'.';
        }
    }
    double value{}, decimal{1.0};
    bool fractional{};
    for (const auto ch : numeric) {
        if (ch == L'.') {
            if (fractional) return std::nullopt;
            fractional = true;
        } else if (digit(ch)) {
            if (fractional) { decimal *= 10.0; value += (ch - L'0') / decimal; }
            else value = value * 10.0 + (ch - L'0');
        } else return std::nullopt;
    }
    if (text.front() == L'-') value = -value;
    if (!std::isfinite(value) || std::abs(value) > 100'000.0) return std::nullopt;
    return Number{value, unit};
}

bool unit_matches(const Number& value, ScaleKind kind) {
    if (value.unit.empty()) return true;
    if (kind == ScaleKind::mil) return value.unit == L"mil" || value.unit == L"mils";
    if (kind == ScaleKind::range) return value.unit == L"m" || value.unit == L"metres";
    return value.unit == L"\u00b0" || value.unit == L"deg";
}

bool numeric_like(std::wstring_view text) {
    const auto value = normalized(text);
    return !value.empty() && (digit(value.front()) || value.front() == L'-' || value.front() == L'+');
}

std::wstring_view canonical_unit(std::wstring_view unit) {
    if (unit == L"mils") return L"mil";
    if (unit == L"metres") return L"m";
    if (unit == L"\u00b0") return L"deg";
    return unit;
}

bool readable(const OcrResult& value, SphHudProvenance& provenance) {
    if (!std::isfinite(value.confidence) || !std::isfinite(value.minimum_confidence) ||
        value.confidence < 0.0F || value.minimum_confidence < 0.0F ||
        value.confidence > 1.0F || value.minimum_confidence > 1.0F || value.line_count > 1)
        return false;
    if (value.confidence > 0.0F || value.minimum_confidence > 0.0F) {
        const auto score = value.minimum_confidence > 0.0F
            ? value.minimum_confidence : value.confidence;
        provenance.minimum_read_score = provenance.ocr_confidence_available
            ? std::min(provenance.minimum_read_score, score) : score;
        provenance.ocr_confidence_available = true;
        if ((value.confidence > 0.0F && value.confidence < 0.90F) || score < 0.70F) return false;
    }
    if (!value.alternate_text.empty() && normalized(value.text) != normalized(value.alternate_text)) {
        const auto first = number(value.text), second = number(value.alternate_text);
        if (!first || !second || first->value != second->value ||
            canonical_unit(first->unit) != canonical_unit(second->unit))
            return false;
    }
    return true;
}

bool header(const SphHudFrame& frame, ImageRect roi, std::wstring_view expected) {
    const auto weapon_literal = [](std::wstring_view value) {
        auto text = normalized(value);
        std::erase_if(text, [](wchar_t ch) { return std::iswspace(ch) != 0; });
        for (auto& ch : text)
            if (ch == L'\u2013' || ch == L'\u2014') ch = L'-';
        return text;
    };
    unsigned found{};
    for (const auto& token : frame.tokens) {
        auto provenance = SphHudProvenance{};
        if (!contains(roi, token.bounds)) continue;
        auto reading = token.reading;
        // Exact weapon characters are retained. OCR spacing and dash glyphs
        // may vary; a missing letter, digit or separator is never supplied.
        if (expected == L"sph-2") {
            reading.text = weapon_literal(reading.text);
            if (!reading.alternate_text.empty()) reading.alternate_text = weapon_literal(reading.alternate_text);
        }
        if (normalized(reading.text) == expected && token.geometry_verified && readable(reading, provenance)) ++found;
    }
    return found == 1;
}

SphHudRefusal collect_labels(const SphHudFrame& frame, const SphHudScaleLayout& scale,
                            ScaleKind kind, SphHudScaleReading& reading,
                            SphHudProvenance& provenance) {
    const bool vertical = kind != ScaleKind::compass;
    for (const auto& token : frame.tokens) {
        if (!contains(scale.labels, token.bounds)) continue;
        const auto parsed = number(token.reading.text);
        if (!parsed) {
            if (numeric_like(token.reading.text)) return kind == ScaleKind::mil
                ? SphHudRefusal::ambiguous_mil_labels : kind == ScaleKind::range
                ? SphHudRefusal::ambiguous_range_labels : SphHudRefusal::ambiguous_compass_labels;
            continue;
        }
        if (!token.geometry_verified || (kind == ScaleKind::compass &&
            token.bounds_source != SphHudTokenBoundsSource::ocr_word)) return SphHudRefusal::invalid_frame;
        if (!unit_matches(*parsed, kind) || (kind == ScaleKind::compass &&
            (parsed->value < 0.0 || parsed->value > 360.0)) ||
            (kind == ScaleKind::range && parsed->value <= 0.0)) return kind == ScaleKind::mil
                ? SphHudRefusal::ambiguous_mil_labels : kind == ScaleKind::range
                ? SphHudRefusal::ambiguous_range_labels : SphHudRefusal::ambiguous_compass_labels;
        if (!readable(token.reading, provenance)) return SphHudRefusal::low_read_confidence;
        const double centre = vertical ? (token.bounds.top + token.bounds.bottom) / 2.0
                                       : (token.bounds.left + token.bounds.right) / 2.0;
        const double extent = vertical ? token.bounds.bottom - token.bounds.top
                                       : token.bounds.right - token.bounds.left;
        // Boxes locate a label's neighbouring tick, not the tick's coordinate.
        // Require an unambiguous closest independently measured major tick.
        const SphHudTick* chosen{};
        double closest = std::numeric_limits<double>::infinity(), second = closest;
        for (const auto& tick : scale.ticks) {
            const auto distance = std::abs((vertical ? tick.position.y : tick.position.x) - centre);
            if (distance < closest) { second = closest; closest = distance; chosen = &tick; }
            else second = std::min(second, distance);
        }
        // Compass text can move to a different phase of the visible ruler.
        // Wide recognition crops cannot establish its absolute anchor. Keep
        // the word-to-major-tick bound independent of glyph/crop width.
        const double maximum_offset = kind == ScaleKind::compass
            ? 3.0 * frame.client_height / 720.0
            : std::max(2.0, extent * 0.75) + frame.layout.tick_uncertainty_px;
        if (!chosen || closest > maximum_offset ||
            second - closest <= frame.layout.tick_uncertainty_px * 2.0) return kind == ScaleKind::mil
                ? SphHudRefusal::ambiguous_mil_labels : kind == ScaleKind::range
                ? SphHudRefusal::ambiguous_range_labels : SphHudRefusal::ambiguous_compass_labels;
        if (std::any_of(reading.labels.begin(), reading.labels.end(), [&](const auto& label) {
            return label.tick == chosen->position;
        })) return kind == ScaleKind::mil ? SphHudRefusal::ambiguous_mil_labels
            : kind == ScaleKind::range ? SphHudRefusal::ambiguous_range_labels
            : SphHudRefusal::ambiguous_compass_labels;
        reading.labels.push_back({parsed->value, token.bounds, chosen->position, token.reading.text,
                                  kind == ScaleKind::compass ? closest : 0.0});
    }
    std::sort(reading.labels.begin(), reading.labels.end(), [vertical](const auto& first, const auto& second) {
        return (vertical ? first.tick.y : first.tick.x) < (vertical ? second.tick.y : second.tick.x);
    });
    if (reading.labels.size() < 2) return kind == ScaleKind::mil ? SphHudRefusal::missing_mil_labels
        : kind == ScaleKind::range ? SphHudRefusal::missing_range_labels
        : SphHudRefusal::missing_compass_labels;
    return SphHudRefusal::none;
}

bool verified_grid(const SphHudScaleLayout& scale, const SphHudScaleReading& reading,
                   ScaleKind kind) {
    if (kind == ScaleKind::range) return true;
    auto ticks = scale.ticks;
    const bool vertical = kind == ScaleKind::mil;
    std::sort(ticks.begin(), ticks.end(), [vertical](const auto& first, const auto& second) {
        return (vertical ? first.position.y : first.position.x) <
               (vertical ? second.position.y : second.position.x);
    });
    const auto index = [&](Point point) {
        return std::find_if(ticks.begin(), ticks.end(), [&](const auto& tick) {
            return tick.position == point;
        }) - ticks.begin();
    };
    // The observed SPH optic's labelled major grid is 10 MIL and 15 degrees.
    // This verifies printed grid semantics; it never converts MIL to bore angle.
    const double step = vertical ? 10.0 : 15.0;
    for (std::size_t i = 1; i < reading.labels.size(); ++i) {
        const auto& before = reading.labels[i - 1];
        const auto& after = reading.labels[i];
        const auto delta = vertical ? after.value - before.value
                                   : std::remainder(after.value - before.value, 360.0);
        if (std::abs(delta - step * (index(after.tick) - index(before.tick))) > 0.01) return false;
    }
    return true;
}

SphHudRefusal interpolate(SphHudScaleReading& reading, ScaleKind kind, double coordinate,
                         double pixel_uncertainty) {
    const bool vertical = kind != ScaleKind::compass;
    const auto position = [vertical](const auto& label) { return vertical ? label.tick.y : label.tick.x; };
    const auto inconsistent = kind == ScaleKind::mil ? SphHudRefusal::inconsistent_mil_scale
        : kind == ScaleKind::range ? SphHudRefusal::inconsistent_range_scale
        : SphHudRefusal::inconsistent_compass_scale;
    if (kind == ScaleKind::compass) {
        double maximum_anchor_offset{};
        for (const auto& label : reading.labels)
            maximum_anchor_offset = std::max(maximum_anchor_offset, label.anchor_offset_px);
        pixel_uncertainty += maximum_anchor_offset;
    }
    std::vector<double> values;
    values.reserve(reading.labels.size());
    for (const auto& label : reading.labels) values.push_back(label.value);
    // Unwrap only the calculation. Evidence retains each printed HUD value.
    if (kind == ScaleKind::compass) {
        for (std::size_t i = 1; i < values.size(); ++i) {
            const auto delta = std::remainder(values[i] - values[i - 1], 360.0);
            if (delta <= 0.0 || delta > 90.0) return inconsistent;
            values[i] = values[i - 1] + delta;
        }
    }
    const auto& first = reading.labels.front();
    const auto& last = reading.labels.back();
    if (coordinate < position(first) || coordinate > position(last)) return vertical
        ? SphHudRefusal::reticle_outside_labels : SphHudRefusal::compass_pointer_outside_labels;
    const double extent = position(last) - position(first);
    if (extent <= 2.0 * pixel_uncertainty) return inconsistent;
    const double slope = (values.back() - values.front()) / extent;
    if (kind != ScaleKind::range && slope <= 0.0) return inconsistent;
    double maximum_error{};
    for (std::size_t i = 1; i < reading.labels.size(); ++i) {
        const auto& before = reading.labels[i - 1];
        const auto& after = reading.labels[i];
        const auto gap = position(after) - position(before);
        const auto step = values[i] - values[i - 1];
        if (gap <= 2.0 * pixel_uncertainty ||
            (kind != ScaleKind::range && (step == 0.0 || step * slope <= 0.0))) return inconsistent;
        if (kind != ScaleKind::range) {
            const auto error = std::abs(values[i] - values.front() - slope * (position(after) - position(first)));
            maximum_error = std::max(maximum_error, error);
            if (error > std::abs(slope) * (pixel_uncertainty * 2.0 + 1.0)) return inconsistent;
        }
    }
    auto upper = std::upper_bound(reading.labels.begin(), reading.labels.end(), coordinate,
        [&](double value, const auto& label) { return value < position(label); });
    if (upper == reading.labels.end()) --upper;
    if (upper == reading.labels.begin()) ++upper;
    const auto& low = *(upper - 1);
    const auto& high = *upper;
    const auto high_index = static_cast<std::size_t>(upper - reading.labels.begin());
    const double local_slope = (values[high_index] - values[high_index - 1]) / (position(high) - position(low));
    reading.value = values[high_index - 1] + local_slope * (coordinate - position(low));
    reading.units_per_pixel = local_slope;
    reading.uncertainty = std::abs(local_slope) * pixel_uncertainty + maximum_error;
    if (kind == ScaleKind::compass) {
        reading.value = std::fmod(reading.value, 360.0);
        if (reading.value < 0.0) reading.value += 360.0;
    }
    return SphHudRefusal::none;
}

bool valid_scale(const SphHudScaleLayout& scale, int width, int height) {
    if (!valid_rect(scale.labels, width, height) || scale.ticks.size() < 3 || scale.ticks.size() > 32)
        return false;
    return std::all_of(scale.ticks.begin(), scale.ticks.end(), [&](const auto& tick) {
        return finite_point(tick.position) && tick.position.x >= 0.0 && tick.position.x < width &&
            tick.position.y >= 0.0 && tick.position.y < height;
    });
}

double gray(const Image& image, double x, double y) {
    const int ix = std::clamp(static_cast<int>(std::lround(x)), 0, image.width - 1);
    const int iy = std::clamp(static_cast<int>(std::lround(y)), 0, image.height - 1);
    const auto offset = (static_cast<std::size_t>(iy) * image.width + ix) * 3;
    return (image.bgr[offset] + image.bgr[offset + 1] + image.bgr[offset + 2]) / 3.0;
}

struct StrokeScore { double contrast{}; double matching_fraction{}; };

StrokeScore stroke(const Image& image, Point from, Point to, bool vertical, double scale) {
    const auto length = static_cast<int>(std::max(std::abs(to.x - from.x), std::abs(to.y - from.y)));
    double total{};
    unsigned matching{};
    for (int i = 0; i <= length; ++i) {
        const double ratio = length ? static_cast<double>(i) / length : 0.0;
        const double x = from.x + (to.x - from.x) * ratio;
        const double y = from.y + (to.y - from.y) * ratio;
        const double foreground = vertical
            ? (gray(image, x - 0.5 * scale, y) + gray(image, x + 0.5 * scale, y)) / 2.0
            : (gray(image, x, y - 0.5 * scale) + gray(image, x, y + 0.5 * scale)) / 2.0;
        const double background = vertical
            ? (gray(image, x - 4.0 * scale, y) + gray(image, x + 4.0 * scale, y)) / 2.0
            : (gray(image, x, y - 4.0 * scale) + gray(image, x, y + 4.0 * scale)) / 2.0;
        const double difference = foreground - background;
        total += difference;
        if (difference > 12.0) ++matching;
    }
    return {total / (length + 1), static_cast<double>(matching) / (length + 1)};
}

struct ReticleObservation { Point point; double uncertainty_px; };

std::optional<ReticleObservation> reticle(const Image& image, double scale) {
    struct Candidate { Point point; double score; };
    std::vector<Candidate> candidates;
    const Point hint{(image.width - 1) / 2.0, (image.height - 1) / 2.0};
    const int radius = std::max(3, static_cast<int>(std::ceil(8.0 * scale)));
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const Point centre{hint.x + dx, hint.y + dy};
            const auto top = centre.y - 40.0 * scale, bottom = centre.y + 40.0 * scale;
            const auto top_bar = stroke(image, {centre.x - 10.0 * scale, top},
                {centre.x + 10.0 * scale, top}, false, scale);
            if (top_bar.contrast < 20.0 || top_bar.matching_fraction < 0.80) continue;
            const auto bottom_bar = stroke(image, {centre.x - 10.0 * scale, bottom},
                {centre.x + 10.0 * scale, bottom}, false, scale);
            if (bottom_bar.contrast < 20.0 || bottom_bar.matching_fraction < 0.80) continue;
            const auto top_stem = stroke(image, {centre.x, top - 26.0 * scale},
                {centre.x, top - 3.0 * scale}, true, scale);
            if (top_stem.contrast < 20.0 || top_stem.matching_fraction < 0.80) continue;
            const auto bottom_stem = stroke(image, {centre.x, bottom + 3.0 * scale},
                {centre.x, bottom + 26.0 * scale}, true, scale);
            if (bottom_stem.contrast < 20.0 || bottom_stem.matching_fraction < 0.80) continue;
            candidates.push_back({centre, std::min({top_bar.contrast, bottom_bar.contrast,
                top_stem.contrast, bottom_stem.contrast})});
        }
    }
    if (candidates.empty()) return std::nullopt;
    const auto best = std::max_element(candidates.begin(), candidates.end(), [](const auto& first, const auto& second) {
        return first.score < second.score;
    });
    double left = best->point.x, right = left, top = best->point.y, bottom = top;
    for (const auto& candidate : candidates) {
        if (candidate.score < best->score * 0.90) continue;
        left = std::min(left, candidate.point.x); right = std::max(right, candidate.point.x);
        top = std::min(top, candidate.point.y); bottom = std::max(bottom, candidate.point.y);
    }
    // An occlusion can shift the winning template without hiding all strokes.
    // Keep its measured localisation interval, and refuse a wide competing
    // interval rather than assigning the nominal screen centre as a fix.
    if (right - left > 1.5 * scale || bottom - top > 1.5 * scale) return std::nullopt;
    const Point centre{(left + right) / 2.0, (top + bottom) / 2.0};
    const double uncertainty = std::max(right - left, bottom - top) / 2.0 + 0.5 * scale;
    return ReticleObservation{centre, std::max(0.75, uncertainty)};
}

ImageRect bounded_rect(const Image& image, double left, double top, double right, double bottom) {
    return {std::clamp(static_cast<int>(std::floor(left)), 0, image.width),
            std::clamp(static_cast<int>(std::floor(top)), 0, image.height),
            std::clamp(static_cast<int>(std::ceil(right)), 0, image.width),
            std::clamp(static_cast<int>(std::ceil(bottom)), 0, image.height)};
}

std::vector<SphHudTick> side_ticks(const Image& image, Point centre, double scale, bool right) {
    std::vector<Point> candidates;
    const double hint = centre.x + (right ? 180.0 : -176.0) * scale;
    const int radius = std::max(3, static_cast<int>(std::ceil(8.0 * scale)));
    const auto min_y = static_cast<int>(centre.y - 165.0 * scale);
    const auto max_y = static_cast<int>(centre.y + 165.0 * scale);
    for (int y = min_y; y <= max_y; ++y) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const auto x = hint + dx;
            const auto horizontal = right
                ? stroke(image, {x - 11.0 * scale, static_cast<double>(y)}, {x - 2.0 * scale, static_cast<double>(y)}, false, scale)
                : stroke(image, {x + 2.0 * scale, static_cast<double>(y)}, {x + 11.0 * scale, static_cast<double>(y)}, false, scale);
            if (horizontal.contrast < 50.0 || horizontal.matching_fraction < 0.80) continue;
            const auto vertical = stroke(image, {x, y - 6.0 * scale}, {x, y + 6.0 * scale}, true, scale);
            if (vertical.contrast >= 50.0 && vertical.matching_fraction >= 0.80)
                candidates.push_back({x, static_cast<double>(y)});
        }
    }
    std::vector<SphHudTick> ticks;
    std::sort(candidates.begin(), candidates.end(), [](Point first, Point second) { return first.y < second.y; });
    for (std::size_t i = 0; i < candidates.size();) {
        auto last = candidates[i];
        Point sum{};
        unsigned count{};
        while (i < candidates.size() && candidates[i].y - last.y <= 2.0 * scale) {
            last = candidates[i];
            sum.x += candidates[i].x; sum.y += candidates[i].y; ++count; ++i;
        }
        ticks.push_back({{sum.x / count, sum.y / count}});
    }
    if (ticks.size() != 3) return {};
    const auto first_gap = ticks[1].position.y - ticks[0].position.y;
    const auto second_gap = ticks[2].position.y - ticks[1].position.y;
    if (std::abs(first_gap - 88.0 * scale) > 4.0 * scale ||
        std::abs(second_gap - first_gap) > 3.0 * scale) return {};
    for (const auto& tick : ticks)
        if (std::abs(tick.position.x - ticks.front().position.x) > 3.0 * scale) return {};
    return ticks;
}

std::optional<SphHudScaleLayout> inner_compass(const Image& image, Point centre, double scale) {
    int selected_y{};
    double best{};
    for (int y = static_cast<int>(centre.y - 155.0 * scale);
         y <= static_cast<int>(centre.y - 135.0 * scale); ++y) {
        const auto score = stroke(image, {centre.x - 150.0 * scale, static_cast<double>(y)},
            {centre.x + 150.0 * scale, static_cast<double>(y)}, false, scale);
        if (score.matching_fraction >= 0.85 && score.contrast > best) {
            selected_y = y; best = score.contrast;
        }
    }
    if (best < 50.0) return std::nullopt;
    std::vector<int> columns;
    for (int x = static_cast<int>(centre.x - 166.0 * scale);
         x <= static_cast<int>(centre.x + 166.0 * scale); ++x) {
        const auto score = stroke(image, {static_cast<double>(x), selected_y - 14.0 * scale},
            {static_cast<double>(x), selected_y - 3.0 * scale}, true, scale);
        if (score.contrast >= 50.0 && score.matching_fraction >= 0.80) columns.push_back(x);
    }
    SphHudScaleLayout layout;
    for (std::size_t i = 0; i < columns.size();) {
        const int first = columns[i];
        int last = first;
        while (++i < columns.size() && columns[i] - last <= 2.0 * scale) last = columns[i];
        layout.ticks.push_back({{(first + last) / 2.0, static_cast<double>(selected_y)}});
    }
    // Trees and small UI ticks can produce isolated vertical strokes. Keep a
    // unique longest measured major-grid chain; a competing full chain is an
    // ambiguity, never an opportunity to prefer one set of numeric readings.
    std::vector<SphHudTick> grid;
    bool ambiguous{};
    for (std::size_t start = 0; start < layout.ticks.size(); ++start) {
        std::vector<SphHudTick> chain{layout.ticks[start]};
        for (std::size_t i = start + 1; i < layout.ticks.size(); ++i) {
            const auto distance = layout.ticks[i].position.x - chain.back().position.x;
            if (std::abs(distance - 55.5 * scale) <= 3.0 * scale) chain.push_back(layout.ticks[i]);
            else if (distance > 58.5 * scale) break;
        }
        if (chain.size() > grid.size()) { grid = std::move(chain); ambiguous = false; }
        else if (chain.size() == grid.size() && chain.size() >= 3 &&
                 std::abs(chain.front().position.x - grid.front().position.x) > 3.0 * scale)
            ambiguous = true;
    }
    if (grid.size() < 3 || grid.size() > 8 || ambiguous) return std::nullopt;
    layout.ticks = std::move(grid);
    layout.labels = bounded_rect(image, layout.ticks.front().position.x - 22.0 * scale,
        selected_y - 34.0 * scale, layout.ticks.back().position.x + 22.0 * scale, selected_y - 16.0 * scale);
    for (const auto& tick : layout.ticks)
        layout.label_regions.push_back(bounded_rect(image, tick.position.x - 21.0 * scale,
            selected_y - 34.0 * scale, tick.position.x + 21.0 * scale, selected_y - 16.0 * scale));
    return layout;
}

bool shell_icon(const Image& image, ImageRect roi, double scale) {
    struct Component { ImageRect bounds; std::vector<unsigned> rows; unsigned pixels; };
    const int width = roi.right - roi.left, height = roi.bottom - roi.top;
    std::vector<bool> visited(static_cast<std::size_t>(width) * height);
    std::vector<Component> components;
    for (int y = roi.top; y < roi.bottom; ++y) {
        for (int x = roi.left; x < roi.right; ++x) {
            const auto local = static_cast<std::size_t>(y - roi.top) * width + x - roi.left;
            if (visited[local] || gray(image, x, y) < 190.0) continue;
            std::vector<std::pair<int, int>> pending{{x, y}};
            visited[local] = true;
            Component component{{x, y, x + 1, y + 1}, std::vector<unsigned>(height), 0};
            for (std::size_t i = 0; i < pending.size(); ++i) {
                const auto [px, py] = pending[i];
                component.bounds.left = std::min(component.bounds.left, px);
                component.bounds.right = std::max(component.bounds.right, px + 1);
                component.bounds.top = std::min(component.bounds.top, py);
                component.bounds.bottom = std::max(component.bounds.bottom, py + 1);
                ++component.rows[py - roi.top]; ++component.pixels;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx, ny = py + dy;
                        if (nx < roi.left || nx >= roi.right || ny < roi.top || ny >= roi.bottom) continue;
                        const auto next = static_cast<std::size_t>(ny - roi.top) * width + nx - roi.left;
                        if (!visited[next] && gray(image, nx, ny) >= 190.0) {
                            visited[next] = true; pending.emplace_back(nx, ny);
                        }
                    }
            }
            components.push_back(std::move(component));
        }
    }
    unsigned matched{};
    for (const auto& tip : components) {
        const auto tw = tip.bounds.right - tip.bounds.left;
        const auto th = tip.bounds.bottom - tip.bounds.top;
        if (tw < 4.0 * scale || tw > 6.5 * scale || th < 9.0 * scale || th > 12.5 * scale) continue;
        const int quarter = std::max(1, th / 4);
        double upper{};
        for (int i = 0; i < quarter; ++i) {
            upper += tip.rows[tip.bounds.top - roi.top + i];
        }
        std::vector<unsigned> body;
        for (int y = tip.bounds.top + th / 2; y < tip.bounds.bottom; ++y)
            body.push_back(tip.rows[y - roi.top]);
        std::sort(body.begin(), body.end());
        const double lower = body.size() % 2 ? body[body.size() / 2]
            : (body[body.size() / 2 - 1] + body[body.size() / 2]) / 2.0;
        // Video compression can erode the terminal antialiased edge to one
        // pixel. A robust body median retains the taper without requiring that
        // single boundary row to have the full projectile width.
        if (upper / quarter > 2.2 * scale || lower < 3.5 * scale) continue;
        for (const auto& base : components) {
            const auto bw = base.bounds.right - base.bounds.left;
            const auto bh = base.bounds.bottom - base.bounds.top;
            const auto gap = base.bounds.top - tip.bounds.bottom;
            if (bw < 4.0 * scale || bw > 6.5 * scale || bh < 3.0 * scale || bh > 5.5 * scale ||
                gap < 0 || gap > 3.0 * scale || base.pixels < bw * bh * 0.70 ||
                std::abs((base.bounds.left + base.bounds.right - tip.bounds.left - tip.bounds.right) / 2.0) > 1.5 * scale)
                continue;
            ++matched;
        }
    }
    // A separate tapered projectile and aligned cartridge base identify this
    // channel; a bright rectangle, digit, MG icon or competing pair does not.
    return matched == 1;
}

bool valid_image(const Image& image) {
    return image.width >= 640 && image.height >= 360 && image.width <= 16384 && image.height <= 16384 &&
        static_cast<std::size_t>(image.width) * image.height <= 16'000'000 &&
        image.bgr.size() == static_cast<std::size_t>(image.width) * image.height * 3 &&
        image.width >= image.height * 1.3;
}

bool valid_frame(const SphHudFrame& frame) {
    if (frame.client_width <= 0 || frame.client_height <= 0 || frame.client_width > 16384 ||
        frame.client_height > 16384 || !valid_rect(frame.captured_client_region, frame.client_width,
        frame.client_height) || frame.frame_id == 0 || frame.provider.empty() || frame.provider.size() > 256 ||
        frame.tokens.empty() || frame.tokens.size() > 512) return false;
    const int width = frame.captured_client_region.right - frame.captured_client_region.left;
    const int height = frame.captured_client_region.bottom - frame.captured_client_region.top;
    return std::all_of(frame.tokens.begin(), frame.tokens.end(), [&](const auto& token) {
        return valid_rect(token.bounds, width, height);
    });
}

void metadata_regions(const Image& image, SphHudLayout& layout) {
    const double scale = image.height / 720.0;
    const double centre_x = (image.width - 1) / 2.0;
    layout.stabilization = bounded_rect(image, centre_x - 60.0 * scale, image.height - 154.0 * scale,
        centre_x + 60.0 * scale, image.height - 126.0 * scale);
    layout.asl = bounded_rect(image, centre_x - 130.0 * scale, image.height - 108.0 * scale,
        centre_x - 55.0 * scale, image.height - 76.0 * scale);
    layout.weapon = bounded_rect(image, image.width - 64.0 * scale, image.height - 126.0 * scale,
        image.width, image.height - 101.0 * scale);
    const auto icon = bounded_rect(image, image.width - 53.0 * scale, image.height - 77.0 * scale,
        image.width - 37.0 * scale, image.height - 47.0 * scale);
    if (shell_icon(image, icon, scale)) {
        layout.ammunition_kind = SphHudAmmoKind::single_shell;
        layout.ammunition = bounded_rect(image, image.width - 62.0 * scale, image.height - 52.0 * scale,
            image.width - 29.0 * scale, image.height - 32.0 * scale);
    }
}

void metadata(const SphHudFrame& frame, SphHudReading& result) {
    const auto read_numeric_box = [&](ImageRect roi) -> std::optional<Number> {
        std::optional<Number> found;
        for (const auto& token : frame.tokens) {
            if (!contains(roi, token.bounds)) continue;
            const auto parsed = number(token.reading.text);
            if (!parsed) { if (numeric_like(token.reading.text)) return std::nullopt; else continue; }
            auto score = result.provenance;
            if (found || !token.geometry_verified || !readable(token.reading, score)) return std::nullopt;
            found = parsed;
        }
        return found;
    };
    if (frame.layout.asl) {
        bool labelled{}, ambiguous{};
        std::optional<Number> found;
        for (const auto& token : frame.tokens) {
            if (!contains(*frame.layout.asl, token.bounds)) continue;
            const auto text = normalized(token.reading.text);
            auto reading = token.reading;
            auto score = result.provenance;
            if (text == L"asl") {
                if (!token.geometry_verified || !readable(reading, score)) ambiguous = true;
                else labelled = true;
                continue;
            }
            if (text.starts_with(L"asl")) {
                labelled = true;
                auto tail = normalized(std::wstring_view(text).substr(3));
                if (!tail.empty() && tail.front() == L':') tail = normalized(std::wstring_view(tail).substr(1));
                reading.text = std::move(tail);
                if (!reading.alternate_text.empty()) {
                    auto alternate = normalized(reading.alternate_text);
                    if (alternate.starts_with(L"asl")) {
                        alternate = normalized(std::wstring_view(alternate).substr(3));
                        if (!alternate.empty() && alternate.front() == L':')
                            alternate = normalized(std::wstring_view(alternate).substr(1));
                    }
                    reading.alternate_text = std::move(alternate);
                }
            }
            const auto value = number(reading.text);
            if (!value) {
                if (text.starts_with(L"asl") || numeric_like(text)) ambiguous = true;
                continue;
            }
            if (found || !token.geometry_verified || !readable(reading, score) ||
                (!value->unit.empty() && canonical_unit(value->unit) != L"m")) {
                ambiguous = true;
                continue;
            }
            found = value;
        }
        if (labelled && !ambiguous && found)
            result.asl = SphHudAsl{found->value, found->unit, *frame.layout.asl};
    }
    if (frame.layout.stabilization) {
        std::optional<bool> state;
        bool ambiguous{};
        for (const auto& token : frame.tokens) {
            if (!contains(*frame.layout.stabilization, token.bounds)) continue;
            const auto text = normalized(token.reading.text);
            auto score = result.provenance;
            if (text != L"stabilized" && text != L"unstabilized") { ambiguous = true; continue; }
            if (state || !token.geometry_verified || !readable(token.reading, score)) { ambiguous = true; continue; }
            state = text == L"stabilized";
        }
        if (!ambiguous) result.stabilized = state;
    }
    if (frame.layout.ammunition && frame.layout.ammunition_kind == SphHudAmmoKind::single_shell) {
        if (const auto value = read_numeric_box(*frame.layout.ammunition);
            value && value->unit.empty() && value->value >= 0.0 && value->value <= 9999.0 &&
            value->value == std::floor(value->value))
            result.ammunition = SphHudAmmo{static_cast<unsigned>(value->value),
                frame.layout.ammunition_kind, *frame.layout.ammunition};
    }
}

}  // namespace

SphHudReading read_sph_hud(const SphHudFrame& frame) {
    SphHudReading result;
    result.provenance = {frame.frame_id, frame.provider, frame.captured_client_region, frame.layout.source};
    if (!valid_frame(frame)) return result;
    const int width = frame.captured_client_region.right - frame.captured_client_region.left;
    const int height = frame.captured_client_region.bottom - frame.captured_client_region.top;
    const auto& layout = frame.layout;
    if (layout.source != SphHudLayoutSource::optic_pixel_geometry) {
        result.refusal = SphHudRefusal::unverified_layout;
        return result;
    }
    result.refusal = SphHudRefusal::invalid_layout;
    if (!finite_point(layout.reticle) || !finite_point(layout.compass_pointer) ||
        layout.reticle.x < 0.0 || layout.reticle.x >= width || layout.reticle.y < 0.0 ||
        layout.reticle.y >= height || !std::isfinite(layout.reticle_uncertainty_px) ||
        !std::isfinite(layout.tick_uncertainty_px) || !std::isfinite(layout.compass_pointer_uncertainty_px) ||
        layout.reticle_uncertainty_px <= 0.0 || layout.tick_uncertainty_px <= 0.0 ||
        layout.compass_pointer_uncertainty_px <= 0.0 || layout.reticle_uncertainty_px > 4.0 ||
        layout.tick_uncertainty_px > 4.0 || layout.compass_pointer_uncertainty_px > 4.0 ||
        !valid_scale(layout.mil_scale, width, height) || !valid_scale(layout.range_scale, width, height) ||
        !valid_scale(layout.compass, width, height) || layout.range_scale.labels.right >= layout.reticle.x ||
        layout.mil_scale.labels.left <= layout.reticle.x || layout.compass.labels.bottom >= layout.reticle.y ||
        layout.compass.labels.top < height * 0.15 ||
        std::abs(layout.compass_pointer.x - layout.reticle.x) >
            layout.reticle_uncertainty_px + layout.compass_pointer_uncertainty_px) return result;
    for (const auto* roi : {&layout.asl, &layout.stabilization, &layout.ammunition, &layout.weapon})
        if (*roi && !valid_rect(**roi, width, height)) return result;
    if (!header(frame, valid_rect(layout.mil_scale.header, width, height)
            ? layout.mil_scale.header : layout.mil_scale.labels, L"mil") ||
        !header(frame, valid_rect(layout.range_scale.header, width, height)
            ? layout.range_scale.header : layout.range_scale.labels, L"rng")) {
        result.refusal = SphHudRefusal::missing_scale_headers;
        return result;
    }
    if (layout.weapon && !header(frame, *layout.weapon, L"sph-2")) {
        result.refusal = SphHudRefusal::wrong_weapon;
        return result;
    }
    SphHudScaleReading mil, range, compass;
    for (auto entry : {std::pair{&layout.mil_scale, std::pair{ScaleKind::mil, &mil}},
                       std::pair{&layout.range_scale, std::pair{ScaleKind::range, &range}},
                       std::pair{&layout.compass, std::pair{ScaleKind::compass, &compass}}}) {
        result.refusal = collect_labels(frame, *entry.first, entry.second.first, *entry.second.second,
                                        result.provenance);
        if (result.refusal != SphHudRefusal::none) return result;
    }
    if (!verified_grid(layout.mil_scale, mil, ScaleKind::mil) ||
        !verified_grid(layout.compass, compass, ScaleKind::compass)) {
        result.refusal = !verified_grid(layout.mil_scale, mil, ScaleKind::mil)
            ? SphHudRefusal::inconsistent_mil_scale : SphHudRefusal::inconsistent_compass_scale;
        return result;
    }
    const double row_tolerance = layout.tick_uncertainty_px * 2.0 + 0.5;
    auto mil_ticks = layout.mil_scale.ticks, range_ticks = layout.range_scale.ticks;
    const auto by_y = [](const auto& first, const auto& second) { return first.position.y < second.position.y; };
    std::sort(mil_ticks.begin(), mil_ticks.end(), by_y);
    std::sort(range_ticks.begin(), range_ticks.end(), by_y);
    if (mil_ticks.size() != range_ticks.size()) {
        result.refusal = SphHudRefusal::scale_row_disagreement;
        return result;
    }
    for (std::size_t i = 0; i < mil_ticks.size(); ++i)
        if (std::abs(mil_ticks[i].position.y - range_ticks[i].position.y) > row_tolerance) {
            result.refusal = SphHudRefusal::scale_row_disagreement;
            return result;
        }
    unsigned paired_labels{};
    for (const auto& label : mil.labels)
        if (std::any_of(range.labels.begin(), range.labels.end(), [&](const auto& other) {
            return std::abs(label.tick.y - other.tick.y) <= row_tolerance;
        })) ++paired_labels;
    if (paired_labels < 2) {
        result.refusal = SphHudRefusal::scale_row_disagreement;
        return result;
    }
    result.refusal = interpolate(mil, ScaleKind::mil, layout.reticle.y,
                                layout.reticle_uncertainty_px + layout.tick_uncertainty_px);
    if (result.refusal != SphHudRefusal::none) return result;
    result.mil_scale = mil;
    result.refusal = interpolate(range, ScaleKind::range, layout.reticle.y,
                                layout.reticle_uncertainty_px + layout.tick_uncertainty_px);
    if (result.refusal != SphHudRefusal::none) return result;
    result.range_scale = range;
    result.refusal = interpolate(compass, ScaleKind::compass, layout.compass_pointer.x,
        layout.compass_pointer_uncertainty_px + layout.tick_uncertainty_px);
    if (result.refusal != SphHudRefusal::none) return result;
    result.compass = compass;
    if (mil.uncertainty > 1.0 || compass.uncertainty > 1.0) {
        result.refusal = SphHudRefusal::excessive_uncertainty;
        return result;
    }
    result.pose = SphHudPose{mil.value, compass.value, mil.uncertainty, compass.uncertainty,
        {(frame.captured_client_region.left + layout.reticle.x) / frame.client_width,
         (frame.captured_client_region.top + layout.reticle.y) / frame.client_height}};
    result.refusal = SphHudRefusal::none;
    metadata(frame, result);
    return result;
}

std::optional<SphHudLayout> extract_sph_hud_layout(const Image& image) {
    if (!valid_image(image)) return std::nullopt;
    const double scale = image.height / 720.0;
    const auto centre = reticle(image, scale);
    if (!centre) return std::nullopt;
    auto right = side_ticks(image, centre->point, scale, true);
    auto left = side_ticks(image, centre->point, scale, false);
    if (right.empty() || left.empty()) return std::nullopt;
    for (std::size_t i = 0; i < right.size(); ++i)
        if (std::abs(right[i].position.y - left[i].position.y) > 2.0 * scale) return std::nullopt;
    auto compass = inner_compass(image, centre->point, scale);
    if (!compass) return std::nullopt;
    SphHudLayout layout;
    layout.source = SphHudLayoutSource::optic_pixel_geometry;
    layout.reticle = centre->point;
    layout.reticle_uncertainty_px = centre->uncertainty_px;
    layout.tick_uncertainty_px = std::max(0.75, scale);
    layout.compass_pointer_uncertainty_px = layout.reticle_uncertainty_px;
    layout.compass_pointer = {centre->point.x, compass->ticks.front().position.y};
    layout.compass = std::move(*compass);
    layout.mil_scale.ticks = std::move(right);
    layout.range_scale.ticks = std::move(left);
    const auto mil_x = layout.mil_scale.ticks.front().position.x;
    const auto range_x = layout.range_scale.ticks.front().position.x;
    layout.mil_scale.labels = bounded_rect(image, mil_x + 4.0 * scale,
        layout.mil_scale.ticks.front().position.y - 10.0 * scale, mil_x + 85.0 * scale,
        layout.mil_scale.ticks.back().position.y + 10.0 * scale);
    layout.range_scale.labels = bounded_rect(image, range_x - 82.0 * scale,
        layout.range_scale.ticks.front().position.y - 10.0 * scale, range_x - 4.0 * scale,
        layout.range_scale.ticks.back().position.y + 10.0 * scale);
    layout.mil_scale.header = bounded_rect(image, mil_x + 43.0 * scale, centre->point.y - 10.0 * scale,
        mil_x + 85.0 * scale, centre->point.y + 10.0 * scale);
    layout.range_scale.header = bounded_rect(image, range_x - 82.0 * scale, centre->point.y - 10.0 * scale,
        range_x - 43.0 * scale, centre->point.y + 10.0 * scale);
    for (const auto& tick : layout.mil_scale.ticks)
        layout.mil_scale.label_regions.push_back(bounded_rect(image, tick.position.x + 4.0 * scale,
            tick.position.y - 10.0 * scale, tick.position.x + 78.0 * scale, tick.position.y + 10.0 * scale));
    for (const auto& tick : layout.range_scale.ticks)
        layout.range_scale.label_regions.push_back(bounded_rect(image, tick.position.x - 60.0 * scale,
            tick.position.y - 10.0 * scale, tick.position.x - 4.0 * scale, tick.position.y + 10.0 * scale));
    metadata_regions(image, layout);
    return layout;
}

std::optional<SphHudLayout> extract_sph_hud_supplemental_layout(const Image& image) {
    if (!valid_image(image)) return std::nullopt;
    SphHudLayout layout;
    metadata_regions(image, layout);
    if (!layout.ammunition || layout.ammunition_kind != SphHudAmmoKind::single_shell) return std::nullopt;
    layout.source = SphHudLayoutSource::supplemental_icon_geometry;
    return layout;
}

SphHudReading read_sph_hud_supplemental(const SphHudFrame& frame) {
    SphHudReading result;
    result.provenance = {frame.frame_id, frame.provider, frame.captured_client_region, frame.layout.source};
    if (!valid_frame(frame)) return result;
    const auto& layout = frame.layout;
    if (layout.source != SphHudLayoutSource::supplemental_icon_geometry &&
        layout.source != SphHudLayoutSource::optic_pixel_geometry) {
        result.refusal = SphHudRefusal::unverified_layout;
        return result;
    }
    const int width = frame.captured_client_region.right - frame.captured_client_region.left;
    const int height = frame.captured_client_region.bottom - frame.captured_client_region.top;
    for (const auto* roi : {&layout.asl, &layout.stabilization, &layout.ammunition, &layout.weapon})
        if (*roi && !valid_rect(**roi, width, height)) return result;
    if (!layout.weapon || !header(frame, *layout.weapon, L"sph-2")) {
        result.refusal = SphHudRefusal::wrong_weapon;
        return result;
    }
    result.refusal = SphHudRefusal::supplemental_only;
    metadata(frame, result);
    return result;
}

}  // namespace wardogs
