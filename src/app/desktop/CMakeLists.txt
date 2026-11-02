find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Vulkan 1.2 REQUIRED COMPONENTS glslangValidator)

# SPIR-V 随可执行文件嵌入 Qt 资源，运行不依赖源码目录或当前工作目录。
set(shader_outputs)
foreach(stage vert frag)
    set(source "${PROJECT_SOURCE_DIR}/shaders/diagnostic.${stage}")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/diagnostic.${stage}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND Vulkan::glslangValidator --target-env vulkan1.2 -V "${source}" -o "${output}"
        DEPENDS "${source}" VERBATIM)
    list(APPEND shader_outputs "${output}")
endforeach()

qt_add_executable(minirender_desktop
    main.cpp vulkan_viewport.cpp vulkan_viewport.hpp
    diagnostic_renderer.cpp diagnostic_renderer.hpp)
qt_add_resources(minirender_desktop diagnostic_shaders
    PREFIX "/shaders" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${shader_outputs})
target_link_libraries(minirender_desktop PRIVATE
    minirender_core minirender_warnings Qt6::Widgets Vulkan::Vulkan)
