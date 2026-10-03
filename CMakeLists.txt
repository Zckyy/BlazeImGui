cmake_minimum_required(VERSION 3.20)
project(BlazeImGui VERSION 1.1.0 LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(BLAZE_BUILD_DEMO   "Build the standalone DX11 demo application" ON)
option(BLAZE_FETCH_IMGUI  "Download Dear ImGui with FetchContent. Turn OFF to provide your own 'imgui' target." ON)
# OFF builds a renderer-agnostic library for Integration C only (e.g. a D3D12 host that
# runs its own ImGui backend): no acrylic blur, no D3D11 / d3dcompiler dependency.
option(BLAZE_RENDERER_DX11 "Build the D3D11 renderer (Integration A/B and acrylic blur)" ON)
option(BLAZE_WARNINGS_AS_ERRORS "Treat warnings as errors in BlazeImGui targets (used by CI)" OFF)
set(BLAZE_IMGUI_TAG "v1.91.8" CACHE STRING "Dear ImGui git tag to fetch")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "BlazeImGui currently targets Windows only.")
endif()

# -----------------------------------------------------------------------------
# Dear ImGui
# -----------------------------------------------------------------------------
if(BLAZE_FETCH_IMGUI)
    include(FetchContent)
    FetchContent_Declare(imgui_src
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        ${BLAZE_IMGUI_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(imgui_src)

    add_library(imgui STATIC
        ${imgui_src_SOURCE_DIR}/imgui.cpp
        ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_src_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_src_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_src_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
    target_include_directories(imgui PUBLIC
        ${imgui_src_SOURCE_DIR}
        ${imgui_src_SOURCE_DIR}/backends)
    target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
    target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)
elseif(NOT TARGET imgui)
    message(FATAL_ERROR "BLAZE_FETCH_IMGUI is OFF but no 'imgui' target exists. Define one (with the win32 backend, plus dx11 when BLAZE_RENDERER_DX11 is ON) before adding BlazeImGui.")
endif()

# -----------------------------------------------------------------------------
# BlazeImGui library
# -----------------------------------------------------------------------------
add_library(blaze_imgui STATIC
    src/blaze.cpp
    src/theme.cpp
    src/widgets.cpp
    src/config.cpp
    src/json.cpp
    src/debug.cpp
    src/perf.cpp
    src/panels/panel_performance.cpp
    src/panels/panel_debug.cpp
    src/panels/panel_config.cpp
    src/panels/panel_settings.cpp)
add_library(Blaze::ImGui ALIAS blaze_imgui)

target_include_directories(blaze_imgui
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(blaze_imgui PUBLIC imgui dxgi psapi shell32)
if(BLAZE_RENDERER_DX11)
    target_sources(blaze_imgui PRIVATE src/blur_dx11.cpp)
    target_link_libraries(blaze_imgui PUBLIC d3d11 d3dcompiler)
else()
    target_sources(blaze_imgui PRIVATE src/blur_none.cpp)
    target_compile_definitions(blaze_imgui PUBLIC BLAZE_NO_DX11)
endif()
target_compile_definitions(blaze_imgui PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
if(MSVC)
    target_compile_options(blaze_imgui PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(imgui PRIVATE /utf-8)
    if(BLAZE_WARNINGS_AS_ERRORS)
        target_compile_options(blaze_imgui PRIVATE /WX)
    endif()
endif()

# -----------------------------------------------------------------------------
# Demo
# -----------------------------------------------------------------------------
if(BLAZE_BUILD_DEMO AND BLAZE_RENDERER_DX11)
    add_executable(blaze_demo WIN32 examples/dx11_demo/main.cpp)
    target_link_libraries(blaze_demo PRIVATE Blaze::ImGui)
    if(MSVC)
        target_compile_options(blaze_demo PRIVATE /W4 /permissive- /utf-8 $<$<BOOL:${BLAZE_WARNINGS_AS_ERRORS}>:/WX>)
    endif()
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT blaze_demo)
endif()

if(BLAZE_BUILD_DEMO)
    # Compile-check the panel template (in both renderer configurations) so it never rots.
    add_library(blaze_template_check OBJECT templates/panel_template.cpp)
    target_link_libraries(blaze_template_check PRIVATE Blaze::ImGui)
    if(MSVC)
        target_compile_options(blaze_template_check PRIVATE /W4 /permissive- /utf-8 $<$<BOOL:${BLAZE_WARNINGS_AS_ERRORS}>:/WX>)
    endif()
endif()
