#version 450

layout(binding = 0) uniform sampler2D checker;
layout(location = 0) in vec2 texcoord;
layout(location = 1) flat in int layer;
layout(location = 0) out vec4 color;

// 纹理为 SRGB 格式，采样器解码为线性值，SRGB 交换链负责最终编码。
void main() {
    color = layer == 0 ? texture(checker, texcoord) : vec4(0.65, 0.025, 0.015, 1.0);
}
