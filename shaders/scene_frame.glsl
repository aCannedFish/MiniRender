#ifndef MINIRENDER_SCENE_FRAME
#define MINIRENDER_SCENE_FRAME
struct DirectionalLight { vec4 direction; vec4 radiance; };
layout(binding = 0, std140) uniform Frame {
    mat4 view_projection;
    vec4 camera_position;
    vec4 environment;
    DirectionalLight lights[8];
    ivec4 counts;
    vec4 display;
    mat4 shadow_view_projection[8];
    vec4 shadow_settings;
} frame;
#endif
