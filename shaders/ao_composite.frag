#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
layout(binding = 1) uniform sampler2D source_hdr;
layout(binding = 2) uniform sampler2D ambient_diffuse;
layout(binding = 3) uniform sampler2D filtered_ao;
layout(binding = 4) uniform sampler2D raw_ao;
layout(location = 0) out vec4 color;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    color = texelFetch(source_hdr, pixel, 0);
    if (frame.ao_settings.x > 0.5) {
        float ao = frame.ao_filter.w > 0.5 ? texelFetch(filtered_ao, pixel, 0).r
                                          : texelFetch(raw_ao, pixel, 0).r;
        vec3 ambient = texelFetch(ambient_diffuse, pixel, 0).rgb;
        color.rgb = max(color.rgb - ambient * (1.0 - ao), vec3(0.0));
    }
}
