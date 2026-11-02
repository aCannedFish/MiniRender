#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"


layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec3 base_color;
layout(location = 3) in vec3 emission;
layout(location = 4) in vec2 material;
layout(location = 0) out vec4 color;
layout(location = 1) out vec4 normal_output;
layout(binding = 1) uniform sampler2DArray shadow_map;
const float PI = 3.141592653589793;

// 分布与 Smith G1 参考 Walter et al. 2007，推导与实现差异见 docs/VIEWPORT.md。
// GGX 的 alpha = roughness^2。这里实现直接光 BRDF，不包含 IBL 卷积。
float distribution_ggx(float ndh, float alpha) {
    float a2 = alpha * alpha;
    float denominator = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 1e-7);
}
float smith_g1(float nd, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * nd / max(nd + sqrt(a2 + (1.0 - a2) * nd * nd), 1e-6);
}

// PCF 参考 Reeves et al. 1987，推导和实现边界见 docs/SHADOWS.md。
// 对 3×3 深度比较结果平均；偏移量在光空间深度范围内，不改变物体几何。
float visibility(int light, float ndl) {
    if (frame.shadow_settings.x < 0.5) { return 1.0; }
    vec4 clip = frame.shadow_view_projection[light] * vec4(world_position, 1.0);
    vec3 position = clip.xyz / clip.w;
    vec2 uv = position.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || position.z < 0.0 || position.z > 1.0) { return 1.0; }
    float bias = max(0.0002 * (1.0 - ndl), 0.00005);
    float result = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * frame.shadow_settings.y;
            float stored_depth = texture(shadow_map, vec3(uv + offset, float(light))).r;
            result += position.z - bias <= stored_depth ? 1.0 : 0.0;
        }
    }
    return result / 9.0;
}

void main() {
    vec3 n = normalize(world_normal);
    vec3 v = normalize(frame.camera_position.xyz - world_position);
    if (dot(n, v) < 0.0) { n = -n; }
    normal_output = vec4(n, 1.0);
    float roughness = max(material.x, 0.045);
    float metallic = material.y;
    float alpha = roughness * roughness;
    vec3 f0 = mix(vec3(0.04), base_color, metallic);
    vec3 result = emission + frame.environment.xyz * mix(base_color, f0, metallic);
    float ndv = max(dot(n, v), 0.0);
    for (int i = 0; i < frame.counts.x; ++i) {
        vec3 l = -normalize(frame.lights[i].direction.xyz);
        float ndl = max(dot(n, l), 0.0);
        if (ndl <= 0.0 || ndv <= 0.0) { continue; }
        vec3 h = normalize(l + v);
        float ndh = max(dot(n, h), 0.0);
        float vdh = max(dot(v, h), 0.0);
        vec3 f = f0 + (vec3(1.0) - f0) * pow(1.0 - vdh, 5.0);
        float d = distribution_ggx(ndh, alpha);
        float g = smith_g1(ndl, alpha) * smith_g1(ndv, alpha);
        vec3 specular = f * d * g / max(4.0 * ndl * ndv, 1e-6);
        vec3 diffuse = (vec3(1.0) - f) * (1.0 - metallic) * base_color / PI;
        result += (diffuse + specular) * frame.lights[i].radiance.rgb * ndl * visibility(i, ndl);
    }
    color = vec4(result, 1.0);
}
