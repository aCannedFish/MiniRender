// 将环境遮蔽应用到环境漫反射附件，并处理原色预览的暗化上限。

#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
layout(binding = 1) uniform sampler2D source_hdr;
layout(binding = 2) uniform sampler2D ambient_diffuse;
layout(binding = 3) uniform sampler2D filtered_ao;
layout(binding = 4) uniform sampler2D raw_ao;
layout(binding = 5) uniform sampler2D screen_transport;
layout(location = 0) out vec4 color;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (int(frame.display.y) >= 13 && int(frame.display.y) <= 18) {
        color = texelFetch(screen_transport, pixel, 0);
        return;
    }
    color = texelFetch(source_hdr, pixel, 0);
    if (frame.ao_settings.x > 0.5) {
        float ao = frame.ao_filter.w > 0.5 ? texelFetch(filtered_ao, pixel, 0).r
                                           : texelFetch(raw_ao, pixel, 0).r;
        // 原色模式将接触暗化限制在 35%，保留底色的可辨认性。
        if (base_color_preview()) {
            ao = max(ao, 0.65);
        }
        vec3 ambient = texelFetch(ambient_diffuse, pixel, 0).rgb;
        color.rgb    = max(color.rgb - ambient * (1.0 - ao), vec3(0.0));
    }
}
