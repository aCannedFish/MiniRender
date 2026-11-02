cmake_minimum_required(VERSION 3.24)
project(MiniRender VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MINIRENDER_BUILD_DESKTOP "Build the Qt/Vulkan desktop application" OFF)
option(MINIRENDER_ENABLE_OPTIX "Enable the NVIDIA backend (B2 checkpoint)" OFF)
include(CTest)
include(cmake/Dependencies.cmake)

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
    src/scene/scene.cpp
    src/scene/editor.cpp
    src/scene/shadow.cpp
    src/assets/obj_loader.cpp
    src/assets/scene_io.cpp)
target_include_directories(minirender_core PUBLIC include)
target_link_libraries(minirender_core PUBLIC glm::glm PRIVATE minirender_warnings nlohmann_json::nlohmann_json tinyobjloader)
target_compile_definitions(minirender_core PUBLIC
    GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)

add_executable(minirender_cli src/app/cli.cpp)
target_link_libraries(minirender_cli PRIVATE minirender_core minirender_warnings)

if(BUILD_TESTING)
    add_executable(minirender_tests tests/core_tests.cpp tests/scene_tests.cpp)
    target_compile_definitions(minirender_tests PRIVATE MINIRENDER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(minirender_tests PRIVATE
        minirender_core Catch2::Catch2WithMain minirender_warnings)
    include(Catch)
    catch_discover_tests(minirender_tests)
    add_test(NAME cli_capabilities COMMAND minirender_cli --capabilities)
    set_tests_properties(cli_capabilities PROPERTIES
        PASS_REGULAR_EXPRESSION "optix: unavailable")
endif()

if(MINIRENDER_BUILD_DESKTOP)
    add_subdirectory(src/app/desktop)
endif()
