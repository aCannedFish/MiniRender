#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;
layout(location = 3) in vec3 in_emission;
layout(location = 4) in vec2 in_material;
layout(location = 5) in vec2 in_uv;


layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec3 base_color;
layout(location = 3) out vec3 emission;
layout(location = 4) out vec2 material;
layout(location = 5) out vec2 mesh_uv;

void main() {
    world_position = in_position;
    world_normal = in_normal;
    base_color = in_color;
    emission = in_emission;
    material = in_material;
    mesh_uv = in_uv;
    gl_Position = frame.view_projection * vec4(in_position, 1.0);
}
