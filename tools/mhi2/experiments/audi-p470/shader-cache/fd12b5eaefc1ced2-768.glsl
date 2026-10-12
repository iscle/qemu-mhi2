precision highp float;
varying mediump vec2 v_texCoord;
uniform sampler2D s_texture0;
uniform float u_displacementWidth;
void main(){gl_FragColor=texture2D(s_texture0,v_texCoord);}
