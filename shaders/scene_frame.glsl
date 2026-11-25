// CPU 与 GPU 共用的帧 uniform 布局及原色模式判定。

#ifndef MINIRENDER_SCENE_FRAME
#define MINIRENDER_SCENE_FRAME
#include "view_modes.h"

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
    vec4             teaching_light;   // 点光源世界坐标与强度
    vec4             teaching_surface; // 高光指数、镜面系数、纹理开关
    vec4             area_settings;    // 可见矩形灯数量和采样参数
    vec4             prt_light[9];     // RGB 为世界空间环境 SH，第一项的 w 表示传输已就绪
    vec4             screen_trace;     // 最大距离、命中厚度、步数、方向样本数
    vec4             screen_options;   // Hi-Z 开关、时间采样序号
    mat4             previous_vp;      // 上一次累积的相机投影
    vec4             history_settings; // 可重用、物体移动、权重、限幅 σ
}

frame;

// 原色预览保留独立模式号；既有法线、深度和 AO 模式不改变编号。
bool base_color_preview() {
    return int(frame.display.y) == VIEW_BASE_COLOR;
}
#endif
