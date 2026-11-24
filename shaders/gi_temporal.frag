// 使用前帧物体位置与相机投影重投影，按 ID、深度和法线拒绝遮挡变化，在 7×7 邻域限幅。
#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
layout(binding = 1) uniform sampler2D spatial;
layout(binding = 2) uniform sampler2D normals;
layout(binding = 3) uniform sampler2D motion;
layout(binding = 4) uniform sampler2D history;
layout(binding = 5) uniform sampler2D previous_guides;
layout(location = 0) out vec4 color;

vec3 decode_normal(float packed) {
    vec2 f = vec2(mod(packed, 1024.0), floor(packed / 1024.0)) / 1023.0 * 2.0 - 1.0;
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * mix(vec2(-1), vec2(1), greaterThanEqual(n.xy, vec2(0)));
    }
    return normalize(n);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(spatial, 0);
    vec4  source = texelFetch(spatial, p, 0);
    color        = vec4(source.rgb, -source.a);
    if (source.a < .5 || frame.history_settings.x < .5) {
        return;
    }
    vec4 old_world = texelFetch(motion, p, 0);
    vec4 clip      = frame.previous_vp * vec4(old_world.xyz, 1);
    if (clip.w <= 0) {
        return;
    }
    vec2 uv = clip.xy / clip.w * .5 + .5;
    if (any(lessThan(uv, vec2(0))) || any(greaterThanEqual(uv, vec2(1)))) {
        return;
    }
    ivec2 q   = ivec2(uv * vec2(size));
    vec4  old = texelFetch(history, q, 0), guide = texelFetch(previous_guides, q, 0);
    if (abs(old.a) != source.a || abs(guide.a - clip.w) > max(.01, .02 * clip.w) ||
        dot(guide.xyz, decode_normal(old_world.w)) < .8) {
        return;
    }
    vec3  mean = vec3(0), square = vec3(0);
    float count = 0;
    for (int j = -3; j <= 3; ++j) {
        for (int i = -3; i <= 3; ++i) {
            ivec2 t = clamp(p + ivec2(i, j), ivec2(0), size - 1);
            vec4  v = texelFetch(spatial, t, 0);
            if (v.a != source.a) {
                continue;
            }
            mean += v.rgb;
            square += v.rgb * v.rgb;
            count += 1;
        }
    }
    mean /= max(count, 1);
    vec3 sigma = sqrt(max(square / max(count, 1) - mean * mean, vec3(0)));
    vec3 prior = clamp(old.rgb, mean - frame.history_settings.w * sigma,
                       mean + frame.history_settings.w * sigma);
    color.a    = source.a;
    color.rgb  = mix(source.rgb, prior, frame.history_settings.z);
}
