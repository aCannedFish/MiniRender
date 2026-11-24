// 每一级以 2×2 纹素的最小/最大窗口深度包住上一层，保留奇数尺寸的最后一行和一列。
#version 450
layout(binding = 1) uniform sampler2D previous;

layout(push_constant) uniform Reduction {
    int first;
}

reduction;
layout(location = 0) out vec2 interval;

void main() {
    ivec2 size = textureSize(previous, 0), base = ivec2(gl_FragCoord.xy) * 2;
    interval = vec2(1, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec2 value = texelFetch(previous, min(base + ivec2(x, y), size - 1), 0).rg;
            interval   = vec2(min(interval.x, value.x),
                              max(interval.y, reduction.first != 0 ? value.x : value.y));
        }
    }
}
