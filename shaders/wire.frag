#version 450
layout(location = 0) out vec4 color;
layout(location = 0) flat in int visible_side;
void main() {
    if (visible_side == 0) {
        discard;
    }
    color = vec4(1.0);
}
