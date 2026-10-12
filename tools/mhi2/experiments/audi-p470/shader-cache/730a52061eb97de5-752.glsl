precision highp float;
varying mediump vec4 v_color;
uniform float u_fadeout;
void main(){gl_FragColor=clamp(vec4(v_color.rgb,v_color.a*u_fadeout),0.0,1.0);}
