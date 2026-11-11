#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

// 后序图保存在一个小 UBO 中。全部节点和帧参数低于 Vulkan 保证的 16 KiB。
struct Node {
    mat4 inverse_transform;
    vec4 parameters;
    vec4 color;
    ivec4 links;
};
layout(binding = 0, std140) uniform SdfFrame {
    vec4 origin;
    vec4 forward;
    vec4 right;
    vec4 up;
    vec4 bounds_min;
    vec4 bounds_max;
    vec4 settings;
    ivec4 counts;
    vec4 light_direction;
    vec4 light_color;
    vec4 ambient;
    ivec4 display;
    Node nodes[64];
}
frame;

vec2 field(vec3 p) {
    vec2 values[64];
    for (int i = 0; i < frame.counts.x; ++i) {
        Node n = frame.nodes[i];
        int kind = n.links.x;
        if (kind < 3) {
            vec3 local = (n.inverse_transform * vec4(p, 1)).xyz;
            float d;
            if (kind == 0) {
                d = length(local) - n.parameters.x;
            } else if (kind == 1) {
                vec3 q = abs(local) - n.parameters.xyz;
                d = length(max(q, vec3(0))) + min(max(q.x, max(q.y, q.z)), 0.0);
            } else {
                d = length(vec2(length(local.xz) - n.parameters.x, local.y)) - n.parameters.y;
            }
            values[i] = vec2(d * n.parameters.w, float(i));
        } else {
            vec2 a = values[n.links.y], b = values[n.links.z];
            if (kind == 5) {
                b.x = -b.x;
            }
            values[i] = kind == 3 ? (a.x <= b.x ? a : b) : (a.x >= b.x ? a : b);
        }
    }
    return values[frame.counts.y];
}

struct Hit {
    int state;
    int steps;
    float distance;
    int material;
    vec3 position;
};

// 状态 0 为未命中，1 为命中，2 为步数耗尽；内部起点沿绝对距离寻找出口。
Hit trace(vec3 origin, vec3 direction, float first, float last, int budget) {
    Hit hit = Hit(0, 0, 0, 0, vec3(0));
    last = min(last, frame.bounds_max.w);
    if (frame.bounds_min.w > .5 || first > last) {
        return hit;
    }
    float padding = 4 * (frame.settings.x + frame.settings.y * last);
    for (int axis = 0; axis < 3; ++axis) {
        float low = frame.bounds_min[axis] - padding, high = frame.bounds_max[axis] + padding;
        if (direction[axis] == 0) {
            if (origin[axis] < low || origin[axis] > high) {
                return hit;
            }
        } else {
            float a = (low - origin[axis]) / direction[axis];
            float b = (high - origin[axis]) / direction[axis];
            first = max(first, min(a, b));
            last = min(last, max(a, b));
            if (first > last) {
                return hit;
            }
        }
    }
    float t = first;
    for (int step = 0; step < budget; ++step) {
        hit.position = origin + direction * t;
        vec2 value = field(hit.position);
        float residual = abs(value.x);
        hit.distance = t;
        hit.steps = step + 1;
        hit.material = int(value.y);
        if (residual <= frame.settings.x + frame.settings.y * t) {
            hit.state = 1;
            return hit;
        }
        float next = t + frame.settings.w * residual;
        if (next > last) {
            return hit;
        }
        if (next <= t) {
            break;
        }
        t = next;
    }
    hit.state = 2;
    return hit;
}

vec3 surface_normal(vec3 p, vec3 fallback) {
    float h = frame.settings.z;
    vec3 n = vec3(field(p + vec3(h, 0, 0)).x - field(p - vec3(h, 0, 0)).x,
                  field(p + vec3(0, h, 0)).x - field(p - vec3(0, h, 0)).x,
                  field(p + vec3(0, 0, h)).x - field(p - vec3(0, 0, h)).x);
    return dot(n, n) > 0 ? normalize(n) : fallback;
}

void main() {
    vec3 direction =
        normalize(frame.forward.xyz + (2 * uv.x - 1) * frame.right.xyz * frame.right.w +
                  (1 - 2 * uv.y) * frame.up.xyz * frame.up.w);
    Hit hit = trace(frame.origin.xyz, direction, frame.origin.w, frame.forward.w, frame.counts.z);
    int mode = frame.display.x;
    vec3 result = mode == 0 ? vec3(.015, .022, .035) : vec3(0);
    if (hit.state == 2) {
        result = vec3(1, 0, 1);
    } else if (mode == 3) {
        float t = float(hit.steps) / float(frame.counts.z);
        result = clamp(vec3(1.5) - abs(4 * t - vec3(3, 2, 1)), vec3(0), vec3(1));
    } else if (hit.state == 1) {
        if (mode == 2) {
            result = vec3(1 / (1 + hit.distance * dot(direction, frame.forward.xyz)));
        } else {
            vec3 n = surface_normal(hit.position, -direction);
            if (dot(n, direction) > 0) {
                n = -n;
            }
            if (mode == 1) {
                result = n * .5 + .5;
            } else {
                vec3 toward_light = -frame.light_direction.xyz;
                float cosine = max(dot(n, toward_light), 0.0);
                float visible = 1;
                if (frame.display.y != 0 && cosine > 0) {
                    float epsilon = frame.settings.x + frame.settings.y * hit.distance;
                    Hit shadow = trace(hit.position + n * (4 * epsilon), toward_light, 2 * epsilon,
                                       frame.bounds_max.w, frame.counts.w);
                    visible = shadow.state == 0 ? 1 : 0;
                }
                result = frame.nodes[hit.material].color.rgb *
                         (frame.ambient.rgb + frame.light_color.rgb * frame.light_direction.w *
                                                  (cosine * visible / 3.141592653589793));
            }
        }
    }
    color = vec4(result, 1);
}
