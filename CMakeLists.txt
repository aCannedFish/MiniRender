cmake_minimum_required(VERSION 3.24)
project(MiniRender VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MINIRENDER_BUILD_DESKTOP "Build the Qt/Vulkan desktop application" OFF)
option(MINIRENDER_ENABLE_OPTIX "Enable the NVIDIA backend (B2 checkpoint)" OFF)
include(CTest)
include(cmake/Dependencies.cmake)
find_package(Threads REQUIRED)

# 不在 core 配置中发现或启用 GPU SDK，确保 CPU 回归可在无 GPU 的 CI 运行。
if(MINIRENDER_ENABLE_OPTIX)
    message(FATAL_ERROR "OptiX backend is not implemented yet; use a CPU/desktop preset until B2.")
endif()

add_library(minirender_warnings INTERFACE)
if(MSVC)
    target_compile_options(minirender_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(minirender_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_library(minirender_core
    src/core/image.cpp
    src/core/texture.cpp
    src/core/sampling.cpp
    src/core/raster_io.cpp
    src/core/exr_io.cpp
    src/scene/scene.cpp
    src/scene/editor.cpp
    src/scene/gizmo.cpp
    src/scene/shadow.cpp
    src/geometry/halfedge.cpp
    src/geometry/loop.cpp
    src/geometry/normals.cpp
    src/geometry/displacement.cpp
    src/geometry/surface.cpp
    src/geometry/simplify.cpp
    src/render/intersection.cpp
    src/render/bvh.cpp
    src/render/bsdf.cpp
    src/render/light.cpp
    src/render/path_tracer.cpp
    src/render/render_session.cpp
    src/render/ssao.cpp
    src/render/ibl.cpp
    src/render/raster_mesh.cpp
    src/assets/obj_loader.cpp
    src/assets/obj_writer.cpp
    src/assets/asset_writer.cpp
    src/assets/staged_file.cpp
    src/assets/scene_io.cpp)
target_include_directories(minirender_core PUBLIC include)
target_link_libraries(minirender_core PUBLIC glm::glm Threads::Threads PRIVATE minirender_warnings nlohmann_json::nlohmann_json tinyobjloader minirender_exr_codec minirender_raster_codec)
target_compile_definitions(minirender_core PUBLIC
    GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)

add_executable(minirender_cli src/app/cli.cpp src/app/render_cli.cpp)
target_link_libraries(minirender_cli PRIVATE minirender_core minirender_warnings)

if(BUILD_TESTING)
    # 教学示例独立运行，较慢的图像生成不加入每次 CTest。
    add_executable(minirender_loop_example examples/loop.cpp)
    target_compile_definitions(minirender_loop_example PRIVATE MINIRENDER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(minirender_loop_example PRIVATE minirender_core minirender_warnings)
    add_executable(minirender_displacement_example examples/displacement.cpp)
    target_compile_definitions(minirender_displacement_example PRIVATE MINIRENDER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(minirender_displacement_example PRIVATE minirender_core minirender_warnings)
    add_executable(minirender_simplify_example examples/simplify.cpp)
    target_compile_definitions(minirender_simplify_example PRIVATE MINIRENDER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(minirender_simplify_example PRIVATE minirender_core minirender_warnings)
    add_executable(minirender_tests tests/core_tests.cpp tests/scene_tests.cpp tests/texture_tests.cpp
        tests/intersection_tests.cpp tests/sampling_tests.cpp tests/light_tests.cpp tests/path_tests.cpp
        tests/session_tests.cpp tests/ssao_tests.cpp tests/ibl_tests.cpp tests/geometry_tests.cpp
        tests/displacement_tests.cpp tests/mesh_io_tests.cpp tests/raster_mesh_tests.cpp
        tests/simplify_tests.cpp tests/gizmo_tests.cpp tests/sided_tests.cpp)
    target_compile_definitions(minirender_tests PRIVATE MINIRENDER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(minirender_tests PRIVATE
        minirender_core Catch2::Catch2WithMain minirender_warnings Threads::Threads)
    include(Catch)
    catch_discover_tests(minirender_tests PROPERTIES TIMEOUT 120)
    add_test(NAME cli_capabilities COMMAND minirender_cli --capabilities)
    set_tests_properties(cli_capabilities PROPERTIES
        PASS_REGULAR_EXPRESSION "optix: unavailable")
    add_test(NAME cli_trace_normals COMMAND minirender_cli --trace-normals
        "${PROJECT_SOURCE_DIR}/scenes/studio.json" "${CMAKE_CURRENT_BINARY_DIR}/cli-normals.png")
    set_tests_properties(cli_trace_normals PROPERTIES PASS_REGULAR_EXPRESSION "triangles: 26")
    add_test(NAME cli_path_render COMMAND minirender_cli --render
        "${PROJECT_SOURCE_DIR}/scenes/studio.json" "${CMAKE_CURRENT_BINARY_DIR}/路径 空格/cli-path"
        --width 4 --height 4 --spp 4 --depth 3 --seed 9 --exposure 0.5 --threads 2 --tile 2 --batch 2)
    set_tests_properties(cli_path_render PROPERTIES PASS_REGULAR_EXPRESSION "paths: 64")
    add_test(NAME cli_invalid_exposure COMMAND minirender_cli --render
        "${PROJECT_SOURCE_DIR}/scenes/studio.json" "${CMAKE_CURRENT_BINARY_DIR}/invalid-path"
        --exposure 1oops)
    set_tests_properties(cli_invalid_exposure PROPERTIES WILL_FAIL TRUE)
endif()

if(MINIRENDER_BUILD_DESKTOP)
    add_subdirectory(src/app/desktop)
endif()
