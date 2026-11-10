#version 450
layout(location = 0) in vec2 disc_coordinate;
layout(location = 1) in vec3 linear_color;
layout(location = 0) out vec4 color;
void main() {
    if (dot(disc_coordinate, disc_coordinate) > 1.0) { discard; }
    color = vec4(linear_color, 1.0);
}
