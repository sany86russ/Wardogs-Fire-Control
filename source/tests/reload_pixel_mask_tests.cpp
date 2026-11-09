#include "reload_pixel_mask.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
std::size_t compared{};

std::uint8_t reference(const std::uint8_t* pixel) {
    const int b = pixel[0], g = pixel[1], r = pixel[2];
    if (g >= 95 && g * 4 > r * 5 && g * 4 > b * 5) return 2;
    if (std::min({r,g,b}) >= 145 && std::max({r,g,b}) - std::min({r,g,b}) <= 45) return 1;
    return 0;
}

std::uint8_t candidate_reference(const std::uint8_t* pixel, std::uint8_t minimum = 64) {
    const int b = pixel[0], g = pixel[1], r = pixel[2];
    if (g >= 95 && g * 4 > r * 5 && g * 4 > b * 5) return 2;
    const int low = std::min({pixel[0],pixel[1],pixel[2]});
    return low >= minimum && std::max({pixel[0],pixel[1],pixel[2]}) - low <= 55 ? 1 : 0;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void exhaustive_colours() {
    std::array<std::uint8_t,256 * 3> input{};
    std::array<std::uint8_t,256> output{}, scalar{}, candidate{};
    for (int r = 0; r < 256; ++r) for (int g = 0; g < 256; ++g) {
        for (int b = 0; b < 256; ++b) {
            input[b * 3] = static_cast<std::uint8_t>(b);
            input[b * 3 + 1] = static_cast<std::uint8_t>(g);
            input[b * 3 + 2] = static_cast<std::uint8_t>(r);
        }
        wardogs::detail::build_reload_pixel_mask(input.data(),256,output.data());
        wardogs::detail::build_reload_pixel_mask_scalar(input.data(),256,scalar.data());
        for (std::size_t b = 0; b < 256; ++b) {
            const auto expected = reference(input.data() + b * 3);
            require(output[b] == expected,"bulk mask differs from original colour thresholds");
            require(scalar[b] == expected,"scalar fallback differs from original colour thresholds");
            ++compared;
        }
        for (const auto cut : std::array<std::uint8_t,6>{64,96,128,160,192,224}) {
            wardogs::detail::build_reload_contour_candidate_mask(input.data(),256,candidate.data(),cut);
            for (std::size_t b = 0; b < 256; ++b) {
                const auto expected = candidate_reference(input.data() + b * 3,cut);
                require(candidate[b] == expected,"bulk contour candidate differs from scalar predicate");
                require(wardogs::detail::reload_contour_candidate_class(input.data() + b * 3,cut) == expected,
                    "scalar contour candidate differs from original predicate");
            }
        }
    }
}

void unaligned_tails() {
    for (std::size_t input_offset = 0; input_offset < 32; ++input_offset)
        for (std::size_t output_offset = 0; output_offset < 16; ++output_offset)
            for (std::size_t count = 0; count < 64; ++count) {
                std::vector<std::uint8_t> input(input_offset + count * 3 + 32,0xD3);
                std::vector<std::uint8_t> output(output_offset + count + 32,0xB6);
                for (std::size_t i = 0; i < count * 3; ++i)
                    input[input_offset + i] = static_cast<std::uint8_t>((i * 53 + count * 17) & 255);
                const auto before = input;
                for (const auto cut : std::array<std::uint8_t,7>{0,64,96,128,160,192,224}) {
                    const bool contour = cut != 0;
                    std::fill(output.begin(),output.end(),std::uint8_t{0xB6});
                    if (contour) wardogs::detail::build_reload_contour_candidate_mask(
                        input.data() + input_offset,count,output.data() + output_offset,cut);
                    else wardogs::detail::build_reload_pixel_mask(input.data() + input_offset,count,
                                                                output.data() + output_offset);
                    require(input == before,"bulk mask modified input pixels");
                    for (std::size_t i = 0; i < output.size(); ++i) {
                        const bool active = i >= output_offset && i < output_offset + count;
                        const auto* pixel = input.data() + input_offset + (active ? (i-output_offset)*3 : 0);
                        const auto expected = active ? (contour ? candidate_reference(pixel,cut) : reference(pixel)) : 0xB6;
                        require(output[i] == expected,"unaligned tail or output guard differs");
                    }
                }
            }
    wardogs::detail::build_reload_pixel_mask(nullptr,0,nullptr);
    wardogs::detail::build_reload_contour_candidate_mask(nullptr,0,nullptr);
}

struct GuardedBuffer {
    std::uint8_t* memory{};
    std::size_t page{};
    GuardedBuffer() {
        SYSTEM_INFO system{}; GetSystemInfo(&system); page = system.dwPageSize;
        memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr,page * 2,
            MEM_COMMIT | MEM_RESERVE,PAGE_READWRITE));
        require(memory != nullptr,"cannot allocate guard-page fixture");
        DWORD previous{};
        if (!VirtualProtect(memory + page,page,PAGE_NOACCESS,&previous)) {
            VirtualFree(memory,0,MEM_RELEASE); memory = nullptr;
            throw std::runtime_error("cannot protect guard-page fixture");
        }
    }
    ~GuardedBuffer() { if (memory) VirtualFree(memory,0,MEM_RELEASE); }
    GuardedBuffer(const GuardedBuffer&) = delete;
    GuardedBuffer& operator=(const GuardedBuffer&) = delete;
};

void protected_ends() {
    GuardedBuffer input,output;
    for (std::size_t count = 0; count < 64; ++count) {
        auto* pixels = input.memory + input.page - count * 3;
        auto* mask = output.memory + output.page - count;
        for (std::size_t i = 0; i < count * 3; ++i)
            pixels[i] = static_cast<std::uint8_t>((i * 31 + count * 37) & 255);
        for (const auto cut : std::array<std::uint8_t,7>{0,64,96,128,160,192,224}) {
            const bool contour = cut != 0;
            if (contour) wardogs::detail::build_reload_contour_candidate_mask(pixels,count,mask,cut);
            else wardogs::detail::build_reload_pixel_mask(pixels,count,mask);
            for (std::size_t i = 0; i < count; ++i)
                require(mask[i] == (contour ? candidate_reference(pixels+i*3,cut) : reference(pixels+i*3)),
                    "protected tail differs");
        }
    }
}
}  // namespace

int main() {
    try {
        exhaustive_colours();
        unaligned_tails();
        protected_ends();
        std::cout << compared << " RGB colours match for the primary mask and all 6 contour cuts; scalar, unaligned tails and protected ends pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
