// 屏幕空间反射与一次漫反射间接光。两种遍历共享投影、厚度测试和出屏回退。
#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "screen_position.glsl"
layout(binding = 1) uniform sampler2D direct_image;
layout(binding = 2) uniform sampler2D normal_image;
layout(binding = 3) uniform sampler2D depth_image;
layout(binding = 4) uniform sampler2D albedo_image;
layout(binding = 5) uniform sampler2D mip1;
layout(binding = 6) uniform sampler2D mip2;
layout(binding = 7) uniform sampler2D mip3;
layout(binding = 8) uniform sampler2D mip4;
layout(binding = 9) uniform sampler2D mip5;
layout(location = 0) out vec4 result;

vec2 depths(ivec2 cell, int level) {
    if (level == 0) {
        return vec2(texelFetch(depth_image, cell, 0).r);
    }
    if (level == 1) {
        return texelFetch(mip1, cell, 0).rg;
    }
    if (level == 2) {
        return texelFetch(mip2, cell, 0).rg;
    }
    if (level == 3) {
        return texelFetch(mip3, cell, 0).rg;
    }
    if (level == 4) {
        return texelFetch(mip4, cell, 0).rg;
    }
    return texelFetch(mip5, cell, 0).rg;
}

float view_depth(float z) {
    return frame.viewport.z * frame.viewport.w /
           (frame.viewport.w - z * (frame.viewport.w - frame.viewport.z));
}

// 每次前进到当前单元的下一条边。投影后的射线在屏幕像素与 NDC 深度空间都是直线。
bool trace_ray(vec3 origin, vec3 direction, ivec2 start, out ivec2 hit, out int iterations) {
    iterations   = 0;
    ivec2 size   = textureSize(depth_image, 0);
    float travel = frame.screen_trace.x;
    if (direction.z > 0.00001) {
        travel = min(travel, (-frame.viewport.z * 1.01 - origin.z) / direction.z);
    }
    if (travel <= 0.0) {
        return false;
    }
    vec4 a  = frame.projection * vec4(origin, 1),
         b  = frame.projection * vec4(origin + direction * travel, 1);
    vec3 p0 = a.xyz / a.w, p1 = b.xyz / b.w;
    vec2 begin = (p0.xy * .5 + .5) * vec2(size), delta = (p1.xy - p0.xy) * .5 * vec2(size);
    // 先裁到窗口，避免掠过近裁面的射线在远离屏幕的端点上损失浮点精度。
    vec2  boundary = mix(vec2(0), vec2(size), step(vec2(0), delta));
    vec2  exit_t   = vec2(abs(delta.x) > 1e-6 ? (boundary.x - begin.x) / delta.x : 1.0,
                          abs(delta.y) > 1e-6 ? (boundary.y - begin.y) / delta.y : 1.0);
    float clipped  = clamp(min(exit_t.x, exit_t.y), 0.0, 1.0);
    delta *= clipped;
    p1.z       = mix(p0.z, p1.z, clipped);
    float span = max(abs(delta.x), abs(delta.y));
    if (span < 1.0) {
        return false;
    }
    int   top = int(frame.screen_options.x) > 0 ? 5 : 0, level = top;
    float offset = max(0.001 / span, 2e-7), s = 1.25 / span;
    for (iterations = 0; iterations < int(frame.screen_trace.z); ++iterations) {
        vec2 pixel = begin + s * delta;
        if (s >= 1 || any(lessThan(pixel, vec2(0))) || any(greaterThanEqual(pixel, vec2(size)))) {
            return false;
        }
        float scale    = float(1 << level);
        ivec2 cell     = ivec2(floor(pixel / scale));
        vec2  boundary = (vec2(cell) + step(vec2(0), delta)) * scale;
        float tx       = abs(delta.x) > 1e-6 ? (boundary.x - begin.x) / delta.x : 1e20;
        float ty       = abs(delta.y) > 1e-6 ? (boundary.y - begin.y) / delta.y : 1e20;
        float end      = min(1.0, max(s + offset, min(tx, ty)));
        vec2  occupied = depths(cell, level);
        float z0 = view_depth(mix(p0.z, p1.z, s)), z1 = view_depth(mix(p0.z, p1.z, end));
        bool  overlaps = occupied.x < 1.0 &&
                         max(z0, z1) >= view_depth(occupied.x) - frame.screen_trace.y &&
                         min(z0, z1) <= view_depth(occupied.y) + frame.screen_trace.y;
        if (overlaps && level > 0) {
            --level;
            continue;
        }
        if (overlaps && cell != start) {
            vec3 normal = mat3(frame.view) * texelFetch(normal_image, cell, 0).xyz;
            if (dot(normal, direction) < -.02) {
                hit = cell;
                return true;
            }
        }
        s     = end + offset;
        level = min(level + 1, top);
    }
    return false;
}

float hash_pixel(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(depth_image, 0);
    int   mode = int(frame.display.y);
    if (mode < 13 || mode > 20) {
        result = vec4(0);
        return;
    }
    float depth  = texelFetch(depth_image, p, 0).r;
    vec3  direct = texelFetch(direct_image, p, 0).rgb;
    vec3  kd     = texelFetch(albedo_image, p, 0).rgb;
    if (mode == 17) {
        result = vec4(kd, 1);
        return;
    }
    if (depth >= 1.0) {
        result = vec4(mode == 16 ? direct : vec3(0), 1);
        return;
    }
    vec3  position   = view_position(p, depth, size);
    vec3  n          = normalize(mat3(frame.view) * texelFetch(normal_image, p, 0).xyz);
    vec3  origin     = position + n * max(.002, frame.screen_trace.y * .6);
    ivec2 hit        = ivec2(0);
    int   iterations = 0;
    if (mode == 13 || mode == 14 || mode == 18) {
        bool valid = trace_ray(origin, reflect(normalize(position), n), p, hit, iterations);
        if (mode == 18) {
            result = vec4(vec3(float(iterations) / frame.screen_trace.z), 1);
        } else if (mode == 14) {
            result = vec4(valid ? vec3((vec2(hit) + .5) / vec2(size), 1) : vec3(0), 1);
        } else {
            result = vec4(valid ? texelFetch(direct_image, hit, 0).rgb : frame.environment.rgb, 1);
        }
        return;
    }
    vec3  t = normalize(cross(n, abs(n.z) < .9 ? vec3(0, 0, 1) : vec3(0, 1, 0)));
    vec3  b = cross(n, t), indirect = vec3(0);
    int   samples  = int(frame.screen_trace.w);
    float rotation = hash_pixel(vec2(p) + frame.screen_options.y) * 6.2831853;
    for (int i = 0; i < samples; ++i) {
        // 余弦半球采样已包含 cos/π，接收面的估计只需乘底色。
        float r = sqrt((float(i) + .5) / float(samples)), phi = float(i) * 2.3999632 + rotation;
        vec3  l = t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(1 - r * r);
        if (trace_ray(origin, l, p, hit, iterations)) {
            indirect += texelFetch(direct_image, hit, 0).rgb;
        }
    }
    indirect *= kd / float(samples);
    result = vec4(mode == 16 ? direct + indirect : indirect, 1);
}
