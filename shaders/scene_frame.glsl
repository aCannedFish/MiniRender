// CPU 与 GPU 共用的帧 uniform 布局及原色模式判定。

#ifndef MINIRENDER_SCENE_FRAME
#define MINIRENDER_SCENE_FRAME

struct DirectionalLight {
    vec4 direction;
    vec4 radiance;
};

layout(binding = 0, std140) uniform Frame {
    mat4             view_projection;
    vec4             camera_position;
    vec4             environment;
    DirectionalLight lights[8];
    ivec4            counts;
    vec4             display;
    mat4             shadow_view_projection[8];
    vec4             shadow_settings;
    vec4             viewport;
    mat4             view;
    mat4             projection;
    mat4             inverse_projection;
    vec4             ao_settings;
    vec4             ao_filter;
    vec4             ao_kernel[64];
    vec4             ibl;
    vec4             preview_edges;
}

frame;

// 原色预览保留独立模式号；既有法线、深度和 AO 模式不改变编号。
bool base_color_preview() {
    return frame.display.y > 5.5;
}
#endif
