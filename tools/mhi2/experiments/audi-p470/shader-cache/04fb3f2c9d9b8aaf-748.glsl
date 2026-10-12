precision highp float;

uniform sampler2D s_texture0;
uniform float u_pointSize;
void main(){gl_FragColor=texture2D(s_texture0,gl_PointCoord);}
