precision highp float;
varying mediump float v_texCoord;
uniform sampler2D s_texture0;
uniform vec2 u_texCoordTrans;
void main(){gl_FragColor=texture2D(s_texture0,vec2(v_texCoord,0.5));}
