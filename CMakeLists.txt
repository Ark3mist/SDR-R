cmake_minimum_required(VERSION 3.20)
project(OpenSDR-X VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# cardputer: SDL2 software-rendered 320x170 keyboard UI (no X11/Wayland/GL needed)
# desktop:   GLFW + OpenGL 3.3 mouse UI (development rig)
set(OPENSDRX_FRONTEND "cardputer" CACHE STRING "Front end to build: cardputer, desktop or both")
set_property(CACHE OPENSDRX_FRONTEND PROPERTY STRINGS cardputer desktop both)
option(OPENSDRX_BUILD_TESTS "Build headless unit tests" ON)

set(BUILD_CARDPUTER OFF)
set(BUILD_DESKTOP OFF)
if(OPENSDRX_FRONTEND MATCHES "^(cardputer|both)$")
    set(BUILD_CARDPUTER ON)
endif()
if(OPENSDRX_FRONTEND MATCHES "^(desktop|both)$")
    set(BUILD_DESKTOP ON)
endif()

# Cardputer Zero = Raspberry Pi CM0 (Cortex-A53, aarch64): NEON for the DSP.
# Apple Silicon (simulator builds) gets plain -O3; Clang already targets its NEON.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND NOT APPLE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=armv8-a+simd -mtune=cortex-a53")
    set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG -march=armv8-a+simd -mtune=cortex-a53")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# Audio back end: ALSA on Linux (Cardputer), SDL2 on macOS.
if(APPLE)
    set(_audio_default sdl)
else()
    set(_audio_default alsa)
endif()
set(OPENSDRX_AUDIO "${_audio_default}" CACHE STRING "Audio back end: alsa or sdl")
set_property(CACHE OPENSDRX_AUDIO PROPERTY STRINGS alsa sdl)

include(FetchContent)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(RTLSDR REQUIRED IMPORTED_TARGET librtlsdr)
if(OPENSDRX_AUDIO STREQUAL "alsa")
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    set(OSX_AUDIO_SRC src/audio/audio_sink_alsa.cpp)
    set(OSX_AUDIO_LIB PkgConfig::ALSA)
else()
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2>=2.0.18)
    set(OSX_AUDIO_SRC src/audio/audio_sink_sdl.cpp)
    set(OSX_AUDIO_LIB PkgConfig::SDL2)
endif()

# JSON for bookmarks: system package (nlohmann-json3-dev) or fetched, header-only.
find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---- Dear ImGui (docking branch, pinned tag) -----------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.5-docking GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui_core STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
target_include_directories(imgui_core PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# ---- Engine (no GUI dependency): rings, DSP, sources, audio --------------------
add_library(osx_core STATIC
    src/audio/audio_sink.cpp
    ${OSX_AUDIO_SRC}
    src/dsp/dsp_engine.cpp
    src/dsp/receiver.cpp
    src/dsp/rds.cpp
    src/dsp/spectral_nr.cpp
    src/dsp/spectrum_analyzer.cpp
    src/dsp/spectrum_data.cpp
    src/engine/bookmarks.cpp
    src/engine/radio.cpp
    src/source/file_source.cpp
    src/source/rtlsdr_source.cpp
    src/source/rtltcp_source.cpp
    src/source/sim_source.cpp)
target_include_directories(osx_core PUBLIC src)
target_link_libraries(osx_core PUBLIC PkgConfig::FFTW3F ${OSX_AUDIO_LIB} PkgConfig::RTLSDR nlohmann_json::nlohmann_json Threads::Threads)
# The DSP inner loops are plain float dot products; let the compiler
# reassociate them so they vectorise (NEON / SSE). No code relies on NaN/Inf.
target_compile_options(osx_core PRIVATE -Wall -Wextra -Wpedantic -ffast-math)

# Theme + colormaps are shared by both front ends (ImGui core only).
set(OSX_UI_COMMON src/gui/theme.cpp src/gui/colormaps.cpp)

# ---- Cardputer Zero front end ---------------------------------------------------
if(BUILD_CARDPUTER)
    if(NOT TARGET PkgConfig::SDL2)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2>=2.0.18)
    endif()
    add_executable(cardsdr
        src/cardputer/main.cpp
        src/cardputer/ui.cpp
        src/cardputer/video.cpp
        src/cardputer/input.cpp
        ${OSX_UI_COMMON}
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
    target_link_libraries(cardsdr PRIVATE osx_core imgui_core PkgConfig::SDL2)
    target_compile_options(cardsdr PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(cardsdr PRIVATE
        OPENSDRX_VERSION="${PROJECT_VERSION}"
        OPENSDRX_FONT_SRC_DIR="${imgui_SOURCE_DIR}/misc/fonts")
    add_custom_command(TARGET cardsdr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:cardsdr>/res/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${imgui_SOURCE_DIR}/misc/fonts/ProggyTiny.ttf ${imgui_SOURCE_DIR}/misc/fonts/ProggyClean.ttf
                $<TARGET_FILE_DIR:cardsdr>/res/fonts/)
    install(TARGETS cardsdr RUNTIME DESTINATION bin)
    install(FILES ${imgui_SOURCE_DIR}/misc/fonts/ProggyTiny.ttf ${imgui_SOURCE_DIR}/misc/fonts/ProggyClean.ttf
            DESTINATION share/opensdrx/fonts)
    install(PROGRAMS run_cardsdr.sh DESTINATION bin)
endif()

# ---- Desktop front end (GLFW + OpenGL 3.3) ----------------------------------------
if(BUILD_DESKTOP)
    find_package(OpenGL REQUIRED)
    find_package(GLEW REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3>=3.3)
    add_executable(opensdrx
        src/main.cpp
        src/gui/app.cpp
        src/gui/frequency_select.cpp
        src/gui/waterfall_gl.cpp
        src/gui/spectrum_view.cpp
        ${OSX_UI_COMMON}
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_link_libraries(opensdrx PRIVATE osx_core imgui_core PkgConfig::GLFW3 GLEW::GLEW OpenGL::GL)
    target_compile_options(opensdrx PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(opensdrx PRIVATE
        OPENSDRX_VERSION="${PROJECT_VERSION}"
        OPENSDRX_FONT_SRC_DIR="${imgui_SOURCE_DIR}/misc/fonts")
    add_custom_command(TARGET opensdrx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:opensdrx>/res/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf
                $<TARGET_FILE_DIR:opensdrx>/res/fonts/Roboto-Medium.ttf)
endif()

if(OPENSDRX_BUILD_TESTS)
    enable_testing()
    add_executable(osx_tests tests/test_core.cpp)
    target_link_libraries(osx_tests PRIVATE osx_core)
    add_test(NAME core COMMAND osx_tests)
    if(BUILD_CARDPUTER)
        add_executable(cardui_tests tests/test_ui.cpp src/cardputer/ui.cpp src/cardputer/input.cpp ${OSX_UI_COMMON})
        target_include_directories(cardui_tests PRIVATE src)
        target_link_libraries(cardui_tests PRIVATE osx_core imgui_core PkgConfig::SDL2)
        target_compile_options(cardui_tests PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME cardui COMMAND cardui_tests)
    endif()
endif()
