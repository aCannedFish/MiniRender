#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "scene_instance.glsl"
layout(location = 0) in vec3 in_position;
void main() {
    gl_Position = frame.view_projection * instances.values[draw_index.instance].object_to_world * vec4(in_position, 1.0);
    // 小的 clip-space 偏移让同一表面的边线通过深度测试，仍保留遮挡关系。
    gl_Position.z -= 2e-6 * gl_Position.w;
}
