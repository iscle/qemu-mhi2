precision highp float;
varying mediump float v_fogFactor;
uniform vec4 u_color;
uniform float u_fogEnd;
uniform float u_fogStart;
uniform vec3 u_origin;
uniform float u_roadGeometryWidthHalf;
uniform vec2 u_scaleH;
uniform float u_scaleXYZ;
uniform float u_widthScale;
void main(){gl_FragColor=vec4(clamp(u_color.rgb,0.0,1.0),clamp(v_fogFactor,0.0,1.0)*u_color.a);}
