#ifndef MINIRENDER_IBL_SAMPLING
#define MINIRENDER_IBL_SAMPLING
layout(binding=4) uniform sampler2D ibl_diffuse_map;
layout(binding=5) uniform sampler2DArray ibl_specular_map;
layout(binding=6) uniform sampler2D ibl_brdf_map;
layout(binding=7) uniform sampler2D ibl_source_map;
const float IBL_PI=3.141592653589793;

vec2 ibl_uv(vec3 d) {
    return vec2(fract((atan(d.z,d.x)+frame.ibl.w)/(2.0*IBL_PI)),
                atan(length(d.xz),d.y)/IBL_PI);
}
ivec2 ibl_pixel(ivec2 p,ivec2 size,bool repeat_x) {
    int x=repeat_x ? (p.x%size.x+size.x)%size.x : clamp(p.x,0,size.x-1);
    return ivec2(x,clamp(p.y,0,size.y-1));
}
// float32 纹理显式插值，源图和烘焙图分别采用中心/极点端点布局。
vec3 ibl_bilinear(sampler2D image,vec2 pixel,bool repeat_x) {
    ivec2 size=textureSize(image,0);
    ivec2 p=ivec2(floor(pixel));
    vec2 t=fract(pixel);
    return mix(mix(texelFetch(image,ibl_pixel(p,size,repeat_x),0).rgb,
                   texelFetch(image,ibl_pixel(p+ivec2(1,0),size,repeat_x),0).rgb,t.x),
               mix(texelFetch(image,ibl_pixel(p+ivec2(0,1),size,repeat_x),0).rgb,
                   texelFetch(image,ibl_pixel(p+ivec2(1,1),size,repeat_x),0).rgb,t.x),t.y);
}
vec3 ibl_diffuse(vec3 n) {
    vec2 uv=ibl_uv(n);
    ivec2 size=textureSize(ibl_diffuse_map,0);
    return ibl_bilinear(ibl_diffuse_map,vec2(uv.x*float(size.x)-0.5,uv.y*float(size.y-1)),true);
}
vec3 ibl_specular_layer(vec2 uv,int layer) {
    ivec2 size=textureSize(ibl_specular_map,0).xy;
    vec2 pixel=vec2(uv.x*float(size.x)-0.5,uv.y*float(size.y-1));
    ivec2 p=ivec2(floor(pixel));
    vec2 t=fract(pixel);
    return mix(mix(texelFetch(ibl_specular_map,ivec3(ibl_pixel(p,size,true),layer),0).rgb,
                   texelFetch(ibl_specular_map,ivec3(ibl_pixel(p+ivec2(1,0),size,true),layer),0).rgb,t.x),
               mix(texelFetch(ibl_specular_map,ivec3(ibl_pixel(p+ivec2(0,1),size,true),layer),0).rgb,
                   texelFetch(ibl_specular_map,ivec3(ibl_pixel(p+ivec2(1,1),size,true),layer),0).rgb,t.x),t.y);
}
vec3 ibl_specular(vec3 r,float roughness) {
    vec2 uv=ibl_uv(r);
    float layer=clamp(roughness,0.0,1.0)*float(textureSize(ibl_specular_map,0).z-1);
    int lower=int(floor(layer));
    int upper=min(lower+1,textureSize(ibl_specular_map,0).z-1);
    return mix(ibl_specular_layer(uv,lower),ibl_specular_layer(uv,upper),fract(layer));
}
vec2 ibl_brdf(float ndv,float roughness) {
    vec2 size=vec2(textureSize(ibl_brdf_map,0)-1);
    return ibl_bilinear(ibl_brdf_map,clamp(vec2(ndv,roughness),vec2(0),vec2(1))*size,false).rg;
}
vec3 ibl_source(vec3 direction) {
    vec2 uv=ibl_uv(direction);
    return ibl_bilinear(ibl_source_map,uv*vec2(textureSize(ibl_source_map,0))-0.5,true);
}
#endif
