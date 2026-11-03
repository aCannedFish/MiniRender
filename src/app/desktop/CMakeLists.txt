find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Concurrent)
find_package(Vulkan 1.2 REQUIRED COMPONENTS glslangValidator)

# SPIR-V 随可执行文件嵌入 Qt 资源，运行不依赖源码目录或当前工作目录。
set(shader_outputs)
foreach(shader diagnostic.vert diagnostic.frag scene.vert scene.frag shadow.vert post.vert post.frag)
    set(source "${PROJECT_SOURCE_DIR}/shaders/${shader}")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND Vulkan::glslangValidator --target-env vulkan1.2 -I${PROJECT_SOURCE_DIR}/shaders -V "${source}" -o "${output}"
        DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/shaders/scene_frame.glsl" VERBATIM)
    list(APPEND shader_outputs "${output}")
endforeach()

qt_add_executable(minirender_desktop
    main.cpp vulkan_viewport.cpp vulkan_viewport.hpp
    diagnostic_renderer.cpp diagnostic_renderer.hpp
    viewport_renderer.hpp gpu_buffer.cpp gpu_buffer.hpp
    gpu_texture.cpp gpu_texture.hpp
    gpu_image.cpp gpu_image.hpp gpu_shader.cpp gpu_shader.hpp
    hdr_targets.cpp hdr_targets.hpp post_process.cpp post_process.hpp
    scene_renderer.cpp scene_renderer.hpp shadow_map.cpp shadow_map.hpp workbench.cpp workbench.hpp
    scene_smoke.cpp scene_smoke.hpp texture_smoke.cpp texture_smoke.hpp)
if(APPLE)
    set_target_properties(minirender_desktop PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.minirender.desktop"
        MACOSX_BUNDLE_BUNDLE_NAME "MiniRender")
endif()
qt_add_resources(minirender_desktop diagnostic_shaders
    PREFIX "/shaders" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${shader_outputs})
target_link_libraries(minirender_desktop PRIVATE
    minirender_core minirender_warnings Qt6::Widgets Qt6::Concurrent Vulkan::Vulkan)

# 示例资源随可执行文件复制，运行时不依赖源码目录。
add_custom_command(TARGET minirender_desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/scenes" "$<TARGET_FILE_DIR:minirender_desktop>/scenes")
