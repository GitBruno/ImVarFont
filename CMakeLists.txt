cmake_minimum_required(VERSION 3.20)
project(ImVarFonts VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Dependencies
#
# FreeType must be present on the system (or via vcpkg toolchain).
# imgui and glfw are fetched at configure time so users don't need to install
# them manually.
# ---------------------------------------------------------------------------

find_package(Freetype REQUIRED)
find_package(OpenGL   REQUIRED)

# HarfBuzz (GPOS/OpenType layout kerning) — optional but enabled when found
find_package(PkgConfig QUIET)
set(IMVARFONT_HARFBUZZ_FOUND FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(HARFBUZZ QUIET IMPORTED_TARGET harfbuzz)
    if(HARFBUZZ_FOUND)
        set(IMVARFONT_HARFBUZZ_FOUND TRUE)
    endif()
endif()

include(FetchContent)

# Dear ImGui  (docking branch – enables ImGuiDockspace for future use)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)

# Native File Dialog Extended
FetchContent_Declare(nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        v1.2.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nfd)

# GLFW
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(imgui glfw)

# ---------------------------------------------------------------------------
# Build imgui as a static library (imgui has no CMakeLists of its own)
# ---------------------------------------------------------------------------

add_library(imgui_impl STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
)
target_compile_definitions(imgui_impl PUBLIC IMGUI_ENABLE_FREETYPE)
target_include_directories(imgui_impl PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_impl PUBLIC glfw OpenGL::GL Freetype::Freetype)

# ---------------------------------------------------------------------------
# ImVarFont library
# ---------------------------------------------------------------------------

add_library(imvarfont STATIC
    imgui_var_font.cpp
)
target_include_directories(imvarfont PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${imgui_SOURCE_DIR}            # for imgui.h
)
target_link_libraries(imvarfont PUBLIC
    Freetype::Freetype
    imgui_impl
)
if(IMVARFONT_HARFBUZZ_FOUND)
    target_compile_definitions(imvarfont PUBLIC IMVARFONT_USE_HARFBUZZ)
    target_link_libraries(imvarfont PUBLIC PkgConfig::HARFBUZZ)
    message(STATUS "ImVarFont: HarfBuzz enabled (GPOS kerning)")
else()
    message(STATUS "ImVarFont: HarfBuzz not found — legacy kern table only")
endif()

# ---------------------------------------------------------------------------
# Example executable
# ---------------------------------------------------------------------------

add_executable(ImVarFontExample
    example/main.cpp
)
target_include_directories(ImVarFontExample PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(ImVarFontExample PRIVATE
    imvarfont
    imgui_impl
    glfw
    OpenGL::GL
    Freetype::Freetype
    nfd
)

# Put the binary next to the source so relative font paths work easily
set_target_properties(ImVarFontExample PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/example
)
