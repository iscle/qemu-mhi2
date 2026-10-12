precision highp float;
varying mediump vec2 v_texCoord;varying mediump float v_fogFactor;
uniform sampler2D s_texture0;
uniform vec4 u_color;
uniform vec3 u_fogColor;
uniform float u_fogEnd;
uniform float u_fogStart;
void main(){float a=texture2D(s_texture0,v_texCoord).a;vec3 c=clamp(mix(u_fogColor,u_color.rgb,clamp(v_fogFactor,0.0,1.0)),0.0,1.0);gl_FragColor=vec4(c*a,a);}
