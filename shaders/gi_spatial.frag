// 在屏幕间接辐亮度上进行 à-trous 滤波，物体 ID、法线与切平面距离保护轮廓。
#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "screen_position.glsl"
layout(binding = 1) uniform sampler2D noisy;
layout(binding = 2) uniform sampler2D normals;
layout(binding = 3) uniform sampler2D depths;

layout(push_constant) uniform Stride {
    int step_size;
}

filter_pass;
layout(location = 0) out vec4 color;
const float kernel[5] = float[5](1, 4, 6, 4, 1);

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(noisy, 0);
    vec4  center = texelFetch(normals, p, 0);
    vec3  source = texelFetch(noisy, p, 0).rgb;
    if (center.a < .5) {
        color = vec4(source, 0);
        return;
    }
    vec3  point = view_position(p, texelFetch(depths, p, 0).r, size);
    vec3  n = mat3(frame.view) * center.xyz, sum = vec3(0);
    float total = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            ivec2 q     = clamp(p + ivec2(i, j) * filter_pass.step_size, ivec2(0), size - 1);
            vec4  guide = texelFetch(normals, q, 0);
            if (guide.a != center.a) {
                continue;
            }
            vec3  sample_color = texelFetch(noisy, q, 0).rgb;
            vec3  sample_pos   = view_position(q, texelFetch(depths, q, 0).r, size);
            float distance     = abs(dot(n, sample_pos - point));
            float w            = kernel[i + 2] * kernel[j + 2] *
                                 pow(clamp(dot(center.xyz, guide.xyz), 0.0, 1.0), 32.0) *
                                 exp(-distance / max(.001, .03 * float(filter_pass.step_size)) -
                                     length(sample_color - source) / (1.0 + length(source)));
            sum += w * sample_color;
            total += w;
        }
    }
    color = vec4(total > 0.0 ? sum / total : source, center.a);
}
