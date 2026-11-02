#version 450

struct DirectionalLight { vec4 direction; vec4 radiance; };
layout(binding = 0, std140) uniform Frame {
    mat4 view_projection;
    vec4 camera_position;
    vec4 environment;
    DirectionalLight lights[8];
    ivec4 counts;
    vec4 display;
} frame;
layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec3 base_color;
layout(location = 3) in vec3 emission;
layout(location = 4) in vec2 material;
layout(location = 0) out vec4 color;
const float PI = 3.141592653589793;

// 分布与 Smith G1 参考 Walter et al. 2007，推导与实现差异见 docs/VIEWPORT.md。
// GGX 的 alpha = roughness^2。这里实现直接光 BRDF，尚不包含阴影或 IBL 卷积。
float distribution_ggx(float ndh, float alpha) {
    float a2 = alpha * alpha;
    float denominator = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 1e-7);
}
float smith_g1(float nd, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * nd / max(nd + sqrt(a2 + (1.0 - a2) * nd * nd), 1e-6);
}

void main() {
    vec3 n = normalize(world_normal);
    vec3 v = normalize(frame.camera_position.xyz - world_position);
    if (dot(n, v) < 0.0) { n = -n; }
    if (frame.display.y > 0.5) {
        color = vec4(n * 0.5 + 0.5, 1.0);
        return;
    }
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
        result += (diffuse + specular) * frame.lights[i].radiance.rgb * ndl;
    }
    // 临时显示变换；A1.3 会移到独立的 HDR 后处理 pass。
    result *= exp2(frame.display.x);
    color = vec4(result / (vec3(1.0) + result), 1.0);
}
