precision highp float;
attribute vec4 a_position;
attribute vec2 a_texCoord;
varying mediump vec2 v_texCoord;
uniform sampler2D s_texture0;
uniform mat4 u_mvpMatrix;
uniform vec2 u_texCoordScaleXY;
void main(){vec4 r[64];vec4 c[64];vec4 v[16];vec4 o[16];vec4 t;
for(int i=0;i<64;i++){r[i]=vec4(0.0);c[i]=vec4(0.0);}for(int i=0;i<16;i++){v[i]=vec4(0.0);o[i]=vec4(0.0);}
c[0]=u_mvpMatrix[0];
c[1]=u_mvpMatrix[1];
c[2]=u_mvpMatrix[2];
c[3]=u_mvpMatrix[3];
c[4]=vec4(u_texCoordScaleXY,0.0,0.0);
v[0]=vec4(a_position);
v[1]=vec4(a_texCoord,0.0,1.0);
t=vec4((v[0].yyyy)*(c[1].xyzw));r[0].xyzw=t.xyzw;
t=vec4((v[0].xxxx)*(c[0].xyzw)+(r[0].xyzw));r[0].xyzw=t.xyzw;
t=vec4((v[0].zzzz)*(c[2].xyzw)+(r[0].xyzw));r[0].xyzw=t.xyzw;
t=vec4((c[3].xyzw)+(r[0].xyzw));o[0].xyzw=t.xyzw;
t=vec4((v[1].xyxx)*(c[4].xyxx));o[1].xy=t.xy;
gl_Position=o[0];
v_texCoord=o[1].xy;
}
