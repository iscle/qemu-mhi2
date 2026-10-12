precision highp float;
varying mediump float v_texCoord;
uniform vec3 u_skyColorBottom;
uniform vec3 u_skyColorTop;
void main(){gl_FragColor=vec4(mix(u_skyColorBottom,u_skyColorTop,clamp(v_texCoord,0.0,1.0)),0.0);}
