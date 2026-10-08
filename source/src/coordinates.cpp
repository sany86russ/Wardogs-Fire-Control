#include "wardogs/core.hpp"

#include <cwctype>
#include <charconv>
#include <bitset>
#include <cmath>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace wardogs {
namespace {

using CharacterSet = std::bitset<128>;

bool is_default_ocr_pattern(std::wstring_view pattern) {
    if (pattern == default_ocr_coordinate_pattern) return true;
    // WritePrivateProfileStringW preserved an imported ANSI INI's encoding.
    // Its best-fit conversion replaced the fullwidth comma and semicolon.
    // Match the entire known template, never a fragment of a user expression.
    constexpr std::wstring_view separator = LR"([\s,，;；:*&#.·]*)";
    constexpr std::wstring_view ansi_separator = LR"([\s,,;;:*&#.·]*)";
    constexpr auto offset = default_ocr_coordinate_pattern.find(separator);
    static_assert(offset != std::wstring_view::npos);
    static_assert(separator.size() == ansi_separator.size());
    return pattern.size() == default_ocr_coordinate_pattern.size() &&
        pattern.substr(0, offset) == default_ocr_coordinate_pattern.substr(0, offset) &&
        pattern.substr(offset, separator.size()) == ansi_separator &&
        pattern.substr(offset + separator.size()) ==
            default_ocr_coordinate_pattern.substr(offset + separator.size());
}

[[noreturn]] void unsafe_pattern() {
    throw std::invalid_argument("Шаблон координат слишком сложный: используйте две числовые группы, метки и разделители без ветвлений");
}

CharacterSet pattern_character(std::wstring_view pattern, std::size_t& position) {
    if (position >= pattern.size()) unsafe_pattern();
    wchar_t character = pattern[position++];
    CharacterSet result;
    if (character == L'\\') {
        if (position >= pattern.size()) unsafe_pattern();
        character = pattern[position++];
        if (character == L'd') {
            for (int digit = '0'; digit <= '9'; ++digit) result.set(digit);
            return result;
        }
        if (character == L's') {
            for (int space : {' ', '\t', '\r', '\n', '\f', '\v'}) result.set(space);
            return result;
        }
        if (character == L't') character = L'\t';
        else if (character == L'n') character = L'\n';
        else if (character == L'r') character = L'\r';
        else if ((character >= L'0' && character <= L'9') ||
                 (character >= L'A' && character <= L'Z') ||
                 (character >= L'a' && character <= L'z')) unsafe_pattern();
    }
    if (character >= 128) unsafe_pattern();
    result.set(static_cast<std::size_t>(character));
    return result;
}

// Avoid invoking an uninterruptible backtracking engine for arbitrary user
// expressions. Flattened groups and disjoint quantified atoms have no nested
// or alternate paths. Counts are small; an unanchored search also requires a
// literal prefix disjoint from its first repeated character set.
void validate_custom_pattern(std::wstring_view pattern) {
    CharacterSet pending, last_mandatory;
    bool has_mandatory = false;
    bool first_variable = true;
    const bool anchored = !pattern.empty() && pattern.front() == L'^';
    int depth = 0;
    int captures = 0;
    std::size_t position = 0;
    while (position < pattern.size()) {
        const wchar_t token = pattern[position];
        if (token == L'^' || token == L'$') { ++position; continue; }
        if (token == L'(') {
            if (++depth > 8) unsafe_pattern();
            ++position;
            if (position < pattern.size() && pattern[position] == L'?') {
                if (pattern.substr(position, 2) != L"?:") unsafe_pattern();
                position += 2;
            } else ++captures;
            continue;
        }
        if (token == L')') {
            if (--depth < 0) unsafe_pattern();
            ++position;
            if (position < pattern.size() &&
                std::wstring_view{L"*+?{"}.find(pattern[position]) != std::wstring_view::npos)
                unsafe_pattern();
            continue;
        }
        if (std::wstring_view{L".*+?{|]}"}.find(token) != std::wstring_view::npos)
            unsafe_pattern();
        CharacterSet characters;
        if (token == L'[') {
            ++position;
            if (position == pattern.size() || pattern[position] == L'^') unsafe_pattern();
            while (position < pattern.size() && pattern[position] != L']') {
                if (pattern[position] == L'[') unsafe_pattern();
                auto first = pattern_character(pattern, position);
                if (position + 1 < pattern.size() && pattern[position] == L'-' &&
                    pattern[position + 1] != L']') {
                    ++position;
                    const auto last = pattern_character(pattern, position);
                    if (first.count() != 1 || last.count() != 1) unsafe_pattern();
                    std::size_t low = 0, high = 0;
                    while (!first.test(low)) ++low;
                    while (!last.test(high)) ++high;
                    if (low > high) unsafe_pattern();
                    for (auto character = low; character <= high; ++character) first.set(character);
                }
                characters |= first;
            }
            if (position == pattern.size() || characters.none()) unsafe_pattern();
            ++position;
        } else characters = pattern_character(pattern, position);
        // Matching is case-insensitive, so complexity checks must compare the
        // same folded character sets as the regex engine.
        for (int upper = 'A'; upper <= 'Z'; ++upper) {
            const int lower = upper + ('a' - 'A');
            if (characters.test(upper) || characters.test(lower)) {
                characters.set(upper);
                characters.set(lower);
            }
        }
        unsigned minimum = 1, maximum = 1;
        if (position < pattern.size()) {
            const auto quantifier = pattern[position];
            if (quantifier == L'*' || quantifier == L'+' || quantifier == L'?') {
                ++position;
                minimum = quantifier == L'+' ? 1 : 0;
                maximum = quantifier == L'?' ? 1 : 65;
            } else if (quantifier == L'{') {
                ++position;
                const auto count = [&] {
                    if (position == pattern.size() || pattern[position] < L'0' || pattern[position] > L'9') unsafe_pattern();
                    unsigned value = 0;
                    while (position < pattern.size() && pattern[position] >= L'0' && pattern[position] <= L'9') {
                        value = value * 10 + static_cast<unsigned>(pattern[position++] - L'0');
                        if (value > 64) unsafe_pattern();
                    }
                    return value;
                };
                minimum = maximum = count();
                if (position < pattern.size() && pattern[position] == L',') {
                    ++position;
                    maximum = position < pattern.size() && pattern[position] == L'}' ? 65 : count();
                }
                if (minimum > maximum || position == pattern.size() || pattern[position++] != L'}') unsafe_pattern();
            }
        }
        const bool variable = maximum != minimum;
        if (variable && (pending & characters).any()) unsafe_pattern();
        if (variable && first_variable && !anchored &&
            (!has_mandatory || (last_mandatory & characters).any())) unsafe_pattern();
        if (minimum > 0) {
            if ((pending & characters).none()) pending.reset();
            last_mandatory = characters;
            has_mandatory = true;
        }
        if (variable) {
            pending |= characters;
            first_variable = false;
        }
    }
    if (depth != 0 || captures != 2) unsafe_pattern();
}

double parse_number(std::wstring value,
                    std::size_t fractional_digits = std::wstring::npos) {
    std::string normalized;
    normalized.reserve(value.size());
    for (wchar_t ch : value) {
        if (std::iswspace(ch)) {
            continue;
        }
        switch (ch) {
            case L'l':
            case L'i':
            case L'I':
            case L'|': ch = L'1'; break;
            case L'o':
            case L'O': ch = L'0'; break;
            default: break;
        }
        if (ch > 127)
            throw std::invalid_argument("Координата содержит неверное число");
        normalized.push_back(static_cast<char>(ch));
    }
    const auto decimal = normalized.find('.');
    if (fractional_digits != std::wstring::npos && decimal != std::wstring::npos &&
        normalized.size() > decimal + 1 + fractional_digits) {
        normalized.resize(decimal + 1 + fractional_digits);
    }
    double result{};
    // Coordinate notation always uses a dot, regardless of Windows locale.
    const char* first = normalized.data();
    const char* last = first + normalized.size();
    if (first != last && *first == '+') ++first;
    const auto parsed = std::from_chars(first, last, result);
    if (parsed.ec == std::errc::invalid_argument) {
        throw std::invalid_argument("Координата содержит неверное число");
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        throw std::invalid_argument("Координата слишком большая или содержит неверное число");
    }
    if (parsed.ptr != last || !std::isfinite(result)) {
        throw std::invalid_argument("Координата должна быть конечным числом");
    }
    return result;
}

Point point_from_match(const std::wsmatch& match) {
    if (match.size() < 3 || !match[1].matched || !match[2].matched)
        throw std::invalid_argument("Шаблон координат должен содержать две группы: x и y");
    return {parse_number(match[1].str(), 2), parse_number(match[2].str(), 2)};
}

bool ocr_digit(wchar_t value) noexcept {
    return (value >= L'0' && value <= L'9') ||
           std::wstring_view{L"liI|Oo"}.find(value) != std::wstring_view::npos;
}

void skip_space(std::wstring_view text, std::size_t& position) {
    while (position < text.size() && std::iswspace(text[position])) ++position;
}

bool scan_ocr_number(std::wstring_view text, std::size_t& position,
                     std::wstring_view& number) {
    skip_space(text, position);
    const auto begin = position;
    if (position < text.size() && (text[position] == L'+' || text[position] == L'-')) {
        ++position;
        skip_space(text, position);
    }
    if (position == text.size() || !ocr_digit(text[position])) return false;
    do {
        ++position;
        skip_space(text, position);
    } while (position < text.size() && ocr_digit(text[position]));
    if (position == text.size() || text[position++] != L'.') return false;
    unsigned digits = 0;
    while (position < text.size()) {
        skip_space(text, position);
        if (position == text.size() || !ocr_digit(text[position])) break;
        ++position;
        ++digits;
    }
    if (digits < 2) return false;
    number = text.substr(begin, position - begin);
    return true;
}

std::vector<Point> scan_default_coordinates(std::wstring_view text) {
    if (text.size() > 8192)
        throw std::invalid_argument("Текст OCR или шаблон координат слишком длинный");
    std::vector<Point> points;
    // The trusted default grammar permits spaces inside a number. Feeding it
    // to a backtracking regex can revisit the same long, malformed digit run
    // exponentially. Labels are disjoint from digits, so scan each run once.
    for (std::size_t begin = 0; begin < text.size(); ++begin) {
        if (text[begin] != L'x' && text[begin] != L'X') continue;
        auto position = begin + 1;
        skip_space(text, position);
        if (position < text.size() && (text[position] == L':' || text[position] == L'='))
            ++position;
        std::wstring_view x, y;
        if (!scan_ocr_number(text, position, x)) continue;
        while (position < text.size() &&
               (std::iswspace(text[position]) ||
                std::wstring_view{L",，;；:*&#.·"}.find(text[position]) != std::wstring_view::npos))
            ++position;
        if (position == text.size() || (text[position] != L'y' && text[position] != L'Y'))
            continue;
        ++position;
        skip_space(text, position);
        if (position < text.size() && (text[position] == L':' || text[position] == L'='))
            ++position;
        if (!scan_ocr_number(text, position, y)) continue;
        points.push_back({parse_number(std::wstring{x}, 2), parse_number(std::wstring{y}, 2)});
        begin = position - 1;
    }
    return points;
}

Point parse_last_match(const std::wstring& text, const std::wregex& expression) {
    std::wsregex_iterator current{text.begin(), text.end(), expression};
    const std::wsregex_iterator end;
    if (current == end) {
        throw std::invalid_argument("Не найдена полная пара координат x и y");
    }
    std::wsmatch latest;
    for (; current != end; ++current) {
        latest = *current;
    }
    return point_from_match(latest);
}

const std::wregex& ocr_expression(std::wstring_view text, std::wstring_view pattern) {
    if (text.size() > 8192 || pattern.size() > 1024)
        throw std::invalid_argument("Текст OCR или шаблон координат слишком длинный");
    try {
        constexpr auto flags = std::regex_constants::ECMAScript |
                               std::regex_constants::icase;
        // OCR repeatedly uses the same user pattern. Keep one compiled pattern
        // per calling thread instead of rebuilding it for every screenshot.
        thread_local std::wstring cached_pattern;
        thread_local std::wregex cached_expression;
        if (cached_pattern != pattern || cached_pattern.empty()) {
            validate_custom_pattern(pattern);
            std::wregex replacement(std::wstring{pattern}, flags);
            cached_pattern = pattern;
            cached_expression = std::move(replacement);
        }
        return cached_expression;
    } catch (const std::regex_error& error) {
        throw std::invalid_argument(std::string{"Неверный шаблон координат: "} + error.what());
    }
}

}  // namespace

Point parse_ocr_coordinate(std::wstring_view text, std::wstring_view pattern) {
    if (is_default_ocr_pattern(pattern)) {
        const auto points = scan_default_coordinates(text);
        if (points.empty()) throw std::invalid_argument("Не найдена полная пара координат x и y");
        return points.back();
    }
    const auto& expression = ocr_expression(text, pattern);
    try {
        return parse_last_match(std::wstring{text}, expression);
    } catch (const std::regex_error&) {
        throw std::invalid_argument("Шаблон координат превышает безопасный предел сложности поиска");
    }
}

void validate_ocr_coordinate_pattern(std::wstring_view pattern) {
    if (is_default_ocr_pattern(pattern)) return;
    (void)ocr_expression({}, pattern);
}

std::wstring normalize_ocr_coordinate_pattern(std::wstring_view pattern) {
    return std::wstring{is_default_ocr_pattern(pattern)
        ? default_ocr_coordinate_pattern : pattern};
}

std::vector<Point> parse_ocr_coordinates(std::wstring_view text,
                                         std::wstring_view pattern) {
    if (is_default_ocr_pattern(pattern)) return scan_default_coordinates(text);
    const auto& expression = ocr_expression(text, pattern);
    const std::wstring value{text};
    std::vector<Point> points;
    try {
        for (std::wsregex_iterator match{value.begin(), value.end(), expression}, end;
             match != end; ++match)
            points.push_back(point_from_match(*match));
    } catch (const std::regex_error&) {
        throw std::invalid_argument("Шаблон координат превышает безопасный предел сложности поиска");
    }
    return points;
}

Point parse_manual_coordinate(std::wstring_view text) {
    if (text.size() > 512)
        throw std::invalid_argument("Координаты слишком длинные");
    static const std::wregex labelled{
        LR"(^\s*x\s*[:=]?\s*([-+]?\d+(?:\.\d+)?)\s*[,，;；]?\s*y\s*[:=]?\s*([-+]?\d+(?:\.\d+)?)\s*$)",
        std::regex_constants::ECMAScript | std::regex_constants::icase};
    static const std::wregex plain{
        LR"(^\s*([-+]?\d+(?:\.\d+)?)\s*[,，\s]\s*([-+]?\d+(?:\.\d+)?)\s*$)",
        std::regex_constants::ECMAScript};
    const std::wstring value{text};
    std::wsmatch match;
    if (std::regex_match(value, match, labelled) || std::regex_match(value, match, plain)) {
        return {parse_number(match[1].str()), parse_number(match[2].str())};
    }
    throw std::invalid_argument("Введите x12.34, y56.78 или 12.34 56.78");
}

}  // namespace wardogs
