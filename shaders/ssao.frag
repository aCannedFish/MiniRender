#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "screen_position.glsl"
layout(binding = 1) uniform sampler2D normal_image;
layout(binding = 2) uniform sampler2D depth_image;
layout(location = 0) out float ao;

// 整数 hash 为每像素生成固定旋转，相机静止时不会随帧数闪烁。
uint hash_pixel(uvec2 pixel) {
    uint value = pixel.x * 0x9e3779b9u ^ pixel.y * 0x85ebca6bu;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    return value;
}
void main() {
    ivec2 size = textureSize(depth_image, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(depth_image, pixel, 0).r;
    vec4 normal = texelFetch(normal_image, pixel, 0);
    ao = 1.0;
    if (frame.ao_settings.x < 0.5 || depth >= 1.0 || normal.a < 0.5) { return; }
    vec3 position = view_position(pixel, depth, size);
    vec3 n = normalize(mat3(frame.view) * normal.xyz);
    vec3 reference = abs(n.z) < 0.999 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    vec3 tangent = normalize(cross(reference, n));
    vec3 bitangent = cross(n, tangent);
    float angle = float(hash_pixel(uvec2(pixel)) & 65535u) * (6.28318530718 / 65536.0);
    vec3 rotated = tangent * cos(angle) + bitangent * sin(angle);
    mat3 basis = mat3(rotated, cross(n, rotated), n);
    float radius = frame.ao_settings.y;
    float occluded = 0.0;
    int count = int(frame.ao_filter.x);
    for (int i = 0; i < count; ++i) {
        vec3 sample_position = position + basis * frame.ao_kernel[i].xyz * radius;
        vec4 clip = frame.projection * vec4(sample_position, 1.0);
        if (clip.w <= 0.0) { continue; }
        vec3 ndc = clip.xyz / clip.w;
        vec2 uv = ndc.xy * 0.5 + 0.5;
        // 屏幕外与背景没有可用遮挡信息，保留为开放方向。
        if (any(lessThan(uv, vec2(0))) || any(greaterThanEqual(uv, vec2(1))) || ndc.z < 0.0 || ndc.z > 1.0) { continue; }
        ivec2 neighbor = ivec2(uv * vec2(size));
        float sample_depth = texelFetch(depth_image, neighbor, 0).r;
        if (sample_depth >= 1.0 || texelFetch(normal_image, neighbor, 0).a < 0.5) { continue; }
        vec3 surface = view_position(neighbor, sample_depth, size);
        float distance = length(surface - position);
        float weight = 1.0 - smoothstep(radius * 0.5, radius, distance);
        // 相机向 -Z 看；较大的 view Z 更靠近相机。
        occluded += (surface.z >= sample_position.z + frame.ao_settings.z ? 1.0 : 0.0) * weight;
    }
    ao = clamp(1.0 - frame.ao_settings.w * occluded / float(count), 0.0, 1.0);
}
