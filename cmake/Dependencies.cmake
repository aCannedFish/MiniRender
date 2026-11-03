include(FetchContent)

# 优先使用相同版本的已安装依赖；否则下载经过校验的固定源码归档。
find_package(glm 1.0.3 EXACT CONFIG QUIET)
if(NOT glm_FOUND)
    set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glm
        URL https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz
        URL_HASH SHA256=6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001)
    FetchContent_MakeAvailable(glm)
endif()

if(BUILD_TESTING)
    find_package(Catch2 3.16.1 EXACT CONFIG QUIET)
    if(NOT Catch2_FOUND)
        FetchContent_Declare(Catch2
            URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz
            URL_HASH SHA256=d16bd1b8b2364918bd472cb216394acb93bde6784890df0d91bccfb3f28d777d)
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
endif()

find_package(nlohmann_json 3.12.0 EXACT CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
        URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

set(TINYOBJLOADER_BUILD_TEST_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(tinyobjloader
    URL https://github.com/tinyobjloader/tinyobjloader/archive/refs/tags/v2.0.0rc13.tar.gz
    URL_HASH SHA256=0feb92b838f8ce4aa6eb0ccc32dff30cb64a891e0ec3bde837fca49c78d44334)
FetchContent_MakeAvailable(tinyobjloader)

# TinyEXR 源码归档包含大量测试图；只取版本固定、逐个校验的 codec 文件。
# SOURCE_SUBDIR 指向不存在的目录，避免执行上游仅用于自测的 CMakeLists。
FetchContent_Declare(tinyexr_header
    URL https://raw.githubusercontent.com/syoyo/tinyexr/v1.0.12/tinyexr.h
    URL_HASH SHA256=e3eb50490af81dc3f5f067cf7f62955894d5db8f88a091c19bc4eef8e468095f
    DOWNLOAD_NO_EXTRACT TRUE SOURCE_SUBDIR minirender_vendor_only)
FetchContent_Declare(tinyexr_miniz_source
    URL https://raw.githubusercontent.com/syoyo/tinyexr/v1.0.12/deps/miniz/miniz.c
    URL_HASH SHA256=44d54a3caedf3eae0adc6b6e5a5024c5ba6543abc81d20e741d0353b801e57d3
    DOWNLOAD_NO_EXTRACT TRUE SOURCE_SUBDIR minirender_vendor_only)
FetchContent_Declare(tinyexr_miniz_header
    URL https://raw.githubusercontent.com/syoyo/tinyexr/v1.0.12/deps/miniz/miniz.h
    URL_HASH SHA256=295d1a0041aea09609598c0f1f35c1977ca05ad662acbadcfdaac44c140af37b
    DOWNLOAD_NO_EXTRACT TRUE SOURCE_SUBDIR minirender_vendor_only)
FetchContent_MakeAvailable(tinyexr_header tinyexr_miniz_source tinyexr_miniz_header)
add_library(minirender_exr_codec STATIC
    "${PROJECT_SOURCE_DIR}/src/external/exr_codec.cpp"
    "${tinyexr_miniz_source_SOURCE_DIR}/miniz.c")
target_include_directories(minirender_exr_codec SYSTEM PUBLIC "${tinyexr_header_SOURCE_DIR}"
    PRIVATE "${tinyexr_miniz_header_SOURCE_DIR}")
target_compile_features(minirender_exr_codec PRIVATE c_std_11)

if(MSVC)
    target_compile_options(minirender_exr_codec PRIVATE /utf-8)
endif()

FetchContent_Declare(stb_image_header
    URL https://raw.githubusercontent.com/nothings/stb/d14d46f6b44456eb88beac58cba833a4135d4c89/stb_image.h
    URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    DOWNLOAD_NO_EXTRACT TRUE SOURCE_SUBDIR minirender_vendor_only)
FetchContent_Declare(stb_write_header
    URL https://raw.githubusercontent.com/nothings/stb/d14d46f6b44456eb88beac58cba833a4135d4c89/stb_image_write.h
    URL_HASH SHA256=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
    DOWNLOAD_NO_EXTRACT TRUE SOURCE_SUBDIR minirender_vendor_only)
FetchContent_MakeAvailable(stb_image_header stb_write_header)
add_library(minirender_raster_codec STATIC "${PROJECT_SOURCE_DIR}/src/external/raster_codec.cpp")
target_include_directories(minirender_raster_codec SYSTEM PUBLIC "${stb_image_header_SOURCE_DIR}" "${stb_write_header_SOURCE_DIR}")
if(MSVC)
    target_compile_options(minirender_raster_codec PRIVATE /utf-8)
endif()
