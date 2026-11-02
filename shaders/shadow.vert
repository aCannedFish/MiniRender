#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
layout(location = 0) in vec3 in_position;
layout(push_constant) uniform ShadowLayer { int index; } layer;
void main() {
    gl_Position = frame.shadow_view_projection[layer.index] * vec4(in_position, 1.0);
}
