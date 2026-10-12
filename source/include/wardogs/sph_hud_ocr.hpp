#pragma once

#include "wardogs/sph_hud.hpp"
#include "wardogs/windows_ocr.hpp"

#include <stop_token>

namespace wardogs {

// Reads only bounded instrument regions of an already captured frame. This
// adapter does not capture a desktop or infer values from firing commands.
// Optional counters also retain the last reached stage when OCR throws.
// A second installed-language engine reads only the stabilization caption.
SphHudFrame recognize_sph_hud_frame(const Image& image, WindowsOcr& ocr,
                                  std::uint64_t frame_id,
                                  std::stop_token stop = {}, RapidOcr* recognition = nullptr,
                                  SphHudRecognitionDiagnostics* diagnostics = nullptr,
                                  WindowsOcr* localized_metadata = nullptr);

}  // namespace wardogs
