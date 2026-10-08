cmake_minimum_required(VERSION 3.24)
project(WarDogsDistanceCalculator VERSION 2.8.0 LANGUAGES C CXX)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(wardogs_i18n STATIC src/localization.cpp)
target_include_directories(wardogs_i18n PUBLIC include src)
target_link_libraries(wardogs_i18n PUBLIC Qt6::Widgets)
if(MSVC)
    target_compile_options(wardogs_i18n PRIVATE /W4 /permissive- /utf-8)
endif()
qt_add_resources(wardogs_i18n localization
    PREFIX "/i18n"
    BASE translations
    FILES translations/main.json translations/dialogs.json
          translations/errors.json translations/common.json translations/qtbase_ru.json
)

add_library(wardogs_updates STATIC src/update_service.cpp src/update_ui.cpp)
target_include_directories(wardogs_updates PUBLIC src)
target_link_libraries(wardogs_updates PUBLIC wardogs_i18n Qt6::Widgets Qt6::Network)
target_compile_definitions(wardogs_updates PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(wardogs_updates PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(wardogs_zstd STATIC
    third_party/zstd/common/debug.c
    third_party/zstd/common/entropy_common.c
    third_party/zstd/common/error_private.c
    third_party/zstd/common/fse_decompress.c
    third_party/zstd/common/xxhash.c
    third_party/zstd/common/zstd_common.c
    third_party/zstd/decompress/huf_decompress.c
    third_party/zstd/decompress/zstd_ddict.c
    third_party/zstd/decompress/zstd_decompress_block.c
    third_party/zstd/decompress/zstd_decompress.c
)
target_include_directories(wardogs_zstd PUBLIC third_party/zstd)
target_include_directories(wardogs_zstd PRIVATE third_party/zstd/common)
target_compile_definitions(wardogs_zstd PRIVATE ZSTD_DISABLE_ASM)

add_library(wardogs_terrain src/terrain_package.cpp)
target_include_directories(wardogs_terrain PUBLIC include)
target_compile_definitions(wardogs_terrain PUBLIC UNICODE _UNICODE NOMINMAX)
target_link_libraries(wardogs_terrain PRIVATE Qt6::Core wardogs_zstd bcrypt)
if(MSVC)
    target_compile_options(wardogs_terrain PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(wardogs_core
    src/calculator.cpp
    src/continuous_calibration.cpp
    src/coordinates.cpp
    src/ghost_reticle.cpp
    src/hotkeys.cpp
    src/image.cpp
    src/logger.cpp
    src/mouse_trigger.cpp
    src/ocr.cpp
    src/presentation.cpp
    src/vehicle_ballistics.cpp
)
target_link_libraries(wardogs_updates PRIVATE wardogs_core)
target_include_directories(wardogs_core PUBLIC include)
target_compile_definitions(wardogs_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(wardogs_core PRIVATE third_party/onnxruntime/include)
target_link_libraries(wardogs_core PRIVATE
    windowscodecs
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib/x64/onnxruntime.lib"
)
if(MSVC)
    target_compile_options(wardogs_core PRIVATE /W4 /permissive- /utf-8)
endif()

if(WIN32)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
        @ONLY
    )
    qt_add_executable(WarDogsDistanceCalculator WIN32
        src/app_icon.cpp
        src/app.cpp
        src/capture.cpp
        src/direction_plot.cpp
        src/ghost_reticle_window.cpp
        src/main_window.cpp
        src/pinned_result_window.cpp
        src/selection_overlay.cpp
        src/settings.cpp
        src/settings_dialog.cpp
        src/vehicle_solution_widget.cpp
        src/window_title_bar.cpp
        src/windows_taskbar.cpp
        src/windows_ocr.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    )
    target_include_directories(WarDogsDistanceCalculator PRIVATE include)
    target_compile_definitions(WarDogsDistanceCalculator PRIVATE
        UNICODE _UNICODE NOMINMAX WARDOGS_VERSION="${PROJECT_VERSION}"
    )
    target_compile_options(WarDogsDistanceCalculator PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(WarDogsDistanceCalculator PRIVATE
        wardogs_core wardogs_terrain wardogs_i18n wardogs_updates Qt6::Widgets dwmapi gdi32 user32 windowsapp
        shell32 ole32 propsys
    )
    add_custom_command(TARGET WarDogsDistanceCalculator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::Core>"
            "$<TARGET_FILE:Qt6::Gui>"
            "$<TARGET_FILE:Qt6::Widgets>"
            "$<TARGET_FILE:Qt6::Network>"
            "$<TARGET_FILE_DIR:WarDogsDistanceCalculator>"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:WarDogsDistanceCalculator>/tls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QSchannelBackendPlugin>"
            "$<TARGET_FILE_DIR:WarDogsDistanceCalculator>/tls"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:WarDogsDistanceCalculator>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
            "$<TARGET_FILE_DIR:WarDogsDistanceCalculator>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib/x64/onnxruntime.dll"
            "$<TARGET_FILE_DIR:WarDogsDistanceCalculator>/onnxruntime.dll"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:WarDogsDistanceCalculator>/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/models/PP-OCRv6_rec_small.onnx"
            "$<TARGET_FILE_DIR:WarDogsDistanceCalculator>/models/PP-OCRv6_rec_small.onnx"
    )
endif()

install(TARGETS WarDogsDistanceCalculator RUNTIME DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/tools/Update.ps1" DESTINATION .)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib/x64/onnxruntime.dll"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/README-EN.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/TERRAIN_DATA_NOTICE.md"
    DESTINATION .
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/QUICKSTART-RU.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/QUICKSTART-EN.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/LOCALIZATION.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/CALCULATIONS-EN.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/RELEASE-NOTES-EN.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/ARCHITECTURE-EN.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/ANTICHEAT-EN.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/RELEASE-NOTES-RU.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/ARCHITECTURE-RU.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/ANTICHEAT-RU.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/SUPPORT-APPROVAL-DRAFT-RU.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/CALCULATIONS-RU.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/COORDINATE-WORKFLOW-RESEARCH-RU.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/COORDINATE-SOURCES-INVESTIGATION-RU.md"
    DESTINATION . OPTIONAL
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/TERRAIN_DATA_NOTICE.md"
    DESTINATION terrain-packs
)
# Community-hosted game data is retained locally for provenance/verification,
# but is not licensed for bundling into this fork's redistributable package.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/LICENSE.txt"
    DESTINATION licenses/zstd
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/models/PP-OCRv6_rec_small.onnx"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/models/LICENSE.PaddleOCR.txt"
    DESTINATION models
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/ThirdPartyNotices.txt"
    DESTINATION licenses/onnxruntime
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/translations/QT-TRANSLATIONS-NOTICE.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/translations/qtbase_ru.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qt/qtbase_ru-6.8.3.ts"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qt/LGPL-3.0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qt/GPL-3.0.txt"
    DESTINATION licenses/qt-translations
)

enable_testing()
add_executable(wardogs_update_tests tests/update_service_tests.cpp)
target_link_libraries(wardogs_update_tests PRIVATE wardogs_updates)
target_compile_options(wardogs_update_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME wardogs_update_tests COMMAND wardogs_update_tests)
add_executable(wardogs_update_probe tools/update_probe.cpp)
target_link_libraries(wardogs_update_probe PRIVATE wardogs_updates)
target_compile_options(wardogs_update_probe PRIVATE /W4 /permissive- /utf-8)
add_executable(wardogs_update_ui_tests tests/update_ui_tests.cpp)
target_link_libraries(wardogs_update_ui_tests PRIVATE wardogs_updates)
target_compile_options(wardogs_update_ui_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME wardogs_update_ui_tests COMMAND wardogs_update_ui_tests)
set_tests_properties(wardogs_update_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
add_test(NAME wardogs_update_installer_tests COMMAND powershell.exe
    -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/update_installer_tests.ps1"
    -ExecutablePath "$<TARGET_FILE:WarDogsDistanceCalculator>")
set_tests_properties(wardogs_update_installer_tests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
add_executable(wardogs_tests tests/core_tests.cpp)
target_link_libraries(wardogs_tests PRIVATE wardogs_core)
if(MSVC)
    target_compile_options(wardogs_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_core_tests COMMAND wardogs_tests)

add_executable(wardogs_mouse_trigger_tests tests/mouse_trigger_tests.cpp)
target_link_libraries(wardogs_mouse_trigger_tests PRIVATE wardogs_core user32)
if(MSVC)
    target_compile_options(wardogs_mouse_trigger_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_mouse_trigger_tests COMMAND wardogs_mouse_trigger_tests)

add_executable(wardogs_hotkeys_tests tests/hotkeys_tests.cpp)
target_link_libraries(wardogs_hotkeys_tests PRIVATE wardogs_core user32)
if(MSVC)
    target_compile_options(wardogs_hotkeys_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_hotkeys_tests COMMAND wardogs_hotkeys_tests)

add_executable(wardogs_capture_tests tests/capture_tests.cpp src/capture.cpp)
target_include_directories(wardogs_capture_tests PRIVATE include)
target_compile_definitions(wardogs_capture_tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(wardogs_capture_tests PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(wardogs_capture_tests PRIVATE user32 gdi32)
add_test(NAME wardogs_capture_tests COMMAND wardogs_capture_tests)

add_executable(capture_fixture_window tools/capture_fixture_window.cpp)
target_link_libraries(capture_fixture_window PRIVATE wardogs_core user32 gdi32)
target_compile_options(capture_fixture_window PRIVATE /W4 /permissive- /utf-8)
add_test(NAME wardogs_native_chat_capture_tests COMMAND capture_fixture_window
    "$<TARGET_FILE:WarDogsDistanceCalculator>"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/coordinate.png"
    "${CMAKE_CURRENT_BINARY_DIR}/Testing/native-chat-capture.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/chat_draft_live_1.png")
set_tests_properties(wardogs_native_chat_capture_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows" TIMEOUT 40 RUN_SERIAL TRUE)

add_executable(wardogs_logger_tests tests/logger_tests.cpp)
target_link_libraries(wardogs_logger_tests PRIVATE wardogs_core)
target_compile_options(wardogs_logger_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME wardogs_logger_tests COMMAND wardogs_logger_tests)

add_executable(wardogs_vehicle_ballistics_tests tests/vehicle_ballistics_tests.cpp)
target_link_libraries(wardogs_vehicle_ballistics_tests PRIVATE wardogs_core)
if(MSVC)
    target_compile_options(wardogs_vehicle_ballistics_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_vehicle_ballistics_tests COMMAND wardogs_vehicle_ballistics_tests)

add_executable(wardogs_continuous_calibration_tests
    tests/continuous_calibration_tests.cpp)
target_link_libraries(wardogs_continuous_calibration_tests PRIVATE wardogs_core)
if(MSVC)
    target_compile_options(wardogs_continuous_calibration_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_continuous_calibration_tests
    COMMAND wardogs_continuous_calibration_tests)

add_executable(wardogs_ghost_reticle_tests tests/ghost_reticle_tests.cpp)
target_link_libraries(wardogs_ghost_reticle_tests PRIVATE wardogs_core)
if(MSVC)
    target_compile_options(wardogs_ghost_reticle_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_ghost_reticle_tests COMMAND wardogs_ghost_reticle_tests)

add_executable(wardogs_terrain_tests tests/terrain_package_tests.cpp)
target_link_libraries(wardogs_terrain_tests PRIVATE wardogs_terrain Qt6::Core)
target_compile_definitions(wardogs_terrain_tests PRIVATE
    WARDOGS_TERRAIN_TEST_PACKAGE=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/terrain_test.wdt"
)
if(MSVC)
    target_compile_options(wardogs_terrain_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_terrain_tests COMMAND wardogs_terrain_tests)
add_custom_command(TARGET wardogs_terrain_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>"
        "$<TARGET_FILE_DIR:wardogs_terrain_tests>"
)

add_executable(wardogs_vehicle_ui_tests
    tests/vehicle_ui_tests.cpp
    src/app_icon.cpp
    src/capture.cpp
    src/ghost_reticle_window.cpp
    src/pinned_result_window.cpp
    src/settings_dialog.cpp
    src/selection_overlay.cpp
    src/vehicle_solution_widget.cpp
    src/window_title_bar.cpp
)
target_include_directories(wardogs_vehicle_ui_tests PRIVATE include src)
target_link_libraries(wardogs_vehicle_ui_tests PRIVATE
    wardogs_core wardogs_i18n Qt6::Widgets dwmapi user32 gdi32
)
if(MSVC)
    target_compile_options(wardogs_vehicle_ui_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_vehicle_ui_tests COMMAND wardogs_vehicle_ui_tests)
add_custom_command(TARGET wardogs_vehicle_ui_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>"
        "$<TARGET_FILE:Qt6::Gui>"
        "$<TARGET_FILE:Qt6::Widgets>"
        "$<TARGET_FILE_DIR:wardogs_vehicle_ui_tests>"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:wardogs_vehicle_ui_tests>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
        "$<TARGET_FILE_DIR:wardogs_vehicle_ui_tests>/platforms"
)

add_executable(wardogs_localization_tests
    tests/localization_tests.cpp
    src/app_icon.cpp
    src/capture.cpp
    src/ghost_reticle_window.cpp
    src/pinned_result_window.cpp
    src/settings_dialog.cpp
    src/selection_overlay.cpp
    src/vehicle_solution_widget.cpp
    src/window_title_bar.cpp
)
target_include_directories(wardogs_localization_tests PRIVATE include src)
target_link_libraries(wardogs_localization_tests PRIVATE
    wardogs_core wardogs_i18n Qt6::Widgets dwmapi user32 gdi32)
if(MSVC)
    target_compile_options(wardogs_localization_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_localization_tests COMMAND wardogs_localization_tests)

add_executable(wardogs_settings_tests
    tests/settings_tests.cpp
    src/settings.cpp
)
target_include_directories(wardogs_settings_tests PRIVATE include)
target_compile_definitions(wardogs_settings_tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(wardogs_settings_tests PRIVATE wardogs_core shell32 ole32)
if(MSVC)
    target_compile_options(wardogs_settings_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME wardogs_settings_tests COMMAND wardogs_settings_tests)

add_executable(wardogs_ocr_tests tests/ocr_tests.cpp)
target_link_libraries(wardogs_ocr_tests PRIVATE wardogs_core)
target_compile_definitions(wardogs_ocr_tests PRIVATE
    WARDOGS_TEST_MODEL=L"${CMAKE_CURRENT_SOURCE_DIR}/assets/models/PP-OCRv6_rec_small.onnx"
    WARDOGS_TEST_IMAGE=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/coordinate.png"
    WARDOGS_TEST_EXTRA_TEXT_IMAGE=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/coordinate_with_extra_text.png"
    WARDOGS_TEST_REPEATED_IMAGE=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/coordinate_repeated_digit.png"
)
if(MSVC)
    target_compile_options(wardogs_ocr_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_custom_command(TARGET wardogs_ocr_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib/x64/onnxruntime.dll"
        "$<TARGET_FILE_DIR:wardogs_ocr_tests>/onnxruntime.dll"
)
add_test(NAME wardogs_ocr_tests COMMAND wardogs_ocr_tests)

add_executable(wardogs_map_ocr_tests tests/map_ocr_tests.cpp)
target_link_libraries(wardogs_map_ocr_tests PRIVATE wardogs_core)
target_compile_options(wardogs_map_ocr_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME wardogs_map_ocr_tests COMMAND wardogs_map_ocr_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/models/PP-OCRv6_rec_small.onnx"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/map_live_axis_x.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/map_live_axis_y.png")

add_executable(ocr_benchmark tools/ocr_benchmark.cpp)
target_link_libraries(ocr_benchmark PRIVATE wardogs_core psapi)
target_compile_options(ocr_benchmark PRIVATE /W4 /permissive- /utf-8)
add_custom_command(TARGET ocr_benchmark POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib/x64/onnxruntime.dll"
        "$<TARGET_FILE_DIR:ocr_benchmark>/onnxruntime.dll"
)

if(WIN32)
    add_executable(windows_ocr_probe
        tools/windows_ocr_probe.cpp
        src/windows_ocr.cpp
    )
    target_include_directories(windows_ocr_probe PRIVATE include)
    target_compile_definitions(windows_ocr_probe PRIVATE UNICODE _UNICODE NOMINMAX)
    target_compile_options(windows_ocr_probe PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(windows_ocr_probe PRIVATE wardogs_core windowsapp psapi)

    add_executable(wardogs_windows_ocr_tests tests/windows_ocr_tests.cpp src/windows_ocr.cpp)
    target_include_directories(wardogs_windows_ocr_tests PRIVATE include)
    target_compile_definitions(wardogs_windows_ocr_tests PRIVATE UNICODE _UNICODE NOMINMAX
        WARDOGS_TEST_IMAGE=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/coordinate.png")
    target_compile_options(wardogs_windows_ocr_tests PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(wardogs_windows_ocr_tests PRIVATE wardogs_core windowsapp)
    add_test(NAME wardogs_windows_ocr_tests COMMAND wardogs_windows_ocr_tests)
    set_tests_properties(wardogs_windows_ocr_tests PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_test(NAME wardogs_app_workflow_tests
    COMMAND WarDogsDistanceCalculator
        "--self-test=${CMAKE_CURRENT_BINARY_DIR}/Testing/app-workflow.json"
        "--test-image=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/coordinate.png")
set_tests_properties(wardogs_app_workflow_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 30)

add_test(NAME wardogs_app_native_workflow_tests
    COMMAND WarDogsDistanceCalculator
        "--self-test=${CMAKE_CURRENT_BINARY_DIR}/Testing/app-native-workflow.json"
        "--test-image=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/coordinate.png")
set_tests_properties(wardogs_app_native_workflow_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows"
    TIMEOUT 30)

add_test(NAME wardogs_app_english_workflow_tests
    COMMAND WarDogsDistanceCalculator --language=en
        "--self-test=${CMAKE_CURRENT_BINARY_DIR}/Testing/app-english-workflow.json"
        "--test-image=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/coordinate.png")
set_tests_properties(wardogs_app_english_workflow_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 40)
add_test(NAME wardogs_app_english_native_workflow_tests
    COMMAND WarDogsDistanceCalculator --language=en
        "--self-test=${CMAKE_CURRENT_BINARY_DIR}/Testing/app-english-native-workflow.json"
        "--test-image=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/coordinate.png")
set_tests_properties(wardogs_app_english_native_workflow_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows" TIMEOUT 40 RUN_SERIAL TRUE)

add_executable(terrain_probe tools/terrain_probe.cpp)
target_compile_options(terrain_probe PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(terrain_probe PRIVATE wardogs_terrain Qt6::Core)
