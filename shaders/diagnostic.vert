#version 450

layout(location = 0) out vec2 texcoord;
layout(location = 1) flat out int layer;

// 先画近处纹理三角形，再画远处红色三角形。
// 若深度写入/测试失效，中心会被红色覆盖，回读测试据此检错。
void main() {
    const vec2 positions[3] = vec2[3](vec2(-0.7, 0.6), vec2(0.7, 0.6), vec2(0.0, -0.7));
    const vec2 uv[3] = vec2[3](vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.5, 0.0));
    layer = gl_InstanceIndex;
    float scale = layer == 0 ? 0.75 : 1.0;
    float depth = layer == 0 ? 0.2 : 0.8;
    gl_Position = vec4(positions[gl_VertexIndex] * scale, depth, 1.0);
    texcoord = uv[gl_VertexIndex];
}
