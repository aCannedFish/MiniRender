#version 450
layout(location = 0) out vec2 uv;

// 两个三角形覆盖 NDC 正方形，片元坐标与左上原点的 CPU 针孔射线一致。
void main() {
    const vec2 positions[6] =
        vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 p = positions[gl_VertexIndex];
    uv = p * .5 + .5;
    gl_Position = vec4(p, 0, 1);
}
