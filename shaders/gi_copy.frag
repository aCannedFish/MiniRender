// 保存上一帧的颜色与标识、法线与线性深度，或在多个计算结果中选择最终显示源。
#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
layout(binding = 1) uniform sampler2D source_a;
layout(binding = 2) uniform sampler2D source_b;
layout(binding = 3) uniform sampler2D source_c;

layout(push_constant) uniform Source {
    int mode;
}

selection;
layout(location = 0) out vec4 result;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (selection.mode == 3) {
        float z     = texelFetch(source_b, p, 0).r;
        float depth = frame.viewport.z * frame.viewport.w /
                      (frame.viewport.w - z * (frame.viewport.w - frame.viewport.z));
        result      = vec4(texelFetch(source_a, p, 0).xyz, depth);
    } else if (selection.mode == 1) {
        result = texelFetch(source_b, p, 0);
    } else if (selection.mode == 2) {
        result = texelFetch(source_c, p, 0);
    } else {
        result = texelFetch(source_a, p, 0);
    }
}
