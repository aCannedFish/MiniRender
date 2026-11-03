#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "scene_instance.glsl"
layout(location = 0) in vec3 in_position;
void main() {
    gl_Position = frame.shadow_view_projection[draw_index.light] *
                  instances.values[draw_index.instance].object_to_world * vec4(in_position, 1.0);
}
