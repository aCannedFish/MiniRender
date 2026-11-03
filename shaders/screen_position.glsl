#ifndef MINIRENDER_SCREEN_POSITION
#define MINIRENDER_SCREEN_POSITION
// Vulkan NDC 的深度为 [0,1]。projection 已含 Y 翻转，重建时直接反解该矩阵。
vec3 view_position(ivec2 pixel, float depth, ivec2 size) {
    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    vec4 position = frame.inverse_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return position.xyz / position.w;
}
#endif
