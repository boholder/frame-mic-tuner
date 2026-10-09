cmake_minimum_required(VERSION 3.16)
project(frame-mic-tuner VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# 回帰テスト（純粋なロジックだけ。cairo・PipeWire・Vulkan・OpenVR は要らない）。
# 使い方: cmake -DBUILD_TESTING=ON -DFRAME_MIC_TUNER_APP=OFF -S . -B build && cmake --build build && ctest --test-dir build
option(BUILD_TESTING "回帰テストをビルドする" OFF)
option(FRAME_MIC_TUNER_APP "アプリ本体をビルドする" ON)

if(FRAME_MIC_TUNER_APP)
  # SteamVR に同梱の OpenVR ライブラリ（Frame の SteamOS 上のパス）
  set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "libopenvr_api.so のあるディレクトリ")

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
  pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
  pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
  # 声のチェックの録音・再生（pw_stream）
  pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

  add_executable(frame-mic-tuner
    src/main.cpp
    src/config.cpp
    src/json.cpp
    src/command.cpp
    src/mic_state.cpp
    src/mic_worker.cpp
    src/mic_panel.cpp
    src/theme.cpp
    src/voice_check.cpp
    src/vr_overlay.cpp
    src/vk_texture.cpp
    src/draw.cpp
    src/i18n.cpp
    vendor/frame-updater/cpp/update_check.cpp
  )
  target_include_directories(frame-mic-tuner PRIVATE third_party/openvr vendor/frame-updater/cpp)
  target_compile_options(frame-mic-tuner PRIVATE -Wall -Wextra)
  # --version で出す版（CHANGELOG.md と合わせる）
  target_compile_definitions(frame-mic-tuner PRIVATE FRAME_MIC_TUNER_VERSION="${PROJECT_VERSION}")
  target_link_libraries(frame-mic-tuner PRIVATE
    PkgConfig::CAIRO
    PkgConfig::FREETYPE
    PkgConfig::VULKAN
    PkgConfig::PIPEWIRE
    Threads::Threads
    "${OPENVR_LIB_DIR}/libopenvr_api.so"
  )
  # 実行時も SteamVR 同梱のライブラリを使う
  set_target_properties(frame-mic-tuner PROPERTIES
    BUILD_RPATH "${OPENVR_LIB_DIR}"
    INSTALL_RPATH "${OPENVR_LIB_DIR}"
  )

  install(TARGETS frame-mic-tuner RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  enable_testing()
  add_executable(frame-mic-tuner-tests
    tests/tests.cpp
    src/json.cpp
    src/config.cpp
    src/i18n.cpp
    src/mic_state.cpp
    src/command.cpp
    src/mic_worker.cpp
    src/theme.cpp
  )
  target_include_directories(frame-mic-tuner-tests PRIVATE src)
  target_compile_options(frame-mic-tuner-tests PRIVATE -Wall -Wextra)
  target_link_libraries(frame-mic-tuner-tests PRIVATE Threads::Threads)
  add_test(NAME unit COMMAND frame-mic-tuner-tests)
endif()
