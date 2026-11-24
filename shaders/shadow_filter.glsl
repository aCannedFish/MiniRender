// 方向光硬阴影、固定核 PCF 和按遮挡间距计算半影的 PCSS。
#ifndef MINIRENDER_SHADOW_FILTER
#define MINIRENDER_SHADOW_FILTER

layout(binding = 1) uniform sampler2DArray shadow_map;

const int SHADOW_HARD = 0;
const int SHADOW_PCF  = 1;
const int SHADOW_PCSS = 2;

const int  DISK_SAMPLES    = 32;
const mat2 GOLDEN_ROTATION = mat2(-0.73736888, 0.67549029, -0.67549029, -0.73736888);

// 深度为方向光正交投影的线性窗口深度。梯度将接收面外推到实际比较的纹素中心。
struct ShadowReceiver {
    int   light;
    vec2  uv;
    float depth;
    vec2  gradient;
};

float reference_depth(ShadowReceiver receiver, vec2 uv) {
    return clamp(receiver.depth + dot(receiver.gradient, uv - receiver.uv), 0.0, 1.0);
}

float stored_depth(int light, ivec2 pixel, ivec2 size) {
    if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, size))) {
        return 1.0;
    }
    return texelFetch(shadow_map, ivec3(pixel, light), 0).r;
}

float blocker_gap(ShadowReceiver receiver, ivec2 pixel, ivec2 size) {
    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    return reference_depth(receiver, uv) - stored_depth(receiver.light, pixel, size);
}

float compare_texel(ShadowReceiver receiver, ivec2 pixel, ivec2 size) {
    return blocker_gap(receiver, pixel, size) > 0.0 ? 0.0 : 1.0;
}

// 在深度比较后进行双线性插值。连续移动光源或相机时，边缘不会按整纹素跳动。
float comparison(ShadowReceiver receiver, vec2 uv, ivec2 size) {
    vec2  pixel  = uv * vec2(size) - 0.5;
    ivec2 base   = ivec2(floor(pixel));
    vec2  t      = fract(pixel);
    float top    = mix(compare_texel(receiver, base, size),
                       compare_texel(receiver, base + ivec2(1, 0), size), t.x);
    float bottom = mix(compare_texel(receiver, base + ivec2(0, 1), size),
                       compare_texel(receiver, base + ivec2(1, 1), size), t.x);
    return mix(top, bottom, t.y);
}

// 稳定的 Vogel 圆盘，不依赖帧号或相机像素；两个轴的 UV 半径允许非方形光源投影。
float disk_pcf(ShadowReceiver receiver, vec2 radius, ivec2 size) {
    if (max(radius.x * size.x, radius.y * size.y) < 0.5) {
        return comparison(receiver, receiver.uv, size);
    }
    float total     = 0.0;
    vec2  direction = vec2(1.0, 0.0);
    for (int i = 0; i < DISK_SAMPLES; ++i) {
        float r = sqrt((float(i) + 0.5) / float(DISK_SAMPLES));
        total += comparison(receiver, receiver.uv + r * direction * radius, size);
        direction = GOLDEN_ROTATION * direction;
    }
    return total / float(DISK_SAMPLES);
}

float mean_blocker_gap(ShadowReceiver receiver, vec2 radius, ivec2 size) {
    float center    = blocker_gap(receiver, ivec2(floor(receiver.uv * vec2(size))), size);
    float sum       = max(center, 0.0);
    float count     = center > 0.0 ? 1.0 : 0.0;
    vec2  direction = vec2(0.0, 1.0);
    for (int i = 0; i < DISK_SAMPLES; ++i) {
        float r   = sqrt((float(i) + 0.5) / float(DISK_SAMPLES));
        vec2  uv  = receiver.uv + r * direction * radius;
        float gap = blocker_gap(receiver, ivec2(floor(uv * vec2(size))), size);
        if (gap > 0.0) {
            sum += gap;
            count += 1.0;
        }
        direction = GOLDEN_ROTATION * direction;
    }
    return count > 0.0 ? sum / count : 0.0;
}

// diagnostic 保存采样深度、平均遮挡深度和归一化半影半径。1 对应 64 纹素。
float shadow_visibility(int light, float ndl, vec3 world_dx, vec3 world_dy, out vec3 diagnostic) {
    diagnostic = vec3(1.0, 1.0, 0.0);
    if (frame.shadow_settings.x < 0.5) {
        return 1.0;
    }
    mat4 projection = frame.shadow_view_projection[light];
    vec3 position   = (projection * vec4(world_position, 1.0)).xyz;
    vec2 uv         = position.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0))) || position.z < 0.0 ||
        position.z > 1.0) {
        return 1.0;
    }

    // 正交投影的 w 恒为 1；屏幕导数在 main 的统一控制流中取得。
    vec3  dx          = (projection * vec4(world_dx, 0.0)).xyz;
    vec3  dy          = (projection * vec4(world_dy, 0.0)).xyz;
    vec2  uv_dx       = dx.xy * 0.5;
    vec2  uv_dy       = dy.xy * 0.5;
    float determinant = uv_dx.x * uv_dy.y - uv_dx.y * uv_dy.x;
    vec2  gradient    = vec2(0.0);
    if (abs(determinant) > 1e-12) {
        gradient =
            vec2(dx.z * uv_dy.y - dy.z * uv_dx.y, uv_dx.x * dy.z - uv_dy.x * dx.z) / determinant;
    }
    float          bias     = max(0.0002 * (1.0 - ndl), 0.00005);
    ShadowReceiver receiver = ShadowReceiver(light, uv, position.z - bias, gradient);
    ivec2          size     = textureSize(shadow_map, 0).xy;
    vec2           texel    = 1.0 / vec2(size);
    diagnostic.x            = stored_depth(light, ivec2(floor(uv * vec2(size))), size);
    int mode                = int(frame.shadow_settings.z);
    if (int(frame.display.y) == 9 || int(frame.display.y) == 10) {
        mode = SHADOW_PCSS;
    }
    float fixed_radius = frame.shadow_settings.y;
    float tangent      = frame.shadow_settings.w;
    if (mode == SHADOW_HARD || ((mode == SHADOW_PCF) ? fixed_radius <= 0.0 : tangent <= 0.0)) {
        return compare_texel(receiver, ivec2(floor(uv * vec2(size))), size);
    }
    if (mode == SHADOW_PCF) {
        diagnostic.z = fixed_radius / 64.0;
        // 半径为一时复用最初的 3×3 过滤核。
        if (fixed_radius == 1.0) {
            ivec2 center = ivec2(floor(uv * vec2(size)));
            float sum    = 0.0;
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    sum += compare_texel(receiver, center + ivec2(x, y), size);
                }
            }
            return sum / 9.0;
        }
        return disk_pcf(receiver, texel * fixed_radius, size);
    }
    // 方向光的半影半径（世界单位）= 遮挡间距 × tan(光源半角)。
    // 正交矩阵行向量将窗口深度差换算到米，再将米分别换算到 u、v。
    mat4 rows = transpose(projection);
    vec2 uv_per_depth =
        0.5 * vec2(length(rows[0].xyz), length(rows[1].xyz)) / max(length(rows[2].xyz), 1e-8);
    vec2  per_gap = tangent * uv_per_depth;
    vec2  limit   = 64.0 * texel;
    vec2  search  = min(limit, position.z * per_gap);
    float gap     = mean_blocker_gap(receiver, search, size);
    vec2  radius  = min(limit, gap * per_gap);
    diagnostic.y  = gap > 0.0 ? clamp(receiver.depth - gap, 0.0, 1.0) : 1.0;
    diagnostic.z  = max(radius.x * size.x, radius.y * size.y) / 64.0;
    return disk_pcf(receiver, radius, size);
}
#endif
