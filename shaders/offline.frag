#version 450
layout(binding = 0) uniform sampler2D linear_image;
layout(push_constant) uniform Display { float exposure; } display;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

// 与 display_image 使用同一曝光和 Reinhard 变换，sRGB 编码由交换链完成。
void main() {
    ivec2 size = textureSize(linear_image, 0);
    ivec2 pixel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
    vec3 value = max(texelFetch(linear_image, pixel, 0).rgb, vec3(0.0));
    // 等价的 Reinhard 形式避免 HDR * exposure 在 float 中溢出。
    color = vec4(value / (value + vec3(exp2(-display.exposure))), 1.0);
}
