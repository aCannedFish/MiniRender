#ifndef MINIRENDER_CLOTH
#define MINIRENDER_CLOTH
#include "scene_instance.glsl"

// 与 CPU ClothBsdf 相同的圆柱角度、相关遮蔽和曲线求积，直接输出单位入射辐射的响应。
struct ClothAngles {
    float theta;
    float phi;
    float psi;
    float cosine;
    float sine;
    float normal;
};

float cloth_hypot(vec2 value) {
    float scale = max(abs(value.x), abs(value.y));
    return scale > 0 ? scale * length(value / scale) : 0;
}
ClothAngles cloth_angles(vec3 direction, vec3 tangent, vec3 normal, vec3 side) {
    float t = dot(direction, tangent), n = dot(direction, normal), b = dot(direction, side);
    float cosine = cloth_hypot(vec2(n, b));
    return ClothAngles(atan(t, cosine), atan(b, n), atan(t, n), cosine, t, n);
}
float cloth_correlated(float a, float b, float difference, float width) {
    float x = difference / width;
    float correlation = exp(-.5 * x * x);
    return (1 - correlation) * a * b + correlation * min(a, b);
}
float cloth_fresnel(float cosine, float ior) {
    if (ior == 1) {
        return 0;
    }
    cosine = clamp(cosine, 0, 1);
    float sin2 = max(0, 1 - cosine * cosine) / (ior * ior);
    float transmitted = sqrt(max(0, 1 - sin2));
    float parallel = (ior * cosine - transmitted) / (ior * cosine + transmitted);
    float perpendicular = (cosine - ior * transmitted) / (cosine + ior * transmitted);
    return .5 * (parallel * parallel + perpendicular * perpendicular);
}
vec3 cloth_axis(vec3 raw, vec3 n, vec2 rotation) {
    vec3 projected = raw - n * dot(raw, n);
    float squared = dot(projected, projected);
    if (squared < 1e-20) {
        vec3 helper = abs(n.z) < .999 ? vec3(0, 0, 1) : vec3(0, 1, 0);
        projected = normalize(cross(helper, n));
    } else {
        projected /= sqrt(squared);
    }
    return rotation.x * projected + rotation.y * cross(n, projected);
}

vec3 cloth_response(ClothData data, vec3 n, vec3 u, vec3 v, vec3 wo, vec3 wi, vec3 dye) {
    float coverage = data.coverage.x + data.coverage.y;
    float normalization = max(0, 1 - coverage) * dot(wo, n);
    vec3 result = vec3(0);
    for (int i = 0; i < data.counts.x; ++i) {
        vec4 sample_value = data.tangents[i];
        if (sample_value.z == 0) {
            continue;
        }
        int thread = int(sample_value.w);
        vec3 axis = thread == 0 ? u : v;
        vec3 tangent = sample_value.x * axis + sample_value.y * n;
        vec3 normal = sample_value.x * n - sample_value.y * axis;
        vec3 side = cross(n, axis);
        ClothAngles incoming = cloth_angles(wi, tangent, normal, side);
        ClothAngles outgoing = cloth_angles(wo, tangent, normal, side);
        if (incoming.normal <= 0 || outgoing.normal <= 0) {
            continue;
        }
        float phi = incoming.phi - outgoing.phi;
        float masking = cloth_correlated(incoming.normal / incoming.cosine,
                                         outgoing.normal / outgoing.cosine, phi, data.settings.y);
        float projection =
            cloth_correlated(incoming.normal / cloth_hypot(vec2(incoming.sine, incoming.normal)),
                             outgoing.normal / cloth_hypot(vec2(outgoing.sine, outgoing.normal)),
                             incoming.psi - outgoing.psi, data.settings.y);
        normalization += sample_value.z * projection;
        float csum = incoming.cosine + outgoing.cosine;
        float ssum = incoming.sine + outgoing.sine;
        float cosine_d_squared = max(.25 * (csum * csum + ssum * ssum), 1e-12);
        float cosine_phi = max(cos(.5 * phi), 0);
        float fresnel = cloth_fresnel(sqrt(cosine_d_squared) * cosine_phi, data.settings.x);
        float transmission = (1 - cloth_fresnel(incoming.cosine, data.settings.x)) *
                             (1 - cloth_fresnel(outgoing.cosine, data.settings.x));
        vec4 widths = data.widths[thread];
        float theta_h = .5 * (incoming.theta + outgoing.theta);
        vec2 offset = theta_h / widths.xy;
        vec2 gaussian = exp(-.5 * offset * offset) * widths.zw;
        float surface = fresnel * cosine_phi * gaussian.x;
        float kd = data.albedo[thread].w;
        float volume =
            transmission * ((1 - kd) * gaussian.y + kd) / (incoming.cosine + outgoing.cosine);
        vec3 scattering = vec3(surface) + volume * data.albedo[thread].rgb * dye;
        result +=
            sample_value.z * masking * projection * incoming.cosine / cosine_d_squared * scattering;
    }
    return normalization > 1e-12 ? result / normalization : vec3(0);
}

#endif
