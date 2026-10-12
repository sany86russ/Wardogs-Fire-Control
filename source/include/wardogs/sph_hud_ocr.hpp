#pragma once

#include "wardogs/sph_hud.hpp"
#include "wardogs/windows_ocr.hpp"

#include <stop_token>

namespace wardogs {

// Reads only bounded instrument regions of an already captured frame. This
// adapter does not capture a desktop or infer values from firing commands.
SphHudFrame recognize_sph_hud_frame(const Image& image, WindowsOcr& ocr,
                                  std::uint64_t frame_id,
                                  std::stop_token stop = {}, RapidOcr* recognition = nullptr);

}  // namespace wardogs
