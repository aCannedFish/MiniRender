#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "screen_position.glsl"
layout(binding = 1) uniform sampler2D source_ao;
layout(binding = 2) uniform sampler2D normal_image;
layout(binding = 3) uniform sampler2D depth_image;
layout(push_constant) uniform Direction { int vertical; } filter_direction;
layout(location = 0) out float ao;

// 两个一维双边 pass。深度与法线权重保留轮廓和折角，不跨越背景混合遮蔽。
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(depth_image, 0);
    float depth = texelFetch(depth_image, pixel, 0).r;
    vec4 normal = texelFetch(normal_image, pixel, 0);
    ao = 1.0;
    if (frame.ao_settings.x < 0.5 || depth >= 1.0 || normal.a < 0.5) { return; }
    float z = view_position(pixel, depth, size).z;
    vec3 n = normalize(normal.xyz);
    float sum = 0.0, weights = 0.0;
    for (int offset = -4; offset <= 4; ++offset) {
        ivec2 neighbor = pixel + (filter_direction.vertical == 0 ? ivec2(offset, 0) : ivec2(0, offset));
        if (any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, size))) { continue; }
        float d = texelFetch(depth_image, neighbor, 0).r;
        vec4 other_normal = texelFetch(normal_image, neighbor, 0);
        if (d >= 1.0 || other_normal.a < 0.5) { continue; }
        vec3 other_n = normalize(other_normal.xyz);
        float dz = (view_position(neighbor, d, size).z - z) / frame.ao_filter.y;
        float weight = exp(-0.5 * (float(offset * offset) / 4.0 + dz * dz));
        weight *= pow(max(dot(n, other_n), 0.0), frame.ao_filter.z);
        sum += texelFetch(source_ao, neighbor, 0).r * weight;
        weights += weight;
    }
    ao = sum / max(weights, 1e-8);
}
