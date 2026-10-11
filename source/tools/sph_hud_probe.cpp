#include "wardogs/sph_hud_ocr.hpp"

#include <Windows.h>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <winrt/base.h>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) { std::wcerr << L"usage: sph_hud_probe <saved-image.png> [recognition-model.onnx]\n"; return 2; }
    try {
        const auto image = wardogs::load_image_file(argv[1]);
        wardogs::WindowsOcr ocr;
        std::unique_ptr<wardogs::RapidOcr> recognition;
        if (argc == 3) recognition = std::make_unique<wardogs::RapidOcr>(argv[2]);
        const auto started = std::chrono::steady_clock::now();
        const auto frame = wardogs::recognize_sph_hud_frame(image, ocr, 1, {}, recognition.get());
        const auto reading = wardogs::read_sph_hud(frame);
        std::cout << std::setprecision(17) << "accepted=" << reading.accepted()
                  << " refusal=" << static_cast<int>(reading.refusal)
                  << " layout=" << static_cast<int>(frame.layout.source)
                  << " tokens=" << frame.tokens.size()
                  << " elapsed_ms=" << std::chrono::duration<double,std::milli>(
                         std::chrono::steady_clock::now() - started).count() << '\n';
        if (reading.pose) std::cout << "bearing=" << reading.pose->bearing_deg
            << " mil=" << reading.pose->sight_mil << " bearing_uncertainty=" << reading.pose->bearing_uncertainty_deg
            << " mil_uncertainty=" << reading.pose->mil_uncertainty << '\n';
        if (reading.ammunition) std::cout << "shell_count=" << reading.ammunition->count
            << " shell_channel=" << static_cast<int>(reading.ammunition->kind) << '\n';
        if (reading.stabilized) std::cout << "stabilized=" << *reading.stabilized << '\n';
        const auto supplemental = wardogs::read_sph_hud_supplemental(frame);
        if (supplemental.ammunition) std::cout << "partial_shell_count=" << supplemental.ammunition->count
            << " partial_shell_channel=" << static_cast<int>(supplemental.ammunition->kind) << '\n';
        if (supplemental.stabilized) std::cout << "partial_stabilized=" << *supplemental.stabilized << '\n';
        for (const auto& token : frame.tokens) std::wcout << L"token=" << token.reading.text
            << L" alternate=" << token.reading.alternate_text << L" bounds=" << token.bounds.left << L','
            << token.bounds.top << L',' << token.bounds.right << L',' << token.bounds.bottom << '\n';
        return 0; // Refusal is an expected diagnostic outcome, not a crash.
    } catch (const winrt::hresult_error& error) {
        std::wcerr << L"Windows OCR failed: " << error.message().c_str() << '\n'; return 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
