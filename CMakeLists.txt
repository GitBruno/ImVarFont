cmake_minimum_required(VERSION 3.20)
project(ImVarFont VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Desktop GL 3.3 by default; GLES 3.0 for Raspberry Pi / embedded.
option(IMVARFONT_GLES "Target OpenGL ES 3.0" OFF)

find_package(Freetype REQUIRED)
find_package(OpenGL REQUIRED)

if(IMVARFONT_GLES)
    set(IMVARFONT_GL_LIBS GLESv2 EGL)
    message(STATUS "ImVarFont: OpenGL ES 3.0")
else()
    set(IMVARFONT_GL_LIBS OpenGL::GL)
endif()

include(FetchContent)

find_package(PkgConfig QUIET)
set(IMVARFONT_HARFBUZZ_FOUND FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(HARFBUZZ QUIET IMPORTED_TARGET harfbuzz)
    if(HARFBUZZ_FOUND)
        set(IMVARFONT_HARFBUZZ_FOUND TRUE)
    endif()
endif()

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        v1.2.1
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(imgui nfd glfw)

# Dear ImGui (+ GLFW / OpenGL3 backends + FreeType)
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
if(IMVARFONT_GLES)
    target_compile_definitions(imgui_impl PUBLIC IMGUI_IMPL_OPENGL_ES3)
endif()
target_include_directories(imgui_impl PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_impl PUBLIC glfw ${IMVARFONT_GL_LIBS} Freetype::Freetype)

# GPU analytic coverage (ImGui-free)
add_library(varfont_gl STATIC varfont_gl.cpp)
target_include_directories(varfont_gl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(varfont_gl PUBLIC ${IMVARFONT_GL_LIBS})

# Dear ImGui integration (+ FreeType morph core)
add_library(imvarfont STATIC imgui_var_font.cpp)
target_include_directories(imvarfont PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
)
target_link_libraries(imvarfont PUBLIC varfont_gl Freetype::Freetype imgui_impl)
if(IMVARFONT_HARFBUZZ_FOUND)
    target_compile_definitions(imvarfont PUBLIC IMVARFONT_USE_HARFBUZZ)
    target_link_libraries(imvarfont PUBLIC PkgConfig::HARFBUZZ)
    message(STATUS "ImVarFont: HarfBuzz enabled")
else()
    message(STATUS "ImVarFont: HarfBuzz not found (kern table only)")
endif()

# Interactive example
add_executable(ImVarFontExample example/main.cpp)
target_include_directories(ImVarFontExample PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(ImVarFontExample PRIVATE
    imvarfont imgui_impl glfw ${IMVARFONT_GL_LIBS} Freetype::Freetype nfd
)
set_target_properties(ImVarFontExample PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/example
)
