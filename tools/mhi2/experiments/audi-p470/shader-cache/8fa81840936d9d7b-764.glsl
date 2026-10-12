precision highp float;
attribute vec4 a_position;
varying mediump float v_texCoord;
uniform mat4 u_mvpMatrix;
uniform float u_texCoordBias;
uniform float u_texCoordScale;
uniform float u_yTranslation;
void main(){vec4 r[64];vec4 c[64];vec4 v[16];vec4 o[16];vec4 t;
for(int i=0;i<64;i++){r[i]=vec4(0.0);c[i]=vec4(0.0);}for(int i=0;i<16;i++){v[i]=vec4(0.0);o[i]=vec4(0.0);}
c[0]=u_mvpMatrix[0];
c[1]=u_mvpMatrix[1];
c[2]=u_mvpMatrix[2];
c[3]=u_mvpMatrix[3];
c[5]=vec4(u_texCoordBias,0.0,0.0,0.0);
c[4]=vec4(u_texCoordScale,0.0,0.0,0.0);
c[6]=vec4(u_yTranslation,0.0,0.0,0.0);
v[0]=vec4(a_position);
t=vec4((c[6].xxxx)+(v[0].yyyy));r[0].x=t.x;
t=vec4((r[0].xxxx)*(c[1].xyzw));r[1].xyzw=t.xyzw;
t=vec4((v[0].xxxx)*(c[0].xyzw)+(r[1].xyzw));r[1].xyzw=t.xyzw;
t=vec4((v[0].zzzz)*(c[2].xyzw)+(r[1].xyzw));r[1].xyzw=t.xyzw;
t=vec4((r[0].xxxx)*(c[4].xxxx));r[0].x=t.x;
t=vec4((c[5].xxxx)+(r[0].xxxx));o[1].x=t.x;
t=vec4((v[0].wwww)*(c[3].xyzw)+(r[1].xyzw));o[0].xyzw=t.xyzw;
gl_Position=o[0];
v_texCoord=o[1].x;
}
