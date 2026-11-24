// 将实例网格变换到指定方向光的阴影裁剪空间。

#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "scene_instance.glsl"
layout(location = 0) in vec3 in_position;

layout(std430,binding=11) readonly buffer RectangleProjections {mat4 matrices[];} area_shadow;
void main() {
    mat4 projection=draw_index.light<0 ? area_shadow.matrices[-1-draw_index.light]:frame.shadow_view_projection[draw_index.light];
    gl_Position = projection *
                  instances.values[draw_index.instance].object_to_world * vec4(in_position, 1.0);
}
