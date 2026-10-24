cmake_minimum_required(VERSION 3.20)
project(FitomSf2IF CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── MSVC UTF-8 オプション ──────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ─── fluidsynth (vcpkg) ─────────────────────────────────────────────────────
# vcpkg install fluidsynth
find_package(fluidsynth CONFIG REQUIRED)
if(TARGET FluidSynth::libfluidsynth)
    set(_FLUIDSYNTH_TARGET FluidSynth::libfluidsynth)
elseif(TARGET fluidsynth::libfluidsynth)
    set(_FLUIDSYNTH_TARGET fluidsynth::libfluidsynth)
else()
    message(FATAL_ERROR
        "fluidsynth の CMake ターゲットが見つかりません。\n"
        "  vcpkg install fluidsynth\n"
        "でインストールしたうえで、CMAKE_TOOLCHAIN_FILE に vcpkg.cmake を指定してください。")
endif()
message(STATUS "fluidsynth found: ${_FLUIDSYNTH_TARGET}")

# 音声出力はfluidsynth自身のaudio driver層(fluid_audio_driver_t)を使う。
# fluidsynthをプラグインへ完全に分離した結果、自前のオーディオライブラリ(RtAudio等)
# を別途持つ理由が無くなったため、依存を増やさずfluidsynthに含まれる音声出力機能を
# そのまま利用する(2026年7月、依存削減のため設計変更)。

# ─── nlohmann/json (vcpkg) ──────────────────────────────────────────────────
# vcpkg install nlohmann-json
find_package(nlohmann_json CONFIG REQUIRED)

# ─── fitom_sf2if — FITOM_X HW I/F プラグイン DLL ────────────────────────────
# plugin_sdk/include/fitom/ に IHWPlugin.h を配置している(FITOM_X からの独立コピー)。
set(FITOM_SDK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugin_sdk/include"
    CACHE PATH "FITOM plugin_sdk/include ディレクトリ")

add_library(FitomSf2IF SHARED
    src/Sf2IfImpl.cpp
    src/FluidSynthEngine.cpp
    src/MidiEventParser.cpp
)

target_compile_definitions(FitomSf2IF PRIVATE FITOM_HW_PLUGIN_EXPORTS)

target_include_directories(FitomSf2IF PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FITOM_SDK_INCLUDE_DIR}
)

target_link_libraries(FitomSf2IF PRIVATE
    ${_FLUIDSYNTH_TARGET}
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(FitomSf2IF PRIVATE /utf-8)
endif()

# Windows: vcpkg が依存 DLL を自動コピー (VCPKG_APPLOCAL_DEPS=ON, CMakePresets.json 参照)
install(TARGETS FitomSf2IF
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# ─── スモークテスト(オプション) ─────────────────────────────────────────────
option(BUILD_FITOMSF2IF_TEST "Build smoke test" OFF)
if(BUILD_FITOMSF2IF_TEST)
    add_executable(FitomSf2IF_test examples/smoke_test.cpp)
    target_include_directories(FitomSf2IF_test PRIVATE
        ${FITOM_SDK_INCLUDE_DIR}
    )
    if(WIN32)
        target_link_libraries(FitomSf2IF_test PRIVATE FitomSf2IF)
    else()
        target_link_libraries(FitomSf2IF_test PRIVATE FitomSf2IF ${CMAKE_DL_LIBS})
    endif()
    if(MSVC)
        target_compile_options(FitomSf2IF_test PRIVATE /utf-8)
    endif()

    # 音声デバイスを使わないので CTest に登録する(スモークテストは実デバイスを鳴らすので登録しない)
    enable_testing()
    add_executable(FitomSf2IF_offline_render_test tests/offline_render_test.cpp)
    target_include_directories(FitomSf2IF_offline_render_test PRIVATE
        ${FITOM_SDK_INCLUDE_DIR}
    )
    target_link_libraries(FitomSf2IF_offline_render_test PRIVATE FitomSf2IF)
    if(MSVC)
        target_compile_options(FitomSf2IF_offline_render_test PRIVATE /utf-8)
    endif()
    add_test(NAME offline_render COMMAND FitomSf2IF_offline_render_test)
endif()
