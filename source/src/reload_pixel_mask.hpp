#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#include <tmmintrin.h>
#endif

namespace wardogs::detail {

// Shared exact scalar definition. Green has precedence where both colour
// predicates match. The bulk path must produce the same byte for every RGB.
inline std::uint8_t reload_pixel_class_exact(const std::uint8_t* bgr) noexcept {
    const int b = bgr[0], g = bgr[1], r = bgr[2];
    if (g >= 95 && g * 4 > r * 5 && g * 4 > b * 5) return 2;
    const int low_bg = b < g ? b : g;
    const int low = low_bg < r ? low_bg : r;
    if (low < 145) return 0;
    const int high_bg = b > g ? b : g;
    const int high = high_bg > r ? high_bg : r;
    return high - low <= 45 ? 1 : 0;
}

inline void build_reload_pixel_mask_scalar(const std::uint8_t* bgr,
    std::size_t pixels, std::uint8_t* output) noexcept {
    for (std::size_t i = 0; i < pixels; ++i)
        output[i] = reload_pixel_class_exact(bgr + i * 3);
}

// Candidate generation for the independent outline pass only. These relaxed
// interiors still need contour support and cannot be accepted as plain glyphs.
inline std::uint8_t reload_contour_candidate_class(const std::uint8_t* bgr,
    std::uint8_t minimum = 64) noexcept {
    const int b = bgr[0], g = bgr[1], r = bgr[2];
    if (g >= 95 && g * 4 > r * 5 && g * 4 > b * 5) return 2;
    const int low_bg = b < g ? b : g;
    const int low = low_bg < r ? low_bg : r;
    if (low < minimum) return 0;
    const int high_bg = b > g ? b : g;
    const int high = high_bg > r ? high_bg : r;
    return high - low <= 55 ? 1 : 0;
}

#if defined(_M_X64) || defined(_M_IX86)
inline bool reload_mask_has_ssse3() noexcept {
    static const bool available = [] {
        int registers[4]{};
        __cpuid(registers, 1);
        return (registers[2] & (1 << 9)) != 0;
    }();
    return available;
}

// Kept out of the dispatcher's instruction stream so old x86 CPUs never
// execute SSSE3 before the CPUID check. Each iteration reads exactly 48 bytes.
template<int MinimumWhite, int MaximumRange>
__declspec(noinline) inline void build_reload_pixel_mask_ssse3(
    const std::uint8_t* bgr, std::size_t pixels, std::uint8_t* output,
    std::uint8_t dynamic_minimum = static_cast<std::uint8_t>(MinimumWhite)) noexcept {
    const auto zero = _mm_setzero_si128();
    const auto ones = _mm_set1_epi8(1), twos = _mm_set1_epi8(2);
    const auto cut = static_cast<std::uint8_t>(MinimumWhite >= 0 ? MinimumWhite : dynamic_minimum);
    const auto minimum_white = _mm_set1_epi8(static_cast<char>(cut));
    const auto maximum_range = _mm_set1_epi8(static_cast<char>(MaximumRange));
    const auto minimum_green = _mm_set1_epi8(95);
    const auto b0 = _mm_setr_epi8(0,3,6,9,12,15,-128,-128,-128,-128,-128,-128,-128,-128,-128,-128);
    const auto b1 = _mm_setr_epi8(-128,-128,-128,-128,-128,-128,2,5,8,11,14,-128,-128,-128,-128,-128);
    const auto b2 = _mm_setr_epi8(-128,-128,-128,-128,-128,-128,-128,-128,-128,-128,-128,1,4,7,10,13);
    const auto g0 = _mm_setr_epi8(1,4,7,10,13,-128,-128,-128,-128,-128,-128,-128,-128,-128,-128,-128);
    const auto g1 = _mm_setr_epi8(-128,-128,-128,-128,-128,0,3,6,9,12,15,-128,-128,-128,-128,-128);
    const auto g2 = _mm_setr_epi8(-128,-128,-128,-128,-128,-128,-128,-128,-128,-128,-128,2,5,8,11,14);
    const auto r0 = _mm_setr_epi8(2,5,8,11,14,-128,-128,-128,-128,-128,-128,-128,-128,-128,-128,-128);
    const auto r1 = _mm_setr_epi8(-128,-128,-128,-128,-128,1,4,7,10,13,-128,-128,-128,-128,-128,-128);
    const auto r2 = _mm_setr_epi8(-128,-128,-128,-128,-128,-128,-128,-128,-128,-128,0,3,6,9,12,15);
    std::size_t i = 0;
    for (; pixels - i >= 16; i += 16) {
        const auto* input = bgr + i * 3;
        const auto x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
        const auto y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + 16));
        const auto z = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + 32));
        const auto b = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(x,b0),_mm_shuffle_epi8(y,b1)),_mm_shuffle_epi8(z,b2));
        const auto g = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(x,g0),_mm_shuffle_epi8(y,g1)),_mm_shuffle_epi8(z,g2));
        const auto r = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(x,r0),_mm_shuffle_epi8(y,r1)),_mm_shuffle_epi8(z,r2));
        const auto low = _mm_min_epu8(_mm_min_epu8(b,g),r);
        const auto high = _mm_max_epu8(_mm_max_epu8(b,g),r);
        const auto range = _mm_sub_epi8(high,low);
        const auto white_mask = _mm_and_si128(
            _mm_cmpeq_epi8(_mm_max_epu8(low,minimum_white),low),
            _mm_cmpeq_epi8(_mm_min_epu8(range,maximum_range),range));
        const auto green_half = [](__m128i bv, __m128i gv, __m128i rv) {
            const auto g4 = _mm_slli_epi16(gv,2);
            const auto r5 = _mm_add_epi16(_mm_slli_epi16(rv,2),rv);
            const auto b5 = _mm_add_epi16(_mm_slli_epi16(bv,2),bv);
            return _mm_and_si128(_mm_cmpgt_epi16(g4,r5),_mm_cmpgt_epi16(g4,b5));
        };
        const auto green_lo = green_half(_mm_unpacklo_epi8(b,zero),_mm_unpacklo_epi8(g,zero),_mm_unpacklo_epi8(r,zero));
        const auto green_hi = green_half(_mm_unpackhi_epi8(b,zero),_mm_unpackhi_epi8(g,zero),_mm_unpackhi_epi8(r,zero));
        const auto green_mask = _mm_and_si128(_mm_packs_epi16(green_lo,green_hi),
            _mm_cmpeq_epi8(_mm_max_epu8(g,minimum_green),g));
        const auto result = _mm_or_si128(_mm_and_si128(green_mask,twos),
            _mm_and_si128(_mm_andnot_si128(green_mask,white_mask),ones));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + i),result);
    }
    for (; i < pixels; ++i) {
        if constexpr (MinimumWhite == 145 && MaximumRange == 45)
            output[i] = reload_pixel_class_exact(bgr + i * 3);
        else
            output[i] = reload_contour_candidate_class(bgr + i * 3,cut);
    }
}
#endif

// Non-overlapping packed BGR input and one output byte per pixel. Empty input
// is allowed. Dispatch is cached, with a scalar path on non-SSSE3 machines.
inline void build_reload_pixel_mask(const std::uint8_t* bgr,
    std::size_t pixels, std::uint8_t* output) noexcept {
    if (pixels == 0) return;
#if defined(_M_X64) || defined(_M_IX86)
    if (reload_mask_has_ssse3()) {
        build_reload_pixel_mask_ssse3<145,45>(bgr,pixels,output);
        return;
    }
#endif
    build_reload_pixel_mask_scalar(bgr,pixels,output);
}

inline void build_reload_contour_candidate_mask(const std::uint8_t* bgr,
    std::size_t pixels, std::uint8_t* output, std::uint8_t minimum = 64) noexcept {
    if (pixels == 0) return;
#if defined(_M_X64) || defined(_M_IX86)
    if (reload_mask_has_ssse3()) {
        if (minimum == 64) build_reload_pixel_mask_ssse3<64,55>(bgr,pixels,output);
        else build_reload_pixel_mask_ssse3<-1,55>(bgr,pixels,output,minimum);
        return;
    }
#endif
    for (std::size_t i = 0; i < pixels; ++i)
        output[i] = reload_contour_candidate_class(bgr + i * 3,minimum);
}

}  // namespace wardogs::detail
