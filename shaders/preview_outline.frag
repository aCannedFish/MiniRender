#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "screen_position.glsl"
layout(binding = 1) uniform sampler2D source_hdr;
layout(binding = 2) uniform sampler2D normal_image;
layout(binding = 3) uniform sampler2D depth_image;
layout(location = 0) out vec4 color;

// 在前景一侧描边。平面距离过滤正常透视深度变化，法线项只保留明显折角。
float edge_at(ivec2 neighbor, ivec2 size, vec3 position, vec3 normal) {
    if (any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, size))) {
        return 0.0;
    }
    vec4  other_normal = texelFetch(normal_image, neighbor, 0);
    float depth        = texelFetch(depth_image, neighbor, 0).r;
    if (other_normal.a < 0.5 || depth >= 1.0) {
        return 1.0;
    }
    vec3  other_position = view_position(neighbor, depth, size);
    vec3  other_n        = normalize(mat3(frame.view) * other_normal.xyz);
    float scale          = max(-position.z, 0.001);
    float behind         = (position.z - other_position.z) / scale;
    float plane_distance = abs(dot(other_position - position, normal)) / scale;
    float silhouette = smoothstep(0.003, 0.012, behind) * smoothstep(0.001, 0.004, plane_distance);
    float crease     = smoothstep(0.15, 0.45, 1.0 - clamp(dot(normal, other_n), -1.0, 1.0));
    // 跨越前后表面时由轮廓项决定，避免背景侧也出现一圈描边。
    crease *= 1.0 - smoothstep(0.01, 0.03, abs(behind));
    return max(silhouette, crease * 0.55);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    color       = texelFetch(source_hdr, pixel, 0);
    if (!base_color_preview() || frame.preview_edges.x <= 0.0) {
        return;
    }
    float depth  = texelFetch(depth_image, pixel, 0).r;
    vec4  normal = texelFetch(normal_image, pixel, 0);
    if (depth >= 1.0 || normal.a < 0.5) {
        return;
    }
    ivec2      size     = textureSize(depth_image, 0);
    vec3       position = view_position(pixel, depth, size);
    vec3       n        = normalize(mat3(frame.view) * normal.xyz);
    const vec2 directions[8] =
        vec2[8](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1), vec2(.7071, .7071),
                vec2(-.7071, .7071), vec2(.7071, -.7071), vec2(-.7071, -.7071));
    float inner  = 0.0;
    float outer  = 0.0;
    float radius = max(frame.preview_edges.y, 1.0);
    for (int i = 0; i < 8; ++i) {
        ivec2 inner_pixel = pixel + ivec2(round(directions[i] * max(radius * 0.5, 1.0)));
        ivec2 outer_pixel = pixel + ivec2(round(directions[i] * radius));
        inner             = max(inner, edge_at(inner_pixel, size, position, n));
        outer             = max(outer, edge_at(outer_pixel, size, position, n));
    }
    // 两圈采样让外缘减弱；随后由原显示 pass 的 FXAA 平滑轮廓。
    float amount = frame.preview_edges.x * (0.5 * inner + 0.5 * outer);
    // 深色表面使用较亮的灰线，浅色表面使用暗线，保持轮廓对比。
    float luminance = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3  ink       = luminance < 0.08 ? vec3(0.25) : color.rgb * 0.05;
    color.rgb       = mix(color.rgb, ink, amount);
}
