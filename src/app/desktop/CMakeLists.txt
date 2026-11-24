# 构建桌面工作台，编译并嵌入 GLSL、样式资源，部署内置场景。

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Concurrent)
find_package(Vulkan 1.2 REQUIRED COMPONENTS glslangValidator)

# SPIR-V 随可执行文件嵌入 Qt 资源，运行不依赖源码目录或当前工作目录。
set(shader_outputs)
foreach(shader gi_spatial.frag gi_copy.frag gi_temporal.frag depth_reduce.frag screen_trace.frag diagnostic.vert diagnostic.frag scene.vert scene.frag shadow.vert
    wire.vert wire.frag post.vert post.frag offline.frag screen.vert ssao.frag ao_filter.frag ao_composite.frag environment.frag
    preview_outline.frag gizmo.vert gizmo.frag point_preview.vert point_preview.frag sdf.vert sdf.frag sdf_display.frag)
    set(source "${PROJECT_SOURCE_DIR}/shaders/${shader}")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND Vulkan::glslangValidator --target-env vulkan1.2 -I${PROJECT_SOURCE_DIR}/shaders -V "${source}" -o "${output}"
        DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/shaders/scene_frame.glsl"
                "${PROJECT_SOURCE_DIR}/shaders/scene_instance.glsl"
                "${PROJECT_SOURCE_DIR}/shaders/shadow_filter.glsl"
                "${PROJECT_SOURCE_DIR}/shaders/rectangle_light.glsl"
                "${PROJECT_SOURCE_DIR}/shaders/rectangle_visibility.glsl"
                "${PROJECT_SOURCE_DIR}/shaders/screen_position.glsl"
                "${PROJECT_SOURCE_DIR}/shaders/ibl_sampling.glsl"
                "${PROJECT_SOURCE_DIR}/shaders/cloth.glsl" VERBATIM)
    list(APPEND shader_outputs "${output}")
endforeach()

qt_add_executable(minirender_desktop
    preview_smoke.cpp preview_smoke.hpp outline_smoke.cpp outline_smoke.hpp
    main.cpp vulkan_viewport.cpp vulkan_viewport.hpp
    editor_theme.cpp editor_theme.hpp workbench_panels.cpp workbench_panels.hpp theme_smoke.cpp theme_smoke.hpp
    builtin_scenes.cpp builtin_scenes.hpp quick_import_menu.cpp quick_import_menu.hpp
    resource_catalog.cpp resource_catalog.hpp catalog_browser.cpp catalog_browser.hpp
    reference_panel.cpp reference_panel.hpp
    comparison_controls.cpp comparison_controls.hpp
    shadow_smoke.cpp shadow_smoke.hpp rectangle_smoke.cpp rectangle_smoke.hpp
    integration_smoke.cpp integration_smoke.hpp
    denoise_smoke.cpp denoise_smoke.hpp
    temporal_transport.cpp temporal_transport.hpp
    screen_transport_smoke.cpp screen_transport_smoke.hpp
    screen_transport.cpp screen_transport.hpp
    prt_smoke.cpp prt_smoke.hpp
    prt_panel.cpp prt_panel.hpp prt_cache.cpp prt_cache.hpp
    rectangle_shadow.cpp rectangle_shadow.hpp
    gpu_rectangle.cpp gpu_rectangle.hpp
    catalog_smoke.cpp catalog_smoke.hpp
    sequence_viewer.cpp sequence_viewer.hpp workbench_catalog.cpp
    presets_smoke.cpp presets_smoke.hpp
    sides_smoke.cpp sides_smoke.hpp
    diagnostic_renderer.cpp diagnostic_renderer.hpp
    viewport_renderer.hpp gpu_buffer.cpp gpu_buffer.hpp
    gpu_texture.cpp gpu_texture.hpp
    gpu_ibl.cpp gpu_ibl.hpp
    gpu_cloth.cpp gpu_cloth.hpp
    gpu_subsurface.cpp gpu_subsurface.hpp
    cloth_smoke.cpp cloth_smoke.hpp
    cloth_material_panel.cpp cloth_material_panel.hpp
    subsurface_material_panel.cpp subsurface_material_panel.hpp
    subsurface_smoke.cpp subsurface_smoke.hpp
    cloth_controller.cpp cloth_controller.hpp
    cloth_panel.cpp cloth_panel.hpp cloth_workbench.cpp
    lightfield_smoke.cpp lightfield_smoke.hpp
    lightfield_panel.cpp lightfield_panel.hpp lightfield_controller.cpp lightfield_controller.hpp
    volume_panel.cpp volume_panel.hpp
    volume_smoke.cpp volume_smoke.hpp
    sppm_smoke.cpp sppm_smoke.hpp
    cloth_collider_panel.cpp cloth_collider_panel.hpp
    cloth_playback_smoke.cpp cloth_playback_smoke.hpp
    cloth_playback_deep_smoke.cpp cloth_playback_deep_smoke.hpp
    cloth_material_editor_smoke.cpp cloth_material_editor_smoke.hpp
    ibl_controller.cpp ibl_controller.hpp environment_panel.cpp environment_panel.hpp
    geometry_controller.cpp geometry_controller.hpp geometry_panel.cpp geometry_panel.hpp
    geometry_smoke.cpp geometry_smoke.hpp
    adaptive_smoke.cpp adaptive_smoke.hpp
    point_preview_renderer.cpp point_preview_renderer.hpp
    sdf_renderer.cpp sdf_renderer.hpp
    sdf_smoke.cpp sdf_smoke.hpp
    sdf_panel.cpp sdf_panel.hpp
    sdf_editor_smoke.cpp sdf_editor_smoke.hpp
    point_cloud_smoke.cpp point_cloud_smoke.hpp
    point_cloud_editor_smoke.cpp point_cloud_editor_smoke.hpp ui_response_probe.hpp
    point_cloud_tasks.cpp
    point_cloud_panel.cpp point_cloud_panel.hpp
    point_cloud_workbench.cpp scene_id.hpp
    gizmo_renderer.cpp gizmo_renderer.hpp
    gizmo_smoke.cpp gizmo_smoke.hpp
    ibl_smoke.cpp ibl_smoke.hpp
    offline_renderer.cpp offline_renderer.hpp offline_panel.cpp offline_panel.hpp
    offline_smoke.cpp offline_smoke.hpp
    screen_pass.cpp screen_pass.hpp ssao_pass.cpp ssao_pass.hpp
    realtime_panel.cpp realtime_panel.hpp
    ssao_smoke.cpp ssao_smoke.hpp
    gpu_image.cpp gpu_image.hpp gpu_shader.cpp gpu_shader.hpp
    hdr_targets.cpp hdr_targets.hpp post_process.cpp post_process.hpp
    scene_renderer.cpp scene_renderer.hpp shadow_map.cpp shadow_map.hpp workbench.cpp workbench.hpp
    workbench_internal.hpp workbench_widgets.cpp workbench_properties.cpp workbench_assets.cpp workbench_geometry.cpp
    scene_smoke.cpp scene_smoke.hpp texture_smoke.cpp texture_smoke.hpp interaction_smoke.cpp interaction_smoke.hpp
    parameter_smoke.cpp parameter_smoke.hpp wire_smoke.cpp wire_smoke.hpp)
set_target_properties(minirender_desktop PROPERTIES OUTPUT_NAME "${PROJECT_NAME}")
if(WIN32)
    enable_language(RC)
    configure_file(icons/renderbox.rc.in renderbox.rc @ONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/renderbox.rc" PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icons/renderbox.ico")
    target_sources(minirender_desktop PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/renderbox.rc")
elseif(APPLE)
    set_source_files_properties(icons/renderbox.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(minirender_desktop PRIVATE icons/renderbox.icns)
    enable_language(OBJCXX)
    target_sources(minirender_desktop PRIVATE regression_activity.mm regression_activity.hpp)
    set_source_files_properties(regression_activity.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_property(TARGET minirender_desktop PROPERTY OBJCXX_STANDARD 20)
    set_target_properties(minirender_desktop PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE "renderbox.icns"
        # 保留已发布应用的标识，安装器和 Launch Services 继续将更名视为同一应用的更新。
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.minirender.desktop"
        MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}")
endif()
qt_add_resources(minirender_desktop diagnostic_shaders
    PREFIX "/shaders" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${shader_outputs})
qt_add_resources(minirender_desktop editor_styles PREFIX "/editor" FILES editor.qss icons/renderbox.png icons/down.xpm icons/up.xpm icons/check.xpm)
target_link_libraries(minirender_desktop PRIVATE
    minirender_core minirender_warnings Qt6::Widgets Qt6::Concurrent Vulkan::Vulkan)

# 示例资源随可执行文件复制，运行时不依赖源码目录。
if(APPLE)
    # MacOS 目录只保存可执行代码，资源放在 Resources，满足 bundle 签名规则。
    set(scene_destination "$<TARGET_BUNDLE_CONTENT_DIR:minirender_desktop>/Resources/scenes")
    add_custom_command(TARGET minirender_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:minirender_desktop>/scenes")
else()
    set(scene_destination "$<TARGET_FILE_DIR:minirender_desktop>/scenes")
endif()
add_custom_command(TARGET minirender_desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/scenes" "${scene_destination}")


# 表数据随程序分发，保留原作者声明与原始论文引用。
add_custom_command(TARGET minirender_desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${scene_destination}/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/third_party/ltc/LICENSE"
            "${scene_destination}/licenses/LTC.txt")
