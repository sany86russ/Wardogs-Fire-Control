#pragma once

#include "wardogs/ocr.hpp"

#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wardogs {

class WindowsOcr {
public:
    // A requested BCP-47 locale resolves only to an installed Windows OCR
    // language. Unsupported locales throw; the default remains English.
    explicit WindowsOcr(std::wstring_view language_tag = L"en-US");
    ~WindowsOcr();
    WindowsOcr(WindowsOcr&&) noexcept;
    WindowsOcr& operator=(WindowsOcr&&) noexcept;
    WindowsOcr(const WindowsOcr&) = delete;
    WindowsOcr& operator=(const WindowsOcr&) = delete;

    // Read-only language discovery. These calls use the same worker MTA
    // lifetime as engine creation and never install a Windows language pack.
    [[nodiscard]] static std::vector<std::wstring> available_recognizer_languages();
    [[nodiscard]] static bool is_language_supported(std::wstring_view language_tag);
    [[nodiscard]] std::wstring_view language_tag() const noexcept;

    OcrResult recognize(const Image& image, std::stop_token stop = {}) const;
    OcrResult recognize_high_contrast(const Image& image, std::stop_token stop = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs
