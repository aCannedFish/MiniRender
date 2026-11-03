#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "scene_instance.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 5) in vec2 in_uv;


layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec3 base_color;
layout(location = 3) out vec3 emission;
layout(location = 4) out vec2 material;
layout(location = 5) out vec2 mesh_uv;

void main() {
    Instance instance = instances.values[draw_index.instance];
    world_position = (instance.object_to_world * vec4(in_position, 1.0)).xyz;
    world_normal = normalize(mat3(instance.normal_to_world) * in_normal);
    base_color = instance.base_color.rgb;
    emission = instance.emission.rgb;
    material = instance.surface.xy;
    mesh_uv = in_uv;
    gl_Position = frame.view_projection * vec4(world_position, 1.0);
}
