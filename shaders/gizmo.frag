#version 450

layout(location = 0) in vec4 vertex_color;
layout(location = 0) out vec4 color;
void main() {
    // 顶点颜色按显示 sRGB 指定，附件会自动编码，因此先转成线性值。
    vec3 linear = mix(pow((vertex_color.rgb + 0.055) / 1.055, vec3(2.4)),
                      vertex_color.rgb / 12.92, lessThanEqual(vertex_color.rgb, vec3(0.04045)));
    color = vec4(linear, vertex_color.a);
}
