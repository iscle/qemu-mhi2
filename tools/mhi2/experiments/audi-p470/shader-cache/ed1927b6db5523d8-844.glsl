precision highp float;
attribute vec4 a_position;
attribute float a_texCoord;
varying mediump float v_texCoord;
uniform sampler2D s_texture0;
uniform mat4 u_mvpMatrix;
uniform vec2 u_texCoordTrans;
void main(){vec4 r[64];vec4 c[64];vec4 v[16];vec4 o[16];vec4 t;
for(int i=0;i<64;i++){r[i]=vec4(0.0);c[i]=vec4(0.0);}for(int i=0;i<16;i++){v[i]=vec4(0.0);o[i]=vec4(0.0);}
c[5]=vec4(0.99000001,0.0,0.0,0.0);
c[0]=u_mvpMatrix[0];
c[1]=u_mvpMatrix[1];
c[2]=u_mvpMatrix[2];
c[3]=u_mvpMatrix[3];
c[4]=vec4(u_texCoordTrans,0.0,0.0);
v[0]=vec4(a_position);
v[1]=vec4(a_texCoord,0.0,0.0,1.0);
t=vec4((v[0].yyyy)*(c[1].xywx));r[0].xyz=t.xyz;
t=vec4((v[0].xxxx)*(c[0].xywx)+(r[0].xyzx));r[0].xyz=t.xyz;
t=vec4((v[0].zzzz)*(c[2].xywx)+(r[0].xyzx));r[0].xyz=t.xyz;
t=vec4((c[3].xywx)+(r[0].xyzx));r[0].xyz=t.xyz;
t=vec4((v[1].xxxx)*(c[4].yyyy)+(c[4].xxxx));o[1].x=t.x;
t=vec4(r[0].xyxx);o[0].xy=t.xy;
t=vec4((abs(r[0].zzzz))*(c[5].xxxx));o[0].z=t.z;
t=vec4(abs(r[0].zzzz));o[0].w=t.w;
gl_Position=o[0];
v_texCoord=o[1].x;
}
