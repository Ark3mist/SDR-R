cmake_minimum_required(VERSION 3.20)
project(OpenSDR-X VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(OPENSDRX_BUILD_TESTS "Build headless unit tests" ON)

include(FetchContent)
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
find_package(PkgConfig)

# ---- GLFW (system package, else FetchContent) -------------------------------
if(PkgConfig_FOUND)
    pkg_check_modules(GLFW3 IMPORTED_TARGET glfw3>=3.3)
endif()
if(GLFW3_FOUND)
    add_library(osx_glfw ALIAS PkgConfig::GLFW3)
else()
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw)
    add_library(osx_glfw ALIAS glfw)
endif()

# ---- GLEW --------------------------------------------------------------------
find_package(GLEW REQUIRED)

# ---- FFTW3 (single precision) -------------------------------------------------
if(PkgConfig_FOUND)
    pkg_check_modules(FFTW3F IMPORTED_TARGET fftw3f)
endif()
if(FFTW3F_FOUND)
    add_library(osx_fftw3f ALIAS PkgConfig::FFTW3F)
else()
    find_library(FFTW3F_LIBRARY NAMES fftw3f libfftw3f-3 REQUIRED)
    find_path(FFTW3F_INCLUDE_DIR fftw3.h REQUIRED)
    add_library(osx_fftw3f_imp UNKNOWN IMPORTED)
    set_target_properties(osx_fftw3f_imp PROPERTIES
        IMPORTED_LOCATION "${FFTW3F_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFTW3F_INCLUDE_DIR}")
    add_library(osx_fftw3f ALIAS osx_fftw3f_imp)
endif()

# ---- Dear ImGui (docking branch, pinned tag) -----------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.5-docking GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC osx_glfw OpenGL::GL)

# ---- Core (GUI-independent: ring buffers, DSP, sources) -----------------------
add_library(osx_core STATIC
    src/dsp/dsp_engine.cpp
    src/dsp/spectrum_analyzer.cpp
    src/source/sim_source.cpp)
target_include_directories(osx_core PUBLIC src)
target_link_libraries(osx_core PUBLIC osx_fftw3f Threads::Threads)

# ---- Application ----------------------------------------------------------------
add_executable(opensdrx
    src/main.cpp
    src/gui/app.cpp
    src/gui/theme.cpp
    src/gui/colormaps.cpp
    src/gui/frequency_select.cpp
    src/gui/waterfall_gl.cpp
    src/gui/spectrum_view.cpp)
target_link_libraries(opensdrx PRIVATE osx_core imgui GLEW::GLEW OpenGL::GL)
target_compile_definitions(opensdrx PRIVATE
    OPENSDRX_VERSION="${PROJECT_VERSION}"
    OPENSDRX_FONT_SRC_DIR="${imgui_SOURCE_DIR}/misc/fonts")

# Ship the UI font next to the binary (res/fonts/), SDR++ style.
add_custom_command(TARGET opensdrx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:opensdrx>/res/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf
            $<TARGET_FILE_DIR:opensdrx>/res/fonts/Roboto-Medium.ttf)

foreach(tgt osx_core opensdrx)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /D_USE_MATH_DEFINES /DNOMINMAX)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

if(OPENSDRX_BUILD_TESTS)
    enable_testing()
    add_executable(osx_tests tests/test_core.cpp)
    target_link_libraries(osx_tests PRIVATE osx_core)
    add_test(NAME core COMMAND osx_tests)
endif()
