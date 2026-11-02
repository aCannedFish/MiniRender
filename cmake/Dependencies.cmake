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
