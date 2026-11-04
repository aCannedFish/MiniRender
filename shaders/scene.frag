#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "scene_instance.glsl"
#include "ibl_sampling.glsl"


layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec3 base_color;
layout(location = 3) in vec3 emission;
layout(location = 4) in vec2 material;
layout(location = 5) in vec2 mesh_uv;
layout(binding = 2) uniform sampler2D base_texture;
layout(location = 0) out vec4 color;
layout(location = 1) out vec4 normal_output;
layout(location = 2) out vec4 ambient_output;
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
float visibility(int light, float ndl, vec3 world_dx, vec3 world_dy) {
    if (frame.shadow_settings.x < 0.5) { return 1.0; }
    vec4 clip = frame.shadow_view_projection[light] * vec4(world_position, 1.0);
    vec3 position = clip.xyz / clip.w;
    vec2 uv = position.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || position.z < 0.0 || position.z > 1.0) { return 1.0; }
    float bias = max(0.0002 * (1.0 - ndl), 0.00005);
    // 将屏幕空间导数换到阴影 UV，按接收面平面修正每个 PCF 样本的比较深度。
    // 导数在 main 的统一控制流中计算，避免光照分支影响相邻像素的导数。
    vec4 clip_dx = frame.shadow_view_projection[light] * vec4(world_dx, 0.0);
    vec4 clip_dy = frame.shadow_view_projection[light] * vec4(world_dy, 0.0);
    vec3 position_dx = (clip_dx.xyz - position * clip_dx.w) / clip.w;
    vec3 position_dy = (clip_dy.xyz - position * clip_dy.w) / clip.w;
    vec2 uv_dx = position_dx.xy * 0.5;
    vec2 uv_dy = position_dy.xy * 0.5;
    float determinant = uv_dx.x * uv_dy.y - uv_dx.y * uv_dy.x;
    vec2 depth_gradient = vec2(0.0);
    if (abs(determinant) > 1e-12) {
        depth_gradient = vec2(position_dx.z * uv_dy.y - position_dy.z * uv_dx.y,
                              uv_dx.x * position_dy.z - uv_dy.x * position_dx.z) / determinant;
    }
    float result = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * frame.shadow_settings.y;
            float stored_depth = texture(shadow_map, vec3(uv + offset, float(light))).r;
            float receiver_depth = position.z + dot(depth_gradient, offset);
            result += receiver_depth - bias <= stored_depth ? 1.0 : 0.0;
        }
    }
    return result / 9.0;
}

ivec2 repeat_pixel(ivec2 pixel, ivec2 extent) { return (pixel % extent + extent) % extent; }
vec3 bilinear_texture(vec2 uv, int level) {
    ivec2 extent = textureSize(base_texture, level);
    vec2 position = uv * vec2(extent) - 0.5;
    ivec2 lower = ivec2(floor(position));
    vec2 t = fract(position);
    vec3 a = texelFetch(base_texture, repeat_pixel(lower, extent), level).rgb;
    vec3 b = texelFetch(base_texture, repeat_pixel(lower + ivec2(1,0), extent), level).rgb;
    vec3 c = texelFetch(base_texture, repeat_pixel(lower + ivec2(0,1), extent), level).rgb;
    vec3 d = texelFetch(base_texture, repeat_pixel(lower + ivec2(1,1), extent), level).rgb;
    return mix(mix(a,b,t.x), mix(c,d,t.x), t.y);
}
vec3 sample_base_texture() {
    vec2 uv = vec2(fract(mesh_uv.x), 1.0 - fract(mesh_uv.y));
    ivec2 extent = textureSize(base_texture, 0);
    int filter_mode = int(instances.values[draw_index.instance].surface.z);
    if (filter_mode == 0) {
        return texelFetch(base_texture, repeat_pixel(ivec2(floor(uv * vec2(extent))), extent), 0).rgb;
    }
    if (filter_mode == 1) { return bilinear_texture(uv, 0); }
    vec2 dx = dFdx(mesh_uv) * vec2(extent);
    vec2 dy = dFdy(mesh_uv) * vec2(extent);
    float lod = clamp(log2(max(max(length(dx), length(dy)), 1e-8)), 0.0, float(textureQueryLevels(base_texture)-1));
    int lower = int(floor(lod));
    int upper = min(lower + 1, textureQueryLevels(base_texture)-1);
    return mix(bilinear_texture(uv, lower), bilinear_texture(uv, upper), fract(lod));
}

void main() {
    vec3 world_dx = dFdx(world_position);
    vec3 world_dy = dFdy(world_position);
    vec3 albedo = base_color * clamp(sample_base_texture(), vec3(0.0), vec3(1.0));
    vec3 n = normalize(world_normal);
    vec3 v = normalize(frame.camera_position.xyz - world_position);
    if (dot(n, v) < 0.0) { n = -n; }
    normal_output = vec4(n, 1.0);
    float roughness = max(material.x, 0.045);
    float metallic = material.y;
    float alpha = roughness * roughness;
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float ndv = max(dot(n, v), 0.0);
    vec3 diffuse_environment=frame.environment.rgb;
    vec3 specular_environment=frame.environment.rgb;
    if(frame.ibl.y>0.5) {
        diffuse_environment+=frame.ibl.z*ibl_diffuse(n);
        specular_environment+=frame.ibl.z*ibl_specular(reflect(-v,n),clamp(material.x,0.0,1.0));
    }
    vec3 ambient_diffuse=diffuse_environment*albedo*(1.0-metallic);
    vec3 specular_factor=albedo;
    if(frame.ibl.x>0.5) {
        vec2 ab=ibl_brdf(ndv,clamp(material.x,0.0,1.0));
        specular_factor=albedo*ab.x+ab.y;
    }
    vec3 result=emission+ambient_diffuse+metallic*specular_environment*specular_factor;
    ambient_output=vec4(ambient_diffuse,1.0);
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
        vec3 diffuse = (vec3(1.0) - f) * (1.0 - metallic) * albedo / PI;
        result += (diffuse + specular) * frame.lights[i].radiance.rgb * ndl * visibility(i, ndl, world_dx, world_dy);
    }
    color = vec4(result, 1.0);
}
