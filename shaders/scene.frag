// 三角表面的原色、GGX 光照、阴影、织物与 IBL 着色，输出颜色和辅助附件。

#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"

#include "scene_instance.glsl"

#include "cloth.glsl"
#include "ibl_sampling.glsl"

#include "rectangle_light.glsl"
#include "rectangle_visibility.glsl"

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec3 base_color;
layout(location = 3) in vec3 emission;
layout(location = 4) in vec2 material;
layout(location = 5) in vec2 mesh_uv;
layout(location = 6) in vec3 world_tangent_u;
layout(location = 7) in vec3 world_tangent_v;
layout(binding = 2) uniform sampler2D base_texture;
layout(binding = 14) uniform sampler2D normal_texture;
layout(location = 8) in vec3 prt_color;
layout(location = 0) out vec4 color;
layout(location = 1) out vec4 normal_output;
layout(location = 2) out vec4 ambient_output;
layout(location = 3) out vec4 albedo_output;
#include "shadow_filter.glsl"
const float PI = 3.141592653589793;

// 分布与 Smith G1 参考 Walter et al. 2007，推导与实现差异见 docs/VIEWPORT.md。
// GGX 的 alpha = roughness^2。这里实现直接光 BRDF，不包含 IBL 卷积。
float distribution_ggx(float ndh, float alpha) {
    float a2          = alpha * alpha;
    float denominator = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 1e-7);
}

float smith_g1(float nd, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * nd / max(nd + sqrt(a2 + (1.0 - a2) * nd * nd), 1e-6);
}

ivec2 repeat_pixel(ivec2 pixel, ivec2 extent) {
    return (pixel % extent + extent) % extent;
}

vec3 bilinear_texture(sampler2D image, vec2 uv, int level) {
    ivec2 extent   = textureSize(image, level);
    vec2  position = uv * vec2(extent) - 0.5;
    ivec2 lower    = ivec2(floor(position));
    vec2  t        = fract(position);
    vec3  a        = texelFetch(image, repeat_pixel(lower, extent), level).rgb;
    vec3  b        = texelFetch(image, repeat_pixel(lower + ivec2(1, 0), extent), level).rgb;
    vec3  c        = texelFetch(image, repeat_pixel(lower + ivec2(0, 1), extent), level).rgb;
    vec3  d        = texelFetch(image, repeat_pixel(lower + ivec2(1, 1), extent), level).rgb;
    return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}

vec3 material_texture(sampler2D image) {
    vec2  uv          = vec2(fract(mesh_uv.x), 1.0 - fract(mesh_uv.y));
    ivec2 extent      = textureSize(image, 0);
    int   filter_mode = int(instances.values[draw_index.instance].surface.z);
    if (filter_mode == 0) {
        return texelFetch(image, repeat_pixel(ivec2(floor(uv * vec2(extent))), extent), 0).rgb;
    }
    if (filter_mode == 1) {
        return bilinear_texture(image, uv, 0);
    }
    vec2  dx    = dFdx(mesh_uv) * vec2(extent);
    vec2  dy    = dFdy(mesh_uv) * vec2(extent);
    float lod   = clamp(log2(max(max(length(dx), length(dy)), 1e-8)), 0.0,
                        float(textureQueryLevels(image) - 1));
    int   lower = int(floor(lod));
    int   upper = min(lower + 1, textureQueryLevels(image) - 1);
    return mix(bilinear_texture(image, uv, lower), bilinear_texture(image, uv, upper), fract(lod));
}

void main() {
    vec3  world_dx = dFdx(world_position);
    vec3  world_dy = dFdy(world_position);
    vec3  albedo   = base_color * clamp(material_texture(base_texture), vec3(0.0), vec3(1.0));
    vec3  n        = normalize(world_normal);
    vec3  v        = normalize(frame.camera_position.xyz - world_position);
    float strength = uintBitsToFloat(instances.values[draw_index.instance].geometry.z);
    vec3  tangent  = world_tangent_u - n * dot(n, world_tangent_u);
    if (strength != 0.0 && dot(tangent, tangent) > 1e-10) {
        tangent        = normalize(tangent);
        vec3 bitangent = cross(n, tangent) * sign(dot(cross(n, tangent), world_tangent_v));
        vec3 local     = material_texture(normal_texture) * 2.0 - 1.0;
        vec3 mapped =
            tangent * (abs(strength) * local.x) + bitangent * (strength * local.y) + n * local.z;
        if (dot(mapped, n) > 1e-8) {
            n = normalize(mapped);
        }
    }
    if (dot(n, v) < 0.0) {
        n = -n;
    }
    normal_output           = vec4(n, float(instances.values[draw_index.instance].geometry.y));
    albedo_output           = vec4(albedo, material.y > .5 ? -1.0 - material.x : material.x);
    ClothData cloth         = instances.values[draw_index.instance].cloth;
    bool      cloth_enabled = cloth.counts.y != 0;
    int       mode          = int(frame.display.y);
    if (mode == 12) {
        color = vec4(
            frame.prt_light[0].w > .5 ? albedo * max(prt_color, vec3(0)) + emission : albedo, 1);
        ambient_output = vec4(0);
        return;
    }
    if (mode >= 8 && mode <= 11) {
        vec3  debug = vec3(0.0);
        float vis   = 1.0;
        if (frame.counts.x > 0) {
            int   light = clamp(frame.counts.z, 0, frame.counts.x - 1);
            float ndl   = max(dot(n, -normalize(frame.lights[light].direction.xyz)), 0.0);
            vis         = shadow_visibility(light, ndl, world_dx, world_dy, debug);
        }
        float value    = mode == 8 ? debug.x : mode == 9 ? debug.y : mode == 10 ? debug.z : vis;
        color          = vec4(vec3(value), 1.0);
        ambient_output = vec4(0.0);
        return;
    }
    if (int(frame.display.y) == 7) {
        // 点光源采用平方反比衰减。半角向量决定 Blinn–Phong 高光的方向与宽度。
        vec3  to_light  = frame.teaching_light.xyz - world_position;
        float distance2 = max(dot(to_light, to_light), 1e-6);
        vec3  l         = to_light * inversesqrt(distance2);
        vec3  h         = l + v;
        float cosine    = max(dot(n, l), 0.0);
        float highlight = cosine > 0.0 && dot(h, h) > 1e-8
                              ? pow(max(dot(n, normalize(h)), 0.0), frame.teaching_surface.x)
                              : 0.0;
        vec3  kd        = frame.teaching_surface.z > 0.5 ? albedo : base_color;
        ambient_output  = vec4(0.05 * kd, 1.0);
        color           = vec4(ambient_output.rgb +
                                   frame.teaching_light.w / distance2 *
                                       (kd * cosine + vec3(frame.teaching_surface.y * highlight)),
                               1.0);
        return;
    }
    if (base_color_preview()) {
        // 均匀白色环境只展示原色；不计算直接光、镜面反射、阴影或 IBL。
        if (cloth_enabled) {
            albedo *=
                cloth.coverage.x * cloth.albedo[0].rgb + cloth.coverage.y * cloth.albedo[1].rgb;
        }
        // 发光面显示色相，亮度归一化，避免灯具自身的高亮淹没材质。
        float peak = max(emission.r, max(emission.g, emission.b));
        if (peak > 0.0) {
            albedo = max(albedo, emission / max(peak, 1.0));
        }
        color          = vec4(albedo, 1.0);
        ambient_output = color;
        return;
    }
    float roughness            = max(material.x, 0.045);
    float metallic             = material.y;
    float alpha                = roughness * roughness;
    vec3  f0                   = mix(vec3(0.04), albedo, metallic);
    float ndv                  = max(dot(n, v), 0.0);
    vec3  diffuse_environment  = frame.environment.rgb;
    vec3  specular_environment = frame.environment.rgb;
    if (frame.ibl.y > 0.5) {
        diffuse_environment += frame.ibl.z * ibl_diffuse(n);
        specular_environment +=
            frame.ibl.z * ibl_specular(reflect(-v, n), clamp(material.x, 0.0, 1.0));
    }
    vec3 ambient_diffuse = diffuse_environment * albedo * (1.0 - metallic);
    vec3 cloth_u = vec3(0), cloth_v = vec3(0);
    if (cloth_enabled) {
        cloth_u = cloth_axis(world_tangent_u, n, cloth.settings.zw);
        cloth_v = cloth_axis(world_tangent_v, n, cloth.settings.zw);
        // 实时环境漫反射采用覆盖率加权的纱线颜色；直接光使用完整微圆柱模型。
        vec3 average =
            cloth.coverage.x * cloth.albedo[0].rgb + cloth.coverage.y * cloth.albedo[1].rgb;
        ambient_diffuse = diffuse_environment * albedo * average;
    }
    vec3 specular_factor = albedo;
    if (frame.ibl.x > 0.5) {
        vec2 ab         = ibl_brdf(ndv, clamp(material.x, 0.0, 1.0));
        specular_factor = albedo * ab.x + ab.y;
    }
    vec3  result = emission + ambient_diffuse + metallic * specular_environment * specular_factor;
    float energy_mode = instances.values[draw_index.instance].emission.w;
    bool  compensated = energy_mode >= 0.0 && frame.ibl.x > 0.5 &&
                        (frame.counts.w < 0 ? energy_mode > 0.5 : frame.counts.w > 0);
    float eo = 1.0, average = 1.0;
    vec3  weight = vec3(0);
    if (compensated) {
        vec3 lut = brdf_energy(ndv, material.x);
        eo       = clamp(lut.r + lut.g, 0.0, 1.0);
        average  = clamp(lut.b, 0.0, 1.0);
        weight   = compensation_color(f0, average);
        // 多次散射瓣近似用余弦卷积环境照明。常量环境下积分为 (1-Eo)。
        result += diffuse_environment * weight * (1.0 - eo);
    }
    ambient_output = vec4(ambient_diffuse, 1.0);
    for (int i = 0; i < frame.counts.x; ++i) {
        vec3  l   = -normalize(frame.lights[i].direction.xyz);
        float ndl = max(dot(n, l), 0.0);
        if (ndl <= 0.0 || ndv <= 0.0) {
            continue;
        }
        vec3  diagnostic;
        float visibility = shadow_visibility(i, ndl, world_dx, world_dy, diagnostic);
        if (cloth_enabled) {
            result += cloth_response(cloth, n, cloth_u, cloth_v, v, l, albedo) *
                      frame.lights[i].radiance.rgb * visibility;
            continue;
        }
        vec3  h        = normalize(l + v);
        float ndh      = max(dot(n, h), 0.0);
        float vdh      = max(dot(v, h), 0.0);
        vec3  f        = f0 + (vec3(1.0) - f0) * pow(1.0 - vdh, 5.0);
        float d        = distribution_ggx(ndh, alpha);
        float g        = smith_g1(ndl, alpha) * smith_g1(ndv, alpha);
        vec3  specular = f * d * g / max(4.0 * ndl * ndv, 1e-6);
        if (compensated && average < 1.0 - 1e-6) {
            vec3  lookup = brdf_energy(ndl, material.x);
            float ei     = clamp(lookup.r + lookup.g, 0.0, 1.0);
            specular += weight * ((1.0 - ei) * (1.0 - eo) / (PI * (1.0 - average)));
        }
        vec3 diffuse = (vec3(1.0) - f) * (1.0 - metallic) * albedo / PI;
        result += (diffuse + specular) * frame.lights[i].radiance.rgb * ndl * visibility;
    }
    for (int i = 0; i < int(frame.area_settings.x); ++i) {
        vec3 diffuse, specular;
        rectangle_response(area.lights[i], world_position, n, v, albedo, material.x, metallic,
                           diffuse, specular);
        vec2 visible =
            rectangle_visibility(i, area.lights[i], world_position, n, v, material.x, f0);
        result += diffuse * visible.x + specular * visible.y;
    }
    color = vec4(result, 1.0);
}
