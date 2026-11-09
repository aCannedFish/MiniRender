#version 450

// CPU 将逻辑像素映射到 Vulkan NDC，覆盖层直接使用显示颜色。
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vertex_color;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    vertex_color = color;
}
