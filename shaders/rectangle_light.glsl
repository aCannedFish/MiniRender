// LTC 矩形灯的地平线裁剪与球面边积分。拟合表来自 Heitz 等，SIGGRAPH 2016。
#ifndef MINIRENDER_RECTANGLE_LIGHT
#define MINIRENDER_RECTANGLE_LIGHT
layout(binding = 8) uniform sampler2D ltc_matrix;
layout(binding = 9) uniform sampler2D ltc_magnitude;

struct Rectangle {
    vec4 corners[4];
    vec4 radiance;
};

layout(std430, binding = 10) readonly buffer Rectangles {
    Rectangle lights[];
}

area;

float rectangle_integral(vec3 corners[4], mat3 transform) {
    vec3 poly[5];
    int  count = 0;
    vec3 prev  = transform * corners[3];
    for (int i = 0; i < 4; ++i) {
        vec3 cur = transform * corners[i];
        if ((cur.z > 0.0) != (prev.z > 0.0)) {
            poly[count++] = prev + (cur - prev) * (prev.z / (prev.z - cur.z));
        }
        if (cur.z > 0.0) {
            poly[count++] = cur;
        }
        prev = cur;
    }
    float sum = 0.0;
    for (int i = 0; i < count; ++i) {
        vec3  a = poly[i], b = poly[(i + 1) % count];
        vec3  c   = cross(a, b);
        float len = length(c);
        if (len > 1e-12) {
            sum += c.z * atan(len, dot(a, b)) / len;
        }
    }
    return clamp(abs(sum) / (2.0 * IBL_PI), 0.0, 1.0);
}

// 分开返回漫反射和镜面辐亮度，便于分别计算光源的可见份额。
void rectangle_response(Rectangle light, vec3 point, vec3 n, vec3 v, vec3 albedo, float roughness,
                        float metallic, out vec3 diffuse, out vec3 specular) {
    diffuse      = vec3(0);
    specular     = vec3(0);
    vec3  normal = cross(light.corners[1].xyz - light.corners[0].xyz,
                         light.corners[3].xyz - light.corners[0].xyz);
    float facing = dot(normal, point - light.corners[0].xyz);
    if (abs(facing) < 1e-8 || (light.radiance.w < 0.5 && facing < 0.0)) {
        return;
    }
    float mu = clamp(dot(n, v), 0.0, 1.0);
    vec3  t  = v - n * mu;
    if (dot(t, t) < 1e-8) {
        t = cross(abs(n.z) < .9 ? vec3(0, 0, 1) : vec3(1, 0, 0), n);
    }
    t          = normalize(t);
    mat3 basis = transpose(mat3(t, cross(n, t), n));
    vec3 corners[4];
    for (int i = 0; i < 4; ++i) {
        corners[i] = basis * (light.corners[i].xyz - point);
    }
    if (metallic < 0.5) {
        diffuse = light.radiance.rgb * albedo * rectangle_integral(corners, mat3(1));
        return;
    }
    vec2 pixel = vec2(clamp(roughness, 0.0, 1.0), sqrt(1.0 - mu)) * 63.0;
    vec3 a = ibl_bilinear(ltc_matrix, pixel, false), b = ibl_bilinear(ltc_magnitude, pixel, false);
    mat3 inverse_fit = mat3(vec3(a.x, 0, a.y), vec3(0, 1, 0), vec3(a.z, 0, b.x));
    specular         = light.radiance.rgb * (albedo * b.y + (1.0 - albedo) * b.z) *
                       rectangle_integral(corners, inverse_fit);
}
#endif
