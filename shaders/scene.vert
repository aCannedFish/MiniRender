// 将实例顶点、法线、UV 与切向变换到实时着色所需空间。

#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "scene_instance.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 5) in vec2 in_uv;
layout(location = 6) in vec3 in_tangent_u;
layout(location = 7) in vec3 in_tangent_v;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec3 base_color;
layout(location = 3) out vec3 emission;
layout(location = 4) out vec2 material;
layout(location = 5) out vec2 mesh_uv;
layout(location = 6) out vec3 world_tangent_u;
layout(location = 7) out vec3 world_tangent_v;

layout(std430,binding=13) readonly buffer PrtTransfer {vec4 coefficients[];} transfer;
layout(location=8) out vec3 prt_color;
void main() {
    Instance instance = instances.values[draw_index.instance];
    prt_color=vec3(0);
    if(int(frame.display.y)==12 && frame.prt_light[0].w>0.5){
        uint base=(instance.geometry.x+uint(gl_VertexIndex))*9;
        for(uint k=0;k<9;++k) prt_color+=frame.prt_light[k].rgb*transfer.coefficients[base+k].rgb;
    }
    world_position    = (instance.object_to_world * vec4(in_position, 1.0)).xyz;
    world_normal      = normalize(mat3(instance.normal_to_world) * in_normal);
    base_color        = instance.base_color.rgb;
    emission          = instance.emission.rgb;
    material          = instance.surface.xy;
    mesh_uv           = in_uv;
    world_tangent_u   = mat3(instance.object_to_world) * in_tangent_u;
    world_tangent_v   = mat3(instance.object_to_world) * in_tangent_v;
    gl_Position       = frame.view_projection * vec4(world_position, 1.0);
}
