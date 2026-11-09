#pragma once

#include "wardogs/reload_vision.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wardogs_ui {

// One explicitly requested reload cycle. add_frame only retains the runtime
// capture in RAM; save must be called after capture/input has finished, and may
// run on a caller-owned background thread. Neither operation captures or sends
// input. Do not call add_frame concurrently with save.
class ReloadDiagnosticSession {
public:
    static constexpr std::size_t maximum_frames = 4;
    static constexpr std::size_t maximum_bytes = 64 * 1024 * 1024;

    explicit ReloadDiagnosticSession(std::filesystem::path directory,
                                     std::size_t memory_limit_bytes = maximum_bytes);

    // client_area is in physical game-client pixels. Observation/glyph bounds
    // are relative to frame, before the caller offsets them into client space.
    // false means image storage was omitted; bounded metadata is still kept.
    [[nodiscard]] bool add_frame(wardogs::Image frame,
        wardogs::ImageRect client_area,
        const wardogs::ReloadVisionDiagnostics& diagnostics,
        std::optional<wardogs::ReloadObservation> observation,
        double capture_ms, double recognition_ms, double elapsed_ms);

    // Refuses an existing destination or reparse-point ancestor. PNGs are
    // written atomically, with session.json last. Failures throw and leave
    // available evidence intact; existing files are never overwritten/deleted.
    void save(const std::string& outcome) const;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] std::size_t frame_count() const noexcept { return frames_.size(); }
    [[nodiscard]] std::size_t stored_bytes() const noexcept { return stored_bytes_; }

private:
    struct Frame {
        int width{}, height{};
        wardogs::ImageRect area{};
        wardogs::ReloadVisionDiagnostics diagnostics;
        std::optional<wardogs::ReloadObservation> observation;
        std::optional<wardogs::Image> image;
        std::string image_status;
        bool glyph_bounds_truncated{};
        double capture_ms{}, recognition_ms{}, elapsed_ms{};
    };

    std::filesystem::path directory_;
    std::size_t memory_limit_bytes_{};
    std::size_t stored_bytes_{};
    std::size_t ignored_frames_{};
    std::vector<Frame> frames_;
};

}  // namespace wardogs_ui
