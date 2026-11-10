#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 0) out vec2 disc_coordinate;
layout(location = 1) out vec3 linear_color;
layout(push_constant) uniform Preview {
    mat4 clip_from_preview;
    vec4 viewport; // inverse extent, point radius, normal length
    ivec4 mode; // 0 points, 1 normals, 2 boundary; y enables normal colors
} preview;

void main() {
    vec3 p = position;
    disc_coordinate = vec2(0.0);
    linear_color = vec3(0.25, 0.65, 0.85);
    if (preview.mode.x == 1) {
        p += float(gl_VertexIndex) * preview.viewport.w * normal;
        linear_color = vec3(1.0, 0.72, 0.16);
    } else if (preview.mode.x == 2) {
        linear_color = vec3(1.0, 0.27, 0.16);
    } else if (preview.mode.y != 0) {
        linear_color = normal * 0.5 + 0.5;
    }
    gl_Position = preview.clip_from_preview * vec4(p, 1.0);
    if (preview.mode.x == 0) {
        const vec2 corners[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(1,1),
                                      vec2(-1,-1), vec2(1,1), vec2(-1,1));
        disc_coordinate = corners[gl_VertexIndex];
        gl_Position.xy += disc_coordinate * 2.0 * preview.viewport.xy *
                          preview.viewport.z * gl_Position.w;
    }
}
