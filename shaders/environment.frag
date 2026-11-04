#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_frame.glsl"
#include "ibl_sampling.glsl"
layout(location=0) out vec4 color;
layout(location=1) out vec4 normal_output;
layout(location=2) out vec4 ambient_output;

void main() {
    vec2 uv=gl_FragCoord.xy*frame.viewport.xy;
    vec4 point=frame.inverse_projection*vec4(uv*2.0-1.0,1.0,1.0);
    vec3 direction=normalize(transpose(mat3(frame.view))*point.xyz);
    color=vec4(frame.environment.rgb+frame.ibl.z*ibl_source(direction),1.0);
    normal_output=vec4(0.0);
    ambient_output=vec4(0.0);
}
