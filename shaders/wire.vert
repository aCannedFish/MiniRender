#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "scene_instance.glsl"
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_geometric_normal;
layout(location = 0) flat out int visible_side;
void main() {
    Instance instance = instances.values[draw_index.instance];
    vec4 world = instance.object_to_world * vec4(in_position, 1.0);
    vec3 outward = mat3(instance.normal_to_world) * in_geometric_normal;
    // 三个面角共用几何法线。逆转置保留镜像后的外侧，边线与表面使用相同朝向。
    visible_side = instance.surface.w > 0.5 ||
                   dot(outward, frame.camera_position.xyz - world.xyz) > 0.0 ? 1 : 0;
    gl_Position = frame.view_projection * world;
    // 小的 clip-space 偏移让同一表面的边线通过深度测试，仍保留遮挡关系。
    gl_Position.z -= 2e-6 * gl_Position.w;
}
