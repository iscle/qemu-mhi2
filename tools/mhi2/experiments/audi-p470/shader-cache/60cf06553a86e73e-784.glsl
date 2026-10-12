precision highp float;
attribute vec4 a_color;
attribute vec4 a_position;
varying mediump vec4 v_color;
uniform float u_dispWidth;
uniform mat4 u_mvpMatrix;
void main(){vec4 r[64];vec4 c[64];vec4 v[16];vec4 o[16];vec4 t;
for(int i=0;i<64;i++){r[i]=vec4(0.0);c[i]=vec4(0.0);}for(int i=0;i<16;i++){v[i]=vec4(0.0);o[i]=vec4(0.0);}
c[5]=vec4(1.0,0.99000001,0.0,0.0);
c[4]=vec4(u_dispWidth,0.0,0.0,0.0);
c[0]=u_mvpMatrix[0];
c[1]=u_mvpMatrix[1];
c[2]=u_mvpMatrix[2];
c[3]=u_mvpMatrix[3];
v[1]=vec4(a_color);
v[0]=vec4(a_position);
t=vec4((v[0].xyzx)*(v[0].wwww));r[0].xyz=t.xyz;
t=vec4((r[0].xyzx)*(c[4].xxxx)+(v[0].xyzx));r[0].xyz=t.xyz;
t=vec4((r[0].yyyy)*(c[1].xywx));r[1].xyz=t.xyz;
t=vec4((r[0].xxxx)*(c[0].xywx)+(r[1].xyzx));r[1].xyz=t.xyz;
t=vec4((r[0].zzzz)*(c[2].xywx)+(r[1].xyzx));r[0].xyz=t.xyz;
t=vec4((c[3].xywx)+(r[0].xyzx));r[0].xyz=t.xyz;
t=vec4(v[1].xyzw);o[1].xyzw=t.xyzw;
t=vec4((r[0].xyzz)*(c[5].xxyx));o[0].xyzw=t.xyzw;
gl_Position=o[0];
v_color=o[1].xyzw;
}
