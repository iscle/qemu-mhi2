precision highp float;

uniform vec4 u_color;
uniform vec3 u_origin;
uniform float u_roadGeometryWidthHalf;
uniform vec2 u_scaleH;
uniform float u_scaleXYZ;
uniform float u_widthScale;
void main(){gl_FragColor=clamp(u_color,0.0,1.0);}
