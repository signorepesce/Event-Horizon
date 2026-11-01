#include <metal_stdlib>
using namespace metal;
constant float PI = 3.14159265359;
float hash(float3 p) {
    p = fract(p*0.1031); p += dot(p,p.yzx+33.33); return fract((p.x+p.y)*p.z);
}
float noise(float3 p) {
    float3 i=floor(p),f=fract(p); f=f*f*f*(f*(f*6.0-15.0)+10.0);
    return mix(mix(mix(hash(i),hash(i+float3(1,0,0)),f.x),mix(hash(i+float3(0,1,0)),hash(i+float3(1,1,0)),f.x),f.y),mix(mix(hash(i+float3(0,0,1)),hash(i+float3(1,0,1)),f.x),mix(hash(i+float3(0,1,1)),hash(i+1.0),f.x),f.y),f.z);
}
float fbm(float3 p,float footprint) {
    float v=0.0,a=0.53;
    for(int i=0;i<7;i++){
        float visibility=1.0-smoothstep(0.20,0.85,footprint);
        v+=a*mix(0.5,noise(p),visibility);
        p=p*2.07+float3(13.1,7.3,5.2);a*=0.49;footprint*=2.07;
    }
    return v;
}
float4 trace(float2 uv,float aspect) {
    float2 screen=(uv-0.5)*float2(aspect,-1.0);
    float3 eye=float3(0,2.65,19.0),forward=normalize(-eye),right=float3(1,0,0),up=cross(right,forward);
    float3 v=normalize(forward+0.98*(screen.x*right+screen.y*up));
    float3 p=eye;
    float h2=dot(cross(p,v),cross(p,v));
    for(int i=0;i<4000;i++) {
        float r=length(p);
        if(r<1.005)return float4(-1,0,0,1);
        if(r>35.0){float3 d=normalize(v);return float4(-2,d.x,d.y,d.z);}
        float dt=clamp(r*0.010,0.004,0.40);
        float3 acc=-1.5*h2*p/pow(r,5.0);
        float3 halfV=v+0.5*acc*dt;
        float3 pn=p+halfV*dt;
        float rn=length(pn);
        float3 vn=halfV-0.5*dt*1.5*h2*pn/pow(rn,5.0);
        if(p.y*pn.y<0.0) {
            float f=p.y/(p.y-pn.y);
            float3 c=mix(p,pn,f);
            float cr=length(c.xz);
            if(cr>=3.0&&cr<13.4) {
                float theta=atan2(c.z,c.x);
                float3 orbit=normalize(float3(-c.z,0,c.x));
                float speed=sqrt(0.5/(cr-1.0));
                float doppler=sqrt(1.0-1.0/cr)*sqrt(1.0-speed*speed)/(1.0+speed*dot(normalize(v),orbit));
                return float4(cr,theta,doppler,1);
            }
        }
        p=pn;v=vn;
    }
    return float4(-1,0,0,1);
}
kernel void geometry(texture2d<float,access::write> map [[texture(0)]],constant float4 &params [[buffer(0)]],uint2 gid [[thread_position_in_grid]]) {
    if(gid.x>=map.get_width()||gid.y>=map.get_height())return;
    float2 size=float2(map.get_width(),map.get_height());
    float aspect=size.x/size.y;
    const int S=4;
    float sumR=0,sumS=0,sumC=0,sumD=0,hits=0,captured=0,escaped=0;
    float3 away=0;
    for(int sy=0;sy<S;sy++)for(int sx=0;sx<S;sx++) {
        float4 t=trace((float2(gid)+(float2(sx,sy)+0.5)/float(S))/size,aspect);
        if(t.x>0){sumR+=t.x;sumS+=sin(t.y);sumC+=cos(t.y);sumD+=t.z;hits+=1;}
        else if(t.x<-1.5){away+=t.yzw;escaped+=1;}
        else captured+=1;
    }
    float4 hit=float4(-1,0,0,1);
    if(hits>0)hit=float4(sumR/hits,atan2(sumS,sumC),sumD/hits,hits/float(S*S));
    else if(escaped>=captured&&length(away)>0.0)hit=float4(-2,normalize(away));
    map.write(hit,gid);
}
float gas(float r,float angle,float phase,float footprint) {
    float theta=angle-phase;
    float3 p=float3(cos(theta)*r*0.85,sin(theta)*r*0.85,r*1.5);
    float n=fbm(p,footprint*1.5);
    float2 curl=float2(fbm(p*0.74+float3(3.7,8.2,1.5),footprint),fbm(p*0.74+float3(8.3,2.1,6.4),footprint))-0.5;
    float warped=r+0.28*(n-0.5)+0.16*curl.x;
    float angleWarp=theta+0.13*n+0.07*curl.y;
    float swirl=fbm(float3(cos(angleWarp)*r*3.0,sin(angleWarp)*r*3.0,warped*11.0),footprint*12.0);
    float fine=fbm(float3(cos(theta+0.04*curl.x)*r*8.0,sin(theta+0.04*curl.x)*r*8.0,warped*26.0),footprint*30.0);
    float micro=fbm(float3(cos(angleWarp)*r*18.0,sin(angleWarp)*r*18.0,warped*47.0),footprint*50.0);
    return 0.115+1.0*n+0.34*swirl+0.145*fine+0.035*micro;
}
kernel void clouds(texture2d<float,access::read> map [[texture(0)]],texture2d<float,access::write> target [[texture(1)]],constant float4 &params [[buffer(0)]],uint2 gid [[thread_position_in_grid]]) {
    if(gid.x>=target.get_width()||gid.y>=target.get_height())return;
    float4 m=map.read(gid);
    float3 color=0.0;
    if(m.x>0) {
        float r=m.x;
        float4 mx=map.read(min(gid+uint2(1,0),uint2(map.get_width()-1,map.get_height()-1)));
        float4 my=map.read(min(gid+uint2(0,1),uint2(map.get_width()-1,map.get_height()-1)));
        float ax=atan2(sin(mx.y-m.y),cos(mx.y-m.y));
        float ay=atan2(sin(my.y-m.y),cos(my.y-m.y));
        float footprint=max(length(float2(mx.x-r,r*ax)),length(float2(my.x-r,r*ay)));
        footprint=clamp(footprint,0.0001,0.3);
        float inner=smoothstep(3.0,3.35,r),outer=1.0-smoothstep(9.0,13.4,r);
        float n=mix(gas(r,m.y,params.x,footprint),gas(r,m.y,params.x*2.0,footprint),1.0-smoothstep(3.0,7.5,r));
        float b=pow(3.0/r,2.5)*inner*outer*pow(n,3.0)*4.2;
        b*=pow(m.z,2.0);
        float heat=clamp((6.0-r)/4.0+0.45*n,0.0,1.0);
        float3 cold=float3(1.0,0.06,0.003),hot=float3(1.0,0.58,0.20);
        color=mix(cold,hot,heat)*b*m.w;
    } else if(m.x<-1.5) {
        float3 d=normalize(m.yzw);
        float4 nx=map.read(min(gid+uint2(1,0),uint2(map.get_width()-1,map.get_height()-1)));
        float4 ny=map.read(min(gid+uint2(0,1),uint2(map.get_width()-1,map.get_height()-1)));
        float spread=0.0;
        if(nx.x<-1.5)spread=max(spread,length(d-normalize(nx.yzw)));
        if(ny.x<-1.5)spread=max(spread,length(d-normalize(ny.yzw)));
        float3 ad=abs(d);
        float2 face;float side;
        if(ad.x>=ad.y&&ad.x>=ad.z){face=d.yz/ad.x;side=d.x>0.0?0.0:1.0;}
        else if(ad.y>=ad.z){face=d.xz/ad.y;side=d.y>0.0?2.0:3.0;}
        else{face=d.xy/ad.z;side=d.z>0.0?4.0:5.0;}
        const float cells=400.0;
        float2 g=(face*0.5+0.5)*cells,id=floor(g),f=fract(g);
        float seed=side*37.0;
        float h=hash(float3(id,seed+17.0));
        float2 center=0.15+0.7*float2(hash(float3(id,seed+3.0)),hash(float3(id,seed+7.0)));
        float2 spot=(id+center)/cells*2.0-1.0;
        float3 aim;
        if(side<1.5)aim=float3(side<0.5?1.0:-1.0,spot.x,spot.y);
        else if(side<3.5)aim=float3(spot.x,side<2.5?1.0:-1.0,spot.y);
        else aim=float3(spot.x,spot.y,side<4.5?1.0:-1.0);
        float angle=length(d-normalize(aim));
        const float size=0.00037;
        float footprint=max(spread,size);
        float star=exp(-angle*angle/(footprint*footprint))*step(0.995,h)*(size*size)/(footprint*footprint);
        float glow=hash(float3(id,seed+29.0));
        float3 tone=mix(float3(1.0,0.80,0.62),float3(0.78,0.86,1.0),hash(float3(id,seed+23.0)));
        color=tone*star*(0.10+0.60*glow*glow*glow);
    }
    target.write(float4(color,1),gid);
}
kernel void downsample(texture2d<float,access::read> source [[texture(0)]],texture2d<float,access::write> target [[texture(1)]],constant float4 &params [[buffer(0)]],uint2 gid [[thread_position_in_grid]]) {
    if(gid.x>=target.get_width()||gid.y>=target.get_height())return;
    float4 c=0;for(uint y=0;y<4;y++)for(uint x=0;x<4;x++)c+=source.read(gid*4+uint2(x,y));
    target.write(c/16.0,gid);
}
kernel void blur(texture2d<float,access::sample> source [[texture(0)]],texture2d<float,access::write> target [[texture(1)]],constant float4 &params [[buffer(0)]],uint2 gid [[thread_position_in_grid]]) {
    if(gid.x>=target.get_width()||gid.y>=target.get_height())return;
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    float2 size=float2(target.get_width(),target.get_height()),uv=(float2(gid)+0.5)/size;
    float4 c=0;float weights=0;
    for(int i=-20;i<=20;i++){float w=exp(-float(i*i)/120.0);c+=source.sample(s,uv+params.xy*float(i)/float2(960,600))*w;weights+=w;}
    target.write(c/weights,gid);
}
kernel void composite(texture2d<float,access::read> source [[texture(0)]],texture2d<float,access::sample> bloom [[texture(1)]],texture2d<float,access::write> target [[texture(2)]],constant float4 &params [[buffer(0)]],uint2 gid [[thread_position_in_grid]]) {
    if(gid.x>=target.get_width()||gid.y>=target.get_height())return;
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    float2 uv=(float2(gid)+0.5)/float2(target.get_width(),target.get_height());
    float3 c=source.read(gid).rgb+bloom.sample(s,uv).rgb*0.22;
    c=1.0-exp(-c*1.25);
    c=pow(max(c,0.0),float3(1.0/2.2));
    float dither=(hash(float3(float2(gid),71.0))-0.5)*(0.65/255.0);
    c=clamp(c+dither*smoothstep(0.002,0.015,max(c.r,max(c.g,c.b))),0.0,1.0);
    target.write(float4(c,1),gid);
}
