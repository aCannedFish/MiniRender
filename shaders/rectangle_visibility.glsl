// 对矩形灯上的分层样本查询深度，按漫反射和镜面贡献分别估算可见份额。
#ifndef MINIRENDER_RECTANGLE_VISIBILITY
#define MINIRENDER_RECTANGLE_VISIBILITY
layout(std430,binding=11) readonly buffer RectangleProjections {mat4 matrices[];} area_shadow;
layout(binding=12) uniform sampler2DArray area_depth;

// 按最大轴选择立方体面。顶点变换和查询共用矩阵，可覆盖灯具两侧的物体。
int shadow_face(vec3 delta){
    vec3 a=abs(delta);
    if(a.x>=a.y && a.x>=a.z) return delta.x>=0.0?0:1;
    if(a.y>=a.z) return delta.y>=0.0?2:3;
    return delta.z>=0.0?4:5;
}

float rectangle_sample_visibility(int sample_index,vec3 origin,vec3 point,vec3 n,float ndl){
    vec3 delta=point-origin;
    int layer=sample_index*6+shadow_face(delta);
    // 随投影在世界空间的纹素宽度调整偏移，减轻倾斜接收面的自遮挡。
    vec3 receiver=point+n*(length(delta)*frame.area_settings.z*(.2+.7*(1.0-ndl)));
    vec4 clip=area_shadow.matrices[layer]*vec4(receiver,1);
    if(clip.w<=0.0) return 1.0;
    vec3 p=clip.xyz/clip.w;
    if(p.z<=0.0 || p.z>=1.0) return 1.0;
    ivec2 extent=textureSize(area_depth,0).xy;
    vec2 pixel=clamp(p.xy*.5+.5,0.0,1.0)*vec2(extent)-.5;
    ivec2 base=ivec2(floor(pixel));
    vec2 f=fract(pixel);
    float vis=0.0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x){
        float z=texelFetch(area_depth,ivec3(clamp(base+ivec2(x,y),ivec2(0),extent-1),layer),0).r;
        vis+=(p.z-1e-6<=z?1.0:0.0)*(x==0?1.0-f.x:f.x)*(y==0?1.0-f.y:f.y);
    }
    return vis;
}

vec2 rectangle_visibility(int index,Rectangle lamp,vec3 point,vec3 n,vec3 v,float roughness,vec3 f0){
    int count=int(frame.area_settings.y);
    if(count==0) return vec2(1);
    vec3 u=lamp.corners[1].xyz-lamp.corners[0].xyz, w=lamp.corners[3].xyz-lamp.corners[0].xyz;
    vec3 normal=normalize(cross(u,w));
    vec2 visible=vec2(0),total=vec2(0);
    float alpha=max(.002,roughness*roughness),a2=alpha*alpha;
    for(int y=0;y<count;++y) for(int x=0;x<count;++x){
        vec3 source=lamp.corners[0].xyz+(float(x)+.5)/float(count)*u+(float(y)+.5)/float(count)*w;
        vec3 delta=source-point;
        float r2=max(dot(delta,delta),1e-10);
        vec3 l=delta*inversesqrt(r2);
        float ndl=max(0.0,dot(n,l));
        if(ndl<=0.0) continue;
        float projected=abs(dot(normal,-l))/r2;
        float h=clamp(dot(n,normalize(l+v)),0.0,1.0);
        float d=h*h*(a2-1.0)+1.0;
        float g=2.0*ndl/(ndl+sqrt(a2+(1.0-a2)*ndl*ndl));
        // 归一化时各采样点共用的系数相消。GGX 方向权重包括 D、G1(L) 和光源立体角。
        float fc=pow(1.0-clamp(dot(v,normalize(l+v)),0.0,1.0),5.0);
        float fresnel=dot(f0+(1.0-f0)*fc,vec3(.2126,.7152,.0722));
        vec2 weight=vec2(ndl,fresnel*g*a2/max(d*d,1e-12))*projected;
        float vis=rectangle_sample_visibility(index*count*count+y*count+x,source,point,n,ndl);
        total+=weight;visible+=weight*vis;
    }
    return vec2(total.x>1e-9?visible.x/total.x:1.0,total.y>1e-9?visible.y/total.y:1.0);
}
#endif
