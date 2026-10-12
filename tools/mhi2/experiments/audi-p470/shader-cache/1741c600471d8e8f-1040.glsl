precision highp float;
varying mediump vec2 v_texCoord;
uniform sampler2D s_texture0;
uniform vec3 u_origin;
uniform float u_roadGeometryWidthHalf;
uniform vec2 u_scaleH;
uniform float u_scaleXYZ;
uniform vec2 u_texCoordsScaleAndOffset;
uniform float u_widthScale;
void main(){gl_FragColor=texture2D(s_texture0,v_texCoord);}
