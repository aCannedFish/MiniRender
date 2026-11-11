#version 450
layout(binding = 0) uniform sampler2D image;
layout(push_constant) uniform Display {
    float exposure;
    int surface;
}
display;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

// 浮点纹理采用最近邻采样。Surface 使用曝光与 Reinhard，诊断图保留原有值。
void main() {
    vec3 linear = texture(image, uv).rgb;
    if (display.surface != 0) {
        linear = max(linear, vec3(0)) * exp2(display.exposure);
        linear /= vec3(1) + linear;
    }
    color = vec4(linear, 1); // sRGB 附件执行显示编码。
}
