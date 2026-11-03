#ifndef MINIRENDER_SCENE_INSTANCE
#define MINIRENDER_SCENE_INSTANCE

// 与 CPU GpuInstance 的 std430 布局一致。网格在局部空间保存，每个实例独立索引。
struct Instance {
    mat4 object_to_world;
    mat4 normal_to_world;
    vec4 base_color;
    vec4 emission;
    vec4 surface;
};
layout(binding = 3, std430) readonly buffer Instances { Instance values[]; } instances;
layout(push_constant) uniform DrawIndex { int light; int instance; } draw_index;

#endif
