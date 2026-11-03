#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
layout(binding = 1) uniform sampler2D hdr_image;
layout(binding = 2) uniform sampler2D normal_image;
layout(binding = 3) uniform sampler2D depth_image;
layout(binding = 4) uniform sampler2D wire_image;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

vec3 encode_srgb(vec3 linear) {
    return mix(1.055 * pow(max(linear, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
               12.92 * linear, lessThanEqual(linear, vec3(0.0031308)));
}
vec3 decode_srgb(vec3 encoded) {
    return mix(pow((encoded + 0.055) / 1.055, vec3(2.4)),
               encoded / 12.92, lessThanEqual(encoded, vec3(0.04045)));
}
vec3 display_texel(ivec2 pixel) {
    ivec2 size = textureSize(hdr_image, 0);
    vec3 linear = max(texelFetch(hdr_image, clamp(pixel, ivec2(0), size - 1), 0).rgb, vec3(0.0));
    linear *= exp2(frame.display.x);
    return encode_srgb(linear / (vec3(1.0) + linear));
}

// 在色调映射后的感知颜色空间插值，不要求 float32 纹理硬件线性过滤。
vec3 display_sample(vec2 coordinate) {
    vec2 pixel = coordinate * vec2(textureSize(hdr_image, 0)) - 0.5;
    ivec2 base = ivec2(floor(pixel));
    vec2 t = fract(pixel);
    return mix(mix(display_texel(base), display_texel(base + ivec2(1, 0)), t.x),
               mix(display_texel(base + ivec2(0, 1)), display_texel(base + ivec2(1, 1)), t.x), t.y);
}
float luma(vec3 rgb) { return dot(rgb, vec3(0.299, 0.587, 0.114)); }

// Lottes FXAA 的局部对比度、轴向边缘分类、双向端点搜索与亚像素混合。
// 在感知颜色空间执行，固定最多 16 步；实现说明与原始白皮书见 docs/HDR.md。
vec3 fxaa(vec2 coordinate) {
    vec2 pixel = frame.viewport.xy;
    vec3 middle = display_sample(coordinate);
    vec3 north = display_sample(coordinate - vec2(0.0, pixel.y));
    vec3 south = display_sample(coordinate + vec2(0.0, pixel.y));
    vec3 west = display_sample(coordinate - vec2(pixel.x, 0.0));
    vec3 east = display_sample(coordinate + vec2(pixel.x, 0.0));
    float m = luma(middle), n = luma(north), s = luma(south), w = luma(west), e = luma(east);
    float low = min(m, min(min(n, s), min(w, e)));
    float high = max(m, max(max(n, s), max(w, e)));
    float contrast = high - low;
    if (contrast < max(0.03125, high * 0.125)) { return middle; }

    vec3 northwest = display_sample(coordinate + vec2(-1, -1) * pixel);
    vec3 northeast = display_sample(coordinate + vec2(1, -1) * pixel);
    vec3 southwest = display_sample(coordinate + vec2(-1, 1) * pixel);
    vec3 southeast = display_sample(coordinate + vec2(1, 1) * pixel);
    float nw = luma(northwest), ne = luma(northeast), sw = luma(southwest), se = luma(southeast);
    float horizontal_score = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) + abs(ne + se - 2.0 * e);
    float vertical_score = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) + abs(sw + se - 2.0 * s);
    bool horizontal = horizontal_score >= vertical_score;
    float before = horizontal ? n : w;
    float after = horizontal ? s : e;
    float before_gradient = abs(before - m);
    float after_gradient = abs(after - m);
    bool use_before = before_gradient >= after_gradient;
    vec2 across = horizontal ? vec2(0.0, pixel.y) : vec2(pixel.x, 0.0);
    across *= use_before ? -1.0 : 1.0;
    vec2 along = horizontal ? vec2(pixel.x, 0.0) : vec2(0.0, pixel.y);
    float edge_luma = 0.5 * (m + (use_before ? before : after));
    float threshold = 0.25 * max(before_gradient, after_gradient);
    vec2 edge_center = coordinate + 0.5 * across;
    float negative_distance = 1.0, positive_distance = 1.0;
    float negative_delta = 0.0, positive_delta = 0.0;
    bool negative_done = false, positive_done = false;
    for (int step = 0; step < 16; ++step) {
        if (!negative_done) {
            negative_delta = luma(display_sample(edge_center - negative_distance * along)) - edge_luma;
            negative_done = abs(negative_delta) >= threshold;
        }
        if (!positive_done) {
            positive_delta = luma(display_sample(edge_center + positive_distance * along)) - edge_luma;
            positive_done = abs(positive_delta) >= threshold;
        }
        if (negative_done && positive_done) { break; }
        if (step < 15) {
            if (!negative_done) { negative_distance += 1.0; }
            if (!positive_done) { positive_distance += 1.0; }
        }
    }
    bool negative_nearer = negative_distance <= positive_distance;
    float endpoint_delta = negative_nearer ? negative_delta : positive_delta;
    float offset = 0.5 - min(negative_distance, positive_distance) / (negative_distance + positive_distance);
    if ((m < edge_luma) == (endpoint_delta < 0.0)) { offset = 0.0; }
    vec3 edge_filtered = display_sample(coordinate + across * offset);
    float local_average = 0.25 * (n + s + w + e);
    float subpixel = clamp((abs(local_average - m) / contrast - 0.25) / 0.75, 0.0, 0.75);
    vec3 box = (middle + north + south + west + east + northwest + northeast + southwest + southeast) / 9.0;
    return mix(edge_filtered, box, subpixel);
}

void main() {
    ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), textureSize(hdr_image, 0) - 1);
    if (frame.display.y > 2.5) {
        color = texelFetch(wire_image, pixel, 0);
        return;
    }
    if (frame.display.y > 1.5) {
        float depth = texelFetch(depth_image, pixel, 0).r;
        float near_plane = frame.viewport.z;
        float far_plane = frame.viewport.w;
        float linear_depth = near_plane * far_plane / (far_plane - depth * (far_plane - near_plane));
        color = vec4(vec3(depth < 1.0 ? 1.0 / (1.0 + linear_depth) : 0.0), 1.0);
        return;
    }
    if (frame.display.y > 0.5) {
        vec4 normal = texelFetch(normal_image, pixel, 0);
        color = vec4(normal.a > 0.5 ? normalize(normal.xyz) * 0.5 + 0.5 : vec3(0.0), 1.0);
        return;
    }
    vec3 encoded = frame.display.z > 0.5 ? fxaa(uv) : display_texel(pixel);
    // SRGB swapchain 会重新编码；先解码避免双重 gamma。
    color = vec4(decode_srgb(encoded), 1.0);
}
