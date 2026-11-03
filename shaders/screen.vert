#version 450

// 全屏三角形。AO 各阶段按 gl_FragCoord 读取附件，无需插值 UV。
void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
