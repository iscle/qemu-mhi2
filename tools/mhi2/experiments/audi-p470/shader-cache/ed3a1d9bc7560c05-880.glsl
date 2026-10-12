precision highp float;
varying mediump vec2 v_texCoord;
uniform sampler2D s_texture0;
void main(){vec4 c=texture2D(s_texture0,v_texCoord);if(c.a<=0.0)discard;gl_FragColor=c;}
